#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/SaveBlueprintTool.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPSaveBlueprintTest,
    "UnrealMCP.Blueprint.Authoring.SaveBlueprint.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPSaveBlueprintTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_SaveBlueprint_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("/Game/UnrealMCP_Automation/%s"), *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);
    UPackage* Package = CreatePackage(*PackagePath);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Package, *BlueprintName, BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("UnrealMCPAutomation")));

    auto CleanupFixture = [&]
    {
        if (Blueprint != nullptr && Blueprint->GetOuter() != GetTransientPackage())
        {
            FAssetRegistryModule::AssetDeleted(Blueprint);
            Blueprint->ClearFlags(RF_Public | RF_Standalone);
            const FName TransientName = MakeUniqueObjectName(GetTransientPackage(), Blueprint->GetClass(), Blueprint->GetFName());
            Blueprint->Rename(*TransientName.ToString(), GetTransientPackage(),
                REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty | REN_ForceNoResetLoaders);
        }
        FString PackageFilename;
        if (FPackageName::DoesPackageExist(PackagePath, &PackageFilename))
        {
            IFileManager::Get().Delete(*PackageFilename, false, true, true);
        }
        if (UPackage* RemainingPackage = FindPackage(nullptr, *PackagePath))
        {
            RemainingPackage->SetDirtyFlag(false);
        }
    };
    ON_SCOPE_EXIT
    {
        CleanupFixture();
    };

    if (!TestNotNull(TEXT("SaveBlueprint fixture created"), Blueprint))
    {
        return false;
    }
    FAssetRegistryModule::AssetCreated(Blueprint);

    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    TestFalse(TEXT("Fixture is not compile-up-to-date"),
        Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

    const FSaveBlueprintTool Tool;
    UnrealMCP::FMCPRequest RefusalRequest;
    RefusalRequest.Id = TEXT("save-blueprint-refusal");
    RefusalRequest.Params = MakeShared<FJsonObject>();
    RefusalRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    const UnrealMCP::FMCPResponse Refusal = Tool.Execute(RefusalRequest);
    TestFalse(TEXT("Expected compile-state refusal is structured, not a JSON-RPC error"), Refusal.Error.IsSet());
    if (TestTrue(TEXT("Compile-state refusal returns a result"), Refusal.Result.IsValid()))
    {
        TestFalse(TEXT("Compile-state refusal is unsuccessful"), Refusal.Result->GetBoolField(TEXT("success")));
        TestFalse(TEXT("Compile-state refusal does not save"), Refusal.Result->GetBoolField(TEXT("saved")));
        TestTrue(TEXT("Compile-state refusal preserves dirty state"), Refusal.Result->GetBoolField(TEXT("isDirty")));
        TestEqual(TEXT("Compile-state refusal has a stable error code"),
            Refusal.Result->GetStringField(TEXT("errorCode")), FString(TEXT("blueprint_compile_not_up_to_date")));
    }
    TestFalse(TEXT("Refused save creates no package file"), FPackageName::DoesPackageExist(PackagePath));

    UnrealMCP::FMCPRequest OverrideRequest;
    OverrideRequest.Id = TEXT("save-blueprint-override");
    OverrideRequest.Params = MakeShared<FJsonObject>();
    OverrideRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    OverrideRequest.Params->SetBoolField(TEXT("requireUpToDateCompile"), false);
    const UnrealMCP::FMCPResponse Override = Tool.Execute(OverrideRequest);
    TestFalse(TEXT("Explicit compile-state override has no JSON-RPC error"), Override.Error.IsSet());
    if (TestTrue(TEXT("Compile-state override returns a result"), Override.Result.IsValid()))
    {
        TestTrue(TEXT("Explicit override saves successfully"), Override.Result->GetBoolField(TEXT("success")));
        TestTrue(TEXT("Explicit override reports saved"), Override.Result->GetBoolField(TEXT("saved")));
        TestFalse(TEXT("Post-save dirty state is measured clean"), Override.Result->GetBoolField(TEXT("isDirty")));
        TestFalse(TEXT("Override does not pretend the stale compile state was validated"), Override.Result->GetBoolField(TEXT("validated")));
    }
    TestTrue(TEXT("Explicit override created the temporary package file"), FPackageName::DoesPackageExist(PackagePath));

    const FMCPToolDefinition Definition = Tool.GetDefinition();
    const TSharedPtr<FJsonObject>* Properties = nullptr;
    TestTrue(TEXT("SaveBlueprint schema contains properties"),
        Definition.InputSchema.IsValid() && Definition.InputSchema->TryGetObjectField(TEXT("properties"), Properties));
    if (Properties != nullptr)
    {
        TestTrue(TEXT("SaveBlueprint schema advertises requireUpToDateCompile"),
            (*Properties)->HasField(TEXT("requireUpToDateCompile")));
    }

    CleanupFixture();
    TestFalse(TEXT("Temporary saved fixture is removed from disk"), FPackageName::DoesPackageExist(PackagePath));

    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
