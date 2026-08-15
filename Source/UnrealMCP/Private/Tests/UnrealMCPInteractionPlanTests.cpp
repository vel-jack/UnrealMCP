#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/ApplyBlueprintInteractionPlanTool.h"

namespace
{
    constexpr TCHAR TestRoot[] = TEXT("/Game/UnrealMCP_Automation");
    constexpr TCHAR ComponentName[] = TEXT("InteractionComponent");
    constexpr TCHAR EventName[] = TEXT("RunInteractionPlan");
    constexpr TCHAR WorkflowId[] = TEXT("Automation.InteractionPlan");

    UBlueprint* CreateActorBlueprint(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            Package,
            *FPackageName::GetLongPackageAssetName(PackagePath),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("UnrealMCPAutomation")));
    }

    UEdGraph* FindEventGraph(const UBlueprint* Blueprint)
    {
        if (Blueprint == nullptr)
        {
            return nullptr;
        }
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (Graph != nullptr && Graph->GetName() == UEdGraphSchema_K2::GN_EventGraph.ToString())
            {
                return Graph;
            }
        }
        return nullptr;
    }

    template <typename NodeType>
    int32 CountNodes(const UEdGraph* Graph)
    {
        int32 Count = 0;
        if (Graph != nullptr)
        {
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                Count += Node != nullptr && Node->IsA<NodeType>() ? 1 : 0;
            }
        }
        return Count;
    }

    template <typename NodeType>
    NodeType* FindWorkflowNode(UEdGraph* Graph, const FString& NodeId, const FString& InWorkflowId = WorkflowId)
    {
        const FString Marker = FString::Printf(TEXT("UnrealMCP.Workflow:%s:%s"), *InWorkflowId, *NodeId);
        if (Graph != nullptr)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (NodeType* TypedNode = Cast<NodeType>(Node);
                    TypedNode != nullptr && TypedNode->NodeComment == Marker)
                {
                    return TypedNode;
                }
            }
        }
        return nullptr;
    }

    bool PinsAreLinked(
        const UEdGraphNode* Source,
        const FName SourcePinName,
        const UEdGraphNode* Target,
        const FName TargetPinName)
    {
        const UEdGraphPin* SourcePin = Source != nullptr
            ? Source->FindPin(SourcePinName, EGPD_Output)
            : nullptr;
        const UEdGraphPin* TargetPin = Target != nullptr
            ? Target->FindPin(TargetPinName, EGPD_Input)
            : nullptr;
        return SourcePin != nullptr && TargetPin != nullptr && SourcePin->LinkedTo.Contains(TargetPin);
    }

    TSharedPtr<FJsonValue> MakeNode(
        const FString& Id,
        const FString& Type,
        const FString& DetailField = FString(),
        const FString& DetailValue = FString())
    {
        TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("id"), Id);
        Node->SetStringField(TEXT("type"), Type);
        if (!DetailField.IsEmpty())
        {
            Node->SetStringField(DetailField, DetailValue);
        }
        return MakeShared<FJsonValueObject>(Node);
    }

    TSharedPtr<FJsonValue> MakeFunctionNode()
    {
        TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("id"), TEXT("activate"));
        Node->SetStringField(TEXT("type"), TEXT("functionCall"));
        Node->SetStringField(TEXT("ownerClassPath"), TEXT("/Script/Engine.ActorComponent"));
        Node->SetStringField(TEXT("functionName"), TEXT("Activate"));
        return MakeShared<FJsonValueObject>(Node);
    }

    TSharedPtr<FJsonValue> MakeFunctionNode(
        const FString& Id,
        const FString& OwnerClassPath,
        const FString& FunctionName,
        const TArray<TSharedPtr<FJsonValue>>& InputDefaults = {})
    {
        TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("id"), Id);
        Node->SetStringField(TEXT("type"), TEXT("functionCall"));
        Node->SetStringField(TEXT("ownerClassPath"), OwnerClassPath);
        Node->SetStringField(TEXT("functionName"), FunctionName);
        if (!InputDefaults.IsEmpty()) Node->SetArrayField(TEXT("inputDefaults"), InputDefaults);
        return MakeShared<FJsonValueObject>(Node);
    }

    TSharedPtr<FJsonValue> MakeInputDefault(const FString& PinName, const FString& FieldName, const FString& Value)
    {
        TSharedRef<FJsonObject> Default = MakeShared<FJsonObject>();
        Default->SetStringField(TEXT("pinName"), PinName);
        Default->SetStringField(FieldName, Value);
        return MakeShared<FJsonValueObject>(Default);
    }

    TSharedPtr<FJsonValue> MakeConnection(
        const FString& SourceId,
        const FString& SourcePin,
        const FString& TargetId,
        const FString& TargetPin)
    {
        TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
        Connection->SetStringField(TEXT("sourceNodeId"), SourceId);
        Connection->SetStringField(TEXT("sourcePinName"), SourcePin);
        Connection->SetStringField(TEXT("targetNodeId"), TargetId);
        Connection->SetStringField(TEXT("targetPinName"), TargetPin);
        return MakeShared<FJsonValueObject>(Connection);
    }

    UnrealMCP::FMCPRequest MakePlanRequest(const FString& ObjectPath, bool bDryRun)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = bDryRun ? TEXT("interaction-plan-dry-run") : TEXT("interaction-plan-apply");
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->SetStringField(TEXT("graphName"), UEdGraphSchema_K2::GN_EventGraph.ToString());
        Request.Params->SetStringField(TEXT("workflowId"), WorkflowId);
        Request.Params->SetArrayField(TEXT("nodes"), {
            MakeNode(TEXT("entry"), TEXT("customEvent"), TEXT("eventName"), EventName),
            MakeNode(TEXT("target"), TEXT("variableGet"), TEXT("variableName"), ComponentName),
            MakeFunctionNode(),
            MakeNode(TEXT("branch"), TEXT("branch"))});
        Request.Params->SetArrayField(TEXT("connections"), {
            MakeConnection(TEXT("entry"), TEXT("then"), TEXT("activate"), TEXT("execute")),
            MakeConnection(TEXT("target"), ComponentName, TEXT("activate"), TEXT("self")),
            MakeConnection(TEXT("activate"), TEXT("then"), TEXT("branch"), TEXT("execute"))});
        Request.Params->SetBoolField(TEXT("dryRun"), bDryRun);
        Request.Params->SetBoolField(TEXT("compileAfterEdit"), true);
        Request.Params->SetBoolField(TEXT("saveAfterEdit"), false);
        return Request;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPInteractionPlanLiveTest,
    "UnrealMCP.Blueprint.Authoring.InteractionPlan.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPInteractionPlanLiveTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetName = FString::Printf(TEXT("BP_InteractionPlan_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), TestRoot, *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    UBlueprint* Blueprint = nullptr;
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr)
        {
            TArray<UObject*> AssetsToDelete{Blueprint};
            ObjectTools::DeleteObjectsUnchecked(AssetsToDelete);
        }
        if (UPackage* Package = FindPackage(nullptr, *PackagePath))
        {
            Package->SetDirtyFlag(false);
        }
    };

    Blueprint = CreateActorBlueprint(PackagePath);
    if (!TestNotNull(TEXT("Temporary Actor Blueprint created"), Blueprint)
        || !TestNotNull(TEXT("Blueprint has an SCS"), Blueprint->SimpleConstructionScript.Get()))
    {
        return false;
    }

    USCS_Node* ComponentNode = Blueprint->SimpleConstructionScript->CreateNode(
        USceneComponent::StaticClass(), FName(ComponentName));
    if (!TestNotNull(TEXT("Concrete SceneComponent SCS member created"), ComponentNode))
    {
        return false;
    }
    Blueprint->SimpleConstructionScript->AddNode(ComponentNode);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    if (!TestTrue(
            TEXT("Fixture Blueprint compiled"),
            Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings))
    {
        return false;
    }

    UEdGraph* EventGraph = FindEventGraph(Blueprint);
    if (!TestNotNull(TEXT("EventGraph exists"), EventGraph))
    {
        return false;
    }
    const int32 InitialEventCount = CountNodes<UK2Node_CustomEvent>(EventGraph);
    const int32 InitialGetCount = CountNodes<UK2Node_VariableGet>(EventGraph);
    const int32 InitialCallCount = CountNodes<UK2Node_CallFunction>(EventGraph);
    const int32 InitialBranchCount = CountNodes<UK2Node_IfThenElse>(EventGraph);

    const FApplyBlueprintInteractionPlanTool Tool;
    UnrealMCP::FMCPRequest Request = MakePlanRequest(ObjectPath, true);
    const UnrealMCP::FMCPResponse DryRunResponse = Tool.Execute(Request);
    TestFalse(TEXT("Dry-run response has no error"), DryRunResponse.Error.IsSet());
    TestEqual(TEXT("Dry run adds no custom event"), CountNodes<UK2Node_CustomEvent>(EventGraph), InitialEventCount);
    TestEqual(TEXT("Dry run adds no variable get"), CountNodes<UK2Node_VariableGet>(EventGraph), InitialGetCount);
    TestEqual(TEXT("Dry run adds no function call"), CountNodes<UK2Node_CallFunction>(EventGraph), InitialCallCount);
    TestEqual(TEXT("Dry run adds no branch"), CountNodes<UK2Node_IfThenElse>(EventGraph), InitialBranchCount);

    Request = MakePlanRequest(ObjectPath, false);
    const UnrealMCP::FMCPResponse ApplyResponse = Tool.Execute(Request);
    TestFalse(TEXT("Apply response has no error"), ApplyResponse.Error.IsSet());
    if (!TestTrue(TEXT("Apply response has a result"), ApplyResponse.Result.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("Apply changed the graph"), ApplyResponse.Result->GetBoolField(TEXT("changed")));
    TestEqual(TEXT("Apply created four nodes"), static_cast<int32>(ApplyResponse.Result->GetNumberField(TEXT("createdNodeCount"))), 4);
    TestEqual(TEXT("Apply created three links"), static_cast<int32>(ApplyResponse.Result->GetNumberField(TEXT("connectedPinCount"))), 3);
    TestTrue(TEXT("Apply compiled the Blueprint"), ApplyResponse.Result->GetBoolField(TEXT("compiled")));
    TestTrue(TEXT("Blueprint compile succeeded"), ApplyResponse.Result->GetBoolField(TEXT("compileSucceeded")));
    TestFalse(TEXT("Apply did not save the Blueprint"), ApplyResponse.Result->GetBoolField(TEXT("saved")));

    UK2Node_CustomEvent* Event = FindWorkflowNode<UK2Node_CustomEvent>(EventGraph, TEXT("entry"));
    UK2Node_VariableGet* Target = FindWorkflowNode<UK2Node_VariableGet>(EventGraph, TEXT("target"));
    UK2Node_CallFunction* Activate = FindWorkflowNode<UK2Node_CallFunction>(EventGraph, TEXT("activate"));
    UK2Node_IfThenElse* Branch = FindWorkflowNode<UK2Node_IfThenElse>(EventGraph, TEXT("branch"));
    TestNotNull(TEXT("Custom event exists"), Event);
    TestNotNull(TEXT("Component variable get exists"), Target);
    TestNotNull(TEXT("Activate call exists"), Activate);
    TestNotNull(TEXT("Branch exists"), Branch);
    TestTrue(TEXT("Event then is linked to Activate execute"),
        PinsAreLinked(Event, UEdGraphSchema_K2::PN_Then, Activate, UEdGraphSchema_K2::PN_Execute));
    TestTrue(TEXT("Component output is linked to Activate self"),
        PinsAreLinked(Target, FName(ComponentName), Activate, UEdGraphSchema_K2::PN_Self));
    TestTrue(TEXT("Activate then is linked to Branch execute"),
        PinsAreLinked(Activate, UEdGraphSchema_K2::PN_Then, Branch, UEdGraphSchema_K2::PN_Execute));
    TestTrue(
        TEXT("Blueprint is compiled after apply"),
        Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

    TSharedRef<FJsonObject> ExistingNode = MakeShared<FJsonObject>();
    ExistingNode->SetStringField(TEXT("id"), TEXT("existingBranch"));
    ExistingNode->SetStringField(TEXT("type"), TEXT("existingNode"));
    ExistingNode->SetStringField(TEXT("nodeGuid"), Branch->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
    UnrealMCP::FMCPRequest ExistingRequest;
    ExistingRequest.Id = TEXT("interaction-plan-existing-node");
    ExistingRequest.Params = MakeShared<FJsonObject>();
    ExistingRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    ExistingRequest.Params->SetStringField(TEXT("graphName"), UEdGraphSchema_K2::GN_EventGraph.ToString());
    ExistingRequest.Params->SetStringField(TEXT("workflowId"), TEXT("ExistingNodeAnchorTest"));
    ExistingRequest.Params->SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(ExistingNode)});
    ExistingRequest.Params->SetArrayField(TEXT("connections"), {});
    ExistingRequest.Params->SetBoolField(TEXT("dryRun"), true);
    ExistingRequest.Params->SetBoolField(TEXT("saveAfterEdit"), false);
    const UnrealMCP::FMCPResponse ExistingResponse = Tool.Execute(ExistingRequest);
    TestFalse(TEXT("Exact existing-node anchor resolves without error"), ExistingResponse.Error.IsSet());
    if (TestTrue(TEXT("Existing-node response has a result"), ExistingResponse.Result.IsValid()))
    {
        TestFalse(TEXT("Existing-node dry run changes nothing"), ExistingResponse.Result->GetBoolField(TEXT("changed")));
        TestEqual(TEXT("Existing-node dry run creates no nodes"), static_cast<int32>(ExistingResponse.Result->GetNumberField(TEXT("createdNodeCount"))), 0);
    }

    const int32 AppliedNodeCount = EventGraph->Nodes.Num();
    Request.Id = TEXT("interaction-plan-idempotent");
    const UnrealMCP::FMCPResponse RetryResponse = Tool.Execute(Request);
    TestFalse(TEXT("Retry response has no error"), RetryResponse.Error.IsSet());
    if (TestTrue(TEXT("Retry response has a result"), RetryResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Retry reports alreadyComplete"), RetryResponse.Result->GetBoolField(TEXT("alreadyComplete")));
        TestFalse(TEXT("Retry reports no change"), RetryResponse.Result->GetBoolField(TEXT("changed")));
        TestEqual(TEXT("Retry creates no nodes"), static_cast<int32>(RetryResponse.Result->GetNumberField(TEXT("createdNodeCount"))), 0);
        TestEqual(TEXT("Retry creates no links"), static_cast<int32>(RetryResponse.Result->GetNumberField(TEXT("connectedPinCount"))), 0);
    }
    TestEqual(TEXT("Retry does not duplicate graph nodes"), EventGraph->Nodes.Num(), AppliedNodeCount);
    TestEqual(TEXT("Exactly one workflow custom event exists"), CountNodes<UK2Node_CustomEvent>(EventGraph), InitialEventCount + 1);
    TestEqual(TEXT("Exactly one workflow variable get exists"), CountNodes<UK2Node_VariableGet>(EventGraph), InitialGetCount + 1);
    TestEqual(TEXT("Exactly one workflow function call exists"), CountNodes<UK2Node_CallFunction>(EventGraph), InitialCallCount + 1);
    TestEqual(TEXT("Exactly one workflow branch exists"), CountNodes<UK2Node_IfThenElse>(EventGraph), InitialBranchCount + 1);

    TSharedRef<FJsonObject> ExistingActivate = MakeShared<FJsonObject>();
    ExistingActivate->SetStringField(TEXT("id"), TEXT("existingActivate"));
    ExistingActivate->SetStringField(TEXT("type"), TEXT("existingNode"));
    ExistingActivate->SetStringField(TEXT("nodeGuid"), Activate->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
    TSharedRef<FJsonObject> Sequence = MakeShared<FJsonObject>();
    Sequence->SetStringField(TEXT("id"), TEXT("preserveLegacyRoute"));
    Sequence->SetStringField(TEXT("type"), TEXT("sequence"));
    Sequence->SetStringField(TEXT("spliceAfterNodeId"), TEXT("existingActivate"));
    Sequence->SetStringField(TEXT("spliceAfterPinName"), UEdGraphSchema_K2::PN_Then.ToString());
    UnrealMCP::FMCPRequest SpliceRequest;
    SpliceRequest.Id = TEXT("interaction-plan-sequence-splice");
    SpliceRequest.Params = MakeShared<FJsonObject>();
    SpliceRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    SpliceRequest.Params->SetStringField(TEXT("graphName"), UEdGraphSchema_K2::GN_EventGraph.ToString());
    SpliceRequest.Params->SetStringField(TEXT("workflowId"), WorkflowId);
    SpliceRequest.Params->SetArrayField(TEXT("nodes"), {
        MakeShared<FJsonValueObject>(ExistingActivate),
        MakeShared<FJsonValueObject>(Sequence),
        MakeNode(TEXT("newRoute"), TEXT("branch"))});
    SpliceRequest.Params->SetArrayField(TEXT("connections"), {
        MakeConnection(TEXT("preserveLegacyRoute"), TEXT("Then_1"), TEXT("newRoute"), TEXT("execute"))});
    SpliceRequest.Params->SetBoolField(TEXT("compileAfterEdit"), true);
    SpliceRequest.Params->SetBoolField(TEXT("saveAfterEdit"), false);
    const UnrealMCP::FMCPResponse SpliceResponse = Tool.Execute(SpliceRequest);
    TestFalse(TEXT("Sequence splice response has no error"), SpliceResponse.Error.IsSet());
    UK2Node_ExecutionSequence* SequenceNode = FindWorkflowNode<UK2Node_ExecutionSequence>(EventGraph, TEXT("preserveLegacyRoute"));
    UK2Node_IfThenElse* NewRoute = FindWorkflowNode<UK2Node_IfThenElse>(EventGraph, TEXT("newRoute"));
    TestNotNull(TEXT("Sequence splice node exists"), SequenceNode);
    TestNotNull(TEXT("New execution route exists"), NewRoute);
    TestTrue(TEXT("Source now enters Sequence"), PinsAreLinked(Activate, UEdGraphSchema_K2::PN_Then, SequenceNode, UEdGraphSchema_K2::PN_Execute));
    TestTrue(TEXT("Sequence Then_0 preserves the legacy route"), PinsAreLinked(SequenceNode, TEXT("Then_0"), Branch, UEdGraphSchema_K2::PN_Execute));
    TestTrue(TEXT("Sequence Then_1 enters the new route"), PinsAreLinked(SequenceNode, TEXT("Then_1"), NewRoute, UEdGraphSchema_K2::PN_Execute));
    SpliceRequest.Id = TEXT("interaction-plan-sequence-splice-retry");
    const UnrealMCP::FMCPResponse SpliceRetryResponse = Tool.Execute(SpliceRequest);
    TestFalse(TEXT("Sequence splice retry has no error"), SpliceRetryResponse.Error.IsSet());
    if (TestTrue(TEXT("Sequence splice retry has a result"), SpliceRetryResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Sequence splice retry is already complete"), SpliceRetryResponse.Result->GetBoolField(TEXT("alreadyComplete")));
    }

    UnrealMCP::FMCPRequest DefaultRequest;
    DefaultRequest.Id = TEXT("interaction-plan-input-defaults");
    DefaultRequest.Params = MakeShared<FJsonObject>();
    DefaultRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    DefaultRequest.Params->SetStringField(TEXT("graphName"), UEdGraphSchema_K2::GN_EventGraph.ToString());
    DefaultRequest.Params->SetStringField(TEXT("workflowId"), TEXT("Automation.InputDefaults"));
    DefaultRequest.Params->SetArrayField(TEXT("nodes"), {
        MakeNode(TEXT("defaultsEntry"), TEXT("customEvent"), TEXT("eventName"), TEXT("RunInputDefaults")),
        MakeFunctionNode(TEXT("sceneLookup"), TEXT("/Script/Engine.Actor"), TEXT("GetComponentByClass"), {
            MakeInputDefault(TEXT("ComponentClass"), TEXT("defaultObjectPath"), TEXT("/Script/Engine.SceneComponent"))}),
        MakeFunctionNode(TEXT("setVisibility"), TEXT("/Script/Engine.SceneComponent"), TEXT("SetVisibility"), {
            MakeInputDefault(TEXT("bNewVisibility"), TEXT("defaultValue"), TEXT("false"))})});
    DefaultRequest.Params->SetArrayField(TEXT("connections"), {
        MakeConnection(TEXT("defaultsEntry"), TEXT("then"), TEXT("setVisibility"), TEXT("execute")),
        MakeConnection(TEXT("sceneLookup"), TEXT("ReturnValue"), TEXT("setVisibility"), TEXT("self"))});
    DefaultRequest.Params->SetBoolField(TEXT("compileAfterEdit"), true);
    DefaultRequest.Params->SetBoolField(TEXT("saveAfterEdit"), false);
    const UnrealMCP::FMCPResponse DefaultResponse = Tool.Execute(DefaultRequest);
    TestFalse(TEXT("Typed object and literal defaults apply without error"), DefaultResponse.Error.IsSet());
    DefaultRequest.Id = TEXT("interaction-plan-input-defaults-retry");
    const UnrealMCP::FMCPResponse DefaultRetryResponse = Tool.Execute(DefaultRequest);
    TestFalse(TEXT("Input-default retry has no error"), DefaultRetryResponse.Error.IsSet());
    if (TestTrue(TEXT("Input-default retry has a result"), DefaultRetryResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Input-default retry is already complete"), DefaultRetryResponse.Result->GetBoolField(TEXT("alreadyComplete")));
    }

    UK2Node_CustomEvent* DefaultsEntry = FindWorkflowNode<UK2Node_CustomEvent>(EventGraph, TEXT("defaultsEntry"), TEXT("Automation.InputDefaults"));
    UK2Node_CallFunction* SetVisibility = FindWorkflowNode<UK2Node_CallFunction>(EventGraph, TEXT("setVisibility"), TEXT("Automation.InputDefaults"));
    TSharedRef<FJsonObject> ExistingDefaultsEntry = MakeShared<FJsonObject>(); ExistingDefaultsEntry->SetStringField(TEXT("id"), TEXT("source")); ExistingDefaultsEntry->SetStringField(TEXT("type"), TEXT("existingNode")); ExistingDefaultsEntry->SetStringField(TEXT("nodeGuid"), DefaultsEntry->NodeGuid.ToString());
    TSharedRef<FJsonObject> ExistingSetVisibility = MakeShared<FJsonObject>(); ExistingSetVisibility->SetStringField(TEXT("id"), TEXT("target")); ExistingSetVisibility->SetStringField(TEXT("type"), TEXT("existingNode")); ExistingSetVisibility->SetStringField(TEXT("nodeGuid"), SetVisibility->NodeGuid.ToString());
    UnrealMCP::FMCPRequest InsertionRequest; InsertionRequest.Id = TEXT("interaction-plan-exec-insertion"); InsertionRequest.Params = MakeShared<FJsonObject>();
    InsertionRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath); InsertionRequest.Params->SetStringField(TEXT("graphName"), UEdGraphSchema_K2::GN_EventGraph.ToString()); InsertionRequest.Params->SetStringField(TEXT("workflowId"), TEXT("Automation.ExecutionInsertion"));
    InsertionRequest.Params->SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(ExistingDefaultsEntry), MakeFunctionNode(TEXT("inserted"), TEXT("/Script/Engine.Actor"), TEXT("SetActorTickEnabled")), MakeShared<FJsonValueObject>(ExistingSetVisibility)});
    TSharedRef<FJsonObject> Insertion = MakeShared<FJsonObject>(); Insertion->SetStringField(TEXT("sourceNodeId"), TEXT("source")); Insertion->SetStringField(TEXT("sourcePinName"), TEXT("then")); Insertion->SetStringField(TEXT("insertedNodeId"), TEXT("inserted")); Insertion->SetStringField(TEXT("insertedInputPinName"), TEXT("execute")); Insertion->SetStringField(TEXT("insertedOutputPinName"), TEXT("then")); Insertion->SetStringField(TEXT("targetNodeId"), TEXT("target")); Insertion->SetStringField(TEXT("targetPinName"), TEXT("execute"));
    InsertionRequest.Params->SetArrayField(TEXT("executionInsertions"), {MakeShared<FJsonValueObject>(Insertion)}); InsertionRequest.Params->SetBoolField(TEXT("saveAfterEdit"), false);
    const UnrealMCP::FMCPResponse InsertionResponse = Tool.Execute(InsertionRequest); TestFalse(TEXT("Execution insertion has no error"), InsertionResponse.Error.IsSet());
    UK2Node_CallFunction* Inserted = FindWorkflowNode<UK2Node_CallFunction>(EventGraph, TEXT("inserted"), TEXT("Automation.ExecutionInsertion"));
    TestTrue(TEXT("Execution insertion connects source to inserted"), PinsAreLinked(DefaultsEntry, TEXT("then"), Inserted, TEXT("execute")));
    TestTrue(TEXT("Execution insertion connects inserted to target"), PinsAreLinked(Inserted, TEXT("then"), SetVisibility, TEXT("execute")));
    InsertionRequest.Id = TEXT("interaction-plan-exec-insertion-retry"); const UnrealMCP::FMCPResponse InsertionRetry = Tool.Execute(InsertionRequest); TestFalse(TEXT("Execution insertion retry has no error"), InsertionRetry.Error.IsSet());

    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
