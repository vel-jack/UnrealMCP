#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/ApplyBlueprintGraphPatchTool.h"
#include "Tools/BlueprintGraphEditToolUtils.h"

namespace
{
    UBlueprint* CreatePatchFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package, *FPackageName::GetLongPackageAssetName(PackagePath),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("UnrealMCPAutomation")));
    }

    TSharedRef<FJsonObject> Node(const FString& Id, const FString& Kind)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("id"), Id); Result->SetStringField(TEXT("kind"), Kind);
        return Result;
    }

    TSharedRef<FJsonObject> Connection(
        const FString& SourceId, const FString& SourcePin,
        const FString& TargetId, const FString& TargetPin)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("sourceNodeId"), SourceId); Result->SetStringField(TEXT("sourcePinName"), SourcePin);
        Result->SetStringField(TEXT("targetNodeId"), TargetId); Result->SetStringField(TEXT("targetPinName"), TargetPin);
        return Result;
    }

    UnrealMCP::FMCPRequest BuildPatchRequest(
        const FString& ObjectPath, const FString& GraphGuid, bool bDryRun)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = bDryRun ? TEXT("graph-patch-dry-run") : TEXT("graph-patch-apply");
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->SetStringField(TEXT("graphGuid"), GraphGuid);
        Request.Params->SetStringField(TEXT("patchId"), TEXT("graph-patch-automation-v1"));
        Request.Params->SetBoolField(TEXT("dryRun"), bDryRun);
        Request.Params->SetBoolField(TEXT("compileAfterEdit"), true);
        Request.Params->SetBoolField(TEXT("saveAfterEdit"), false);

        TSharedRef<FJsonObject> FirstGet = Node(TEXT("firstFlag"), TEXT("variableGet"));
        FirstGet->SetStringField(TEXT("variableName"), TEXT("PatchFlag"));
        FirstGet->SetNumberField(TEXT("positionX"), 0); FirstGet->SetNumberField(TEXT("positionY"), 300);
        TSharedRef<FJsonObject> SecondGet = Node(TEXT("secondFlag"), TEXT("variableGet"));
        SecondGet->SetStringField(TEXT("variableName"), TEXT("PatchFlag"));
        SecondGet->SetNumberField(TEXT("positionX"), 0); SecondGet->SetNumberField(TEXT("positionY"), 500);
        TSharedRef<FJsonObject> Not = Node(TEXT("invert"), TEXT("typedOperator"));
        Not->SetStringField(TEXT("operator"), TEXT("BooleanNot"));
        Not->SetNumberField(TEXT("positionX"), 350); Not->SetNumberField(TEXT("positionY"), 300);
        TSharedRef<FJsonObject> Branch = Node(TEXT("branch"), TEXT("branch"));
        Branch->SetNumberField(TEXT("positionX"), 700); Branch->SetNumberField(TEXT("positionY"), 300);
        Request.Params->SetArrayField(TEXT("nodes"), {
            MakeShared<FJsonValueObject>(FirstGet), MakeShared<FJsonValueObject>(SecondGet),
            MakeShared<FJsonValueObject>(Not), MakeShared<FJsonValueObject>(Branch)});
        Request.Params->SetArrayField(TEXT("connections"), {
            MakeShared<FJsonValueObject>(Connection(TEXT("firstFlag"), TEXT("PatchFlag"), TEXT("invert"), TEXT("A"))),
            MakeShared<FJsonValueObject>(Connection(TEXT("invert"), TEXT("ReturnValue"), TEXT("branch"), TEXT("Condition")))});
        return Request;
    }

    UnrealMCP::FMCPRequest BuildWildcardPatchRequest(
        const FString& ObjectPath, const FString& GraphGuid, bool bDryRun)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = bDryRun ? TEXT("graph-patch-wildcard-dry-run") : TEXT("graph-patch-wildcard-apply");
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->SetStringField(TEXT("graphGuid"), GraphGuid);
        Request.Params->SetStringField(TEXT("patchId"), TEXT("graph-patch-wildcard-v1"));
        Request.Params->SetBoolField(TEXT("dryRun"), bDryRun);
        Request.Params->SetBoolField(TEXT("compileAfterEdit"), true);
        Request.Params->SetBoolField(TEXT("saveAfterEdit"), false);

        TSharedRef<FJsonObject> Flag = Node(TEXT("flag"), TEXT("variableGet"));
        Flag->SetStringField(TEXT("variableName"), TEXT("PatchFlag"));
        TSharedRef<FJsonObject> FirstKnot = Node(TEXT("firstKnot"), TEXT("reroute"));
        TSharedRef<FJsonObject> SecondKnot = Node(TEXT("secondKnot"), TEXT("reroute"));
        TSharedRef<FJsonObject> Branch = Node(TEXT("branch"), TEXT("branch"));
        Request.Params->SetArrayField(TEXT("nodes"), {
            MakeShared<FJsonValueObject>(Flag), MakeShared<FJsonValueObject>(FirstKnot),
            MakeShared<FJsonValueObject>(SecondKnot), MakeShared<FJsonValueObject>(Branch)});

        // Deliberately start wildcard-to-wildcard. Later connections must propagate
        // the Boolean type across both reroutes during transient preflight.
        Request.Params->SetArrayField(TEXT("connections"), {
            MakeShared<FJsonValueObject>(Connection(TEXT("firstKnot"), TEXT("OutputPin"), TEXT("secondKnot"), TEXT("InputPin"))),
            MakeShared<FJsonValueObject>(Connection(TEXT("secondKnot"), TEXT("OutputPin"), TEXT("branch"), TEXT("Condition"))),
            MakeShared<FJsonValueObject>(Connection(TEXT("flag"), TEXT("PatchFlag"), TEXT("firstKnot"), TEXT("InputPin")))});
        return Request;
    }

    bool NodePinsHaveCategory(const TSharedPtr<FJsonObject>& Result, const FString& NodeId, const FString& Category)
    {
        for (const TSharedPtr<FJsonValue>& NodeValue : Result->GetArrayField(TEXT("nodes")))
        {
            const TSharedPtr<FJsonObject> NodeObject = NodeValue->AsObject();
            if (!NodeObject.IsValid() || NodeObject->GetStringField(TEXT("id")) != NodeId) continue;
            const TArray<TSharedPtr<FJsonValue>>& Pins = NodeObject->GetArrayField(TEXT("pins"));
            return !Pins.IsEmpty() && Pins.ContainsByPredicate([&](const TSharedPtr<FJsonValue>& PinValue)
            {
                const TSharedPtr<FJsonObject> PinObject = PinValue->AsObject();
                return PinObject.IsValid() && PinObject->GetStringField(TEXT("category")) == Category;
            }) && !Pins.ContainsByPredicate([](const TSharedPtr<FJsonValue>& PinValue)
            {
                const TSharedPtr<FJsonObject> PinObject = PinValue->AsObject();
                return PinObject.IsValid() && PinObject->GetStringField(TEXT("category")) == UEdGraphSchema_K2::PC_Wildcard.ToString();
            });
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPGraphPatchLiveTest,
    "UnrealMCP.Blueprint.Authoring.GraphPatch.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPGraphPatchLiveTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_GraphPatch_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("/Game/UnrealMCP_Automation/%s"), *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);
    UBlueprint* Blueprint = CreatePatchFixture(PackagePath);
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
    };
    if (!TestNotNull(TEXT("Patch fixture created"), Blueprint)
        || !TestTrue(TEXT("Patch fixture has EventGraph"), Blueprint->UbergraphPages.Num() > 0)) return false;

    FEdGraphPinType FlagType; FlagType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    TestTrue(TEXT("PatchFlag variable created"),
        FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("PatchFlag"), FlagType));
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    const FString GraphGuid = Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
    const int32 InitialNodeCount = Graph->Nodes.Num();
    const FApplyBlueprintGraphPatchTool Tool;

    UnrealMCP::FMCPRequest DryRun = BuildPatchRequest(ObjectPath, GraphGuid, true);
    const UnrealMCP::FMCPResponse DryResponse = Tool.Execute(DryRun);
    TestFalse(TEXT("Patch dry-run has no error"), DryResponse.Error.IsSet());
    if (TestTrue(TEXT("Patch dry-run returns result"), DryResponse.Result.IsValid()))
    {
        TestEqual(TEXT("Patch dry-run predicts every node"), DryResponse.Result->GetArrayField(TEXT("nodes")).Num(), 4);
        TestFalse(TEXT("Patch dry-run returns revision"), DryResponse.Result->GetStringField(TEXT("initialGraphRevision")).IsEmpty());
    }
    TestEqual(TEXT("Patch dry-run adds no nodes"), Graph->Nodes.Num(), InitialNodeCount);

    UnrealMCP::FMCPRequest Apply = BuildPatchRequest(ObjectPath, GraphGuid, false);
    const UnrealMCP::FMCPResponse ApplyResponse = Tool.Execute(Apply);
    TestFalse(TEXT("Patch apply has no error"), ApplyResponse.Error.IsSet());
    FString AppliedRevision;
    TMap<FString, FString> NodeGuids;
    if (TestTrue(TEXT("Patch apply returns result"), ApplyResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Patch changed graph"), ApplyResponse.Result->GetBoolField(TEXT("changed")));
        TestTrue(TEXT("Patch compiled"), ApplyResponse.Result->GetBoolField(TEXT("compiled")));
        TestTrue(TEXT("Patch compilation succeeded"), ApplyResponse.Result->GetBoolField(TEXT("compileSucceeded")));
        TestFalse(TEXT("Patch remained unsaved"), ApplyResponse.Result->GetBoolField(TEXT("saved")));
        AppliedRevision = ApplyResponse.Result->GetStringField(TEXT("finalGraphRevision"));
        for (const TSharedPtr<FJsonValue>& Value : ApplyResponse.Result->GetArrayField(TEXT("nodes")))
        {
            const TSharedPtr<FJsonObject> Item = Value->AsObject();
            NodeGuids.Add(Item->GetStringField(TEXT("id")), Item->GetStringField(TEXT("nodeGuid")));
            TestFalse(TEXT("Applied node returns complete pins"), Item->GetArrayField(TEXT("pins")).IsEmpty());
        }
    }
    TestEqual(TEXT("Patch created exactly four nodes"), Graph->Nodes.Num(), InitialNodeCount + 4);

    const UnrealMCP::FMCPResponse RetryResponse = Tool.Execute(Apply);
    TestFalse(TEXT("Patch retry has no error"), RetryResponse.Error.IsSet());
    if (TestTrue(TEXT("Patch retry returns result"), RetryResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Patch retry is already complete"), RetryResponse.Result->GetBoolField(TEXT("alreadyComplete")));
        TestFalse(TEXT("Patch retry does not change graph"), RetryResponse.Result->GetBoolField(TEXT("changed")));
    }
    TestEqual(TEXT("Patch retry creates no duplicates"), Graph->Nodes.Num(), InitialNodeCount + 4);

    UnrealMCP::FMCPRequest Replace;
    Replace.Id = TEXT("graph-patch-replace"); Replace.Params = MakeShared<FJsonObject>();
    Replace.Params->SetStringField(TEXT("objectPath"), ObjectPath); Replace.Params->SetStringField(TEXT("graphGuid"), GraphGuid);
    Replace.Params->SetStringField(TEXT("patchId"), TEXT("graph-patch-replace-v1"));
    Replace.Params->SetStringField(TEXT("expectedGraphRevision"), AppliedRevision);
    Replace.Params->SetBoolField(TEXT("compileAfterEdit"), true); Replace.Params->SetBoolField(TEXT("saveAfterEdit"), false);
    TArray<TSharedPtr<FJsonValue>> ExistingNodes;
    for (const FString& Id : {TEXT("firstFlag"), TEXT("secondFlag"), TEXT("invert"), TEXT("branch")})
    {
        TSharedRef<FJsonObject> Existing = Node(Id, TEXT("existingNode"));
        Existing->SetStringField(TEXT("nodeGuid"), NodeGuids.FindChecked(Id));
        ExistingNodes.Add(MakeShared<FJsonValueObject>(Existing));
    }
    Replace.Params->SetArrayField(TEXT("nodes"), ExistingNodes);
    TSharedRef<FJsonObject> Replacement = Connection(TEXT("secondFlag"), TEXT("PatchFlag"), TEXT("branch"), TEXT("Condition"));
    Replacement->SetStringField(TEXT("existingSourceNodeId"), TEXT("firstFlag"));
    Replacement->SetStringField(TEXT("existingSourcePinName"), TEXT("PatchFlag"));
    Replacement->SetBoolField(TEXT("confirmDataReplacement"), true);
    Replace.Params->SetArrayField(TEXT("connections"), {MakeShared<FJsonValueObject>(Replacement)});

    // The live Condition source is invert, so an intentionally wrong exact confirmation must be rejected without mutation.
    const UnrealMCP::FMCPResponse RejectedReplacement = Tool.Execute(Replace);
    TestTrue(TEXT("Wrong exact replacement is rejected"), RejectedReplacement.Error.IsSet());
    TestEqual(TEXT("Rejected replacement preserves node count"), Graph->Nodes.Num(), InitialNodeCount + 4);

    Replacement->SetStringField(TEXT("existingSourceNodeId"), TEXT("invert"));
    Replacement->SetStringField(TEXT("existingSourcePinName"), TEXT("ReturnValue"));
    Replace.Params->SetArrayField(TEXT("connections"), {MakeShared<FJsonValueObject>(Replacement)});
    const UnrealMCP::FMCPResponse ReplacementResponse = Tool.Execute(Replace);
    TestFalse(TEXT("Exact confirmed replacement has no error"), ReplacementResponse.Error.IsSet());
    if (TestTrue(TEXT("Exact confirmed replacement returns result"), ReplacementResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Exact replacement changed graph"), ReplacementResponse.Result->GetBoolField(TEXT("changed")));
        const TArray<TSharedPtr<FJsonValue>>& Results = ReplacementResponse.Result->GetArrayField(TEXT("connections"));
        TestTrue(TEXT("Exact replacement reports replaced link"), Results[0]->AsObject()->GetBoolField(TEXT("replacedExistingDataLink")));
    }

    // A stale revision must also fail before mutation.
    Replace.Params->SetStringField(TEXT("expectedGraphRevision"), TEXT("stale-revision"));
    const UnrealMCP::FMCPResponse StaleResponse = Tool.Execute(Replace);
    TestTrue(TEXT("Stale graph revision is rejected"), StaleResponse.Error.IsSet());
    TestEqual(TEXT("Stale revision preserves node count"), Graph->Nodes.Num(), InitialNodeCount + 4);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPGraphPatchWildcardPreflightTest,
    "UnrealMCP.Blueprint.Authoring.GraphPatch.WildcardPreflight.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPGraphPatchWildcardPreflightTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_GraphPatchWildcard_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("/Game/UnrealMCP_Automation/%s"), *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);
    UBlueprint* Blueprint = CreatePatchFixture(PackagePath);
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
    };
    if (!TestNotNull(TEXT("Wildcard patch fixture created"), Blueprint)
        || !TestTrue(TEXT("Wildcard patch fixture has EventGraph"), Blueprint->UbergraphPages.Num() > 0)) return false;

    FEdGraphPinType FlagType; FlagType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    TestTrue(TEXT("Wildcard fixture Boolean variable created"),
        FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("PatchFlag"), FlagType));
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    const FString GraphGuid = Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
    const int32 InitialNodeCount = Graph->Nodes.Num();
    const FApplyBlueprintGraphPatchTool Tool;

    const UnrealMCP::FMCPResponse DryResponse = Tool.Execute(BuildWildcardPatchRequest(ObjectPath, GraphGuid, true));
    TestFalse(TEXT("Wildcard patch dry-run has no error"), DryResponse.Error.IsSet());
    if (TestTrue(TEXT("Wildcard patch dry-run returns result"), DryResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Wildcard simulation is reported"),
            DryResponse.Result->GetBoolField(TEXT("wildcardSpecializationSimulated")));
        TestTrue(TEXT("At least one connection reports wildcard resolution"),
            DryResponse.Result->GetNumberField(TEXT("wildcardResolvedConnectionCount")) >= 1.0);
        TestTrue(TEXT("First reroute predicts Boolean pins"),
            NodePinsHaveCategory(DryResponse.Result, TEXT("firstKnot"), UEdGraphSchema_K2::PC_Boolean.ToString()));
        TestTrue(TEXT("Second reroute predicts Boolean pins"),
            NodePinsHaveCategory(DryResponse.Result, TEXT("secondKnot"), UEdGraphSchema_K2::PC_Boolean.ToString()));
    }
    TestEqual(TEXT("Wildcard dry-run leaves live graph untouched"), Graph->Nodes.Num(), InitialNodeCount);

    const UnrealMCP::FMCPResponse ApplyResponse = Tool.Execute(BuildWildcardPatchRequest(ObjectPath, GraphGuid, false));
    TestFalse(TEXT("Wildcard patch apply has no error"), ApplyResponse.Error.IsSet());
    if (TestTrue(TEXT("Wildcard patch apply returns result"), ApplyResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Wildcard patch changes graph"), ApplyResponse.Result->GetBoolField(TEXT("changed")));
        TestTrue(TEXT("Wildcard patch compiles"), ApplyResponse.Result->GetBoolField(TEXT("compileSucceeded")));
        TestFalse(TEXT("Wildcard patch remains unsaved"), ApplyResponse.Result->GetBoolField(TEXT("saved")));
    }
    TestEqual(TEXT("Wildcard patch creates four nodes"), Graph->Nodes.Num(), InitialNodeCount + 4);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPGraphPatchCompileRollbackTest,
    "UnrealMCP.Blueprint.Authoring.GraphPatch.CompileRollback.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPGraphPatchCompileRollbackTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_GraphPatchRollback_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("/Game/UnrealMCP_Automation/%s"), *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);
    UBlueprint* Blueprint = CreatePatchFixture(PackagePath);
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
    };
    if (!TestNotNull(TEXT("Rollback fixture created"), Blueprint)
        || !TestTrue(TEXT("Rollback fixture has EventGraph"), Blueprint->UbergraphPages.Num() > 0)) return false;

    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UFunction* ArrayAddFunction = UKismetArrayLibrary::StaticClass()->FindFunctionByName(
        GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Add));
    if (!TestNotNull(TEXT("Array Add function resolved"), ArrayAddFunction)) return false;
    UK2Node_CallArrayFunction* InvalidArrayCall = NewObject<UK2Node_CallArrayFunction>(Graph);
    InvalidArrayCall->SetFromFunction(ArrayAddFunction);
    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement InvalidPlacement;
    InvalidPlacement.X = 0; InvalidPlacement.Y = 600;
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, InvalidArrayCall, InvalidPlacement);
    UK2Node_CustomEvent* InvalidEntry = NewObject<UK2Node_CustomEvent>(Graph);
    InvalidEntry->CustomFunctionName = TEXT("TriggerInvalidArrayCall");
    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement EntryPlacement;
    EntryPlacement.X = -350; EntryPlacement.Y = 600;
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, InvalidEntry, EntryPlacement);
    UEdGraphPin* EntryExec = InvalidEntry->FindPin(UEdGraphSchema_K2::PN_Then);
    UEdGraphPin* ArrayExec = InvalidArrayCall->FindPin(UEdGraphSchema_K2::PN_Execute);
    const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
    if (!TestNotNull(TEXT("Rollback fixture K2 schema resolved"), Schema)
        || !TestNotNull(TEXT("Invalid fixture event exec resolved"), EntryExec)
        || !TestNotNull(TEXT("Invalid Array Add exec resolved"), ArrayExec)
        || !TestTrue(TEXT("Invalid call is reachable from custom event"), Schema->TryCreateConnection(EntryExec, ArrayExec)))
    {
        return false;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    FCompilerResultsLog InvalidLog; InvalidLog.bSilentMode = true;
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &InvalidLog);
    if (!TestTrue(TEXT("Fixture is deterministically compiler-invalid"), InvalidLog.NumErrors > 0)) return false;

    const int32 InitialNodeCount = Graph->Nodes.Num();
    const FString InitialRevision = UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
    const FString GraphGuid = Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
    TestFalse(TEXT("Temporary rollback fixture does not exist on disk"), FPackageName::DoesPackageExist(PackagePath));

    UnrealMCP::FMCPRequest Request;
    Request.Id = TEXT("graph-patch-compile-rollback");
    Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    Request.Params->SetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->SetStringField(TEXT("patchId"), TEXT("graph-patch-compile-rollback-v1"));
    Request.Params->SetStringField(TEXT("expectedGraphRevision"), InitialRevision);
    Request.Params->SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(Node(TEXT("rolledBackBranch"), TEXT("branch")))});
    Request.Params->SetBoolField(TEXT("compileAfterEdit"), true);
    Request.Params->SetBoolField(TEXT("saveAfterEdit"), true);

    const FApplyBlueprintGraphPatchTool Tool;
    const UnrealMCP::FMCPResponse Response = Tool.Execute(Request);
    TestTrue(TEXT("Compile failure returns an MCP error"), Response.Error.IsSet());
    if (Response.Error.IsSet())
    {
        TestTrue(TEXT("Compile failure reports successful immediate rollback"),
            Response.Error.GetValue().Message.Contains(TEXT("rolled back=true")));
    }
    TestEqual(TEXT("Rollback restores exact node count"), Graph->Nodes.Num(), InitialNodeCount);
    TestEqual(TEXT("Rollback restores exact graph revision"),
        UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph), InitialRevision);
    TestFalse(TEXT("Compile failure never saves the temporary asset"), FPackageName::DoesPackageExist(PackagePath));
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
