#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_Composite.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/AddBlueprintBranchNodeTool.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/ConnectBlueprintPinsTool.h"
#include "Tools/DeleteBlueprintNodeTool.h"

namespace
{
    TSharedRef<FJsonObject> CollapsedGraphParams(const FString& ObjectPath, const UEdGraph* Graph)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Params->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString());
        return Params;
    }
}

// R1: node tools must work inside a collapsed (K2Node_Composite) graph, whose outer is a node, not the Blueprint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPCollapsedGraphLiveTest,
    "UnrealMCP.Blueprint.Authoring.CollapsedGraph.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPCollapsedGraphLiveTest::RunTest(const FString& Parameters)
{
    const FString Name = FString::Printf(TEXT("BP_Collapsed_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
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

    // PostPlacedNewNode creates the composite's BoundGraph as a subgraph of the event graph.
    UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(Blueprint->UbergraphPages[0]);
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Blueprint->UbergraphPages[0], Composite, {});
    UEdGraph* Collapsed = Composite->BoundGraph;
    if (!TestNotNull(TEXT("Collapsed graph exists"), Collapsed)) return false;
    TestTrue(TEXT("Collapsed graph is owned by a node"), Cast<UK2Node_Composite>(Collapsed->GetOuter()) != nullptr);

    const FAddBlueprintBranchNodeTool AddTool;
    FString Guids[2];
    for (int32 Index = 0; Index < 2; ++Index)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = TEXT("collapsed-add");
        Request.Params = CollapsedGraphParams(ObjectPath, Collapsed);
        const UnrealMCP::FMCPResponse Response = AddTool.Execute(Request);
        if (!TestFalse(TEXT("Add in collapsed graph has no error"), Response.Error.IsSet())
            || !TestTrue(TEXT("Add returns a result"), Response.Result.IsValid())) return false;
        Guids[Index] = Response.Result->GetStringField(TEXT("nodeGuid"));
    }

    UEdGraphNode* First = nullptr;
    UEdGraphNode* Second = nullptr;
    FString Error;
    TestTrue(TEXT("First node is in the collapsed graph"), UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Collapsed, Guids[0], First, Error));
    TestTrue(TEXT("Second node is in the collapsed graph"), UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Collapsed, Guids[1], Second, Error));

    UnrealMCP::FMCPRequest Connect;
    Connect.Id = TEXT("collapsed-connect");
    Connect.Params = CollapsedGraphParams(ObjectPath, Collapsed);
    Connect.Params->SetStringField(TEXT("sourceNodeGuid"), Guids[0]);
    Connect.Params->SetStringField(TEXT("sourcePinName"), TEXT("then"));
    Connect.Params->SetStringField(TEXT("targetNodeGuid"), Guids[1]);
    Connect.Params->SetStringField(TEXT("targetPinName"), TEXT("execute"));
    const UnrealMCP::FMCPResponse ConnectResponse = FConnectBlueprintPinsTool().Execute(Connect);
    TestFalse(TEXT("Connect in collapsed graph has no error"), ConnectResponse.Error.IsSet());
    UK2Node_IfThenElse* FirstBranch = Cast<UK2Node_IfThenElse>(First);
    TestTrue(TEXT("Link exists in collapsed graph"),
        FirstBranch != nullptr && FirstBranch->GetThenPin()->LinkedTo.ContainsByPredicate(
            [Second](const UEdGraphPin* Pin) { return Pin && Pin->GetOwningNode() == Second; }));

    UnrealMCP::FMCPRequest Delete;
    Delete.Id = TEXT("collapsed-delete");
    Delete.Params = CollapsedGraphParams(ObjectPath, Collapsed);
    Delete.Params->SetStringField(TEXT("nodeGuid"), Guids[1]);
    Delete.Params->SetBoolField(TEXT("confirm"), true);
    const UnrealMCP::FMCPResponse DeleteResponse = FDeleteBlueprintNodeTool().Execute(Delete);
    TestFalse(TEXT("Delete in collapsed graph has no error"), DeleteResponse.Error.IsSet());
    UEdGraphNode* Gone = nullptr;
    TestFalse(TEXT("Deleted node left the collapsed graph"),
        UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Collapsed, Guids[1], Gone, Error));
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
