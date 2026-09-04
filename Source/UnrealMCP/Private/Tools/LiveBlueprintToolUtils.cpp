#include "Tools/LiveBlueprintToolUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_Literal.h"
#include "K2Node_Variable.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/WorldToolUtils.h"
#include "UObject/Package.h"

namespace UnrealMCP::LiveBlueprintToolUtils
{
    FString String(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name)
    {
        FString Value;
        if (Params) Params->TryGetStringField(Name, Value);
        return Value;
    }

    int32 Bound(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, int32 Default, int32 Min, int32 Max)
    {
        int32 Value = Default;
        if (Params) Params->TryGetNumberField(Name, Value);
        return FMath::Clamp(Value, Min, Max);
    }

    bool Resolve(const TSharedPtr<FJsonObject>& Params, FTarget& Target, FString& Error)
    {
        check(IsInGameThread());
        for (const TCHAR* Field : {TEXT("objectPath"), TEXT("mapPath"), TEXT("level"), TEXT("graphPath")})
        {
            FString Value;
            if (Params && Params->HasField(Field) && !Params->TryGetStringField(Field, Value))
            {
                Error = FString::Printf(TEXT("invalid_target_type: %s must be a string."), Field);
                return false;
            }
        }
        const FString ObjectPath = String(Params, TEXT("objectPath"));
        const FString MapPath = String(Params, TEXT("mapPath"));
        const FString LevelSelector = String(Params, TEXT("level"));
        if ((!ObjectPath.IsEmpty() && (!MapPath.IsEmpty() || !LevelSelector.IsEmpty()))
            || (!MapPath.IsEmpty() && !LevelSelector.IsEmpty()))
        {
            Error = TEXT("conflicting_target: use only one of objectPath, mapPath, or level.");
            return false;
        }

        UEdGraph* DirectGraph = nullptr;
        if (!ObjectPath.IsEmpty())
        {
            // FindObject never loads a package or invokes PostLoad/Blueprint compilation.
            UObject* Object = FindObject<UObject>(nullptr, *ObjectPath);
            DirectGraph = Cast<UEdGraph>(Object);
            Target.Blueprint = DirectGraph ? DirectGraph->GetTypedOuter<UBlueprint>() : Cast<UBlueprint>(Object);
            if (!Target.Blueprint)
            {
                Error = TEXT("live_object_unavailable: objectPath must identify a resident Blueprint or graph. Open its map/asset in the editor first.");
                return false;
            }
            Target.Level = Target.Blueprint->GetTypedOuter<ULevel>();
            Target.World = Target.Level ? Target.Level->GetWorld() : nullptr;
        }
        else
        {
            if (!WorldToolUtils::GetEditorWorld(Target.World, Error)) return false;
            if (MapPath.IsEmpty())
            {
                if (!LevelSelector.IsEmpty() && LevelSelector != TEXT("current") && LevelSelector != TEXT("persistent"))
                {
                    Error = TEXT("invalid_level: level must be current or persistent; use mapPath for a loaded sublevel.");
                    return false;
                }
                Target.Level = LevelSelector == TEXT("persistent") ? Target.World->PersistentLevel.Get() : Target.World->GetCurrentLevel();
            }
            else
            {
                for (ULevel* Level : Target.World->GetLevels())
                {
                    if (!Level) continue;
                    const FString Package = Level->GetOutermost()->GetName();
                    const UWorld* OuterWorld = Level->GetTypedOuter<UWorld>();
                    if (MapPath == Package || (OuterWorld && MapPath == OuterWorld->GetPathName()) || MapPath == Level->GetPathName())
                    {
                        if (Target.Level)
                        {
                            Error = TEXT("ambiguous_level: mapPath matched more than one loaded level; use an exact level object path.");
                            return false;
                        }
                        Target.Level = Level;
                    }
                }
            }
            if (!Target.Level)
            {
                Error = TEXT("live_level_unavailable: requested level is not loaded in the editor world. Open/load it manually; inspection does not switch maps.");
                return false;
            }
            // Even GetLevelScriptBlueprint(true) assigns FriendlyName in UE 5.4.
            // Read the existing pointer directly to preserve all authored state.
            Target.Blueprint = Target.Level->LevelScriptBlueprint;
            if (!Target.Blueprint)
            {
                Error = TEXT("level_blueprint_missing: this level has no existing LevelScriptBlueprint; inspection will not create one.");
                return false;
            }
        }
        if (Target.Blueprint->GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor)
            || (Target.World && Target.World->WorldType != EWorldType::Editor))
        {
            Error = TEXT("editor_only: PIE and non-editor worlds are not inspection targets.");
            return false;
        }

        Target.Blueprint->GetAllGraphs(Target.Graphs);
        const FString GraphPath = String(Params, TEXT("graphPath"));
        if (DirectGraph && !GraphPath.IsEmpty() && GraphPath != DirectGraph->GetPathName())
        {
            Error = TEXT("conflicting_graph: objectPath and graphPath select different graphs.");
            return false;
        }
        Target.Graphs.RemoveAll([&](UEdGraph* Graph)
        {
            return !Graph || (DirectGraph && Graph != DirectGraph) || (!GraphPath.IsEmpty() && Graph->GetPathName() != GraphPath);
        });
        if ((DirectGraph || !GraphPath.IsEmpty()) && Target.Graphs.IsEmpty())
        {
            Error = TEXT("graph_not_found: graphPath is not owned by the resolved live Blueprint.");
            return false;
        }
        Target.Graphs.Sort([](const UEdGraph& A, const UEdGraph& B) { return A.GetPathName() < B.GetPathName(); });
        return true;
    }

    TSharedRef<FJsonObject> DescribeTarget(const FTarget& Target)
    {
        auto Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("source"), TEXT("live_editor"));
        Result->SetBoolField(TEXT("readOnly"), true);
        Result->SetBoolField(TEXT("indexUsed"), false);
        Result->SetStringField(TEXT("objectPath"), Target.Blueprint->GetPathName());
        Result->SetStringField(TEXT("blueprintClassPath"), Target.Blueprint->GetClass()->GetPathName());
        Result->SetBoolField(TEXT("packageDirty"), Target.Blueprint->GetOutermost()->IsDirty());
        if (Target.Level)
        {
            Result->SetStringField(TEXT("mapPath"), Target.Level->GetOutermost()->GetName());
            Result->SetStringField(TEXT("levelPath"), Target.Level->GetPathName());
            Result->SetStringField(TEXT("worldPath"), Target.World ? Target.World->GetPathName() : FString());
            Result->SetBoolField(TEXT("isPersistentLevel"), Target.World && Target.Level == Target.World->PersistentLevel);
            Result->SetBoolField(TEXT("isCurrentLevel"), Target.World && Target.Level == Target.World->GetCurrentLevel());
        }
        return Result;
    }

    FMCPResponse ReadError(const FMCPRequest& Request, const FString& Message)
    {
        FMCPResponse Response;
        Response.Id = Request.Id;
        FMCPError Error;
        Error.Code = EMCPErrorCode::InvalidParams;
        Error.Message = Message;
        Error.Data = MakeShared<FJsonObject>();
        FString Code, Detail;
        if (!Message.Split(TEXT(":"), &Code, &Detail)) Code = TEXT("live_inspection_failed");
        Error.Data->SetStringField(TEXT("errorCode"), Code);
        Error.Data->SetStringField(TEXT("source"), TEXT("live_editor"));
        Error.Data->SetBoolField(TEXT("readOnly"), true);
        Error.Data->SetBoolField(TEXT("indexUsed"), false);
        Response.Error = Error;
        return Response;
    }

    TSharedRef<FJsonObject> DescribeGraph(UEdGraph* Graph)
    {
        auto Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
        Result->SetStringField(TEXT("graphName"), Graph->GetName());
        Result->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Result->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
        Result->SetStringField(TEXT("revision"), BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph));
        return Result;
    }

    FString MemberName(const UEdGraphNode* Node)
    {
        if (const auto* Call = Cast<UK2Node_CallFunction>(Node)) return Call->GetFunctionName().ToString();
        if (const auto* Variable = Cast<UK2Node_Variable>(Node)) return Variable->GetVarName().ToString();
        if (const auto* Event = Cast<UK2Node_CustomEvent>(Node)) return Event->CustomFunctionName.ToString();
        if (const auto* Event = Cast<UK2Node_Event>(Node)) return Event->EventReference.GetMemberName().ToString();
        return FString();
    }

    TSharedRef<FJsonObject> Endpoint(const UEdGraphPin* Pin)
    {
        auto Result = MakeShared<FJsonObject>();
        if (!Pin || !Pin->GetOwningNode()) return Result;
        const UEdGraphNode* Node = Pin->GetOwningNode();
        Result->SetStringField(TEXT("nodePath"), Node->GetPathName());
        Result->SetStringField(TEXT("graphPath"), Node->GetGraph()->GetPathName());
        Result->SetStringField(TEXT("nodeGuid"), BlueprintGraphEditToolUtils::GetNodeGuid(Node));
        Result->SetStringField(TEXT("pinId"), BlueprintGraphEditToolUtils::GetPinId(Pin));
        Result->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
        return Result;
    }

    TSharedRef<FJsonObject> DescribeNode(UEdGraphNode* Node, bool bIncludePins)
    {
        auto Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("source"), TEXT("live_editor"));
        Result->SetStringField(TEXT("nodePath"), Node->GetPathName());
        Result->SetStringField(TEXT("graphPath"), Node->GetGraph()->GetPathName());
        if (UBlueprint* Blueprint = Node->GetTypedOuter<UBlueprint>()) Result->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
        Result->SetStringField(TEXT("nodeGuid"), BlueprintGraphEditToolUtils::GetNodeGuid(Node));
        Result->SetStringField(TEXT("nodeClassPath"), Node->GetClass()->GetPathName());
        Result->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Result->SetStringField(TEXT("memberName"), MemberName(Node));
        Result->SetStringField(TEXT("comment"), Node->NodeComment);
        Result->SetBoolField(TEXT("enabled"), Node->IsNodeEnabled());
        Result->SetNumberField(TEXT("positionX"), Node->NodePosX);
        Result->SetNumberField(TEXT("positionY"), Node->NodePosY);
        if (const auto* Call = Cast<UK2Node_CallFunction>(Node))
        {
            if (const UFunction* Function = Call->GetTargetFunction())
            {
                Result->SetStringField(TEXT("functionPath"), Function->GetPathName());
                Result->SetStringField(TEXT("memberOwnerClass"), Function->GetOwnerClass()->GetPathName());
                Result->SetBoolField(TEXT("isLatent"), Function->HasMetaData(TEXT("Latent")));
                Result->SetBoolField(TEXT("isPure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
                Result->SetStringField(TEXT("continuation"), Function->HasMetaData(TEXT("Latent")) ? TEXT("deferred_exec_outputs") : TEXT("connected_exec_outputs"));
            }
        }
        if (const auto* Variable = Cast<UK2Node_Variable>(Node))
        {
            UBlueprint* Blueprint = Node->GetTypedOuter<UBlueprint>();
            UClass* Scope = Blueprint ? Blueprint->GeneratedClass : nullptr;
            if (UClass* Owner = Variable->VariableReference.GetMemberParentClass(Scope)) Result->SetStringField(TEXT("memberOwnerClass"), Owner->GetPathName());
            Result->SetBoolField(TEXT("selfContext"), Variable->VariableReference.IsSelfContext());
        }
        if (const auto* Literal = Cast<UK2Node_Literal>(Node))
        {
            if (AActor* Actor = Literal->GetReferencedLevelActor())
            {
                Result->SetObjectField(TEXT("referencedActor"), WorldToolUtils::SerializeActorSummary(Actor, false));
                Result->SetStringField(TEXT("referenceConfidence"), TEXT("exact_editor_reference"));
            }
        }
        if (bIncludePins)
        {
            auto Pins = BlueprintGraphEditToolUtils::SerializePins(Node);
            int32 Index = 0;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                auto Item = Pins[Index++]->AsObject();
                TArray<TSharedPtr<FJsonValue>> Links;
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (Linked && Linked->GetOwningNode()) Links.Add(MakeShared<FJsonValueObject>(Endpoint(Linked)));
                }
                Item->SetArrayField(TEXT("links"), Links);
                Item->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);
                Item->SetStringField(TEXT("valueSemantics"), TEXT("authored_default_not_runtime_evaluation"));
            }
            Result->SetArrayField(TEXT("pins"), Pins);
        }
        return Result;
    }

    TSharedPtr<FJsonObject> TargetSchema()
    {
        using namespace BlueprintEditToolUtils;
        auto Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Exact resident Blueprint or graph object path, including embedded LevelScriptBlueprint objects. Never loads assets.")));
        Properties->SetObjectField(TEXT("mapPath"), BuildStringProperty(TEXT("Loaded map package, world object path, or exact level path in the editor world. Exclusive with objectPath/level.")));
        Properties->SetObjectField(TEXT("level"), BuildStringProperty(TEXT("current (default) or persistent in the open editor world. Exclusive with objectPath/mapPath.")));
        Properties->SetObjectField(TEXT("graphPath"), BuildStringProperty(TEXT("Optional exact graph object path within the resolved Blueprint.")));
        return Properties;
    }

    bool Inspect(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& Result, FString& Error)
    {
        FTarget Target;
        if (!Resolve(Params, Target, Error)) return false;
        const FString Mode = String(Params, TEXT("mode"));
        if (!Mode.IsEmpty() && Mode != TEXT("graphs") && Mode != TEXT("nodes"))
        {
            Error = TEXT("invalid_mode: mode must be graphs or nodes.");
            return false;
        }
        const int32 Offset = Bound(Params, TEXT("offset"), 0, 0, MAX_int32);
        const int32 Limit = Bound(Params, TEXT("limit"), 50, 1, 200);
        const FString Guid = String(Params, TEXT("nodeGuid"));
        const FString Query = String(Params, TEXT("query"));
        const bool bNodes = Mode == TEXT("nodes") || !Guid.IsEmpty() || !Query.IsEmpty();
        FGuid ParsedGuid;
        if (!Guid.IsEmpty() && !FGuid::Parse(Guid, ParsedGuid))
        {
            Error = TEXT("invalid_node_guid: nodeGuid must be a GUID.");
            return false;
        }
        TArray<TSharedPtr<FJsonValue>> Items;
        int32 Matched = 0, Scanned = 0;
        for (UEdGraph* Graph : Target.Graphs)
        {
            if (!bNodes)
            {
                ++Scanned;
                if (Matched++ >= Offset && Items.Num() < Limit) Items.Add(MakeShared<FJsonValueObject>(DescribeGraph(Graph)));
                continue;
            }
            TArray<UEdGraphNode*> Nodes;
            for (UEdGraphNode* Node : Graph->Nodes) if (Node) Nodes.Add(Node);
            Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.GetPathName() < B.GetPathName(); });
            for (UEdGraphNode* Node : Nodes)
            {
                ++Scanned;
                if (!Guid.IsEmpty() && Node->NodeGuid != ParsedGuid) continue;
                if (!Query.IsEmpty() && !MemberName(Node).Contains(Query) && !Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(Query) && !Node->GetName().Contains(Query)) continue;
                if (Matched++ >= Offset && Items.Num() < Limit) Items.Add(MakeShared<FJsonValueObject>(DescribeNode(Node, true)));
            }
        }
        Result = DescribeTarget(Target);
        Result->SetArrayField(bNodes ? TEXT("nodes") : TEXT("graphs"), Items);
        Result->SetNumberField(TEXT("scannedCount"), Scanned);
        Result->SetNumberField(TEXT("matchedCount"), Matched);
        Result->SetNumberField(TEXT("offset"), Offset);
        Result->SetNumberField(TEXT("count"), Items.Num());
        const bool bMore = static_cast<int64>(Offset) + Items.Num() < Matched;
        Result->SetBoolField(TEXT("hasMore"), bMore);
        Result->SetBoolField(TEXT("truncated"), bMore || Offset > 0);
        Result->SetBoolField(TEXT("coverageComplete"), !bMore && Offset == 0);
        if (bMore) Result->SetNumberField(TEXT("nextOffset"), Offset + Items.Num());
        return true;
    }
}
