#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Index/UnrealMCPProjectIndexInternal.h"
#include "Index/UnrealMCPProjectIndex.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Index/RefreshProjectIndexTool.h"
#include "UnrealMCPModule.h"
#include "K2Node_Composite.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/Blueprint/Graph/AddBlueprintBranchNodeTool.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Graph/ConnectBlueprintPinsTool.h"
#include "Tools/Blueprint/Graph/DeleteBlueprintNodeTool.h"

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
    FString FixtureFilename;
    auto& ProjectIndex = FUnrealMCPModule::Get().GetProjectIndex();
    const auto IndexStatus = ProjectIndex.GetStatusSnapshot();
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
        if (!FixtureFilename.IsEmpty())
        {
            TestTrue(TEXT("Temporary fixture file deleted"), IFileManager::Get().Delete(*FixtureFilename, false, true));
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles({FixtureFilename});
            FUnrealMCPProjectIndex::FRefreshResult Cleanup;
            FString CleanupError;
            TestTrue(TEXT("Temporary index rows cleaned up"), ProjectIndex.RefreshProjectIndex({ObjectPath}, Cleanup, CleanupError));
            TestEqual(TEXT("Temporary indexed asset removed"), Cleanup.RemovedAssetCount, 1);
        }
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
    TSet<FString> RemovedGuids;
    TArray<UEdGraphNode*> BulkNodes;
    for (int32 Index = 0; Index < 200; ++Index)
    {
        auto* Branch = NewObject<UK2Node_IfThenElse>(Collapsed);
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Collapsed, Branch, {});
        if (!BulkNodes.IsEmpty())
            CastChecked<UK2Node_IfThenElse>(BulkNodes.Last())->GetThenPin()->MakeLinkTo(Branch->GetExecPin());
        BulkNodes.Add(Branch);
        RemovedGuids.Add(UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Branch));
    }
    TArray<FIndexedBlueprintGraphRow> Graphs;
    TArray<FIndexedBlueprintNodeRow> Nodes;
    TArray<FIndexedBlueprintPinRow> Pins;
    TArray<FIndexedBlueprintEdgeRow> Edges;
    TestTrue(TEXT("Index extraction before bulk deletion"), ExtractBlueprintGraphRows(Blueprint, Graphs, Nodes, Pins, Edges));
    TestEqual(TEXT("All 200 fixture nodes were indexed"), Nodes.FilterByPredicate([&](const FIndexedBlueprintNodeRow& Row)
        { return RemovedGuids.Contains(Row.NodeGuid); }).Num(), 200);
    // The extraction regression also runs without a previously built sample index.
    const bool bVerifySqlite = IndexStatus.bHasUsableIndex;
    auto Refresh = [&]()
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = TEXT("bulk-index-refresh");
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetArrayField(TEXT("objectPaths"), {MakeShared<FJsonValueString>(ObjectPath)});
        const auto Response = FRefreshProjectIndexTool().Execute(Request);
        if (Response.Error.IsSet()) AddError(Response.Error->Message);
        return Response.Result.IsValid() && Response.Result->GetIntegerField(TEXT("refreshedAssetCount")) == 1;
    };
    auto VerifySqlite = [&]()
    {
        const auto CheckRows = [&](const TCHAR* Sql, int32 Expected, const TCHAR* Label)
        {
            FSQLitePreparedStatement Query(ProjectIndex.Database, Sql, ESQLitePreparedStatementFlags::None);
            if (!Query.IsValid() || !Query.SetBindingValueByIndex(1, *ObjectPath)) return false;
            int32 Count = 0;
            const int64 Rows = Query.Execute([&](const FSQLitePreparedStatement& Row)
            {
                return Row.GetColumnValueByIndex(0, Count) ? ESQLitePreparedStatementExecuteRowResult::Continue : ESQLitePreparedStatementExecuteRowResult::Error;
            });
            return TestEqual(TEXT("SQLite count query returns one row"), Rows, int64(1)) && TestEqual(Label, Count, Expected);
        };
        return CheckRows(TEXT("SELECT count(*) FROM blueprint_nodes WHERE blueprint_object_path = ?1"), Nodes.Num(), TEXT("SQLite node count matches current graph"))
            && CheckRows(TEXT("SELECT count(*) FROM blueprint_pins WHERE blueprint_object_path = ?1"), Pins.Num(), TEXT("SQLite pin count matches current graph"))
            && CheckRows(TEXT("SELECT count(*) FROM blueprint_edges WHERE blueprint_object_path = ?1"), Edges.Num(), TEXT("SQLite edge count matches current graph"));
    };
    if (bVerifySqlite)
    {
        if (!TestTrue(TEXT("Save temporary fixture for registry discovery"), UnrealMCP::BlueprintEditToolUtils::SaveAsset(Blueprint, FixtureFilename, Error))) return false;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles({FixtureFilename});
        if (!TestTrue(TEXT("Populate the SQLite index with 200 connected nodes"), Refresh()) || !VerifySqlite()) return false;
    }
    else AddInfo(TEXT("SQLite verification skipped because the project index has not been built."));
    for (UEdGraphNode* Node : BulkNodes) Node->DestroyNode();
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    Graphs.Reset(); Nodes.Reset(); Pins.Reset(); Edges.Reset();
    TestTrue(TEXT("Index extraction after bulk deletion and compile"), ExtractBlueprintGraphRows(Blueprint, Graphs, Nodes, Pins, Edges));
    TestFalse(TEXT("Deleted nodes leave no index rows"), Nodes.ContainsByPredicate([&](const FIndexedBlueprintNodeRow& Row)
        { return RemovedGuids.Contains(Row.NodeGuid); }));
    TestFalse(TEXT("Deleted nodes leave no pin rows"), Pins.ContainsByPredicate([&](const FIndexedBlueprintPinRow& Row)
        { return RemovedGuids.Contains(Row.NodeGuid); }));
    TestFalse(TEXT("Deleted nodes leave no edge rows"), Edges.ContainsByPredicate([&](const FIndexedBlueprintEdgeRow& Row)
        { return RemovedGuids.Contains(Row.SourceNodeGuid) || RemovedGuids.Contains(Row.TargetNodeGuid); }));
    if (bVerifySqlite && (!TestTrue(TEXT("Refresh the same asset after bulk deletion and compile"), Refresh()) || !VerifySqlite())) return false;
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
