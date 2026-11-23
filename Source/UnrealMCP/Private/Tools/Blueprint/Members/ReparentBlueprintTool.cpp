#include "Tools/Blueprint/Members/ReparentBlueprintTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/InheritableComponentHandler.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectHash.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"

namespace
{
    constexpr int32 MaxEntries = 200;

    // A member declared on Owner survives the reparent only when the new parent also derives from Owner.
    struct FReparentScope
    {
        UClass* OldParent = nullptr;
        UClass* NewParent = nullptr;

        bool IsLost(const UClass* Owner) const
        {
            // Members resolved through a skeleton class belong to SKEL_X_C, which X_C does not derive from.
            if (Owner != nullptr) Owner = const_cast<UClass*>(Owner)->GetAuthoritativeClass();
            return Owner != nullptr && OldParent->IsChildOf(Owner) && !NewParent->IsChildOf(Owner);
        }
    };

    struct FReport
    {
        TArray<TSharedPtr<FJsonValue>> BreakingNodes;
        TArray<TSharedPtr<FJsonValue>> DroppedOverrides;
        TArray<TSharedPtr<FJsonValue>> DefaultChanges;
        TArray<TSharedPtr<FJsonValue>> LostInterfaces;
        int32 Total = 0;

        static void Add(TArray<TSharedPtr<FJsonValue>>& List, int32& Total, const TSharedRef<FJsonObject>& Entry)
        {
            ++Total;
            if (List.Num() < MaxEntries) List.Add(MakeShared<FJsonValueObject>(Entry));
        }
    };

    TSharedRef<FJsonObject> NodeEntry(const UEdGraphNode* Node, const TCHAR* Kind, const FString& Member, const UClass* Owner)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("kind"), Kind);
        Entry->SetStringField(TEXT("graph"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
        Entry->SetStringField(TEXT("nodeGuid"), UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node));
        Entry->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Entry->SetStringField(TEXT("member"), Member);
        Entry->SetStringField(TEXT("declaredIn"), Owner ? const_cast<UClass*>(Owner)->GetAuthoritativeClass()->GetPathName() : FString());
        return Entry;
    }

    void ScanGraphs(UBlueprint* Blueprint, const FReparentScope& Scope, FReport& Report)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (const UEdGraph* Graph : Graphs)
        {
            if (Graph == nullptr) continue;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
                {
                    if (!Variable->VariableReference.IsSelfContext()) continue;
                    const FProperty* Property = Variable->VariableReference.ResolveMember<FProperty>(Blueprint->SkeletonGeneratedClass);
                    if (Property && Scope.IsLost(Property->GetOwnerClass()))
                        FReport::Add(Report.BreakingNodes, Report.Total, NodeEntry(Node, TEXT("variable"), Property->GetName(), Property->GetOwnerClass()));
                }
                else if (const UK2Node_CallParentFunction* ParentCall = Cast<UK2Node_CallParentFunction>(Node))
                {
                    const UFunction* Function = ParentCall->GetTargetFunction();
                    if (Function && Scope.IsLost(Function->GetOwnerClass()))
                        FReport::Add(Report.DroppedOverrides, Report.Total, NodeEntry(Node, TEXT("parent_call"), Function->GetName(), Function->GetOwnerClass()));
                }
                else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
                {
                    if (!Call->FunctionReference.IsSelfContext()) continue;
                    const UFunction* Function = Call->GetTargetFunction();
                    if (Function && Scope.IsLost(Function->GetOwnerClass()))
                        FReport::Add(Report.BreakingNodes, Report.Total, NodeEntry(Node, TEXT("function_call"), Function->GetName(), Function->GetOwnerClass()));
                }
                else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
                {
                    if (Event->IsA<UK2Node_CustomEvent>() || !Event->bOverrideFunction) continue;
                    const UClass* Owner = Event->EventReference.GetMemberParentClass(Blueprint->SkeletonGeneratedClass);
                    if (Scope.IsLost(Owner))
                        FReport::Add(Report.DroppedOverrides, Report.Total, NodeEntry(Node, TEXT("event_override"), Event->EventReference.GetMemberName().ToString(), Owner));
                }
            }
        }

        for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            const UFunction* Overridden = Graph ? Scope.OldParent->FindFunctionByName(Graph->GetFName()) : nullptr;
            if (Overridden && Scope.IsLost(Overridden->GetOwnerClass()))
            {
                TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("kind"), TEXT("function_override"));
                Entry->SetStringField(TEXT("graph"), Graph->GetName());
                Entry->SetStringField(TEXT("member"), Overridden->GetName());
                Entry->SetStringField(TEXT("declaredIn"), Overridden->GetOwnerClass()->GetPathName());
                FReport::Add(Report.DroppedOverrides, Report.Total, Entry);
            }
        }

        if (UInheritableComponentHandler* Handler = Blueprint->InheritableComponentHandler)
        {
            for (auto It = Handler->CreateRecordIterator(); It; ++It)
            {
                UClass* Owner = It->ComponentKey.GetComponentOwner();
                if (!Scope.IsLost(Owner)) continue;
                TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("kind"), TEXT("component_override"));
                Entry->SetStringField(TEXT("member"), It->ComponentKey.GetSCSVariableName().ToString());
                Entry->SetStringField(TEXT("declaredIn"), Owner->GetPathName());
                FReport::Add(Report.DroppedOverrides, Report.Total, Entry);
            }
        }

        for (const FImplementedInterface& Interface : Scope.OldParent->Interfaces)
        {
            if (Interface.Class && !Scope.NewParent->ImplementsInterface(Interface.Class))
                Report.LostInterfaces.Add(MakeShared<FJsonValueString>(Interface.Class->GetPathName()));
        }
    }

    FString ExportValue(const FProperty* Property, const UObject* Object)
    {
        FString Value;
        Property->ExportText_InContainer(0, Value, Object, Object, const_cast<UObject*>(Object), PPF_None);
        return Value;
    }

    FString PropertyIdentity(const FProperty* Property)
    {
        UClass* Owner = Property ? Property->GetOwnerClass() : nullptr;
        return Owner ? Owner->GetAuthoritativeClass()->GetPathName() + TEXT(":") + Property->GetName() : FString();
    }

    struct FDefaultUsage
    {
        TSet<FString> GraphProperties;
        TArray<UClass*> Descendants;
        TArray<TSharedPtr<FJsonValue>> FailedDescendants;
        bool bComplete = true;
        bool bTruncated = false;
    };

    FDefaultUsage GatherDefaultUsage(UBlueprint* Blueprint)
    {
        FDefaultUsage Usage;
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (const UEdGraph* Graph : Graphs) if (Graph) for (const UEdGraphNode* Node : Graph->Nodes)
            if (const auto* Variable = Cast<UK2Node_Variable>(Node))
                if (const FProperty* P = Variable->VariableReference.ResolveMember<FProperty>(Blueprint->SkeletonGeneratedClass))
                    Usage.GraphProperties.Add(PropertyIdentity(P));

        if (!Blueprint->GeneratedClass) { Usage.bComplete = false; return Usage; }
        constexpr int32 MaxDescendants = 256;
        TArray<UClass*> Loaded;
        GetDerivedClasses(Blueprint->GeneratedClass, Loaded, true);
        Loaded.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });
        auto AddClass = [&](UClass* Class)
        {
            if (!Class || Class->HasAnyClassFlags(CLASS_NewerVersionExists)) return;
            Class = Class->GetAuthoritativeClass();
            if (Class == Blueprint->GeneratedClass || !Class->IsChildOf(Blueprint->GeneratedClass) || Usage.Descendants.Contains(Class)) return;
            if (Usage.Descendants.Num() >= MaxDescendants) { Usage.bTruncated = true; Usage.bComplete = false; return; }
            Usage.Descendants.Add(Class);
        };
        for (UClass* Class : Loaded) AddClass(Class);

        // The registry includes unloaded descendants; suppress compile-on-load during this read-only report.
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Usage.bComplete &= !Registry.IsLoadingAssets();
        TSet<FTopLevelAssetPath> Derived;
        Registry.GetDerivedClassNames({Blueprint->GeneratedClass->GetClassPathName()}, {}, Derived);
        TArray<FTopLevelAssetPath> Paths = Derived.Array();
        Paths.Sort([](const FTopLevelAssetPath& A, const FTopLevelAssetPath& B) { return A.ToString() < B.ToString(); });
        int32 Examined = 0;
        for (const FTopLevelAssetPath& Path : Paths)
        {
            if (++Examined > MaxDescendants) { Usage.bTruncated = true; Usage.bComplete = false; break; }
            const FString ClassName = Path.GetAssetName().ToString();
            const FString AssetPath = Path.GetPackageName().ToString() + TEXT(".") + ClassName.LeftChop(2);
            UBlueprint* Child = ClassName.EndsWith(TEXT("_C"))
                ? LoadObject<UBlueprint>(nullptr, *AssetPath, nullptr, LOAD_DisableCompileOnLoad) : nullptr;
            if (!Child || !Child->GeneratedClass || !Child->GeneratedClass->IsChildOf(Blueprint->GeneratedClass))
            {
                Usage.bComplete = false;
                Usage.FailedDescendants.Add(MakeShared<FJsonValueString>(Path.ToString()));
                continue;
            }
            AddClass(Child->GeneratedClass);
        }
        return Usage;
    }

    bool HasDescendantOverride(const FProperty* Property, const FDefaultUsage& Usage)
    {
        const UClass* DeclaringClass = Property->GetOwnerClass()->GetAuthoritativeClass();
        for (UClass* Class : Usage.Descendants)
        {
            UClass* Parent = Class->GetSuperClass();
            if (!Parent || !Class->IsChildOf(DeclaringClass) || !Parent->IsChildOf(DeclaringClass)) continue;
            const UObject* Defaults = Class->GetDefaultObject();
            const UObject* ParentDefaults = Parent->GetDefaultObject();
            if (Defaults && ParentDefaults && !Property->Identical_InContainer(Defaults, ParentDefaults)) return true;
        }
        return false;
    }

    // Defaults the Blueprint now inherits from the old parent chain: either lost (declared on a class the new
    // parent lacks) or changing (shared declaration, but the two parents' defaults differ and the Blueprint
    // does not override it). The latter is the "ModelScale silently became 0" class of bug.
    void ScanDefaults(UBlueprint* Blueprint, const FReparentScope& Scope, FReport& Report, const FDefaultUsage* Usage)
    {
        const UObject* ChildDefaults = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
        const UObject* OldDefaults = Scope.OldParent->GetDefaultObject();
        const UObject* NewDefaults = Scope.NewParent->GetDefaultObject();
        if (ChildDefaults == nullptr || !ChildDefaults->IsA(Scope.OldParent)) return;

        for (TFieldIterator<FProperty> It(Scope.OldParent); It; ++It)
        {
            const FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)
                || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_InstancedReference | CPF_ContainsInstancedReference))
                continue;

            if (Usage && !Usage->GraphProperties.Contains(PropertyIdentity(Property))
                && Property->Identical_InContainer(ChildDefaults, OldDefaults) && !HasDescendantOverride(Property, *Usage)) continue;

            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("property"), Property->GetName());
            Entry->SetStringField(TEXT("declaredIn"), Property->GetOwnerClass()->GetPathName());
            Entry->SetStringField(TEXT("currentValue"), ExportValue(Property, ChildDefaults));
            if (Scope.IsLost(Property->GetOwnerClass()))
            {
                Entry->SetStringField(TEXT("change"), TEXT("lost"));
                Entry->SetBoolField(TEXT("overriddenInChild"), !Property->Identical_InContainer(ChildDefaults, OldDefaults));
                FReport::Add(Report.DefaultChanges, Report.Total, Entry);
            }
            else if (Property->Identical_InContainer(ChildDefaults, OldDefaults) && !Property->Identical_InContainer(OldDefaults, NewDefaults))
            {
                Entry->SetStringField(TEXT("change"), TEXT("inherited_value_changes"));
                Entry->SetStringField(TEXT("newValue"), ExportValue(Property, NewDefaults));
                FReport::Add(Report.DefaultChanges, Report.Total, Entry);
            }
        }
    }
}

FReparentBlueprintTool::FReparentBlueprintTool()
    : FMCPToolBase(
        TEXT("ReparentBlueprint"),
        TEXT("Changes a Blueprint's parent class. Dry-run/preflight lists graph nodes using old-parent members the new parent lacks (graph, nodeGuid, member), dropped overrides, lost interfaces, and inherited defaults that are lost or change value. onlyUsed filters defaultChanges by graph usage or target/descendant overrides and reports descendant coverage. Apply refreshes nodes, compiles, and reports located errors; never saves a failed compile unless allowSaveWithErrors=true."))
{
}

UnrealMCP::FMCPResponse FReparentBlueprintTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, NewParentPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty()
        || !Request.Params->TryGetStringField(TEXT("newParentClassPath"), NewParentPath) || NewParentPath.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("ReparentBlueprint requires objectPath and newParentClassPath."));
    }
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bConfirm = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compile"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("save"), false);
    const bool bOnlyUsed = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("onlyUsed"), false);
    const bool bAllowSaveWithErrors = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("allowSaveWithErrors"), false);
    if (!bDryRun && !bConfirm)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("ReparentBlueprint requires confirm=true unless dryRun=true. Run a dryRun first and review the report."));
    }

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    FString ExecutionError;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        UClass* NewParent = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)
            || !BlueprintEditToolUtils::ResolveClass(NewParentPath, NewParent, OutError))
        {
            return false;
        }
        FReparentScope Scope{Blueprint->ParentClass, NewParent};
        if (Scope.OldParent == nullptr)
        {
            OutError = TEXT("The Blueprint has no current parent class.");
            return false;
        }
        if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(NewParent))
        {
            OutError = FString::Printf(TEXT("%s cannot be a Blueprint parent."), *NewParent->GetName());
            return false;
        }
        if (Blueprint->GeneratedClass && (NewParent == Blueprint->GeneratedClass || NewParent->IsChildOf(Blueprint->GeneratedClass)
            || (Blueprint->SkeletonGeneratedClass && NewParent->IsChildOf(Blueprint->SkeletonGeneratedClass))))
        {
            OutError = TEXT("The new parent is this Blueprint or one of its children; that would be a cycle.");
            return false;
        }

        Result->SetStringField(TEXT("objectPath"), ObjectPath);
        Result->SetStringField(TEXT("oldParentClassPath"), Scope.OldParent->GetPathName());
        Result->SetStringField(TEXT("newParentClassPath"), NewParent->GetPathName());
        Result->SetBoolField(TEXT("dryRun"), bDryRun);
        Result->SetBoolField(TEXT("onlyUsed"), bOnlyUsed);
        if (Scope.OldParent == NewParent)
        {
            Result->SetBoolField(TEXT("unchanged"), true);
            return true;
        }

        FReport Report;
        ScanGraphs(Blueprint, Scope, Report);
        FDefaultUsage Usage;
        if (bOnlyUsed)
        {
            Usage = GatherDefaultUsage(Blueprint);
            Result->SetStringField(TEXT("defaultUsageCoverage"), TEXT("Target variable get/set nodes, target CDO overrides, loaded descendants and Asset Registry-derived Blueprint descendants; CDO overrides are value differences from the immediate parent."));
            Result->SetBoolField(TEXT("descendantCoverageComplete"), Usage.bComplete);
            Result->SetBoolField(TEXT("descendantScanTruncated"), Usage.bTruncated);
            Result->SetNumberField(TEXT("descendantsScanned"), Usage.Descendants.Num());
            Result->SetArrayField(TEXT("descendantLoadFailures"), Usage.FailedDescendants);
        }
        ScanDefaults(Blueprint, Scope, Report, bOnlyUsed ? &Usage : nullptr);
        Result->SetArrayField(TEXT("breakingNodes"), Report.BreakingNodes);
        Result->SetArrayField(TEXT("droppedOverrides"), Report.DroppedOverrides);
        Result->SetArrayField(TEXT("defaultChanges"), Report.DefaultChanges);
        Result->SetArrayField(TEXT("lostInterfaces"), Report.LostInterfaces);
        Result->SetNumberField(TEXT("reportedCount"), Report.Total);
        Result->SetBoolField(TEXT("truncated"), Report.BreakingNodes.Num() + Report.DroppedOverrides.Num() + Report.DefaultChanges.Num() < Report.Total);
        if (bDryRun) return true;

        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "ReparentBlueprint", "UnrealMCP Reparent Blueprint"));
            Blueprint->Modify();
            Blueprint->ParentClass = NewParent;
            FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        Result->SetBoolField(TEXT("reparented"), true);

        bool bCompiled = false;
        if (bCompile)
        {
            TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
            bCompiled = BlueprintEditToolUtils::CompileWithLocatedMessages(Blueprint, Errors, Warnings);
            Result->SetBoolField(TEXT("compiled"), bCompiled);
            Result->SetStringField(TEXT("status"), BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status));
            Result->SetArrayField(TEXT("errors"), Errors);
            Result->SetArrayField(TEXT("warnings"), Warnings);
        }

        bool bSaved = false;
        if (bSave)
        {
            if (!bCompiled && !bAllowSaveWithErrors)
            {
                Result->SetStringField(TEXT("saveSkippedReason"), bCompile
                    ? TEXT("Compile failed; fix the errors or pass allowSaveWithErrors=true.")
                    : TEXT("Not compiled; pass compile=true or allowSaveWithErrors=true."));
            }
            else
            {
                FString Filename, IndexError;
                if (!BlueprintEditToolUtils::SaveAsset(Blueprint, Filename, OutError)) return false;
                bSaved = true;
                Result->SetStringField(TEXT("savedFilename"), Filename);
                Result->SetBoolField(TEXT("indexRefreshed"), BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError));
            }
        }
        Result->SetBoolField(TEXT("saved"), bSaved);
        Result->SetBoolField(TEXT("isDirty"), Blueprint->GetPackage()->IsDirty());
        return true;
    }, ExecutionError);

    if (!bOk) return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FReparentBlueprintTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Blueprint to reparent.")));
    P->SetObjectField(TEXT("newParentClassPath"), BuildStringProperty(TEXT("New parent class, e.g. /Game/BP_IconEntity.BP_IconEntity_C or /Script/Engine.Pawn.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Report only; never mutates.")));
    P->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required to apply.")));
    P->SetObjectField(TEXT("compile"), BuildBoolProperty(TEXT("Compile after reparenting. Default true.")));
    P->SetObjectField(TEXT("save"), BuildBoolProperty(TEXT("Save after a successful compile. Default false.")));
    P->SetObjectField(TEXT("onlyUsed"), BuildBoolProperty(TEXT("Filter defaultChanges to properties used by target get/set nodes or overridden in the target/descendants. Descendant coverage is reported and capped at 256. Default false.")));
    P->SetObjectField(TEXT("allowSaveWithErrors"), BuildBoolProperty(TEXT("Save even when the compile fails or was skipped.")));
    Schema->SetObjectField(TEXT("properties"), P);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("newParentClassPath"))});
    return Schema;
}
