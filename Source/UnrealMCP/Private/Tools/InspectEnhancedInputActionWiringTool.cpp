#include "Tools/InspectEnhancedInputActionWiringTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UnrealType.h"

namespace
{
    const TCHAR* PhaseNames[] = {TEXT("Started"), TEXT("Triggered"), TEXT("Ongoing"), TEXT("Canceled"), TEXT("Completed")};

    TSharedRef<FJsonObject> SerializePhase(UEdGraphNode* Node, const FString& Phase)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("phase"), Phase);
        UEdGraphPin* Pin = Node != nullptr ? Node->FindPin(*Phase, EGPD_Output) : nullptr;
        Result->SetBoolField(TEXT("pinFound"), Pin != nullptr); Result->SetStringField(TEXT("pinId"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Pin));
        TArray<TSharedPtr<FJsonValue>> Routes;
        if (Pin != nullptr) for (UEdGraphPin* Linked : Pin->LinkedTo)
        {
            UEdGraphNode* Target = Linked != nullptr ? Linked->GetOwningNode() : nullptr;
            TSharedRef<FJsonObject> Route = MakeShared<FJsonObject>();
            Route->SetStringField(TEXT("targetNodeGuid"), UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Target));
            Route->SetStringField(TEXT("targetNodeTitle"), Target != nullptr ? Target->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());
            Route->SetStringField(TEXT("targetNodeClassPath"), Target != nullptr ? Target->GetClass()->GetPathName() : FString());
            Route->SetStringField(TEXT("targetPinId"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Linked)); Route->SetStringField(TEXT("targetPinName"), Linked != nullptr ? Linked->PinName.ToString() : FString());
            Routes.Add(MakeShared<FJsonValueObject>(Route));
        }
        Result->SetNumberField(TEXT("routeCount"), Routes.Num()); Result->SetArrayField(TEXT("routes"), Routes); return Result;
    }
}

FInspectEnhancedInputActionWiringTool::FInspectEnhancedInputActionWiringTool()
    : FMCPToolBase(TEXT("InspectEnhancedInputActionWiring"), TEXT("Finds Enhanced Input Action event nodes and reports exact Started/Triggered/Ongoing/Canceled/Completed execution routes."))
{
}

UnrealMCP::FMCPResponse FInspectEnhancedInputActionWiringTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, GraphName, GraphGuid, InputActionPath, NodeGuid, Error;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("InspectEnhancedInputActionWiring requires objectPath."));
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName); Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("inputActionPath"), InputActionPath); Request.Params->TryGetStringField(TEXT("nodeGuid"), NodeGuid);
    TArray<TSharedPtr<FJsonValue>> Nodes;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr; if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        UClass* NodeClass = LoadObject<UClass>(nullptr, TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction"));
        FObjectPropertyBase* ActionProperty = NodeClass != nullptr ? FindFProperty<FObjectPropertyBase>(NodeClass, TEXT("InputAction")) : nullptr;
        if (NodeClass == nullptr || ActionProperty == nullptr) { OutError = TEXT("Enhanced Input Blueprint nodes are unavailable. Enable the Enhanced Input plugin."); return false; }
        TArray<UEdGraph*> Graphs;
        if (!GraphName.IsEmpty() || !GraphGuid.IsEmpty()) { UEdGraph* Graph = nullptr; if (!BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError)) return false; Graphs.Add(Graph); }
        else Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs) for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node == nullptr || !Node->IsA(NodeClass)) continue;
            const FString CurrentGuid = BlueprintGraphEditToolUtils::GetNodeGuid(Node);
            UObject* Action = ActionProperty->GetObjectPropertyValue_InContainer(Node); const FString CurrentActionPath = Action != nullptr ? Action->GetPathName() : FString();
            if (!NodeGuid.IsEmpty() && !CurrentGuid.Equals(NodeGuid, ESearchCase::IgnoreCase)) continue;
            if (!InputActionPath.IsEmpty() && !CurrentActionPath.Equals(InputActionPath, ESearchCase::IgnoreCase)) continue;
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("graphName"), Graph->GetName()); Item->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Item->SetStringField(TEXT("nodeGuid"), CurrentGuid); Item->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString()); Item->SetStringField(TEXT("inputActionPath"), CurrentActionPath);
            TArray<TSharedPtr<FJsonValue>> Phases; for (const TCHAR* Phase : PhaseNames) Phases.Add(MakeShared<FJsonValueObject>(SerializePhase(Node, Phase))); Item->SetArrayField(TEXT("phases"), Phases);
            Nodes.Add(MakeShared<FJsonValueObject>(Item));
        }
        return true;
    }, Error);
    if (!bSucceeded) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    FMCPResponse Response; Response.Id = Request.Id; TSharedRef<FJsonObject> Result = BuildBooleanResult(true); Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetNumberField(TEXT("nodeCount"), Nodes.Num()); Result->SetArrayField(TEXT("nodes"), Nodes); Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FInspectEnhancedInputActionWiringTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object")); TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path."))); P->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Optional exact graph name."))); P->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Optional exact graph GUID.")));
    P->SetObjectField(TEXT("inputActionPath"), BuildStringProperty(TEXT("Optional exact InputAction object path filter."))); P->SetObjectField(TEXT("nodeGuid"), BuildStringProperty(TEXT("Optional exact Enhanced Input node GUID filter.")));
    Schema->SetObjectField(TEXT("properties"), P); Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath"))}); return Schema;
}
