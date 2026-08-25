#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/AddBlueprintTypedOperatorNodeTool.h"

namespace
{
    constexpr TCHAR TestRoot[] = TEXT("/Game/UnrealMCP_Automation");

    UBlueprint* CreateActorBlueprintFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        const FName AssetName(*FPackageName::GetLongPackageAssetName(PackagePath));
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package, AssetName, BPTYPE_Normal,
            UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("UnrealMCPAutomation")));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPTypedOperatorsLiveTest,
    "UnrealMCP.Blueprint.Authoring.TypedOperators.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPTypedOperatorsLiveTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_Operators_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), TestRoot, *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);

    UBlueprint* Blueprint = CreateActorBlueprintFixture(PackagePath);
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
    };

    if (!TestNotNull(TEXT("Temporary Actor Blueprint created"), Blueprint)
        || !TestTrue(TEXT("Blueprint has an event graph"), Blueprint->UbergraphPages.Num() > 0))
    {
        return false;
    }

    const TArray<FString> Operators = {
        TEXT("ObjectEqual"), TEXT("ObjectNotEqual"), TEXT("BooleanAnd"), TEXT("BooleanOr"),
        TEXT("BooleanNot"), TEXT("VectorAdd"), TEXT("VectorSubtract"), TEXT("VectorNearlyEqual")};
    const FAddBlueprintTypedOperatorNodeTool Tool;
    for (int32 Index = 0; Index < Operators.Num(); ++Index)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = FString::Printf(TEXT("operator-%d"), Index);
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->SetStringField(TEXT("graphName"), Blueprint->UbergraphPages[0]->GetName());
        Request.Params->SetStringField(TEXT("operator"), Operators[Index]);
        Request.Params->SetNumberField(TEXT("positionX"), Index * 350);
        Request.Params->SetNumberField(TEXT("positionY"), 300);
        if (Operators[Index] == TEXT("VectorNearlyEqual"))
        {
            Request.Params->SetNumberField(TEXT("tolerance"), 0.25);
        }

        const UnrealMCP::FMCPResponse Response = Tool.Execute(Request);
        TestFalse(*FString::Printf(TEXT("%s has no MCP error"), *Operators[Index]), Response.Error.IsSet());
        if (!TestTrue(*FString::Printf(TEXT("%s returns a result"), *Operators[Index]), Response.Result.IsValid()))
        {
            continue;
        }
        TestTrue(*FString::Printf(TEXT("%s succeeds"), *Operators[Index]),
            Response.Result->GetBoolField(TEXT("success")));
        TestFalse(*FString::Printf(TEXT("%s resolves an exact function"), *Operators[Index]),
            Response.Result->GetStringField(TEXT("functionName")).IsEmpty());
        TestFalse(*FString::Printf(TEXT("%s returns pins"), *Operators[Index]),
            Response.Result->GetArrayField(TEXT("pins")).IsEmpty());
        if (Operators[Index] == TEXT("VectorNearlyEqual"))
        {
            TestEqual(TEXT("Vector tolerance is applied"),
                FCString::Atod(*Response.Result->GetStringField(TEXT("appliedTolerance"))), 0.25);
        }
    }

    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    TestTrue(TEXT("Blueprint compiles with all common typed operators"),
        Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
