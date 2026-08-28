#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/AddBlueprintFunctionCallNodeTool.h"
#include "Tools/AddBlueprintBranchNodeTool.h"
#include "Tools/AddBlueprintRerouteNodeTool.h"
#include "Tools/AddBlueprintTypedOperatorNodeTool.h"
#include "Tools/AddBlueprintVariableGetNodeTool.h"
#include "Tools/AddBlueprintVariableSetNodeTool.h"

namespace
{
    const TSharedPtr<FJsonObject> FindPin(
        const TArray<TSharedPtr<FJsonValue>>& Pins,
        const FString& PinName,
        const FString& Direction)
    {
        for (const TSharedPtr<FJsonValue>& Value : Pins)
        {
            const TSharedPtr<FJsonObject> Pin = Value.IsValid() ? Value->AsObject() : nullptr;
            if (Pin.IsValid()
                && Pin->GetStringField(TEXT("pinName")).Equals(PinName, ESearchCase::IgnoreCase)
                && Pin->GetStringField(TEXT("direction")).Equals(Direction, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    UBlueprint* CreateFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package, *FPackageName::GetLongPackageAssetName(PackagePath),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("UnrealMCPAutomation")));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPFunctionCallPinPredictionTest,
    "UnrealMCP.Blueprint.Authoring.FunctionCallPinPrediction.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPFunctionCallPinPredictionTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_PinPrediction_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("/Game/UnrealMCP_Automation/%s"), *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);
    UBlueprint* Blueprint = CreateFixture(PackagePath);
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
    };

    if (!TestNotNull(TEXT("Temporary Actor Blueprint created"), Blueprint)
        || !TestTrue(TEXT("Fixture has an event graph"), Blueprint->UbergraphPages.Num() > 0))
    {
        return false;
    }
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    FEdGraphPinType FlagType;
    FlagType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    const FName FlagName(TEXT("PreviewFlag"));
    if (!TestTrue(TEXT("Fixture Boolean variable created"),
        FBlueprintEditorUtils::AddMemberVariable(Blueprint, FlagName, FlagType)))
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    const int32 InitialNodeCount = Graph->Nodes.Num();

    UnrealMCP::FMCPRequest InstanceRequest;
    InstanceRequest.Id = TEXT("instance-preview");
    InstanceRequest.Params = MakeShared<FJsonObject>();
    InstanceRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    InstanceRequest.Params->SetStringField(TEXT("graphName"), Graph->GetName());
    InstanceRequest.Params->SetStringField(TEXT("ownerClassPath"), USceneComponent::StaticClass()->GetPathName());
    InstanceRequest.Params->SetStringField(TEXT("functionName"), GET_FUNCTION_NAME_CHECKED(USceneComponent, K2_SetRelativeLocation).ToString());
    InstanceRequest.Params->SetBoolField(TEXT("dryRun"), true);
    const FAddBlueprintFunctionCallNodeTool FunctionTool;
    const UnrealMCP::FMCPResponse InstanceResponse = FunctionTool.Execute(InstanceRequest);
    TestFalse(TEXT("Instance dry-run has no error"), InstanceResponse.Error.IsSet());
    if (TestTrue(TEXT("Instance dry-run returns a result"), InstanceResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Instance dry-run predicts pins"), InstanceResponse.Result->GetBoolField(TEXT("pinsPredicted")));
        TestTrue(TEXT("External instance call requires an explicit target"), InstanceResponse.Result->GetBoolField(TEXT("requiresExplicitTarget")));
        const TArray<TSharedPtr<FJsonValue>>& Pins = InstanceResponse.Result->GetArrayField(TEXT("pins"));
        TestFalse(TEXT("Instance dry-run returns complete pins"), Pins.IsEmpty());
        const TSharedPtr<FJsonObject> SelfPin = FindPin(Pins, TEXT("self"), TEXT("input"));
        if (TestTrue(TEXT("Instance preview exposes self"), SelfPin.IsValid()))
        {
            TestTrue(TEXT("Self pin is classified"), SelfPin->GetBoolField(TEXT("isSelfPin")));
            TestTrue(TEXT("Self pin requires an explicit target"), SelfPin->GetBoolField(TEXT("requiresExplicitTarget")));
            TestTrue(TEXT("Self pin is data"), SelfPin->GetBoolField(TEXT("isData")));
        }
    }

    UnrealMCP::FMCPRequest StaticRequest;
    StaticRequest.Id = TEXT("static-preview");
    StaticRequest.Params = MakeShared<FJsonObject>();
    StaticRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    StaticRequest.Params->SetStringField(TEXT("graphName"), Graph->GetName());
    StaticRequest.Params->SetStringField(TEXT("operator"), TEXT("VectorNearlyEqual"));
    StaticRequest.Params->SetNumberField(TEXT("tolerance"), 0.125);
    StaticRequest.Params->SetBoolField(TEXT("dryRun"), true);
    const FAddBlueprintTypedOperatorNodeTool OperatorTool;
    const UnrealMCP::FMCPResponse StaticResponse = OperatorTool.Execute(StaticRequest);
    TestFalse(TEXT("Typed operator dry-run has no error"), StaticResponse.Error.IsSet());
    if (TestTrue(TEXT("Typed operator dry-run returns a result"), StaticResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Typed operator dry-run predicts pins"), StaticResponse.Result->GetBoolField(TEXT("pinsPredicted")));
        const TArray<TSharedPtr<FJsonValue>>& Pins = StaticResponse.Result->GetArrayField(TEXT("pins"));
        TestFalse(TEXT("Typed operator dry-run returns complete pins"), Pins.IsEmpty());
        const TSharedPtr<FJsonObject> TolerancePin = FindPin(Pins, TEXT("ErrorTolerance"), TEXT("input"));
        if (TestTrue(TEXT("Typed operator preview exposes tolerance"), TolerancePin.IsValid()))
        {
            TestEqual(TEXT("Predicted tolerance is applied"), FCString::Atod(*TolerancePin->GetStringField(TEXT("defaultValue"))), 0.125);
        }
    }

    UnrealMCP::FMCPRequest BranchRequest;
    BranchRequest.Id = TEXT("branch-preview");
    BranchRequest.Params = MakeShared<FJsonObject>();
    BranchRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    BranchRequest.Params->SetStringField(TEXT("graphName"), Graph->GetName());
    BranchRequest.Params->SetBoolField(TEXT("dryRun"), true);
    const FAddBlueprintBranchNodeTool BranchTool;
    const UnrealMCP::FMCPResponse BranchResponse = BranchTool.Execute(BranchRequest);
    TestFalse(TEXT("Branch dry-run has no error"), BranchResponse.Error.IsSet());
    if (TestTrue(TEXT("Branch dry-run returns a result"), BranchResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Branch dry-run predicts pins"), BranchResponse.Result->GetBoolField(TEXT("pinsPredicted")));
        const TArray<TSharedPtr<FJsonValue>>& Pins = BranchResponse.Result->GetArrayField(TEXT("pins"));
        TestTrue(TEXT("Branch preview exposes execute"), FindPin(Pins, TEXT("execute"), TEXT("input")).IsValid());
        const TSharedPtr<FJsonObject> Condition = FindPin(Pins, TEXT("Condition"), TEXT("input"));
        if (TestTrue(TEXT("Branch preview exposes Condition"), Condition.IsValid()))
        {
            TestEqual(TEXT("Branch Condition is Boolean"), Condition->GetStringField(TEXT("category")), UEdGraphSchema_K2::PC_Boolean.ToString());
        }
        TestTrue(TEXT("Branch preview exposes true route"), FindPin(Pins, TEXT("then"), TEXT("output")).IsValid());
        TestTrue(TEXT("Branch preview exposes false route"), FindPin(Pins, TEXT("else"), TEXT("output")).IsValid());
    }

    for (const bool bSet : {false, true})
    {
        UnrealMCP::FMCPRequest VariableRequest;
        VariableRequest.Id = bSet ? TEXT("variable-set-preview") : TEXT("variable-get-preview");
        VariableRequest.Params = MakeShared<FJsonObject>();
        VariableRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        VariableRequest.Params->SetStringField(TEXT("graphName"), Graph->GetName());
        VariableRequest.Params->SetStringField(TEXT("variableName"), FlagName.ToString());
        VariableRequest.Params->SetBoolField(TEXT("dryRun"), true);
        const UnrealMCP::FMCPResponse VariableResponse = bSet
            ? FAddBlueprintVariableSetNodeTool().Execute(VariableRequest)
            : FAddBlueprintVariableGetNodeTool().Execute(VariableRequest);
        TestFalse(bSet ? TEXT("Variable Set dry-run has no error") : TEXT("Variable Get dry-run has no error"), VariableResponse.Error.IsSet());
        if (TestTrue(bSet ? TEXT("Variable Set dry-run returns a result") : TEXT("Variable Get dry-run returns a result"), VariableResponse.Result.IsValid()))
        {
            TestTrue(bSet ? TEXT("Variable Set predicts pins") : TEXT("Variable Get predicts pins"), VariableResponse.Result->GetBoolField(TEXT("pinsPredicted")));
            const TArray<TSharedPtr<FJsonValue>>& Pins = VariableResponse.Result->GetArrayField(TEXT("pins"));
            const TSharedPtr<FJsonObject> ValuePin = FindPin(Pins, FlagName.ToString(), bSet ? TEXT("input") : TEXT("output"));
            if (TestTrue(bSet ? TEXT("Variable Set exposes typed value") : TEXT("Variable Get exposes typed value"), ValuePin.IsValid()))
            {
                TestEqual(bSet ? TEXT("Variable Set value is Boolean") : TEXT("Variable Get value is Boolean"),
                    ValuePin->GetStringField(TEXT("category")), UEdGraphSchema_K2::PC_Boolean.ToString());
            }
        }
    }

    UnrealMCP::FMCPRequest RerouteRequest;
    RerouteRequest.Id = TEXT("reroute-preview");
    RerouteRequest.Params = MakeShared<FJsonObject>();
    RerouteRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    RerouteRequest.Params->SetStringField(TEXT("graphName"), Graph->GetName());
    RerouteRequest.Params->SetBoolField(TEXT("dryRun"), true);
    const FAddBlueprintRerouteNodeTool RerouteTool;
    const UnrealMCP::FMCPResponse RerouteResponse = RerouteTool.Execute(RerouteRequest);
    TestFalse(TEXT("Reroute dry-run has no error"), RerouteResponse.Error.IsSet());
    if (TestTrue(TEXT("Reroute dry-run returns a result"), RerouteResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Reroute dry-run predicts pins"), RerouteResponse.Result->GetBoolField(TEXT("pinsPredicted")));
        const TArray<TSharedPtr<FJsonValue>>& Pins = RerouteResponse.Result->GetArrayField(TEXT("pins"));
        TestEqual(TEXT("Reroute preview has two pins"), Pins.Num(), 2);
        for (const TSharedPtr<FJsonValue>& PinValue : Pins)
        {
            TestEqual(TEXT("Reroute preview remains wildcard"),
                PinValue->AsObject()->GetStringField(TEXT("category")), UEdGraphSchema_K2::PC_Wildcard.ToString());
        }
    }

    TestEqual(TEXT("Dry-runs do not add graph nodes"), Graph->Nodes.Num(), InitialNodeCount);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
