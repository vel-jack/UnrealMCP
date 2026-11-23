#include "Tools/Git/ConflictResolution/ApplyBlueprintMergeStructureOps.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Tools/Blueprint/Shared/BlueprintComponentEditUtils.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"
#include "Tools/Blueprint/Members/ReparentBlueprintTool.h"
#include "UObject/UObjectIterator.h"

namespace
{
    namespace Edit = UnrealMCP::BlueprintEditToolUtils;

    struct FOperation
    {
        FString Kind;
        FName First;
        FName Second;
        FString DefaultValue;
        FEdGraphPinType Type;
        UClass* Class = nullptr;
        TArray<UK2Node_EditablePinBase*> Terminals;
        bool bInput = false;
        FString PreviousDefault;
    };

    FString HashPackage(const UBlueprint* Blueprint)
    {
        FString Filename;
        if (!FPackageName::DoesPackageExist(Blueprint->GetOutermost()->GetName(), &Filename)) return {};
        const int64 Size = IFileManager::Get().FileSize(*Filename);
        TArray<uint8> Bytes;
        if (Size <= 0 || Size > 64 * 1024 * 1024 || !FFileHelper::LoadFileToArray(Bytes, *Filename) || Bytes.Num() != Size) return {};
        return FMD5::HashBytes(Bytes.GetData(), Bytes.Num());
    }

    TArray<FString> LinkEvidence(const UBlueprint* Blueprint)
    {
        TArray<FString> Links;
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph) continue;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin || Pin->Direction != EGPD_Output) continue;
                    for (const UEdGraphPin* Other : Pin->LinkedTo)
                    {
                        if (!Other || !Other->GetOwningNodeUnchecked()) continue;
                        Links.Add(FString::Printf(TEXT("%s:%s:%s:%d:%s:%s:%d"), *Graph->GraphGuid.ToString(),
                            *Node->NodeGuid.ToString(), *Pin->PinName.ToString(), Pin->bOrphanedPin,
                            *Other->GetOwningNode()->NodeGuid.ToString(), *Other->PinName.ToString(), Other->bOrphanedPin));
                    }
                }
            }
        }
        Links.Sort();
        return Links;
    }

    FString ReadDefault(const FProperty* Property, UObject* CDO)
    {
        FString Value;
        if (Property && CDO) Property->ExportText_InContainer(0, Value, CDO, nullptr, CDO, PPF_None);
        return Value;
    }

    TArray<FString> StructureEvidence(UBlueprint* Blueprint)
    {
        TArray<FString> Evidence;
        Evidence.Add(GetPathNameSafe(Blueprint->ParentClass));
        for (const auto& Interface : Blueprint->ImplementedInterfaces)
            Evidence.Add(GetPathNameSafe(Interface.Interface));
        for (const auto& Variable : Blueprint->NewVariables)
        {
            auto* CDO = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
            const auto* Property = CDO ? FindFProperty<FProperty>(Blueprint->GeneratedClass, Variable.VarName) : nullptr;
            Evidence.Add(Variable.VarGuid.ToString() + Variable.VarName.ToString() + Variable.DefaultValue + ReadDefault(Property, CDO));
        }
        if (auto* SCS = Blueprint->SimpleConstructionScript.Get())
            for (USCS_Node* Component : SCS->GetAllNodes())
                if (Component)
                    Evidence.Add(Component->VariableGuid.ToString() + GetNameSafe(SCS->FindParentNode(Component)));
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
            if (Graph) Evidence.Add(Graph->GraphGuid.ToString() + UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph));
        Evidence.Sort();
        return Evidence;
    }

    bool VerifyOperations(UBlueprint* Target, const TArray<FOperation>& Operations, FString& Error)
    {
        auto* CDO = Target->GeneratedClass->GetDefaultObject(false);
        for (const FOperation& Op : Operations)
        {
            bool bVerified = false;
            if (Op.Kind == TEXT("renameVariable") || Op.Kind == TEXT("removeVariable"))
            {
                bVerified = !FindFProperty<FProperty>(Target->GeneratedClass, Op.First);
                if (Op.Kind == TEXT("renameVariable"))
                    {
                    const auto* Renamed = FindFProperty<FProperty>(Target->GeneratedClass, Op.Second);
                    bVerified = bVerified && Renamed && ReadDefault(Renamed, CDO) == Op.PreviousDefault;
                }
            }
            else if (Op.Kind == TEXT("setVariableType"))
            {
                auto* Property = FindFProperty<FProperty>(Target->GeneratedClass, Op.First);
                FEdGraphPinType CompiledType;
                bVerified = Property && CDO && Property->GetOwnerClass() == Target->GeneratedClass
                    && GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, CompiledType) && CompiledType == Op.Type;
                if (bVerified)
                {
                    void* Expected = Property->AllocateAndInitializeValue();
                    const TCHAR* End = Op.DefaultValue.IsEmpty() ? TEXT("") : Property->ImportText_Direct(*Op.DefaultValue, Expected, CDO, PPF_None);
                    bVerified = End && FString(End).TrimStartAndEnd().IsEmpty()
                        && Property->Identical(Expected, Property->ContainerPtrToValuePtr<void>(CDO), PPF_None);
                    Property->DestroyAndFreeValue(Expected);
                }
            }
            else if (Op.Kind == TEXT("setParameterType"))
            {
                const auto* Function = Target->GeneratedClass->FindFunctionByName(Op.Second);
                const auto* Property = Function ? FindFProperty<FProperty>(Function, Op.First) : nullptr;
                FEdGraphPinType CompiledType;
                bVerified = Property && Property->HasAnyPropertyFlags(CPF_Parm)
                    && GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, CompiledType)
                    && CompiledType == Op.Type;
            }
            else if (Op.Kind == TEXT("removeInterface"))
                bVerified = !Target->GeneratedClass->ImplementsInterface(Op.Class);
            else if (Op.Kind == TEXT("reparent"))
                bVerified = Target->ParentClass == Op.Class && Target->GeneratedClass->GetSuperClass() == Op.Class;
            else if (Op.Kind == TEXT("moveComponent"))
            {
                auto* Component = UnrealMCP::BlueprintComponentEditUtils::FindComponentNode(Target, Op.First.ToString());
                auto* Parent = UnrealMCP::BlueprintComponentEditUtils::FindComponentNode(Target, Op.Second.ToString());
                bVerified = Component && Parent && Target->SimpleConstructionScript->FindParentNode(Component) == Parent;
            }
            if (!bVerified)
            { Error = FString::Printf(TEXT("Compiled readback did not match %s."), *Op.Kind); return false; }
        }
        return true;
    }

    struct FWorkingAsset
    {
        UBlueprint* Blueprint = nullptr;
        FString DiskHash;
        TArray<FString> Links;
        TArray<FString> Structure;
        bool bDirtyBefore = false;
        TSharedPtr<FJsonObject> Receipt;
    };

    bool AffectsType(const UClass* Owner, const UClass* TargetClass)
    {
        return Owner && TargetClass && Owner->GetAuthoritativeClass()->IsChildOf(TargetClass->GetAuthoritativeClass());
    }

    bool FindCallers(UBlueprint* Target, const TSet<const UClass*>& RemovedInterfaces,
        TArray<UBlueprint*>& Callers, FString& Error)
    {
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        if (Registry.IsLoadingAssets()) { Error = TEXT("Wait for Asset Registry discovery before planning structural changes."); return false; }
        TArray<FName> Referencers;
        Registry.GetReferencers(Target->GetOutermost()->GetFName(), Referencers);
        const FTopLevelAssetPath OwnClass = Target->GeneratedClass->GetClassPathName();
        TSet<FTopLevelAssetPath> Descendants;
        Registry.GetDerivedClassNames({OwnClass}, {}, Descendants);
        Descendants.Remove(OwnClass);
        for (const auto& Class : Descendants)
        {
            Referencers.AddUnique(Class.GetPackageName());
            TArray<FName> DescendantReferencers;
            Registry.GetReferencers(Class.GetPackageName(), DescendantReferencers);
            for (FName Package : DescendantReferencers) Referencers.AddUnique(Package);
            if (Referencers.Num() > 17)
            { Error = TEXT("Structural merge refuses more than 16 dependent packages. No dependent was skipped."); return false; }
        }
        Referencers.Remove(Target->GetOutermost()->GetFName());
        if (Referencers.Num() > 16)
        { Error = TEXT("Structural merge refuses more than 16 dependent packages. No dependent was skipped."); return false; }
        for (FName Package : Referencers)
        {
            TArray<FAssetData> Assets;
            Registry.GetAssetsByPackageName(Package, Assets);
            bool bFound = false;
            for (const FAssetData& Asset : Assets)
                if (UBlueprint* Caller = Cast<UBlueprint>(Asset.GetAsset()))
                {
                    Callers.AddUnique(Caller);
                    bFound = true;
                }
            if (!bFound)
            {
                Error = FString::Printf(TEXT("Dependent package %s could not be verified as a Blueprint asset."), *Package.ToString());
                return false;
            }
        }
        for (TObjectIterator<UBlueprint> It; It; ++It)
        {
            UBlueprint* Candidate = *It;
            if (Candidate == Target || !Candidate->IsAsset() || Candidate->GetOutermost()->HasAnyPackageFlags(PKG_ForDiffing)) continue;
            if (AffectsType(Candidate->ParentClass, Target->GeneratedClass))
            {
                Callers.AddUnique(Candidate);
                continue;
            }
            TArray<UK2Node_CallFunction*> Calls;
            FBlueprintEditorUtils::GetAllNodesOfClass(Candidate, Calls);
            for (UK2Node_CallFunction* Call : Calls)
                if (Call && AffectsType(Call->FunctionReference.GetMemberParentClass(Candidate->GeneratedClass), Target->GeneratedClass))
                {
                    Callers.AddUnique(Candidate);
                    break;
                }
        }
        if (Callers.Num() > 16)
        { Error = TEXT("Structural merge exceeds the 16 Blueprint caller limit."); return false; }
        auto Depth = [](const UBlueprint& Blueprint)
        {
            int32 DepthValue = 0;
            for (const UClass* Current = Blueprint.ParentClass; Current; Current = Current->GetSuperClass()) ++DepthValue;
            return DepthValue;
        };
        Callers.Sort([&](const UBlueprint& A, const UBlueprint& B)
        {
            const int32 ADepth = Depth(A), BDepth = Depth(B);
            return ADepth == BDepth ? A.GetPathName() < B.GetPathName() : ADepth < BDepth;
        });
        for (UBlueprint* Caller : Callers)
        {
            if (Caller->GetOutermost()->IsDirty())
            { Error = FString::Printf(TEXT("Dependent Blueprint has unsaved changes: %s."), *Caller->GetPathName()); return false; }
            if (RemovedInterfaces.IsEmpty()) continue;
            TArray<UK2Node_CallFunction*> Calls;
            FBlueprintEditorUtils::GetAllNodesOfClass(Caller, Calls);
            for (const UK2Node_CallFunction* Call : Calls)
                if (Call && RemovedInterfaces.Contains(Call->FunctionReference.GetMemberParentClass(Caller->GeneratedClass)))
                { Error = FString::Printf(TEXT("An interface call in %s must be reconciled before interface removal."), *Caller->GetPathName()); return false; }
        }
        return true;
    }

    bool ReferencesVariable(UBlueprint* Blueprint, FName Name, const UClass* TargetClass)
    {
        TArray<UK2Node_Variable*> Nodes;
        FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Nodes);
        return Nodes.ContainsByPredicate([&](const UK2Node_Variable* Node)
        {
            return Node && Node->GetVarName() == Name && !Node->VariableReference.IsLocalScope()
                && AffectsType(Node->VariableReference.GetMemberParentClass(Blueprint->GeneratedClass), TargetClass);
        });
    }

    bool ReadType(const FJsonObject& Item, FEdGraphPinType& Type, FString& Error)
    {
        FString Name, Object, Container = TEXT("none"), Value, ValueObject;
        if (!Item.TryGetStringField(TEXT("type"), Name))
        { Error = TEXT("A declaration type is required."); return false; }
        Item.TryGetStringField(TEXT("typeObjectPath"), Object);
        Item.TryGetStringField(TEXT("containerType"), Container);
        Item.TryGetStringField(TEXT("valueType"), Value);
        Item.TryGetStringField(TEXT("valueTypeObjectPath"), ValueObject);
        return Edit::BuildContainerPinType(Name, Object, Container, Value, ValueObject, Type, Error);
    }

    TSharedPtr<FUserPinInfo> FindParameter(const UK2Node_EditablePinBase* Node, FName Name)
    {
        for (const auto& Pin : Node->UserDefinedPins)
            if (Pin && Pin->PinName == Name) return Pin;
        return nullptr;
    }

    bool IsVariableEdit(const FOperation& Op)
    {
        return Op.Kind == TEXT("renameVariable") || Op.Kind == TEXT("removeVariable") || Op.Kind == TEXT("setVariableType");
    }

    bool Preflight(UBlueprint* Blueprint, const TArray<TSharedPtr<FJsonValue>>& Values,
        TArray<FOperation>& Operations, TSet<const UClass*>& RemovedInterfaces, FString& Error)
    {
        TSet<FString> Targets;
        TMap<USCS_Node*, USCS_Node*> PlannedParents;
        for (const auto& Value : Values)
        {
            const auto& Item = Value->AsObject();
            FOperation Op;
            FString First, Second;
            if (!Item || !Item->TryGetStringField(TEXT("kind"), Op.Kind))
            { Error = TEXT("Each structure operation requires kind."); return false; }
            FString TargetKey;
            TArray<FString> Allowed = {TEXT("kind")};
            if (IsVariableEdit(Op))
            {
                if (!Item->TryGetStringField(TEXT("variableName"), First) || First.IsEmpty())
                { Error = TEXT("Variable operation requires variableName."); return false; }
                Allowed.Add(TEXT("variableName"));
                Op.First = FName(*First);
                if (!Blueprint->NewVariables.ContainsByPredicate([&](const auto& Var) { return Var.VarName == Op.First; })
                    || FBlueprintEditorUtils::IsVariableUsed(Blueprint, Op.First)
                    || ReferencesVariable(Blueprint, Op.First, Blueprint->GeneratedClass))
                { Error = TEXT("Variable edits require an unused locally declared member."); return false; }
                const auto* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, Op.First);
                if (!Property || Property->GetOwnerClass() != Blueprint->GeneratedClass)
                { Error = TEXT("Variable declaration must match a compiled local property."); return false; }
                Op.PreviousDefault = ReadDefault(Property, Blueprint->GeneratedClass->GetDefaultObject(false));
                TargetKey = TEXT("variable/") + First.ToLower();
                if (Op.Kind == TEXT("renameVariable"))
                {
                    Allowed.Add(TEXT("newName"));
                    FKismetNameValidator Validator(Blueprint);
                    if (!Item->TryGetStringField(TEXT("newName"), Second) || Validator.IsValid(Second) != EValidatorResult::Ok
                        || Targets.Contains(TEXT("variable/") + Second.ToLower()))
                    { Error = TEXT("New variable name is invalid or already planned."); return false; }
                    Op.Second = FName(*Second);
                    Targets.Add(TEXT("variable/") + Second.ToLower());
                }
                if (Op.Kind == TEXT("setVariableType"))
                {
                    Allowed.Append({TEXT("type"), TEXT("defaultValue"), TEXT("typeObjectPath"), TEXT("containerType"),
                        TEXT("valueType"), TEXT("valueTypeObjectPath")});
                    if (!Item->TryGetStringField(TEXT("defaultValue"), Op.DefaultValue))
                    { Error = TEXT("Type changes require an explicit defaultValue for the new type."); return false; }
                    if (!ReadType(*Item, Op.Type, Error)) return false;
                }
            }
            else if (Op.Kind == TEXT("setParameterType"))
            {
                Allowed.Append({TEXT("nodeGuid"), TEXT("parameterName"), TEXT("type"), TEXT("typeObjectPath"),
                    TEXT("containerType"), TEXT("valueType"), TEXT("valueTypeObjectPath")});
                FGuid Guid;
                if (!Item->TryGetStringField(TEXT("nodeGuid"), First) || !FGuid::Parse(First, Guid)
                    || !Item->TryGetStringField(TEXT("parameterName"), Second) || Second.IsEmpty())
                { Error = TEXT("Signature edit requires a function entry or result nodeGuid and parameterName."); return false; }
                Op.First = FName(*Second);
                for (UEdGraph* Graph : Blueprint->FunctionGraphs)
                {
                    if (!Graph) continue;
                    for (UEdGraphNode* Node : Graph->Nodes)
                    {
                        if (!Node || Node->NodeGuid != Guid || !(Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>())) continue;
                        Op.Second = Graph->GetFName();
                        Op.bInput = Node->IsA<UK2Node_FunctionEntry>();
                        if (!FindParameter(CastChecked<UK2Node_EditablePinBase>(Node), Op.First)) break;
                        for (UEdGraphNode* Terminal : Graph->Nodes)
                            if (Terminal && (Op.bInput ? Terminal->IsA<UK2Node_FunctionEntry>() : Terminal->IsA<UK2Node_FunctionResult>()))
                            {
                                auto* Editable = CastChecked<UK2Node_EditablePinBase>(Terminal);
                                if (FindParameter(Editable, Op.First)) Op.Terminals.Add(Editable);
                            }
                        break;
                    }
                }
                if (Op.Terminals.IsEmpty() || (Blueprint->ParentClass && Blueprint->ParentClass->FindFunctionByName(Op.Second))
                    || Blueprint->ImplementedInterfaces.ContainsByPredicate([&](const auto& Interface)
                        { return Interface.Interface && Interface.Interface->FindFunctionByName(Op.Second); }))
                { Error = TEXT("Signature edit requires a locally authored function parameter."); return false; }
                if (!ReadType(*Item, Op.Type, Error)) return false;
                TargetKey = TEXT("signature/") + Op.Second.ToString().ToLower() + TEXT("/") + Second.ToLower();
            }
            else if (Op.Kind == TEXT("removeInterface") || Op.Kind == TEXT("reparent"))
            {
                const bool bParent = Op.Kind == TEXT("reparent");
                Allowed.Add(bParent ? TEXT("newParentClassPath") : TEXT("interfaceClassPath"));
                if (!Item->TryGetStringField(Allowed.Last(), First) || First.IsEmpty())
                { Error = TEXT("Interface and reparent operations require a class path."); return false; }
                if (!Edit::ResolveClass(First, Op.Class, Error)) return false;
                TargetKey = bParent ? TEXT("parentClass") : TEXT("interface/") + Op.Class->GetPathName();
                if (bParent)
                {
                    const UPackage* NewParentPackage = Op.Class->GetOutermost();
                    if (!NewParentPackage->HasAnyPackageFlags(PKG_CompiledIn)
                        && (NewParentPackage->IsDirty() || !FPackageName::DoesPackageExist(NewParentPackage->GetName())))
                    { Error = TEXT("Save the new parent Blueprint separately before planning a reparent."); return false; }
                    UnrealMCP::FMCPRequest Request;
                    Request.Params = MakeShared<FJsonObject>();
                    Request.Params->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
                    Request.Params->SetStringField(TEXT("newParentClassPath"), First);
                    Request.Params->SetBoolField(TEXT("dryRun"), true);
                    const auto Preview = FReparentBlueprintTool().Execute(Request);
                    if (!Preview.Result)
                    { Error = Preview.Error.IsSet() ? Preview.Error->Message : TEXT("Parent preflight failed."); return false; }
                    bool bUnchanged = false;
                    if (Preview.Result->TryGetBoolField(TEXT("unchanged"), bUnchanged) && bUnchanged)
                    { Error = TEXT("Blueprint already has the requested parent."); return false; }
                    if (Preview.Result->GetBoolField(TEXT("truncated"))
                        || !Preview.Result->GetArrayField(TEXT("breakingNodes")).IsEmpty()
                        || !Preview.Result->GetArrayField(TEXT("droppedOverrides")).IsEmpty()
                        || !Preview.Result->GetArrayField(TEXT("lostInterfaces")).IsEmpty()
                        || !Preview.Result->GetArrayField(TEXT("defaultChanges")).IsEmpty())
                    { Error = TEXT("Reparent requires reconciliation of breaking nodes, overrides, inherited interfaces or changed inherited defaults. Run ReparentBlueprint dryRun for details."); return false; }
                }
                else
                {
                    if (!Blueprint->ImplementedInterfaces.ContainsByPredicate([&](const auto& Interface) { return Interface.Interface == Op.Class; }))
                    { Error = TEXT("Interface removal requires a directly implemented interface."); return false; }
                    RemovedInterfaces.Add(Op.Class);
                }
            }
            else if (Op.Kind == TEXT("moveComponent"))
            {
                Allowed.Append({TEXT("componentName"), TEXT("parentComponentName")});
                if (!Item->TryGetStringField(TEXT("componentName"), First) || !Item->TryGetStringField(TEXT("parentComponentName"), Second))
                { Error = TEXT("Component move requires componentName and parentComponentName."); return false; }
                Op.First = FName(*First);
                Op.Second = FName(*Second);
                TargetKey = TEXT("component/") + First.ToLower();
                auto* Component = UnrealMCP::BlueprintComponentEditUtils::FindComponentNode(Blueprint, First);
                auto* Parent = UnrealMCP::BlueprintComponentEditUtils::FindComponentNode(Blueprint, Second);
                auto* SCS = Blueprint->SimpleConstructionScript.Get();
                if (!SCS || !Component || !Parent || Component == Parent || SCS->GetDefaultSceneRootNode() == Component
                    || !Component->ComponentTemplate || !Parent->ComponentTemplate
                    || !Component->ComponentTemplate->IsA<USceneComponent>() || !Parent->ComponentTemplate->IsA<USceneComponent>())
                { Error = TEXT("Move requires distinct local scene components and cannot move the default scene root."); return false; }
                for (USCS_Node* Ancestor = Parent; Ancestor; Ancestor = PlannedParents.Contains(Ancestor)
                    ? PlannedParents.FindChecked(Ancestor) : SCS->FindParentNode(Ancestor))
                    if (Ancestor == Component) { Error = TEXT("Component moves create an attachment cycle."); return false; }
                PlannedParents.Add(Component, Parent);
            }
            else
            {
                Error = FString::Printf(TEXT("Unsupported structural merge operation: %s."), *Op.Kind);
                return false;
            }
            for (const auto& Field : Item->Values)
                if (!Allowed.Contains(FString(Field.Key)))
                { Error = FString::Printf(TEXT("Unknown field on %s: %s."), *Op.Kind, *FString(Field.Key)); return false; }
            if (Targets.Contains(TargetKey))
            { Error = TEXT("A declaration, interface, component or parent may be addressed only once per plan."); return false; }
            Targets.Add(TargetKey);
            Operations.Add(MoveTemp(Op));
        }
        return true;
    }
}

FApplyBlueprintMergeStructureOps::FApplyBlueprintMergeStructureOps()
    : FMCPToolBase(TEXT("InternalBlueprintMergeStructureOps"), TEXT("Executes a checked structural Blueprint transaction."))
{
}

UnrealMCP::FMCPResponse FApplyBlueprintMergeStructureOps::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString Error;
    const auto Params = Request.Params;
    const bool bDry = Edit::GetOptionalBool(Params, TEXT("dryRun"), true);
    const bool bSave = Edit::GetOptionalBool(Params, TEXT("saveAfterEdit"), false);
    auto Result = BuildBooleanResult(false);
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Target = nullptr;
        if (!Edit::ResolveBlueprint(Params->GetStringField(TEXT("objectPath")), Target, OutError)) return false;
        if (!Target->GeneratedClass || Target->GetOutermost()->IsDirty())
        { OutError = TEXT("Structural merge requires a clean working Blueprint with a generated class."); return false; }
        TArray<FOperation> Operations;
        TSet<const UClass*> RemovedInterfaces;
        if (!Preflight(Target, Params->GetArrayField(TEXT("operations")), Operations, RemovedInterfaces, OutError)) return false;
        TArray<UBlueprint*> Callers;
        if (!FindCallers(Target, RemovedInterfaces, Callers, OutError)) return false;
        for (UBlueprint* Caller : Callers)
            for (const FOperation& Op : Operations)
                if (IsVariableEdit(Op) && ReferencesVariable(Caller, Op.First, Target->GeneratedClass))
                { OutError = FString::Printf(TEXT("A variable reference in %s needs explicit reconciliation."), *Caller->GetPathName()); return false; }
        if (Callers.ContainsByPredicate([&](const UBlueprint* Caller) { return AffectsType(Caller->ParentClass, Target->GeneratedClass); })
            && Operations.ContainsByPredicate(IsVariableEdit))
        { OutError = TEXT("Variable declaration edits with descendants require a separate reviewed migration."); return false; }

        TArray<FWorkingAsset> Working;
        Callers.Insert(Target, 0);
        TArray<TSharedPtr<FJsonValue>> Packages;
        for (UBlueprint* Asset : Callers)
        {
            FWorkingAsset& Entry = Working.AddDefaulted_GetRef();
            Entry.Blueprint = Asset;
            Entry.DiskHash = HashPackage(Asset);
            Entry.bDirtyBefore = Asset->GetOutermost()->IsDirty();
            if (Entry.DiskHash.IsEmpty() || Entry.bDirtyBefore)
            { OutError = FString::Printf(TEXT("Dependent package must be saved and clean: %s."), *Asset->GetPathName()); return false; }
            Entry.Receipt = MakeShared<FJsonObject>();
            Entry.Receipt->SetStringField(TEXT("objectPath"), Asset->GetPathName());
            Entry.Receipt->SetStringField(TEXT("workingHash"), Entry.DiskHash);
            TArray<TSharedPtr<FJsonValue>> GraphRevisions;
            TArray<UEdGraph*> Graphs;
            Asset->GetAllGraphs(Graphs);
            for (const UEdGraph* Graph : Graphs)
                if (Graph) GraphRevisions.Add(MakeShared<FJsonValueString>(BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph)));
            Entry.Receipt->SetArrayField(TEXT("graphRevisions"), GraphRevisions);
            Packages.Add(MakeShared<FJsonValueObject>(Entry.Receipt));
            if (!bDry)
            {
                Entry.Links = LinkEvidence(Asset);
                Entry.Structure = StructureEvidence(Asset);
            }
        }
        Result->SetArrayField(TEXT("packages"), Packages);
        if (bDry) return true;
        const auto& ExpectedPackages = Params->GetArrayField(TEXT("expectedPackages"));
        if (Packages.Num() != ExpectedPackages.Num())
        { OutError = TEXT("Dependent package set changed during preflight."); return false; }
        for (int32 Index = 0; Index < Packages.Num(); ++Index)
            if (!FJsonValue::CompareEqual(*Packages[Index], *ExpectedPackages[Index]))
            { OutError = TEXT("A reviewed package changed during preflight."); return false; }
        if (!GEditor || GEditor->IsTransactionActive()) { OutError = TEXT("Structural merge requires an idle editor transaction."); return false; }

        TArray<UPackage*> DirtyBefore;
        FEditorFileUtils::GetDirtyContentPackages(DirtyBefore);
        auto HasUnexpectedDirty = [&]()
        {
            TArray<UPackage*> DirtyAfter;
            FEditorFileUtils::GetDirtyContentPackages(DirtyAfter);
            return DirtyAfter.ContainsByPredicate([&](const UPackage* Package)
            {
                return !DirtyBefore.Contains(Package) && !Callers.ContainsByPredicate([&](const UBlueprint* Asset)
                    { return Asset->GetOutermost() == Package; });
            });
        };

        bool bEditsOk = true;
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "ApplyStructuralMerge", "UnrealMCP Apply Blueprint Structural Merge"));
            if (!GUndo) { OutError = TEXT("Editor transaction recording is unavailable."); return false; }
            for (auto& Entry : Working)
            {
                Entry.Blueprint->Modify();
                TArray<UEdGraph*> Graphs;
                Entry.Blueprint->GetAllGraphs(Graphs);
                for (UEdGraph* Graph : Graphs)
                    if (Graph)
                    {
                        Graph->Modify();
                        for (UEdGraphNode* Node : Graph->Nodes) if (Node) Node->Modify();
                    }
            }
            for (const FOperation& Op : Operations)
            {
                if (Op.Kind == TEXT("renameVariable")) FBlueprintEditorUtils::RenameMemberVariable(Target, Op.First, Op.Second);
                else if (Op.Kind == TEXT("removeVariable")) FBlueprintEditorUtils::RemoveMemberVariable(Target, Op.First);
                else if (Op.Kind == TEXT("setVariableType"))
                {
                    const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Target, Op.First);
                    if (Index == INDEX_NONE) { OutError = TEXT("Variable disappeared while applying the plan."); bEditsOk = false; break; }
                    FBlueprintEditorUtils::ChangeMemberVariableType(Target, Op.First, Op.Type);
                    const int32 ChangedIndex = FBlueprintEditorUtils::FindNewVariableIndex(Target, Op.First);
                    if (ChangedIndex == INDEX_NONE || Target->NewVariables[ChangedIndex].VarType != Op.Type)
                    { OutError = TEXT("Engine refused the variable type change."); bEditsOk = false; break; }
                    Target->NewVariables[ChangedIndex].DefaultValue = Op.DefaultValue;
                }
                else if (Op.Kind == TEXT("setParameterType"))
                {
                    for (UK2Node_EditablePinBase* Terminal : Op.Terminals)
                    {
                        auto Pin = FindParameter(Terminal, Op.First);
                        Pin->PinType = Op.Type;
                        if (!Op.Type.bIsConst && Terminal->ShouldUseConstRefParams())
                            Pin->PinType.bIsConst = Op.Type.IsArray() || Op.Type.bIsReference;
                        Pin->PinDefaultValue.Reset();
                        Terminal->ReconstructNode();
                    }
                }
                else if (Op.Kind == TEXT("removeInterface"))
                    FBlueprintEditorUtils::RemoveInterface(Target, Op.Class->GetClassPathName(), true);
                else if (Op.Kind == TEXT("reparent"))
                {
                    Target->ParentClass = Op.Class;
                }
                else if (Op.Kind == TEXT("moveComponent"))
                {
                    auto* Component = BlueprintComponentEditUtils::FindComponentNode(Target, Op.First.ToString());
                    auto* Parent = BlueprintComponentEditUtils::FindComponentNode(Target, Op.Second.ToString());
                    auto* SCS = Target->SimpleConstructionScript.Get();
                    bool bCycle = false;
                    for (USCS_Node* Ancestor = Parent; Ancestor; Ancestor = SCS->FindParentNode(Ancestor))
                        if (Ancestor == Component) { bCycle = true; break; }
                    if (bCycle) { OutError = TEXT("Component moves create an attachment cycle."); bEditsOk = false; break; }
                    SCS->Modify();
                    Component->Modify();
                    Parent->Modify();
                    if (auto* OldParent = SCS->FindParentNode(Component)) OldParent->Modify();
                    SCS->RemoveNode(Component, false);
                    Parent->AddChildNode(Component);
                }
            }
            if (bEditsOk)
            {
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
                for (auto& Entry : Working)
                {
                    UBlueprint* Asset = Entry.Blueprint;
                    FBlueprintEditorUtils::RefreshAllNodes(Asset);
                    TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
                    const bool bCompiled = Edit::CompileWithLocatedMessages(Asset, Errors, Warnings);
                    Entry.Receipt->SetBoolField(TEXT("compiled"), bCompiled);
                    if (!Errors.IsEmpty()) Entry.Receipt->SetArrayField(TEXT("errors"), Errors);
                    if (!Warnings.IsEmpty()) Entry.Receipt->SetArrayField(TEXT("warnings"), Warnings);
                    if (!bCompiled || LinkEvidence(Asset) != Entry.Links)
                    {
                        OutError = FString::Printf(TEXT("Compilation or linked-pin preservation failed for %s."), *Asset->GetPathName());
                        bEditsOk = false;
                        break;
                    }
                    if (Asset == Target && !VerifyOperations(Target, Operations, OutError))
                    {
                        bEditsOk = false;
                        break;
                    }
                }
            }
        }
        if (bEditsOk && HasUnexpectedDirty())
        {
            OutError = TEXT("Structural compilation touched an unreviewed package.");
            bEditsOk = false;
        }
        if (!bEditsOk)
        {
            const bool bUndone = GEditor->UndoTransaction();
            bool bRestored = bUndone;
            for (auto& Entry : Working)
            {
                bool bStateRestored = false;
                if (bUndone)
                {
                    TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
                    Edit::CompileWithLocatedMessages(Entry.Blueprint, Errors, Warnings);
                    if (Entry.Blueprint == Target && Target->GeneratedClass)
                    {
                        UObject* CDO = Target->GeneratedClass->GetDefaultObject(false);
                        for (const FOperation& Op : Operations)
                            if (CDO && IsVariableEdit(Op))
                                if (auto* Property = FindFProperty<FProperty>(Target->GeneratedClass, Op.First);
                                    Property && ReadDefault(Property, CDO) != Op.PreviousDefault)
                                    Property->ImportText_Direct(*Op.PreviousDefault, Property->ContainerPtrToValuePtr<void>(CDO), CDO, PPF_None);
                    }
                    bStateRestored = StructureEvidence(Entry.Blueprint) == Entry.Structure
                        && LinkEvidence(Entry.Blueprint) == Entry.Links;
                    if (bStateRestored) Entry.Blueprint->GetOutermost()->SetDirtyFlag(Entry.bDirtyBefore);
                }
                Entry.Receipt->SetBoolField(TEXT("stateRestored"), bStateRestored);
                Entry.Receipt->SetBoolField(TEXT("isDirty"), Entry.Blueprint->GetOutermost()->IsDirty());
                Entry.Receipt->SetBoolField(TEXT("saved"), false);
                bRestored &= bStateRestored;
            }
            Result->SetBoolField(TEXT("rolledBack"), bRestored && !HasUnexpectedDirty());
            return false;
        }
        bool bSaveFailed = false;
        for (auto& Entry : Working)
        {
            bool bSaved = false;
            if (bSave && !bSaveFailed && Entry.Blueprint->GetOutermost()->IsDirty())
            {
                FString Filename;
                bSaved = Edit::SaveAsset(Entry.Blueprint, Filename, OutError);
                bSaveFailed = !bSaved;
            }
            Entry.Receipt->SetBoolField(TEXT("saved"), bSaved);
            Entry.Receipt->SetBoolField(TEXT("isDirty"), Entry.Blueprint->GetOutermost()->IsDirty());
        }
        Result->SetBoolField(TEXT("applied"), true);
        Result->SetBoolField(TEXT("saveFailed"), bSaveFailed);
        return !bSaveFailed;
    }, Error);
    Result->SetBoolField(TEXT("success"), bOk);
    if (!bOk) Result->SetStringField(TEXT("error"), Error);
    FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}
