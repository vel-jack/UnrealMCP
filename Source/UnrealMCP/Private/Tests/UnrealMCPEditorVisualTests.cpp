#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/EditorVisualTools.h"

namespace
{
    UnrealMCP::FMCPRequest VisualRequest()
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = TEXT("editor-visual-test"); Request.Params = MakeShared<FJsonObject>();
        return Request;
    }

    TArray<FString> CaptureFiles()
    {
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMCP/Captures")), TEXT("*"), true, false);
        Files.Sort();
        return Files;
    }

    struct FVisualBlueprintFixture
    {
        UBlueprint* Blueprint = nullptr;
        FVisualBlueprintFixture()
        {
            const FString Name = TEXT("BP_VisualDryRun_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
                CreatePackage(*(TEXT("/Game/UnrealMCP_Automation/") + Name)), FName(*Name),
                BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
            if (Blueprint) FAssetRegistryModule::AssetCreated(Blueprint);
        }
        ~FVisualBlueprintFixture()
        {
            if (!Blueprint) return;
            UPackage* Package = Blueprint->GetPackage();
            FAssetRegistryModule::AssetDeleted(Blueprint);
            ObjectTools::DeleteObjectsUnchecked({Blueprint});
            Package->SetDirtyFlag(false);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPEditorVisualReadOnlyTest, "UnrealMCP.Editor.Visual.ReadOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPEditorVisualReadOnlyTest::RunTest(const FString& Parameters)
{
    const TArray<FString> BeforeFiles = CaptureFiles();
    const FString CaptureDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMCP/Captures"));
    const bool bDirectoryBefore = IFileManager::Get().DirectoryExists(*CaptureDirectory);
    FCaptureEditorWidgetTool Capture;
    auto Request = VisualRequest();
    TestTrue(TEXT("Capture never implicitly selects a widget"), Capture.Execute(Request).Error.IsSet());
    Request.Params->SetNumberField(TEXT("widgetRef"), 12);
    TestTrue(TEXT("Capture rejects nonstring reference"), Capture.Execute(Request).Error.IsSet());
    Request.Params->SetStringField(TEXT("widgetRef"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    const auto Stale = Capture.Execute(Request);
    TestTrue(TEXT("Capture rejects an unknown/stale reference"), Stale.Error.IsSet());
    if (FSlateApplication::IsInitialized() && Stale.Error.IsSet())
        TestTrue(TEXT("Unknown reference reports stale identity"), Stale.Error->Message.Contains(TEXT("stale_widget_ref")));
    TestTrue(TEXT("Rejected captures create no files"), CaptureFiles() == BeforeFiles);
    TestEqual(TEXT("Rejected captures create no directory"), IFileManager::Get().DirectoryExists(*CaptureDirectory), bDirectoryBefore);

    const auto CaptureSchema = Capture.GetDefinition().InputSchema;
    TestTrue(TEXT("Capture schema requires explicit widgetRef"), CaptureSchema->GetArrayField(TEXT("required")).ContainsByPredicate(
        [](const TSharedPtr<FJsonValue>& Value) { return Value->AsString() == TEXT("widgetRef"); }));
    FGetEditorWidgetSnapshotTool Snapshot;
    Request = VisualRequest(); Request.Params->SetNumberField(TEXT("windowRef"), 7);
    TestTrue(TEXT("Snapshot rejects nonstring window selector"), Snapshot.Execute(Request).Error.IsSet());
    Request = VisualRequest();
    const auto Listed = Snapshot.Execute(Request);
    if (!FSlateApplication::IsInitialized())
    {
        TestTrue(TEXT("No Slate session produces a native availability error"), Listed.Error.IsSet());
        return !HasAnyErrors();
    }
    if (Listed.Error.IsSet()) { AddError(Listed.Error->Message); return false; }
    const auto& Windows = Listed.Result->GetArrayField(TEXT("windows"));
    TestEqual(TEXT("Window count matches listing"), Listed.Result->GetIntegerField(TEXT("returnedCount")), Windows.Num());
    TestTrue(TEXT("Window list bounded"), Windows.Num() <= 128);
    if (Windows.IsEmpty())
    {
        TestEqual(TEXT("Headless session lists no visible windows"), Listed.Result->GetIntegerField(TEXT("returnedCount")), 0);
        return !HasAnyErrors();
    }
    const FString WindowRef = Windows[0]->AsObject()->GetStringField(TEXT("windowRef"));
    Request.Params->SetStringField(TEXT("windowRef"), WindowRef);
    Request.Params->SetNumberField(TEXT("maxDepth"), 0);
    Request.Params->SetNumberField(TEXT("maxWidgets"), 1);
    const auto Tree = Snapshot.Execute(Request);
    if (Tree.Error.IsSet()) { AddError(Tree.Error->Message); return false; }
    TestEqual(TEXT("Depth-zero tree contains only root"), Tree.Result->GetIntegerField(TEXT("returnedCount")), 1);
    TestEqual(TEXT("Depth-zero root has no expanded children"), Tree.Result->GetObjectField(TEXT("root"))->GetArrayField(TEXT("children")).Num(), 0);
    for (double Invalid : {-1.0, 8.5, 9.0})
    {
        Request.Params->SetNumberField(TEXT("maxDepth"), Invalid);
        TestTrue(TEXT("Snapshot rejects invalid depth"), Snapshot.Execute(Request).Error.IsSet());
    }
    Request.Params->SetNumberField(TEXT("maxDepth"), 1);
    for (double Invalid : {0.0, 1.5, 1001.0})
    {
        Request.Params->SetNumberField(TEXT("maxWidgets"), Invalid);
        TestTrue(TEXT("Snapshot rejects invalid widget bound"), Snapshot.Execute(Request).Error.IsSet());
    }
    Request.Params->SetNumberField(TEXT("maxWidgets"), 1);
    Request.Params->SetStringField(TEXT("includeLabels"), TEXT("true"));
    TestTrue(TEXT("Snapshot rejects nonboolean label selector"), Snapshot.Execute(Request).Error.IsSet());
    TestTrue(TEXT("Snapshot never writes capture files"), CaptureFiles() == BeforeFiles);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPFrameBlueprintDryRunTest, "UnrealMCP.Editor.Visual.FrameBlueprintDryRun",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPFrameBlueprintDryRunTest::RunTest(const FString& Parameters)
{
    FVisualBlueprintFixture Fixture;
    if (!TestNotNull(TEXT("Temporary Blueprint exists"), Fixture.Blueprint)) return false;
    UBlueprint* Blueprint = Fixture.Blueprint;
    if (!TestTrue(TEXT("Fixture has event graph"), !Blueprint->UbergraphPages.IsEmpty())) return false;
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    Blueprint->LastEditedDocuments.Add(FEditedDocumentInfo(Graph, FVector2D(125, 250), 0.75f));
    const auto DocumentsBefore = Blueprint->LastEditedDocuments;
    Blueprint->GetPackage()->SetDirtyFlag(false);
    const auto StatusBefore = Blueprint->Status;
    const FString RevisionBefore = UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
    const int32 NodeCountBefore = Graph->Nodes.Num();
    const auto EditorBefore = FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, false);
    const auto ActiveWindowBefore = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetActiveTopLevelWindow() : nullptr;
    const auto FilesBefore = CaptureFiles();
    FFrameBlueprintGraphTool Tool;
    auto Request = VisualRequest();
    Request.Params->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
    Request.Params->SetBoolField(TEXT("dryRun"), true);
    TestTrue(TEXT("Framing requires an exact graph selector"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetNumberField(TEXT("graphName"), 12);
    TestTrue(TEXT("Framing rejects nonstring graph name"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetStringField(TEXT("graphName"), Graph->GetName());
    Request.Params->SetStringField(TEXT("graphGuid"), TEXT("not-a-guid"));
    TestTrue(TEXT("Framing rejects malformed graph GUID"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetStringField(TEXT("graphGuid"), FGuid::NewGuid().ToString());
    TestTrue(TEXT("Both graph selectors must match"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString());
    auto Result = Tool.Execute(Request);
    if (Result.Error.IsSet()) { AddError(Result.Error->Message); return false; }
    TestTrue(TEXT("Dry framing records whole graph"), Result.Result->GetBoolField(TEXT("wholeGraph")));
    TestFalse(TEXT("Dry framing starts no pending UI operation"), Result.Result->GetBoolField(TEXT("framingPending")));
    TestFalse(TEXT("Dry framing supplies no capture reference"), Result.Result->HasField(TEXT("widgetRef")));
    Request.Params->RemoveField(TEXT("graphName"));
    TestFalse(TEXT("GUID-only selector works"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetNumberField(TEXT("nodeGuids"), 1);
    TestTrue(TEXT("Node selector must be an array"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetArrayField(TEXT("nodeGuids"), {MakeShared<FJsonValueNumber>(1)});
    TestTrue(TEXT("Node GUID entries must be strings"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetArrayField(TEXT("nodeGuids"), {MakeShared<FJsonValueString>(FGuid::NewGuid().ToString())});
    TestTrue(TEXT("Node identity must belong to selected graph"), Tool.Execute(Request).Error.IsSet());
    TArray<TSharedPtr<FJsonValue>> TooMany;
    for (int32 Index = 0; Index < 129; ++Index) TooMany.Add(MakeShared<FJsonValueString>(FGuid::NewGuid().ToString()));
    Request.Params->SetArrayField(TEXT("nodeGuids"), TooMany);
    TestTrue(TEXT("Node selection bounded at 128"), Tool.Execute(Request).Error.IsSet());
    if (!Graph->Nodes.IsEmpty())
    {
        Request.Params->SetArrayField(TEXT("nodeGuids"), {MakeShared<FJsonValueString>(Graph->Nodes[0]->NodeGuid.ToString())});
        Result = Tool.Execute(Request);
        if (Result.Error.IsSet()) { AddError(Result.Error->Message); return false; }
        TestEqual(TEXT("Dry framing validates exact node identity"), Result.Result->GetIntegerField(TEXT("selectedNodeCount")), 1);
    }
    Request.Params->SetStringField(TEXT("dryRun"), TEXT("true"));
    TestTrue(TEXT("Framing rejects nonboolean dryRun"), Tool.Execute(Request).Error.IsSet());
    TestFalse(TEXT("Dry and rejected framing preserve package dirty state"), Blueprint->GetPackage()->IsDirty());
    TestTrue(TEXT("Dry and rejected framing preserve Blueprint status"), Blueprint->Status == StatusBefore);
    TestEqual(TEXT("Dry and rejected framing preserve node count"), Graph->Nodes.Num(), NodeCountBefore);
    TestEqual(TEXT("Dry and rejected framing preserve graph revision"), UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph), RevisionBefore);
    TestTrue(TEXT("Dry framing preserves saved graph view and zoom"), Blueprint->LastEditedDocuments == DocumentsBefore);
    TestTrue(TEXT("Dry framing opens no Blueprint editor"), FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, false) == EditorBefore);
    if (FSlateApplication::IsInitialized()) TestTrue(TEXT("Dry framing preserves active editor window"), FSlateApplication::Get().GetActiveTopLevelWindow() == ActiveWindowBefore);
    TestTrue(TEXT("Dry framing writes no captures"), CaptureFiles() == FilesBefore);
    const auto Props = Tool.GetDefinition().InputSchema->GetObjectField(TEXT("properties"));
    for (const TCHAR* Selector : {TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("nodeGuids"), TEXT("dryRun")})
        TestTrue(TEXT("Framing schema exposes exact selectors"), Props->HasField(Selector));
    return !HasAnyErrors();
}
#endif
