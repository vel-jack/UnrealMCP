#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_VariableGet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Tools/Blueprint/Members/BlueprintMemberRemovalTools.h"
#include "Tools/Data/UserDefinedStructMemberTools.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    UnrealMCP::FMCPResponse Call(const FMCPToolBase& Tool, UObject* Asset, TFunctionRef<void(FJsonObject&)> Fill)
    {
        UnrealMCP::FMCPRequest R; R.Id = TEXT("member-edit-test"); R.Params = MakeShared<FJsonObject>();
        R.Params->SetStringField(TEXT("objectPath"), Asset->GetPathName()); Fill(*R.Params); return Tool.Execute(R);
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPMemberRemovalTest, "UnrealMCP.Blueprint.MemberRemoval.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPMemberRemovalTest::RunTest(const FString& Parameters)
{
    UPackage* Package = CreatePackage(*(TEXT("/Game/UnrealMCP_Automation/MemberRemoval_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    ON_SCOPE_EXIT { Package->SetDirtyFlag(false); };
    TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
        TEXT("BP_MemberRemoval"), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
    FEdGraphPinType Type; Type.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(BP.Get(), TEXT("Unused"), Type, TEXT("7"));
    UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(BP.Get(), TEXT("UnusedFunction"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph(BP.Get(), Function, false, static_cast<UClass*>(nullptr));
    FKismetEditorUtilities::CompileBlueprint(BP.Get());
    auto VariableParams = [](FJsonObject& P) { P.SetStringField(TEXT("variableName"), TEXT("Unused")); };
    auto Dry = Call(FRemoveBlueprintMemberTool(false), BP.Get(), [&](FJsonObject& P) { VariableParams(P); P.SetBoolField(TEXT("dryRun"), true); });
    TestFalse(TEXT("Dry run succeeds"), Dry.Error.IsSet());
    TestEqual(TEXT("Dry run preserves member"), BP->NewVariables.Num(), 1);
    TestTrue(TEXT("Confirmation required"), Call(FRemoveBlueprintMemberTool(false), BP.Get(), VariableParams).Error.IsSet());
    UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(BP->UbergraphPages[0]);
    Getter->VariableReference.SetSelfMember(TEXT("Unused"));
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(BP->UbergraphPages[0], Getter, {});
    FKismetEditorUtilities::CompileBlueprint(BP.Get());
    auto Apply = [&](FJsonObject& P) { VariableParams(P); P.SetBoolField(TEXT("confirm"), true); };
    TestTrue(TEXT("Used variable refused"), Call(FRemoveBlueprintMemberTool(false), BP.Get(), Apply).Error.IsSet());
    FBlueprintEditorUtils::RemoveNode(BP.Get(), Getter, true);
    FKismetEditorUtilities::CompileBlueprint(BP.Get());
    const int32 DefaultBefore = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("Unused"))->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject());
    auto Removed = Call(FRemoveBlueprintMemberTool(false), BP.Get(), Apply);
    if (Removed.Error.IsSet()) AddError(Removed.Error->Message);
    TestEqual(TEXT("Variable removed"), BP->NewVariables.Num(), 0);
    TestTrue(TEXT("Removal undoable"), GEditor->UndoTransaction());
    FKismetEditorUtilities::CompileBlueprint(BP.Get());
    TestEqual(TEXT("Undo restores declaration"), BP->NewVariables.Num(), 1);
    TestTrue(TEXT("Undo restores default"), FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("Unused"))->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) == DefaultBefore);
    TestTrue(TEXT("Removal redoable"), GEditor->RedoTransaction());
    FKismetEditorUtilities::CompileBlueprint(BP.Get());
    TestEqual(TEXT("Redo removes declaration"), BP->NewVariables.Num(), 0);
    TestTrue(TEXT("Redo can be undone again"), GEditor->UndoTransaction());
    FKismetEditorUtilities::CompileBlueprint(BP.Get());
    TestEqual(TEXT("Repeated undo restores default"), FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("Unused"))->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()), DefaultBefore);
    UK2Node_CustomEvent* Trigger = NewObject<UK2Node_CustomEvent>(BP->UbergraphPages[0]);
    Trigger->CustomFunctionName = TEXT("RollbackTrigger");
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(BP->UbergraphPages[0], Trigger, {});
    UK2Node_DynamicCast* Broken = NewObject<UK2Node_DynamicCast>(BP->UbergraphPages[0]);
    Broken->TargetType = AActor::StaticClass();
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(BP->UbergraphPages[0], Broken, {});
    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Trigger->FindPin(UEdGraphSchema_K2::PN_Then), Broken->GetExecPin());
    const auto FailedRemoval = Call(FRemoveBlueprintMemberTool(false), BP.Get(), Apply);
    TestTrue(TEXT("Failed compile returns error"), FailedRemoval.Error.IsSet());
    TestEqual(TEXT("Failed removal restores member"), BP->NewVariables.Num(), 1);
    FBlueprintEditorUtils::RemoveNode(BP.Get(), Broken, true);
    FBlueprintEditorUtils::RemoveNode(BP.Get(), Trigger, true);
    FKismetEditorUtilities::CompileBlueprint(BP.Get());
    auto FunctionResult = Call(FRemoveBlueprintMemberTool(true), BP.Get(), [](FJsonObject& P)
    { P.SetStringField(TEXT("functionName"), TEXT("UnusedFunction")); P.SetBoolField(TEXT("confirm"), true); });
    if (FunctionResult.Error.IsSet()) AddError(FunctionResult.Error->Message);
    TestTrue(TEXT("Function graph removed"), !BP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* G) { return G && G->GetFName() == TEXT("UnusedFunction"); }));
    TestTrue(TEXT("Inherited member refused"), Call(FRemoveBlueprintMemberTool(false), BP.Get(), [](FJsonObject& P)
    { P.SetStringField(TEXT("variableName"), TEXT("bHidden")); P.SetBoolField(TEXT("confirm"), true); }).Error.IsSet());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPStructMemberTest, "UnrealMCP.Struct.MemberEditing.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPStructMemberTest::RunTest(const FString& Parameters)
{
    UPackage* Package = CreatePackage(*(TEXT("/Game/UnrealMCP_Automation/StructEditing_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    ON_SCOPE_EXIT { Package->SetDirtyFlag(false); };
    TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(Package, TEXT("S_MemberEditing"), RF_Transactional));
    const FGuid Original = FStructureEditorUtils::GetVarDesc(Struct.Get())[0].VarGuid;
    FStructureEditorUtils::RenameVariable(Struct.Get(), Original, TEXT("Keep"));
    FStructureEditorUtils::ChangeVariableDefaultValue(Struct.Get(), Original, TEXT("True"));
    auto AddParams = [](FJsonObject& P)
    { P.SetStringField(TEXT("memberName"), TEXT("Samples")); P.SetStringField(TEXT("type"), TEXT("int")); P.SetStringField(TEXT("containerType"), TEXT("array")); };
    const auto Dry = Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::Add), Struct.Get(), [&](FJsonObject& P)
    { AddParams(P); P.SetBoolField(TEXT("dryRun"), true); });
    TestFalse(TEXT("Typed dry run succeeds"), Dry.Error.IsSet());
    TestEqual(TEXT("Dry run preserves members"), FStructureEditorUtils::GetVarDesc(Struct.Get()).Num(), 1);
    TestTrue(TEXT("Confirmation required"), Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::Add), Struct.Get(), AddParams).Error.IsSet());
    const auto Added = Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::Add), Struct.Get(), [&](FJsonObject& P)
    { AddParams(P); P.SetBoolField(TEXT("confirm"), true); });
    if (Added.Error.IsSet()) { AddError(Added.Error->Message); return false; }
    TestEqual(TEXT("Member added"), FStructureEditorUtils::GetVarDesc(Struct.Get()).Num(), 2);
    const auto Inspected = Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::Inspect), Struct.Get(), [](FJsonObject&) {});
    TestEqual(TEXT("Inspection returns authored members"), Inspected.Result->GetArrayField(TEXT("members")).Num(), 2);
    TestTrue(TEXT("Duplicate name refused"), Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::Add), Struct.Get(), [&](FJsonObject& P)
    { AddParams(P); P.SetBoolField(TEXT("confirm"), true); }).Error.IsSet());
    const auto Removed = Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::Remove), Struct.Get(), [&](FJsonObject& P)
    { P.SetStringField(TEXT("memberGuid"), Added.Result->GetStringField(TEXT("memberGuid"))); P.SetBoolField(TEXT("confirm"), true); });
    if (Removed.Error.IsSet()) AddError(Removed.Error->Message);
    TestEqual(TEXT("Selected member removed"), FStructureEditorUtils::GetVarDesc(Struct.Get()).Num(), 1);
    TestEqual(TEXT("Surviving GUID preserved"), FStructureEditorUtils::GetVarDesc(Struct.Get())[0].VarGuid, Original);
    TestEqual(TEXT("Surviving default preserved"), FStructureEditorUtils::GetVarDesc(Struct.Get())[0].DefaultValue, FString(TEXT("True")));
    TestTrue(TEXT("Last member removal refused"), Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::Remove), Struct.Get(), [](FJsonObject& P)
    { P.SetStringField(TEXT("memberName"), TEXT("Keep")); P.SetBoolField(TEXT("confirm"), true); }).Error.IsSet());
    TestTrue(TEXT("Struct edit undoable"), GEditor->UndoTransaction());
    TestEqual(TEXT("Undo restores struct member"), FStructureEditorUtils::GetVarDesc(Struct.Get()).Num(), 2);
    const auto Renamed = Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::Rename), Struct.Get(), [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("memberGuid"), Original.ToString());
        P.SetStringField(TEXT("newName"), TEXT("Renamed"));
        P.SetBoolField(TEXT("confirm"), true);
    });
    if (Renamed.Error.IsSet()) { AddError(Renamed.Error->Message); return false; }
    TestEqual(TEXT("Rename preserves GUID"), FStructureEditorUtils::GetVarDesc(Struct.Get())[0].VarGuid, Original);
    TestEqual(TEXT("Rename preserves default"), FStructureEditorUtils::GetVarDesc(Struct.Get())[0].DefaultValue, FString(TEXT("True")));
    const auto Default = Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::SetDefault), Struct.Get(), [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("memberGuid"), Original.ToString());
        P.SetStringField(TEXT("defaultValue"), TEXT("False"));
        P.SetBoolField(TEXT("confirm"), true);
    });
    if (Default.Error.IsSet()) { AddError(Default.Error->Message); return false; }
    TestEqual(TEXT("Default updated"), FStructureEditorUtils::GetVarDesc(Struct.Get())[0].DefaultValue, FString(TEXT("False")));

    auto* Enum = CastChecked<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package, TEXT("E_MemberEditing"), RF_Public | RF_Transactional));
    FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
    const int32 OriginalCount = Enum->NumEnums() - 1;
    const FName FirstEntry = Enum->GetNameByIndex(0);
    const int64 FirstValue = Enum->GetValueByIndex(0);
    UPackage* UserPackage = CreatePackage(*(TEXT("/Game/UnrealMCP_Automation/EnumUser_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    ON_SCOPE_EXIT { UserPackage->SetDirtyFlag(false); };
    TStrongObjectPtr<UBlueprint> User(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), UserPackage,
        TEXT("BP_EnumUser"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
    FEdGraphPinType EnumType;
    EnumType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    EnumType.PinSubCategoryObject = Enum;
    FBlueprintEditorUtils::AddMemberVariable(User.Get(), TEXT("Choice"), EnumType, FirstEntry.ToString());
    UEdGraph* UserGraph = User->UbergraphPages[0];
    auto* Choice = NewObject<UK2Node_VariableGet>(UserGraph);
    Choice->VariableReference.SetSelfMember(TEXT("Choice"));
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(UserGraph, Choice, {});
    FKismetEditorUtilities::CompileBlueprint(User.Get());
    const auto Appended = Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::AddEnum), Enum, [](FJsonObject& P)
    {
        P.SetStringField(TEXT("displayName"), TEXT("Appended"));
        P.SetBoolField(TEXT("confirm"), true);
        P.SetBoolField(TEXT("allowReferenced"), true);
        P.SetBoolField(TEXT("compileDependents"), true);
    });
    if (Appended.Error.IsSet()) { AddError(Appended.Error->Message); return false; }
    TestEqual(TEXT("Append preserves first internal name"), Enum->GetNameByIndex(0), FirstEntry);
    TestEqual(TEXT("Append preserves first numeric value"), Enum->GetValueByIndex(0), FirstValue);
    const auto& Dependents = Appended.Result->GetArrayField(TEXT("dependentBlueprints"));
    if (!TestEqual(TEXT("Unsaved dependent Blueprint found"), Dependents.Num(), 1)) return false;
    const auto& Dependent = Dependents[0]->AsObject();
    TestEqual(TEXT("Dependent compiles"), Dependent->GetIntegerField(TEXT("errorCount")), 0);
    TestTrue(TEXT("Dependent pin located"), !Dependent->GetArrayField(TEXT("affectedPinsAfter")).IsEmpty());
    const auto Relabeled = Call(FUserDefinedStructMemberTool(EUserStructMemberOperation::SetEnumDisplayName), Enum, [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("entryName"), Appended.Result->GetStringField(TEXT("entryName")));
        P.SetStringField(TEXT("displayName"), TEXT("Relabeled"));
        P.SetBoolField(TEXT("confirm"), true);
        P.SetBoolField(TEXT("allowReferenced"), true);
    });
    if (Relabeled.Error.IsSet()) { AddError(Relabeled.Error->Message); return false; }
    TestEqual(TEXT("Relabel preserves appended numeric value"), Enum->GetValueByIndex(OriginalCount), static_cast<int64>(OriginalCount));
    TestEqual(TEXT("Enum label updated"), Enum->GetDisplayNameTextByIndex(OriginalCount).ToString(), FString(TEXT("Relabeled")));
    return !HasAnyErrors();
}
#endif
