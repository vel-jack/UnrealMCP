#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/WireBlueprintEventToFunctionTool.h"

namespace
{
    constexpr TCHAR TestRoot[] = TEXT("/Game/UnrealMCP_Automation");
    constexpr TCHAR EventName[] = TEXT("ActivateInteraction");
    constexpr TCHAR ComponentName[] = TEXT("InteractionComponent");
    constexpr TCHAR FunctionName[] = TEXT("Activate");
    constexpr TCHAR OwnerClassPath[] = TEXT("/Script/Engine.ActorComponent");

    UBlueprint* CreateActorBlueprint(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        const FName AssetName(*FPackageName::GetLongPackageAssetName(PackagePath));
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            Package,
            AssetName,
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

    UK2Node_CustomEvent* FindCustomEvent(UEdGraph* Graph)
    {
        if (Graph == nullptr)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
            if (Event != nullptr && Event->CustomFunctionName == FName(EventName))
            {
                return Event;
            }
        }
        return nullptr;
    }

    UK2Node_CallFunction* FindActivateCall(UEdGraph* Graph)
    {
        if (Graph == nullptr)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            const UFunction* Function = Call != nullptr ? Call->GetTargetFunction() : nullptr;
            if (Function != nullptr
                && Function->GetFName() == FName(FunctionName)
                && Function->GetOwnerClass() == UActorComponent::StaticClass())
            {
                return Call;
            }
        }
        return nullptr;
    }

    UK2Node_VariableGet* FindComponentGet(UEdGraph* Graph)
    {
        if (Graph == nullptr)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
            if (Get != nullptr && Get->VariableReference.GetMemberName() == FName(ComponentName))
            {
                return Get;
            }
        }
        return nullptr;
    }

    bool PinsAreLinked(const UEdGraphNode* Source, const FName SourcePinName,
        const UEdGraphNode* Target, const FName TargetPinName)
    {
        const UEdGraphPin* SourcePin = Source != nullptr
            ? Source->FindPin(SourcePinName, EGPD_Output)
            : nullptr;
        const UEdGraphPin* TargetPin = Target != nullptr
            ? Target->FindPin(TargetPinName, EGPD_Input)
            : nullptr;
        return SourcePin != nullptr && TargetPin != nullptr
            && SourcePin->LinkedTo.Contains(TargetPin);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPWireBlueprintEventToFunctionLiveTest,
    "UnrealMCP.Blueprint.Authoring.WireEventToFunction.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPWireBlueprintEventToFunctionLiveTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetName = FString::Printf(TEXT("BP_WireEvent_%s"), *Suffix);
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
    if (!TestNotNull(TEXT("ActorComponent SCS node created"), ComponentNode))
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
    const int32 InitialCallCount = CountNodes<UK2Node_CallFunction>(EventGraph);
    const int32 InitialGetCount = CountNodes<UK2Node_VariableGet>(EventGraph);

    UnrealMCP::FMCPRequest Request;
    Request.Id = TEXT("wire-event-dry-run");
    Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    Request.Params->SetStringField(TEXT("graphName"), UEdGraphSchema_K2::GN_EventGraph.ToString());
    Request.Params->SetStringField(TEXT("eventName"), EventName);
    Request.Params->SetStringField(TEXT("ownerClassPath"), OwnerClassPath);
    Request.Params->SetStringField(TEXT("functionName"), FunctionName);
    Request.Params->SetStringField(TEXT("targetVariableName"), ComponentName);
    Request.Params->SetBoolField(TEXT("saveAfterEdit"), false);
    Request.Params->SetBoolField(TEXT("compileAfterEdit"), true);
    Request.Params->SetBoolField(TEXT("dryRun"), true);

    const FWireBlueprintEventToFunctionTool Tool;
    const UnrealMCP::FMCPResponse DryRunResponse = Tool.Execute(Request);
    TestFalse(TEXT("Dry-run response has no error"), DryRunResponse.Error.IsSet());
    TestEqual(TEXT("Dry run adds no custom event"), CountNodes<UK2Node_CustomEvent>(EventGraph), InitialEventCount);
    TestEqual(TEXT("Dry run adds no function call"), CountNodes<UK2Node_CallFunction>(EventGraph), InitialCallCount);
    TestEqual(TEXT("Dry run adds no variable get"), CountNodes<UK2Node_VariableGet>(EventGraph), InitialGetCount);

    Request.Id = TEXT("wire-event-apply");
    Request.Params->SetBoolField(TEXT("dryRun"), false);
    const UnrealMCP::FMCPResponse ApplyResponse = Tool.Execute(Request);
    TestFalse(TEXT("Apply response has no error"), ApplyResponse.Error.IsSet());
    if (!TestTrue(TEXT("Apply response has a result"), ApplyResponse.Result.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("Apply changed the graph"), ApplyResponse.Result->GetBoolField(TEXT("changed")));
    TestTrue(TEXT("Apply compiled the Blueprint"), ApplyResponse.Result->GetBoolField(TEXT("compileSucceeded")));
    TestFalse(TEXT("Apply did not save the Blueprint"), ApplyResponse.Result->GetBoolField(TEXT("saved")));

    UK2Node_CustomEvent* Event = FindCustomEvent(EventGraph);
    UK2Node_CallFunction* Call = FindActivateCall(EventGraph);
    UK2Node_VariableGet* Get = FindComponentGet(EventGraph);
    TestNotNull(TEXT("Custom event was created"), Event);
    TestNotNull(TEXT("Activate call was created"), Call);
    TestNotNull(TEXT("Component variable get was created"), Get);
    TestTrue(TEXT("Event execution is wired to Activate"),
        PinsAreLinked(Event, UEdGraphSchema_K2::PN_Then, Call, UEdGraphSchema_K2::PN_Execute));
    TestTrue(TEXT("Component value is wired to the call target"),
        PinsAreLinked(Get, FName(ComponentName), Call, UEdGraphSchema_K2::PN_Self));

    const int32 AppliedEventCount = CountNodes<UK2Node_CustomEvent>(EventGraph);
    const int32 AppliedCallCount = CountNodes<UK2Node_CallFunction>(EventGraph);
    const int32 AppliedGetCount = CountNodes<UK2Node_VariableGet>(EventGraph);
    Request.Id = TEXT("wire-event-idempotent");
    const UnrealMCP::FMCPResponse RetryResponse = Tool.Execute(Request);
    TestFalse(TEXT("Retry response has no error"), RetryResponse.Error.IsSet());
    if (TestTrue(TEXT("Retry response has a result"), RetryResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Retry reports alreadyComplete"), RetryResponse.Result->GetBoolField(TEXT("alreadyComplete")));
        TestFalse(TEXT("Retry reports no change"), RetryResponse.Result->GetBoolField(TEXT("changed")));
    }
    TestEqual(TEXT("Retry does not duplicate the custom event"), CountNodes<UK2Node_CustomEvent>(EventGraph), AppliedEventCount);
    TestEqual(TEXT("Retry does not duplicate the function call"), CountNodes<UK2Node_CallFunction>(EventGraph), AppliedCallCount);
    TestEqual(TEXT("Retry does not duplicate the variable get"), CountNodes<UK2Node_VariableGet>(EventGraph), AppliedGetCount);
    TestTrue(
        TEXT("Blueprint remains compiled after idempotent retry"),
        Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
