#include "Tools/Blueprint/Graph/WrapWithIsValidTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Tools/Blueprint/Graph/ApplyBlueprintGraphPatchTool.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"

FWrapWithIsValidTool::FWrapWithIsValidTool()
    : FMCPToolBase(TEXT("WrapWithIsValid"),
        TEXT("Atomically inserts an Is Valid branch before a Blueprint execution input, with an optional invalid route. Defaults to dry-run."))
{
}

UnrealMCP::FMCPResponse FWrapWithIsValidTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    using namespace BlueprintEditToolUtils;
    auto Read = [&](const TCHAR* Key)
    {
        FString Value;
        if (Request.Params) Request.Params->TryGetStringField(Key, Value);
        return Value;
    };
    const FString Path = Read(TEXT("objectPath")), GraphName = Read(TEXT("graphName")), Guid = Read(TEXT("graphGuid"));
    const FString TargetGuid = Read(TEXT("nodeGuid")), InputName = Read(TEXT("execPinName"));
    const FString SourceGuid = Read(TEXT("objectSourceNodeGuid")), SourceName = Read(TEXT("objectSourcePinName"));
    const FString ElseGuid = Read(TEXT("elseNodeGuid")), ElseName = Read(TEXT("elseExecPinName"));
    const FString Expected = Read(TEXT("expectedRevision"));
    const bool bDryRun = GetOptionalBool(Request.Params, TEXT("dryRun"), true);
    if (Path.IsEmpty() || (GraphName.IsEmpty() && Guid.IsEmpty()) || TargetGuid.IsEmpty() || InputName.IsEmpty()
        || SourceGuid.IsEmpty() || SourceName.IsEmpty() || (ElseGuid.IsEmpty() != ElseName.IsEmpty())
        || (!bDryRun && (Expected.IsEmpty() || !GetOptionalBool(Request.Params, TEXT("confirm"), false))))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide graph and exact pin endpoints. Apply requires confirm=true and expectedRevision from preview."));

    FString Error;
    FMCPRequest Patch = Request;
    Patch.Params = MakeShared<FJsonObject>();
    const bool bPrepared = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        UEdGraphNode* Target = nullptr;
        UEdGraphNode* Source = nullptr;
        UEdGraphNode* Else = nullptr;
        using namespace BlueprintGraphEditToolUtils;
        if (!ResolveBlueprint(Path, Blueprint, OutError) || !ResolveGraph(Blueprint, GraphName, Guid, Graph, OutError)
            || !ResolveNode(Graph, TargetGuid, Target, OutError) || !ResolveNode(Graph, SourceGuid, Source, OutError)
            || (!ElseGuid.IsEmpty() && !ResolveNode(Graph, ElseGuid, Else, OutError)))
            return false;

        const FString Revision = ComputeGraphRevision(Graph);
        if (!Expected.IsEmpty() && Expected != Revision)
        {
            OutError = TEXT("Graph changed since preview.");
            return false;
        }
        const UEdGraphPin* Input = Target->FindPin(FName(*InputName), EGPD_Input);
        const UEdGraphPin* Object = Source->FindPin(FName(*SourceName), EGPD_Output);
        const UEdGraphPin* Invalid = Else ? Else->FindPin(FName(*ElseName), EGPD_Input) : nullptr;
        if (!Input || Input->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec || Input->LinkedTo.Num() != 1
            || !Input->LinkedTo[0] || !Object || Object->PinType.PinCategory != UEdGraphSchema_K2::PC_Object
            || Object->PinType.ContainerType != EPinContainerType::None || Source == Target
            || (Else && (!Invalid || Invalid == Input || Invalid->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)))
        {
            OutError = TEXT("Expected one incoming exec link, a scalar object output, and an optional distinct invalid exec input.");
            return false;
        }
        const UEdGraphPin* Predecessor = Input->LinkedTo[0];
        TArray<TSharedPtr<FJsonValue>> Nodes, Connections;
        auto AddNode = [&](const TCHAR* Id, const TCHAR* Kind, const UEdGraphNode* Existing = nullptr)
        {
            auto Node = MakeShared<FJsonObject>();
            Node->SetStringField(TEXT("id"), Id);
            Node->SetStringField(TEXT("kind"), Kind);
            if (Existing) Node->SetStringField(TEXT("nodeGuid"), Existing->NodeGuid.ToString());
            Nodes.Add(MakeShared<FJsonValueObject>(Node));
            return Node;
        };
        auto Link = [](const TCHAR* From, const FString& FromPin, const TCHAR* To, const FString& ToPin)
        {
            auto Connection = MakeShared<FJsonObject>();
            Connection->SetStringField(TEXT("sourceNodeId"), From);
            Connection->SetStringField(TEXT("sourcePinName"), FromPin);
            Connection->SetStringField(TEXT("targetNodeId"), To);
            Connection->SetStringField(TEXT("targetPinName"), ToPin);
            return Connection;
        };
        AddNode(TEXT("target"), TEXT("existingNode"), Target);
        AddNode(TEXT("source"), TEXT("existingNode"), Source);
        AddNode(TEXT("predecessor"), TEXT("existingNode"), Predecessor->GetOwningNode());
        AddNode(TEXT("branch"), TEXT("branch"));
        auto Valid = AddNode(TEXT("valid"), TEXT("functionCall"));
        Valid->SetStringField(TEXT("ownerClassPath"), TEXT("/Script/Engine.KismetSystemLibrary"));
        Valid->SetStringField(TEXT("functionName"), TEXT("IsValid"));
        Connections.Add(MakeShared<FJsonValueObject>(Link(TEXT("source"), SourceName, TEXT("valid"), TEXT("Object"))));
        Connections.Add(MakeShared<FJsonValueObject>(Link(TEXT("valid"), TEXT("ReturnValue"), TEXT("branch"), TEXT("Condition"))));
        if (Else)
        {
            AddNode(TEXT("invalid"), TEXT("existingNode"), Else);
            Connections.Add(MakeShared<FJsonValueObject>(Link(TEXT("branch"), TEXT("else"), TEXT("invalid"), ElseName)));
        }

        auto Splice = Link(TEXT("predecessor"), Predecessor->PinName.ToString(), TEXT("target"), InputName);
        Splice->SetBoolField(TEXT("confirm"), true);
        Splice->SetStringField(TEXT("insertNodeId"), TEXT("branch"));
        Splice->SetStringField(TEXT("insertInputPinName"), TEXT("execute"));
        Splice->SetStringField(TEXT("insertOutputPinName"), TEXT("then"));
        Patch.Params->SetStringField(TEXT("objectPath"), Path);
        Patch.Params->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString());
        Patch.Params->SetStringField(TEXT("expectedGraphRevision"), Revision);
        Patch.Params->SetStringField(TEXT("patchId"), TEXT("WrapWithIsValid_") + Target->NodeGuid.ToString() + TEXT("_") + InputName);
        Patch.Params->SetArrayField(TEXT("nodes"), Nodes);
        Patch.Params->SetArrayField(TEXT("connections"), Connections);
        Patch.Params->SetArrayField(TEXT("spliceExec"), {MakeShared<FJsonValueObject>(Splice)});
        Patch.Params->SetBoolField(TEXT("dryRun"), bDryRun);
        return true;
    }, Error);
    if (!bPrepared) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    Patch.Params->SetStringField(TEXT("operationId"), Read(TEXT("operationId")));
    return FApplyBlueprintGraphPatchTool().Execute(Patch);
}

TSharedPtr<FJsonObject> FWrapWithIsValidTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    for (const TCHAR* Key : {TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("nodeGuid"),
        TEXT("execPinName"), TEXT("objectSourceNodeGuid"), TEXT("objectSourcePinName"), TEXT("elseNodeGuid"),
        TEXT("elseExecPinName"), TEXT("expectedRevision")})
        Properties->SetObjectField(Key, BuildStringProperty(Key));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preview only. Defaults to true.")));
    Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Confirm apply.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required;
    for (const TCHAR* Key : {TEXT("objectPath"), TEXT("nodeGuid"), TEXT("execPinName"), TEXT("objectSourceNodeGuid"), TEXT("objectSourcePinName")})
        Required.Add(MakeShared<FJsonValueString>(Key));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
