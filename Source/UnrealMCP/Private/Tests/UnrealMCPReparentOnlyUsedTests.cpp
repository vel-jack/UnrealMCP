#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "ObjectTools.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Members/ReparentBlueprintTool.h"
#include "Tools/Blueprint/Members/BlueprintClassDefaultsTools.h"
#include "UObject/UnrealType.h"

namespace
{
    struct FOnlyUsedFixture
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        TArray<FString> Paths;
        UBlueprint* Create(const TCHAR* Prefix, UClass* Parent)
        {
            const FString AssetName = FString(Prefix) + TEXT("_") + Suffix;
            UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(Parent,
                CreatePackage(*(TEXT("/Game/UnrealMCP_Automation/") + AssetName)), FName(*AssetName),
                BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
            FAssetRegistryModule::AssetCreated(BP); Paths.Add(BP->GetPathName()); return BP;
        }
        static void Compile(UBlueprint* BP)
        { FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection); }
        static void Set(UBlueprint* BP, const TCHAR* Variable, const TCHAR* Value)
        { FindFProperty<FStrProperty>(BP->GeneratedClass, Variable)->SetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject(), Value); }
        ~FOnlyUsedFixture()
        {
            for (int32 I = Paths.Num() - 1; I >= 0; --I) if (UObject* Object = FindObject<UObject>(nullptr, *Paths[I]))
            {
                UPackage* Package = Object->GetPackage(); FAssetRegistryModule::AssetDeleted(Object);
                ObjectTools::DeleteObjectsUnchecked({Object}); Package->SetDirtyFlag(false);
            }
        }
    };
    bool HasDefault(const TSharedPtr<FJsonObject>& Result, const TCHAR* Property)
    {
        return Result.IsValid() && Result->GetArrayField(TEXT("defaultChanges")).ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Entry)
        { return Entry->AsObject()->GetStringField(TEXT("property")) == Property; });
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPReparentOnlyUsedTest, "UnrealMCP.Blueprint.Authoring.ReparentOnlyUsed.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPReparentOnlyUsedTest::RunTest(const FString& Parameters)
{
    FOnlyUsedFixture Fixture;
    UBlueprint* Base = Fixture.Create(TEXT("BP_OnlyUsedBase"), AActor::StaticClass());
    FEdGraphPinType String; String.PinCategory = UEdGraphSchema_K2::PC_String;
    for (const TCHAR* Variable : {TEXT("GraphRead"), TEXT("TargetOverride"), TEXT("DescendantOverride"), TEXT("Unused")})
        FBlueprintEditorUtils::AddMemberVariable(Base, Variable, String, TEXT("base"));
    Fixture.Compile(Base);
    for (const TCHAR* Variable : {TEXT("GraphRead"), TEXT("TargetOverride"), TEXT("DescendantOverride"), TEXT("Unused")}) Fixture.Set(Base, Variable, TEXT("base"));
    CastChecked<AActor>(Base->GeneratedClass->GetDefaultObject())->Tags.Add(TEXT("BaseTag"));
    UBlueprint* Target = Fixture.Create(TEXT("BP_OnlyUsedTarget"), Base->GeneratedClass);
    for (const TCHAR* Variable : {TEXT("GraphRead"), TEXT("Tags")})
    {
        auto* Getter = NewObject<UK2Node_VariableGet>(Target->UbergraphPages[0]);
        Getter->VariableReference.SetSelfMember(Variable);
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Target->UbergraphPages[0], Getter, {});
    }
    Fixture.Compile(Target); Fixture.Set(Target, TEXT("TargetOverride"), TEXT("target override"));
    UBlueprint* Descendant = Fixture.Create(TEXT("BP_OnlyUsedDescendant"), Target->GeneratedClass);
    Fixture.Compile(Descendant); Fixture.Set(Descendant, TEXT("DescendantOverride"), TEXT("descendant override"));
    UBlueprint* GrandChild = Fixture.Create(TEXT("BP_OnlyUsedGrandChild"), Descendant->GeneratedClass); Fixture.Compile(GrandChild);
    UBlueprint* NewParent = Fixture.Create(TEXT("BP_OnlyUsedNewParent"), AActor::StaticClass()); Fixture.Compile(NewParent);
    const bool bDirtyBefore = Target->GetPackage()->IsDirty();
    auto Run = [&](bool bFilter)
    {
        UnrealMCP::FMCPRequest Request; Request.Id = TEXT("reparent-only-used"); Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), Target->GetPathName());
        Request.Params->SetStringField(TEXT("newParentClassPath"), NewParent->GeneratedClass->GetPathName());
        Request.Params->SetBoolField(TEXT("dryRun"), true);
        if (bFilter) Request.Params->SetBoolField(TEXT("onlyUsed"), true);
        return FReparentBlueprintTool().Execute(Request);
    };
    const auto Full = Run(false);
    if (Full.Error.IsSet()) { AddError(Full.Error->Message); return false; }
    TestTrue(TEXT("Default behavior retains unused inherited default"), HasDefault(Full.Result, TEXT("Unused")));
    TestFalse(TEXT("Filter defaults off"), Full.Result->GetBoolField(TEXT("onlyUsed")));
    const auto Filtered = Run(true);
    if (Filtered.Error.IsSet()) { AddError(Filtered.Error->Message); return false; }
    TestTrue(TEXT("Inherited skeleton-owned graph read retained"), HasDefault(Filtered.Result, TEXT("GraphRead")));
    TestTrue(TEXT("Native graph read retained"), HasDefault(Filtered.Result, TEXT("Tags")));
    TestTrue(TEXT("Target CDO override retained"), HasDefault(Filtered.Result, TEXT("TargetOverride")));
    TestTrue(TEXT("Descendant CDO override retained"), HasDefault(Filtered.Result, TEXT("DescendantOverride")));
    TestFalse(TEXT("Unused unoverridden default excluded"), HasDefault(Filtered.Result, TEXT("Unused")));
    TestTrue(TEXT("Loaded descendants inspected"), Filtered.Result->GetIntegerField(TEXT("descendantsScanned")) >= 2);
    TestTrue(TEXT("Coverage completeness reported"), Filtered.Result->HasField(TEXT("descendantCoverageComplete")));
    TestFalse(TEXT("Small scan not truncated"), Filtered.Result->GetBoolField(TEXT("descendantScanTruncated")));
    TestTrue(TEXT("Dry run preserves parent"), Target->ParentClass == Base->GeneratedClass);
    TestEqual(TEXT("Dry run preserves target dirty state"), Target->GetPackage()->IsDirty(), bDirtyBefore);
    UnrealMCP::FMCPRequest Defaults; Defaults.Id = TEXT("declaring-class-filter"); Defaults.Params = MakeShared<FJsonObject>();
    Defaults.Params->SetStringField(TEXT("objectPath"), Target->GetPathName());
    Defaults.Params->SetStringField(TEXT("declaredInOnly"), Base->GeneratedClass->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Names;
    for (const TCHAR* Name : {TEXT("GraphRead"), TEXT("TargetOverride"), TEXT("DescendantOverride"), TEXT("Unused"), TEXT("Tags")})
        Names.Add(MakeShared<FJsonValueString>(Name));
    Defaults.Params->SetArrayField(TEXT("propertyNames"), Names);
    Defaults.Params->SetNumberField(TEXT("offset"), 1); Defaults.Params->SetNumberField(TEXT("limit"), 2);
    const auto Page = FGetBlueprintClassDefaultsTool().Execute(Defaults);
    if (Page.Error.IsSet()) { AddError(Page.Error->Message); return false; }
    TestEqual(TEXT("Declaring-class filter precedes pagination total"), Page.Result->GetIntegerField(TEXT("total")), 4);
    TestEqual(TEXT("Declaring-class filter page size"), Page.Result->GetIntegerField(TEXT("count")), 2);
    TestTrue(TEXT("Declaring-class page has remaining property"), Page.Result->GetBoolField(TEXT("hasMore")));
    Defaults.Params->SetStringField(TEXT("declaredInOnly"), NewParent->GeneratedClass->GetPathName());
    const auto Empty = FGetBlueprintClassDefaultsTool().Execute(Defaults);
    if (Empty.Error.IsSet()) { AddError(Empty.Error->Message); return false; }
    TestEqual(TEXT("Unrelated class has no declaring properties"), Empty.Result->GetIntegerField(TEXT("total")), 0);
    return !HasAnyErrors();
}
#endif
