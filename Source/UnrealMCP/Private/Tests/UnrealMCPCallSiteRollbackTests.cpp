#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/RefreshBlueprintCallSitesEngine.h"

namespace
{
    UBlueprint* CreateRollbackFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package, *FPackageName::GetLongPackageAssetName(PackagePath),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("UnrealMCPAutomation")));
    }

    FEdGraphPinType MakeIntPinType()
    {
        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
        return PinType;
    }

    UK2Node_FunctionEntry* FindEntry(UEdGraph* Graph)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                return Entry;
            }
        }
        return nullptr;
    }

    template <typename TNode>
    TNode* PlaceNode(UEdGraph* Graph)
    {
        TNode* Node = NewObject<TNode>(Graph);
        Graph->AddNode(Node, false, false);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        return Node;
    }

    void DiscardFixture(UBlueprint* Blueprint)
    {
        if (Blueprint == nullptr)
        {
            return;
        }
        UPackage* OriginalPackage = Blueprint->GetPackage();
        FAssetRegistryModule::AssetDeleted(Blueprint);
        Blueprint->ClearFlags(RF_Standalone | RF_Public);
        const FName TransientName = MakeUniqueObjectName(GetTransientPackage(), Blueprint->GetClass(), Blueprint->GetFName());
        Blueprint->Rename(*TransientName.ToString(), GetTransientPackage(),
            REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty | REN_ForceNoResetLoaders);
        if (OriginalPackage != nullptr)
        {
            OriginalPackage->SetDirtyFlag(false);
        }
    }
}

// Exercises the compile-failure rollback branch with a real failure rather than an injected one:
// removing a function parameter leaves the caller's still-linked pin orphaned after reconstruction,
// which is exactly the situation the rollback exists for. Driving the engine directly is deliberate;
// the tool discovers call sites through the SQLite index, and that index never sees the transient
// packages automation fixtures create.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPCallSiteRollbackTest,
    "UnrealMCP.Blueprint.Authoring.CallSiteRollback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealMCPCallSiteRollbackTest::RunTest(const FString& Parameters)
{
    // The whole point of this fixture is to make the caller fail to compile after reconstruction, so
    // the Blueprint compiler's complaint about the orphaned pin is the expected result, not a defect.
    AddExpectedError(TEXT("In use pin .* no longer exists on node"), EAutomationExpectedErrorFlags::Contains, 0);

    const FString OwnerPath = TEXT("/Game/UnrealMCPAutomation/CallSiteRollbackOwner");
    const FString CallerPath = TEXT("/Game/UnrealMCPAutomation/CallSiteRollbackCaller");

    UBlueprint* Owner = CreateRollbackFixture(OwnerPath);
    UBlueprint* Caller = CreateRollbackFixture(CallerPath);
    ON_SCOPE_EXIT
    {
        DiscardFixture(Owner);
        DiscardFixture(Caller);
    };

    if (!TestNotNull(TEXT("owner fixture"), Owner) || !TestNotNull(TEXT("caller fixture"), Caller))
    {
        return false;
    }
    FAssetRegistryModule::AssetCreated(Owner);
    FAssetRegistryModule::AssetCreated(Caller);

    // Owner: function Foo with one int input.
    UEdGraph* FooGraph = FBlueprintEditorUtils::CreateNewGraph(
        Owner, TEXT("Foo"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Owner, FooGraph, true, nullptr);
    UK2Node_FunctionEntry* Entry = FindEntry(FooGraph);
    if (!TestNotNull(TEXT("Foo entry node"), Entry))
    {
        return false;
    }
    Entry->CreateUserDefinedPin(TEXT("Value"), MakeIntPinType(), EGPD_Output, false);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Owner);
    FKismetEditorUtilities::CompileBlueprint(Owner, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);

    UFunction* Foo = Owner->GeneratedClass != nullptr ? Owner->GeneratedClass->FindFunctionByName(TEXT("Foo")) : nullptr;
    if (!TestNotNull(TEXT("compiled Foo function"), Foo))
    {
        return false;
    }

    // Caller: custom event -> Foo, with a variable feeding the Value pin so the pin carries a real link.
    // Foo is an instance function on the owner, so the call also needs a Target; an owner-typed
    // variable keeps this a genuine cross-Blueprint call rather than a self-call.
    FBlueprintEditorUtils::AddMemberVariable(Caller, TEXT("Feed"), MakeIntPinType());
    FEdGraphPinType OwnerRefType;
    OwnerRefType.PinCategory = UEdGraphSchema_K2::PC_Object;
    OwnerRefType.PinSubCategoryObject = Owner->GeneratedClass;
    FBlueprintEditorUtils::AddMemberVariable(Caller, TEXT("OwnerRef"), OwnerRefType);
    UEdGraph* EventGraph = Caller->UbergraphPages.Num() > 0 ? Caller->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("caller event graph"), EventGraph))
    {
        return false;
    }

    UK2Node_CustomEvent* Event = PlaceNode<UK2Node_CustomEvent>(EventGraph);
    Event->CustomFunctionName = TEXT("RollbackFixtureEvent");
    Event->AllocateDefaultPins();

    UK2Node_CallFunction* Call = PlaceNode<UK2Node_CallFunction>(EventGraph);
    Call->SetFromFunction(Foo);
    Call->AllocateDefaultPins();

    UK2Node_VariableGet* Get = PlaceNode<UK2Node_VariableGet>(EventGraph);
    Get->VariableReference.SetSelfMember(TEXT("Feed"));
    Get->AllocateDefaultPins();

    UK2Node_VariableGet* GetOwner = PlaceNode<UK2Node_VariableGet>(EventGraph);
    GetOwner->VariableReference.SetSelfMember(TEXT("OwnerRef"));
    GetOwner->AllocateDefaultPins();

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    UEdGraphPin* EventThen = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* CallExec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    UEdGraphPin* ValuePin = Call->FindPin(TEXT("Value"), EGPD_Input);
    UEdGraphPin* SelfPin = Call->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
    UEdGraphPin* GetValue = Get->FindPin(TEXT("Feed"), EGPD_Output);
    UEdGraphPin* GetOwnerValue = GetOwner->FindPin(TEXT("OwnerRef"), EGPD_Output);

    if (!TestNotNull(TEXT("event then pin"), EventThen) || !TestNotNull(TEXT("call exec pin"), CallExec)
        || !TestNotNull(TEXT("call Value pin"), ValuePin) || !TestNotNull(TEXT("variable get pin"), GetValue)
        || !TestNotNull(TEXT("call self pin"), SelfPin) || !TestNotNull(TEXT("owner get pin"), GetOwnerValue))
    {
        return false;
    }
    TestTrue(TEXT("exec link created"), Schema->TryCreateConnection(EventThen, CallExec));
    TestTrue(TEXT("Value link created"), Schema->TryCreateConnection(GetValue, ValuePin));
    TestTrue(TEXT("target link created"), Schema->TryCreateConnection(GetOwnerValue, SelfPin));

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Caller);
    FKismetEditorUtilities::CompileBlueprint(Caller, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    if (!TestTrue(TEXT("caller compiles before the signature change"),
        Caller->Status == BS_UpToDate || Caller->Status == BS_UpToDateWithWarnings))
    {
        return false;
    }

    const FString CallNodeGuid = Call->NodeGuid.ToString();
    const int32 PinCountBefore = Call->Pins.Num();
    Caller->GetPackage()->SetDirtyFlag(false);

    // Break the signature: drop Value from Foo, leaving the caller's link pointing at a pin that no
    // longer exists on the function.
    TSharedPtr<FUserPinInfo> ValueParameter;
    for (const TSharedPtr<FUserPinInfo>& PinInfo : Entry->UserDefinedPins)
    {
        if (PinInfo.IsValid() && PinInfo->PinName == FName(TEXT("Value")))
        {
            ValueParameter = PinInfo;
            break;
        }
    }
    if (!TestTrue(TEXT("Value parameter found"), ValueParameter.IsValid()))
    {
        return false;
    }
    Entry->RemoveUserDefinedPin(ValueParameter);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Owner);
    FKismetEditorUtilities::CompileBlueprint(Owner, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);

    // Run the engine over the now-stale call site.
    UnrealMCP::CallSiteRefresh::FCallSiteRow Row;
    Row.BlueprintObjectPath = FString::Printf(TEXT("%s.%s"), *CallerPath, *FPackageName::GetLongPackageAssetName(CallerPath));
    Row.GraphName = EventGraph->GetName();
    Row.NodeGuid = CallNodeGuid;

    UnrealMCP::CallSiteRefresh::FOptions Options;
    Options.bCompileCallers = true;

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    const bool bSucceeded = UnrealMCP::CallSiteRefresh::RefreshCallSitesInBlueprint(
        Row.BlueprintObjectPath, { Row }, Options, Result);

    TestFalse(TEXT("a caller that cannot compile is not reported as success"), bSucceeded);
    TestTrue(TEXT("compile was attempted"), Result->GetBoolField(TEXT("compiled")));
    TestFalse(TEXT("compile is reported as failed"), Result->GetBoolField(TEXT("compileSucceeded")));
    TestTrue(TEXT("the edit was rolled back"), Result->GetBoolField(TEXT("rolledBack")));
    TestTrue(TEXT("restoration was actually verified"), Result->GetBoolField(TEXT("restorationVerified")));
    TestTrue(TEXT("the graph was restored to its original pin shape"), Result->GetBoolField(TEXT("graphRestored")));
    TestTrue(TEXT("the package dirty flag was restored"), Result->GetBoolField(TEXT("dirtyRestored")));
    TestFalse(TEXT("a clean package is not left dirty after rollback"), Result->GetBoolField(TEXT("dirty")));

    // Independently confirm the live graph really is back, rather than trusting the reported flags.
    UEdGraphNode* RestoredNode = nullptr;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Node != nullptr && Node->NodeGuid.ToString() == CallNodeGuid)
        {
            RestoredNode = Node;
            break;
        }
    }
    if (TestNotNull(TEXT("call node still present after rollback"), RestoredNode))
    {
        TestEqual(TEXT("call node pin count is unchanged after rollback"), RestoredNode->Pins.Num(), PinCountBefore);
    }

    return true;
}

#endif
