#include "Tools/FindClassUsagesTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Variable.h"
#include "Misc/PackageName.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    struct FUsageCollector
    {
        FName TargetPackage;
        FString NativeTargetPath; // set for /Script targets, where a package holds many unrelated classes
        int32 Limit = 100;
        int32 Total = 0;
        TArray<TSharedPtr<FJsonValue>> Hits;

        bool Matches(const UObject* Object) const
        {
            if (Object == nullptr) return false;
            return NativeTargetPath.IsEmpty()
                ? Object->GetOutermost()->GetFName() == TargetPackage
                : Object->GetPathName().Equals(NativeTargetPath, ESearchCase::IgnoreCase);
        }

        void Add(const TCHAR* Kind, const FString& Graph, const FString& NodeGuid, const FString& Pin, const FString& Detail)
        {
            ++Total;
            if (Hits.Num() >= Limit)
            {
                return;
            }
            TSharedRef<FJsonObject> Hit = MakeShared<FJsonObject>();
            Hit->SetStringField(TEXT("kind"), Kind);
            Hit->SetStringField(TEXT("graph"), Graph);
            Hit->SetStringField(TEXT("nodeGuid"), NodeGuid);
            Hit->SetStringField(TEXT("pin"), Pin);
            Hit->SetStringField(TEXT("detail"), Detail);
            Hits.Add(MakeShared<FJsonValueObject>(Hit));
        }

        bool PinTypeHits(const FEdGraphPinType& Type) const
        {
            return Matches(Type.PinSubCategoryObject.Get()) || Matches(Type.PinValueType.TerminalSubCategoryObject.Get());
        }
    };

    void ScanNode(const UEdGraphNode* Node, const FString& GraphName, FUsageCollector& C)
    {
        const FString NodeGuid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
        const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

        if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
        {
            if (C.Matches(CastNode->TargetType))
            {
                C.Add(TEXT("cast_target"), GraphName, NodeGuid, FString(), Title);
            }
        }
        if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
        {
            if (const UFunction* Function = Call->GetTargetFunction())
            {
                if (C.Matches(Function->GetOwnerClass()))
                {
                    C.Add(TEXT("function_call"), GraphName, NodeGuid, FString(), Title);
                }
            }
        }
        if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
        {
            if (C.Matches(Var->VariableReference.GetMemberParentClass(nullptr)))
            {
                C.Add(TEXT("variable_access"), GraphName, NodeGuid, FString(), Var->GetVarName().ToString());
            }
        }
        if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            for (const FBPVariableDescription& Local : Entry->LocalVariables)
            {
                if (C.PinTypeHits(Local.VarType))
                {
                    C.Add(TEXT("local_variable"), GraphName, NodeGuid, Local.VarName.ToString(), Title);
                }
            }
        }

        const bool bIsSignature = Node->IsA<UK2Node_FunctionEntry>() || Node->GetClass()->GetName().Contains(TEXT("FunctionResult"));
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (C.PinTypeHits(Pin->PinType))
            {
                C.Add(bIsSignature ? TEXT("function_parameter") : TEXT("pin_type"), GraphName, NodeGuid, Pin->PinName.ToString(), Title);
            }
            if (C.Matches(Pin->DefaultObject))
            {
                C.Add(TEXT("pin_default"), GraphName, NodeGuid, Pin->PinName.ToString(), Title);
            }
        }
    }
}

FFindClassUsagesTool::FFindClassUsagesTool()
    : FMCPToolBase(TEXT("FindClassUsages"), TEXT("Finds every use of a class/struct/enum/asset inside one Blueprint, tagged by kind (cast_target, function_call, variable_access, pin_type, pin_default, function_parameter, local_variable, member_variable, component, parent_class, interface) with graph/node GUID/pin, including collapsed graphs. Reports registryDependency and a stale_import_only hit when the package still depends on the target but no live use exists."))
{
}

UnrealMCP::FMCPResponse FFindClassUsagesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString TargetPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty()
        || !Request.Params->TryGetStringField(TEXT("classOrAssetPath"), TargetPath) || TargetPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindClassUsages requires objectPath (Blueprint) and classOrAssetPath."));
    }

    double LimitValue = 100;
    Request.Params->TryGetNumberField(TEXT("limit"), LimitValue);

    FUsageCollector Collector;
    Collector.Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
    // Accepts /Game/Pkg.Obj, /Game/Pkg.Obj_C or a bare package name; usage is matched by owning package.
    Collector.TargetPackage = FName(*FPackageName::ObjectPathToPackageName(TargetPath));
    if (TargetPath.StartsWith(TEXT("/Script/")))
    {
        if (!TargetPath.Contains(TEXT(".")))
        {
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
                TEXT("Native targets need a full path such as /Script/Engine.Pawn; a bare /Script package matches every class in it."));
        }
        Collector.NativeTargetPath = TargetPath;
    }

    bool bRegistryDependency = false;
    int32 GraphCount = 0;
    FString ExecutionError;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            UBlueprint* Blueprint = UnrealMCP::BlueprintToolUtils::LoadBlueprintFromObjectPath(ObjectPath);
            if (!Blueprint)
            {
                OutError = TEXT("FindClassUsages could not load a Blueprint from objectPath.");
                return false;
            }

            if (Collector.Matches(Blueprint->ParentClass))
            {
                Collector.Add(TEXT("parent_class"), FString(), FString(), FString(), Blueprint->ParentClass->GetName());
            }
            for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
            {
                if (Collector.Matches(Interface.Interface))
                {
                    Collector.Add(TEXT("interface"), FString(), FString(), FString(), Interface.Interface->GetName());
                }
            }
            for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
            {
                if (Collector.PinTypeHits(Variable.VarType))
                {
                    Collector.Add(TEXT("member_variable"), FString(), FString(), Variable.VarName.ToString(), FString());
                }
            }
            if (Blueprint->SimpleConstructionScript)
            {
                for (const USCS_Node* ScsNode : Blueprint->SimpleConstructionScript->GetAllNodes())
                {
                    if (ScsNode && Collector.Matches(ScsNode->ComponentClass))
                    {
                        Collector.Add(TEXT("component"), FString(), FString(), ScsNode->GetVariableName().ToString(), FString());
                    }
                }
            }

            // GetAllGraphs recurses into sub-graphs, so collapsed graphs are covered.
            TArray<UEdGraph*> Graphs;
            Blueprint->GetAllGraphs(Graphs);
            GraphCount = Graphs.Num();
            for (const UEdGraph* Graph : Graphs)
            {
                for (const UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node)
                    {
                        ScanNode(Node, Graph->GetPathName(Blueprint), Collector);
                    }
                }
            }

            FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            TArray<FName> Dependencies;
            Registry.Get().GetDependencies(Blueprint->GetOutermost()->GetFName(), Dependencies);
            bRegistryDependency = Dependencies.Contains(Collector.TargetPackage);
            return true;
        },
        ExecutionError);

    if (!bOk)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    // The package still points at the target while nothing live uses it: a stale import-table entry.
    if (bRegistryDependency && Collector.Total == 0 && Collector.NativeTargetPath.IsEmpty())
    {
        Collector.Add(TEXT("stale_import_only"), FString(), FString(), FString(), TEXT("Package depends on target but no graph, variable, component or pin uses it. Recompile and resave, or fix redirectors."));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("target"), Collector.TargetPackage.ToString());
    Result->SetBoolField(TEXT("registryDependency"), bRegistryDependency);
    Result->SetNumberField(TEXT("graphsScanned"), GraphCount);
    Result->SetNumberField(TEXT("totalHits"), Collector.Total);
    Result->SetBoolField(TEXT("truncated"), Collector.Total > Collector.Hits.Num());
    Result->SetArrayField(TEXT("usages"), Collector.Hits);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindClassUsagesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    auto AddProp = [&Properties](const TCHAR* PropName, const TCHAR* PropType, const TCHAR* PropDesc)
    {
        TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
        Prop->SetStringField(TEXT("type"), PropType);
        Prop->SetStringField(TEXT("description"), PropDesc);
        Properties->SetObjectField(PropName, Prop);
    };
    AddProp(TEXT("objectPath"), TEXT("string"), TEXT("Blueprint to scan, e.g. /Game/BP_Plan.BP_Plan"));
    AddProp(TEXT("classOrAssetPath"), TEXT("string"), TEXT("Class, struct, enum or asset to look for, e.g. /Game/BP_Vehicle.BP_Vehicle or /Game/BP_Vehicle.BP_Vehicle_C"));
    AddProp(TEXT("limit"), TEXT("integer"), TEXT("Max hits returned (default 100, max 500); totalHits still counts everything."));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Required.Add(MakeShared<FJsonValueString>(TEXT("classOrAssetPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
