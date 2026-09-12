#include "Tools/GetBlueprintOverviewTool.h"

#include "EdGraph/EdGraph.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/Package.h"

FGetBlueprintOverviewTool::FGetBlueprintOverviewTool()
    : FMCPToolBase(TEXT("GetBlueprintOverview"), TEXT("Inspect a Blueprint asset in one bounded live read: parent, compile/dirty state, graph, variable, component and interface inventory. Filter and page members before requesting detailed pins. No compile, save, index refresh or level editing.")) {}

UnrealMCP::FMCPResponse FGetBlueprintOverviewTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, Query, Kind = TEXT("all");
    int32 Offset = 0, Limit = 30;
    bool bAllowLoad = true;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Requires an exact Blueprint objectPath."));
    for (const FString& Field : { FString(TEXT("query")), FString(TEXT("kind")), FString(TEXT("allowLoad")) })
    {
        const auto* Value = Request.Params->Values.Find(Field);
        if (Value && (!Value->IsValid() || (*Value)->Type != (Field == TEXT("allowLoad") ? EJson::Boolean : EJson::String)))
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("query/kind must be strings and allowLoad must be a boolean."));
    }
    Request.Params->TryGetStringField(TEXT("query"), Query);
    Request.Params->TryGetStringField(TEXT("kind"), Kind);
    Request.Params->TryGetBoolField(TEXT("allowLoad"), bAllowLoad);
    for (const FString& Field : { FString(TEXT("offset")), FString(TEXT("limit")) })
    {
        double Value = Field == TEXT("offset") ? Offset : Limit;
        if (Request.Params->HasField(Field) && (!Request.Params->TryGetNumberField(Field, Value) || !FMath::IsFinite(Value) ||
            Value != FMath::FloorToDouble(Value) || Value < 0 || Value > MAX_int32))
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("offset and limit must be nonnegative integers."));
        if (Field == TEXT("offset")) Offset = static_cast<int32>(Value); else Limit = static_cast<int32>(Value);
    }
    if (Limit < 1 || Limit > 100 || Query.Len() > 256 ||
        (Kind != TEXT("all") && Kind != TEXT("graph") && Kind != TEXT("variable") && Kind != TEXT("component") && Kind != TEXT("interface")))
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("limit must be 1-100, query at most 256 characters, kind all/graph/variable/component/interface."));

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    FString Error;
    const bool bSuccess = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *ObjectPath);
        const bool bWasResident = Blueprint != nullptr;
        if (!Blueprint && bAllowLoad) Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
        if (!Blueprint || Blueprint->IsA<ULevelScriptBlueprint>())
        {
            OutError = TEXT("Blueprint asset unavailable. Use an exact asset objectPath; level scripts are outside this overview. allowLoad=false requires a resident asset.");
            return false;
        }

        struct FMember
        {
            FString Kind, Name;
            UEdGraph* Graph = nullptr;
            const FBPVariableDescription* Variable = nullptr;
            const USCS_Node* Component = nullptr;
            UClass* Interface = nullptr;
        };
        TArray<FMember> Inventory;
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
            if (Graph) Inventory.Add({ TEXT("graph"), Graph->GetName(), Graph });
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
            Inventory.Add({ TEXT("variable"), Variable.VarName.ToString(), nullptr, &Variable });
        int32 ComponentCount = 0;
        if (Blueprint->SimpleConstructionScript)
            for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
                if (Node)
                {
                    Inventory.Add({ TEXT("component"), Node->GetVariableName().ToString(), nullptr, nullptr, Node });
                    ++ComponentCount;
                }
        for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
            if (Interface.Interface) Inventory.Add({ TEXT("interface"), Interface.Interface->GetName(), nullptr, nullptr, nullptr, Interface.Interface });
        Inventory.Sort([](const FMember& A, const FMember& B)
        {
            if (A.Kind != B.Kind) return A.Kind < B.Kind;
            if (A.Name != B.Name) return A.Name < B.Name;
            return A.Graph && B.Graph && A.Graph->GetPathName() < B.Graph->GetPathName();
        });

        int32 Matched = 0;
        TArray<TSharedPtr<FJsonValue>> Members;
        for (const FMember& Member : Inventory)
        {
            if ((Kind != TEXT("all") && Kind != Member.Kind) || (!Query.IsEmpty() && !Member.Name.Contains(Query))) continue;
            const int32 MatchIndex = Matched++;
            if (MatchIndex < Offset || Members.Num() >= Limit) continue;
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("kind"), Member.Kind);
            Item->SetStringField(TEXT("name"), Member.Name);
            if (Member.Graph)
            {
                Item->SetStringField(TEXT("graphPath"), Member.Graph->GetPathName());
                Item->SetStringField(TEXT("graphGuid"), Member.Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
                Item->SetStringField(TEXT("graphRevision"), UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Member.Graph));
                Item->SetNumberField(TEXT("nodeCount"), Member.Graph->Nodes.Num());
            }
            if (Member.Variable)
            {
                Item->SetStringField(TEXT("guid"), Member.Variable->VarGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
                Item->SetObjectField(TEXT("type"), UnrealMCP::BlueprintToolUtils::SerializePinType(Member.Variable->VarType));
            }
            if (Member.Component)
            {
                Item->SetStringField(TEXT("classPath"), GetPathNameSafe(Member.Component->ComponentClass));
                Item->SetStringField(TEXT("templatePath"), GetPathNameSafe(Member.Component->ComponentTemplate));
            }
            if (Member.Interface) Item->SetStringField(TEXT("classPath"), Member.Interface->GetPathName());
            Members.Add(MakeShared<FJsonValueObject>(Item));
        }
        const bool bHasMore = static_cast<int64>(Offset) + Members.Num() < Matched;
        Result->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
        Result->SetStringField(TEXT("source"), TEXT("live_editor"));
        Result->SetBoolField(TEXT("readOnly"), true);
        Result->SetBoolField(TEXT("indexUsed"), false);
        Result->SetBoolField(TEXT("loadedForInspection"), !bWasResident);
        Result->SetBoolField(TEXT("packageDirty"), Blueprint->GetOutermost()->IsDirty());
        Result->SetStringField(TEXT("compileStatus"), UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status));
        Result->SetStringField(TEXT("parentClassPath"), GetPathNameSafe(Blueprint->ParentClass));
        Result->SetStringField(TEXT("scope"), TEXT("declared_members_and_owned_graphs; inherited_members_excluded; live_pages_not_a_snapshot"));
        Result->SetNumberField(TEXT("graphCount"), Graphs.Num());
        Result->SetNumberField(TEXT("variableCount"), Blueprint->NewVariables.Num());
        Result->SetNumberField(TEXT("componentCount"), ComponentCount);
        Result->SetNumberField(TEXT("interfaceCount"), Blueprint->ImplementedInterfaces.Num());
        Result->SetNumberField(TEXT("scannedCount"), Inventory.Num());
        Result->SetNumberField(TEXT("matchedCount"), Matched);
        Result->SetNumberField(TEXT("returnedCount"), Members.Num());
        Result->SetNumberField(TEXT("offset"), Offset);
        Result->SetBoolField(TEXT("hasMore"), bHasMore);
        Result->SetBoolField(TEXT("truncated"), bHasMore || (Offset > 0 && Matched > 0));
        Result->SetBoolField(TEXT("coverageComplete"), Offset == 0 && !bHasMore);
        if (bHasMore) Result->SetNumberField(TEXT("nextOffset"), Offset + Members.Num());
        Result->SetArrayField(TEXT("members"), Members);
        return true;
    }, Error);
    if (!bSuccess) return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Error);
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetBlueprintOverviewTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Exact Blueprint asset object path. Required; no current-level fallback.")));
    Properties->SetObjectField(TEXT("kind"), BuildStringProperty(TEXT("all (default), graph, variable, component, or interface.")));
    Properties->SetObjectField(TEXT("query"), BuildStringProperty(TEXT("Case-insensitive member-name substring, at most 256 characters.")));
    Properties->SetObjectField(TEXT("allowLoad"), BuildBoolProperty(TEXT("Load the exact asset if needed; default true. False restricts inspection to resident objects.")));
    for (const FString& Field : { FString(TEXT("offset")), FString(TEXT("limit")) })
    {
        auto Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), TEXT("integer"));
        Property->SetNumberField(TEXT("minimum"), Field == TEXT("offset") ? 0 : 1);
        Property->SetNumberField(TEXT("maximum"), Field == TEXT("offset") ? MAX_int32 : 100);
        Property->SetNumberField(TEXT("default"), Field == TEXT("offset") ? 0 : 30);
        Properties->SetObjectField(Field, Property);
    }
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), { MakeShared<FJsonValueString>(TEXT("objectPath")) });
    return Schema;
}
