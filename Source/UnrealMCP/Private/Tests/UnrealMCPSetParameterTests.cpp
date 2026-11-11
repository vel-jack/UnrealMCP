#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/SetBlueprintParameterTool.h"

// Custom event parameter retype + rename, the change the editor's Details panel makes by hand.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPSetParameterLiveTest,
    "UnrealMCP.Blueprint.Authoring.SetParameter.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPSetParameterLiveTest::RunTest(const FString& Parameters)
{
    const FString Name = FString::Printf(TEXT("BP_SetParam_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/UnrealMCP_Automation/%s"), *Name);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *Name);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), CreatePackage(*PackagePath), FName(*Name), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("UnrealMCPAutomation")));
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
    };
    if (!TestNotNull(TEXT("Fixture Blueprint"), Blueprint) || Blueprint->UbergraphPages.Num() == 0) return false;
    UEdGraph* Graph = Blueprint->UbergraphPages[0];

    // Event "ServerRotate(Marker: Actor)" whose Marker feeds DestroyActor's Target.
    UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
    Event->CustomFunctionName = TEXT("ServerRotate");
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Event, {});
    FEdGraphPinType ActorType;
    ActorType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ActorType.PinSubCategoryObject = AActor::StaticClass();
    Event->CreateUserDefinedPin(TEXT("Marker"), ActorType, EGPD_Output);

    UK2Node_CallFunction* Destroy = NewObject<UK2Node_CallFunction>(Graph);
    Destroy->FunctionReference.SetExternalMember(TEXT("K2_DestroyActor"), AActor::StaticClass());
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Destroy, {});
    TestTrue(TEXT("Marker linked to Target"),
        GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Event->FindPin(TEXT("Marker")), Destroy->FindPin(UEdGraphSchema_K2::PN_Self)));

    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    UK2Node_CallFunction* Caller = NewObject<UK2Node_CallFunction>(Graph);
    Caller->FunctionReference.SetSelfMember(TEXT("ServerRotate"));
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Caller, {});
    if (!TestNotNull(TEXT("Call site has Marker pin"), Caller->FindPin(TEXT("Marker")))) return false;

    const FSetBlueprintParameterTool Tool;
    auto Call = [&](const TFunction<void(FJsonObject&)>& Fill)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = TEXT("set-parameter");
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->SetStringField(TEXT("nodeGuid"), Event->NodeGuid.ToString());
        Request.Params->SetStringField(TEXT("parameterName"), TEXT("Marker"));
        Fill(*Request.Params);
        return Tool.Execute(Request);
    };

    // Retyping to int would break the link: dry-run measures it, unconfirmed apply refuses, nothing changes.
    const UnrealMCP::FMCPResponse Dry = Call([](FJsonObject& P) { P.SetStringField(TEXT("type"), TEXT("int")); P.SetBoolField(TEXT("dryRun"), true); });
    if (TestTrue(TEXT("Dry-run succeeds"), Dry.Result.IsValid()))
    {
        TestEqual(TEXT("Dry-run reports the broken link"), static_cast<int32>(Dry.Result->GetNumberField(TEXT("brokenLinkCount"))), 1);
        TestTrue(TEXT("Dry-run rolled back"), Dry.Result->GetBoolField(TEXT("rolledBack")));
    }
    TestEqual(TEXT("Dry-run restores event-side link"), Event->FindPin(TEXT("Marker"))->LinkedTo.Num(), 1);
    TestEqual(TEXT("Dry-run restores target-side link"), Destroy->FindPin(UEdGraphSchema_K2::PN_Self)->LinkedTo.Num(), 1);
    TestTrue(TEXT("Unconfirmed breaking change is refused"),
        Call([](FJsonObject& P) { P.SetStringField(TEXT("type"), TEXT("int")); }).Error.IsSet());
    TestTrue(TEXT("Type unchanged after refusal"), Event->FindPin(TEXT("Marker")) != nullptr
        && Event->FindPin(TEXT("Marker"))->PinType.PinSubCategoryObject == AActor::StaticClass());
    TestEqual(TEXT("Link kept after refusal"), Event->FindPin(TEXT("Marker"))->LinkedTo.Num(), 1);

    // Actor -> Pawn plus rename: compatible, so the link and the call site follow.
    const UnrealMCP::FMCPResponse Applied = Call([](FJsonObject& P)
    {
        P.SetStringField(TEXT("type"), TEXT("object"));
        P.SetStringField(TEXT("typeObjectPath"), APawn::StaticClass()->GetPathName());
        P.SetStringField(TEXT("newName"), TEXT("Weapon"));
    });
    if (Applied.Error.IsSet()) AddError(Applied.Error->Message);
    if (!TestTrue(TEXT("Retype and rename succeed"), Applied.Result.IsValid())) return false;
    TestTrue(TEXT("Compiled"), Applied.Result->GetBoolField(TEXT("compiled")));

    UEdGraphPin* Weapon = Event->FindPin(TEXT("Weapon"));
    if (!TestNotNull(TEXT("Event pin renamed"), Weapon)) return false;
    TestNull(TEXT("Old event pin gone"), Event->FindPin(TEXT("Marker")));
    TestTrue(TEXT("Event pin is Pawn"), Weapon->PinType.PinSubCategoryObject == APawn::StaticClass());
    TestTrue(TEXT("Link survives"), Weapon->LinkedTo.Num() == 1 && Weapon->LinkedTo[0]->GetOwningNode() == Destroy);
    UEdGraphPin* CallerPin = Caller->FindPin(TEXT("Weapon"));
    TestTrue(TEXT("Call site pin renamed and retyped"), CallerPin != nullptr && CallerPin->PinType.PinSubCategoryObject == APawn::StaticClass());
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
