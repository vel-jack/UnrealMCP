#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ObjectTools.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Members/AddBlueprintVariableTool.h"
#include "Tools/Blueprint/Members/BlueprintClassDefaultsTools.h"
#include "Tools/Blueprint/Members/BlueprintLocalVariableTools.h"
#include "Tools/Blueprint/Inspection/CompileBlueprintTool.h"
#include "Tools/Git/CompareBlueprintMergeStructureTool.h"
#include "Tools/Blueprint/Graph/CompareBlueprintGraphPreservationTool.h"
#include "Tools/Assets/ConsolidateAndDeleteAssetTool.h"
#include "Tools/Blueprint/Inspection/FindClassUsagesTool.h"
#include "Tools/Blueprint/Members/RetargetBlueprintNodeTool.h"
#include "Tools/Blueprint/Members/SetBlueprintComponentDefaultsTool.h"
#include "UObject/SavePackage.h"

// Tests for the tooling-requirements P1 tools: R2 retarget, R3 locals, R7 class usages, R11 compile, R12 consolidate.
namespace UnrealMCPToolingP1
{
    struct FFixture
    {
        TArray<TWeakObjectPtr<UBlueprint>> Blueprints;

        UBlueprint* Create(const TCHAR* Prefix, UClass* Parent = AActor::StaticClass(), EBlueprintType BlueprintType = BPTYPE_Normal)
        {
            const FString Name = FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
                Parent, CreatePackage(*FString::Printf(TEXT("/Game/UnrealMCP_Automation/%s"), *Name)), FName(*Name), BlueprintType,
                UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("UnrealMCPAutomation")));
            FAssetRegistryModule::AssetCreated(Blueprint);
            Blueprints.Add(Blueprint);
            return Blueprint;
        }

        ~FFixture()
        {
            for (const TWeakObjectPtr<UBlueprint>& Weak : Blueprints)
            {
                UBlueprint* Blueprint = Weak.Get();
                if (Blueprint == nullptr) continue;
                UPackage* Package = Blueprint->GetPackage();
                FAssetRegistryModule::AssetDeleted(Blueprint);
                ObjectTools::DeleteObjectsUnchecked({Blueprint});
                if (Package) Package->SetDirtyFlag(false);
            }
        }
    };

    FString PathOf(const UObject* Object) { return Object->GetPathName(); }

    UnrealMCP::FMCPResponse Run(const FMCPToolBase& Tool, const TSharedRef<FJsonObject>& Params)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = TEXT("tooling-p1");
        Request.Params = Params;
        return Tool.Execute(Request);
    }

    template <typename TNode>
    TNode* Place(UEdGraph* Graph)
    {
        TNode* Node = NewObject<TNode>(Graph);
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, {});
        return Node;
    }

    FEdGraphPinType ObjectType(UClass* Class)
    {
        FEdGraphPinType Type;
        Type.PinCategory = UEdGraphSchema_K2::PC_Object;
        Type.PinSubCategoryObject = Class;
        return Type;
    }

    TSet<FString> Kinds(const UnrealMCP::FMCPResponse& Response)
    {
        TSet<FString> Result;
        if (!Response.Result.IsValid()) return Result;
        for (const TSharedPtr<FJsonValue>& Hit : Response.Result->GetArrayField(TEXT("usages")))
        {
            Result.Add(Hit->AsObject()->GetStringField(TEXT("kind")));
        }
        return Result;
    }

    void Compile(UBlueprint* Blueprint)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    }
}

using namespace UnrealMCPToolingP1;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPRetargetNodeTest, "UnrealMCP.Blueprint.Authoring.RetargetNode.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPRetargetNodeTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Blueprint = Fixture.Create(TEXT("BP_Retarget"));
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
    CastNode->TargetType = APawn::StaticClass();
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, CastNode, {});
    UK2Node_CallFunction* Destroy = Place<UK2Node_CallFunction>(Graph);
    Destroy->FunctionReference.SetExternalMember(TEXT("K2_DestroyActor"), AActor::StaticClass());
    Destroy->ReconstructNode();
    TestTrue(TEXT("Cast result linked"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(CastNode->GetCastResultPin(), Destroy->FindPin(UEdGraphSchema_K2::PN_Self)));

    // Simulate the target class being deleted: this is the "Bad cast node" state.
    CastNode->TargetType = nullptr;
    CastNode->ReconstructNode();

    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
    Params->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString());
    Params->SetStringField(TEXT("nodeGuid"), CastNode->NodeGuid.ToString());
    Params->SetStringField(TEXT("newClassPath"), PathOf(ACharacter::StaticClass()));
    TestTrue(TEXT("Retarget without confirm is refused"), Run(FRetargetBlueprintNodeTool(), Params).Error.IsSet());

    Params->SetBoolField(TEXT("confirm"), true);
    const UnrealMCP::FMCPResponse Response = Run(FRetargetBlueprintNodeTool(), Params);
    if (Response.Error.IsSet()) AddError(Response.Error->Message);
    if (!TestTrue(TEXT("Retarget succeeds"), Response.Result.IsValid())) return false;
    TestTrue(TEXT("Reported as broken before"), Response.Result->GetBoolField(TEXT("wasBroken")));
    TestTrue(TEXT("Cast now targets Character"), CastNode->TargetType == ACharacter::StaticClass());
    UEdGraphPin* Result = CastNode->GetCastResultPin();
    TestTrue(TEXT("Result link survives"), Result && Result->LinkedTo.Num() == 1 && Result->LinkedTo[0]->GetOwningNode() == Destroy);
    TestFalse(TEXT("No orphan pins left"), CastNode->Pins.ContainsByPredicate([](const UEdGraphPin* Pin) { return Pin->bOrphanedPin; }));
    return !HasAnyErrors();
}

static FORCENOINLINE bool VerifySideReplay(FAutomationTestBase& Test, const FString& Path, const FString& StageRevision,
    UBlueprint* Blueprint, UEdGraph* Graph)
{
    using namespace UnrealMCPToolingP1;
    const FPreviewBlueprintSideReplayTool Tool;
    auto Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("path"), Path);
    Args->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString());
    Args->SetStringField(TEXT("stageRevision"), StageRevision);
    Args->SetStringField(TEXT("side"), TEXT("incoming"));
    const FString Before = UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
    const auto Preview = Run(Tool, Args);
    if (Preview.Error.IsSet()) { Test.AddError(Preview.Error->Message); return false; }
    const TSharedPtr<FJsonObject>* Step = nullptr;
    const bool bStep = Preview.Result->TryGetObjectField(TEXT("step"), Step);
    if (!bStep)
    {
        FString Error;
        Preview.Result->TryGetStringField(TEXT("patchPreflightError"), Error);
        Test.AddError(TEXT("Selected historical edge did not generate a patch. ") + Error);
        return false;
    }
    Test.TestEqual(TEXT("Historical link generates one connection"),
        (*Step)->GetObjectField(TEXT("arguments"))->GetArrayField(TEXT("connections")).Num(), 1);
    Test.TestEqual(TEXT("Side replay preview leaves the live graph unchanged"),
        UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph), Before);
    Test.TestFalse(TEXT("Side replay preview leaves Blueprint clean"), Blueprint->GetOutermost()->IsDirty());
    return !Test.HasAnyErrors();
}

static FORCENOINLINE bool VerifySavedReplay(FAutomationTestBase& Test, UK2Node_FunctionEntry* CompareEntry, UBlueprint* Child,
    const FString& File, const FSavePackageArgs& SaveArgs, bool bExerciseLfs, const FString& LfsObjectFile,
    const TArray<uint8>& LfsBytes, const FString& Path, const FString (&StagedBlobIds)[3], UEdGraph* CompareFunction)
{
    using namespace UnrealMCPToolingP1;
    UPackage* ChildPackage = Child->GetOutermost();
    CompareEntry->FindPinChecked(UEdGraphSchema_K2::PN_Then)->BreakAllPinLinks();
    Compile(Child);
    if (!Test.TestTrue(TEXT("Save working replay fixture"), UPackage::SavePackage(ChildPackage, Child, *File, SaveArgs))) return false;

    if (bExerciseLfs && (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(LfsObjectFile), true)
        || !FFileHelper::SaveArrayToFile(LfsBytes, *LfsObjectFile))) return false;
    if (!VerifySideReplay(Test, Path, StagedBlobIds[0] + TEXT(":") + StagedBlobIds[1] + TEXT(":") + StagedBlobIds[2], Child, CompareFunction)) return false;
    if (bExerciseLfs && !IFileManager::Get().Delete(*LfsObjectFile)) return false;
    return true;
}

static FORCENOINLINE bool VerifyConflictStaging(FAutomationTestBase& Test, TFunctionRef<bool(const FString&, FString&)> Git,
    const FString& Path, const FString& StagesBefore, const FString& StageRevision, UPackage* ChildPackage,
    const FString (&Trees)[3], const FString& EmptyTree, const FString (&RenamedTrees)[3], const FString& RenamedPath)
{
    using namespace UnrealMCPToolingP1;
    FString StagesAfter;
    const FBlueprintGitConflictTool Discovery(false), StageTool(true);
    const auto Conflicts = Run(Discovery, MakeShared<FJsonObject>());
    if (Conflicts.Error.IsSet()) { Test.AddError(Conflicts.Error->Message); return false; }
    Test.TestTrue(TEXT("Discovery identifies the exact unresolved fixture"),
        Conflicts.Result->GetArrayField(TEXT("rows")).ContainsByPredicate(
            [&](const auto& Row) { return Row->AsArray()[0]->AsString() == Path; }));
    auto StageArgs = MakeShared<FJsonObject>();
    StageArgs->SetStringField(TEXT("path"), Path);
    StageArgs->SetStringField(TEXT("stageRevision"), StageRevision);
    const auto StagePreview = Run(StageTool, StageArgs);
    if (StagePreview.Error.IsSet()) { Test.AddError(StagePreview.Error->Message); return false; }
    if (!Git(FString::Printf(TEXT("ls-files -u -- \"%s\""), *Path), StagesAfter)) return false;
    Test.TestEqual(TEXT("Staging preview leaves Git stages intact"), StagesAfter, StagesBefore);
    StageArgs->SetBoolField(TEXT("dryRun"), false);
    StageArgs->SetStringField(TEXT("previewToken"), StagePreview.Result->GetStringField(TEXT("previewToken")));
    Test.TestTrue(TEXT("Staging without confirmation is refused"), Run(StageTool, StageArgs).Error.IsSet());
    StageArgs->SetBoolField(TEXT("confirm"), true);
    ChildPackage->SetDirtyFlag(true);
    Test.TestTrue(TEXT("Staging rejects newly dirty Blueprint"), Run(StageTool, StageArgs).Error.IsSet());
    ChildPackage->SetDirtyFlag(false);
    const auto Staged = Run(StageTool, StageArgs);
    if (Staged.Error.IsSet()) { Test.AddError(Staged.Error->Message); return false; }
    Test.TestTrue(TEXT("Confirmed fixture was staged"), Staged.Result->GetBoolField(TEXT("staged")));
    StagesAfter.Reset();
    if (!Git(FString::Printf(TEXT("ls-files -u -- \"%s\""), *Path), StagesAfter)) return false;
    Test.TestTrue(TEXT("Staged fixture has no unmerged entries"), StagesAfter.IsEmpty());
    const auto Resolved = Run(Discovery, MakeShared<FJsonObject>());
    Test.TestTrue(TEXT("Discovery no longer lists the resolved conflict"), Resolved.Result && Resolved.Result->GetIntegerField(TEXT("total")) == 0);
    FString Ignored;
    const FCompareBlueprintMergeStructureTool ReviewTool;
    auto Review = MakeShared<FJsonObject>();
    Review->SetStringField(TEXT("path"), Path);
    auto StageReviewed = [&](const UnrealMCP::FMCPResponse& Evidence)
    {
        if (Evidence.Error.IsSet()) { Test.AddError(Evidence.Error->Message); return false; }
        StageArgs->SetStringField(TEXT("stageRevision"), Evidence.Result->GetStringField(TEXT("stageRevision")));
        StageArgs->SetBoolField(TEXT("dryRun"), true);
        const auto Preview = Run(StageTool, StageArgs);
        if (Preview.Error.IsSet()) { Test.AddError(Preview.Error->Message); return false; }
        StageArgs->SetStringField(TEXT("previewToken"), Preview.Result->GetStringField(TEXT("previewToken")));
        StageArgs->SetBoolField(TEXT("dryRun"), false);
        const auto Applied = Run(StageTool, StageArgs);
        if (Applied.Error.IsSet()) { Test.AddError(Applied.Error->Message); return false; }
        return Test.TestTrue(TEXT("Reviewed conflict staged"), Applied.Result->GetBoolField(TEXT("staged")));
    };
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *EmptyTree, *Trees[1], *Trees[2]), Ignored)
        || !StageReviewed(Run(ReviewTool, Review))) return false;
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *RenamedTrees[0], *RenamedTrees[1], *RenamedTrees[2]), Ignored)) return false;
    auto StagePaths = MakeShared<FJsonObject>();
    StagePaths->SetStringField(TEXT("current"), RenamedPath);
    Review->SetObjectField(TEXT("stagePaths"), StagePaths);
    StageArgs->SetObjectField(TEXT("stagePaths"), StagePaths);
    StageArgs->SetArrayField(TEXT("removedPaths"), {MakeShared<FJsonValueString>(RenamedPath)});
    if (!StageReviewed(Run(ReviewTool, Review))) return false;
    FString Remaining;
    if (!Git(TEXT("ls-files -u"), Remaining)) return false;
    Test.TestTrue(TEXT("Split rename resolves both approved index paths"), Remaining.IsEmpty());
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *Trees[0], *EmptyTree, *Trees[2]), Ignored)) return false;
    Review->RemoveField(TEXT("stagePaths"));
    StageArgs->RemoveField(TEXT("stagePaths"));
    StageArgs->RemoveField(TEXT("removedPaths"));
    StageArgs->SetStringField(TEXT("resolution"), TEXT("delete"));
    StageArgs->SetBoolField(TEXT("dryRun"), true);
    const auto DeleteReview = Run(ReviewTool, Review);
    if (DeleteReview.Error.IsSet()) { Test.AddError(DeleteReview.Error->Message); return false; }
    StageArgs->SetStringField(TEXT("stageRevision"), DeleteReview.Result->GetStringField(TEXT("stageRevision")));
    Test.TestTrue(TEXT("Staging a deletion refuses an asset still on disk"), Run(StageTool, StageArgs).Error.IsSet());
    ResetLoaders(ChildPackage);
    const FString WorkingFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()) / Path;
    if (!Test.TestTrue(TEXT("Remove the disposable fixture before staging deletion"), IFileManager::Get().Delete(*WorkingFile))) return false;
    if (!StageReviewed(DeleteReview)) return false;
    Remaining.Reset();
    if (!Git(TEXT("ls-files --stage"), Remaining)) return false;
    Test.TestTrue(TEXT("Modify/delete resolution leaves no entry for the deleted fixture"), Remaining.IsEmpty());
    Test.AddInfo(TEXT("Standalone Git conflict discovery, preview and guarded staging completed."));

    return !Test.HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPInheritedComponentDefaultsTest, "UnrealMCP.Blueprint.Authoring.InheritedComponentDefaults.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPInheritedComponentDefaultsTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    auto* Parent = Fixture.Create(TEXT("BP_OverrideParent"));
    auto* Node = Parent->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("FixtureComponent"));
    if (!TestNotNull(TEXT("Parent component"), Node)) return false;
    Parent->SimpleConstructionScript->AddNode(Node);
    Compile(Parent);
    auto* Child = Fixture.Create(TEXT("BP_OverrideChild"), Parent->GeneratedClass);
    const FComponentKey Key(Node);
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("objectPath"), PathOf(Child));
    Params->SetStringField(TEXT("componentName"), TEXT("FixtureComponent"));
    Params->SetBoolField(TEXT("dryRun"), true);
    Params->SetBoolField(TEXT("saveAfterEdit"), false);
    auto Values = MakeShared<FJsonObject>();
    Values->SetStringField(TEXT("RelativeLocation"), TEXT("(X=0,Y=0,Z=15)"));
    Params->SetObjectField(TEXT("propertyDefaults"), Values);
    const FSetBlueprintComponentDefaultsTool Tool;
    TestTrue(TEXT("Local selection rejects inherited component"), Run(Tool, Params).Error.IsSet());
    Params->SetStringField(TEXT("inheritedOverrideGuid"), FGuid::NewGuid().ToString());
    TestTrue(TEXT("Unmatched override GUID rejected"), Run(Tool, Params).Error.IsSet());
    Params->SetStringField(TEXT("inheritedOverrideGuid"), Key.GetAssociatedGuid().ToString());
    TestTrue(TEXT("Missing override requires explicit creation"), Run(Tool, Params).Error.IsSet());
    Params->SetBoolField(TEXT("createInheritedOverride"), true);
    const auto CreatePreview = Run(Tool, Params);
    if (!TestTrue(TEXT("Creation preview succeeds"), CreatePreview.Result.IsValid()))
    {
        if (CreatePreview.Error.IsSet()) AddError(CreatePreview.Error->Message);
        return false;
    }
    TestTrue(TEXT("Preview reports creation"), CreatePreview.Result->GetBoolField(TEXT("wouldCreateOverride")));
    TestNull(TEXT("Preview creates no handler"), Child->GetInheritableComponentHandler(false));
    Params->SetBoolField(TEXT("dryRun"), false);
    const auto Created = Run(Tool, Params);
    if (!TestTrue(TEXT("Creation compiles and verifies readback"), Created.Result.IsValid() && Created.Result->GetBoolField(TEXT("createdOverride"))))
    {
        if (Created.Error.IsSet()) AddError(Created.Error->Message);
        return false;
    }
    TestTrue(TEXT("Creation cannot replace an existing override"), Run(Tool, Params).Error.IsSet());
    Params->RemoveField(TEXT("createInheritedOverride"));
    Params->SetBoolField(TEXT("dryRun"), true);
    Values->SetStringField(TEXT("RelativeLocation"), TEXT("(X=0,Y=0,Z=25)"));
    const auto Preview = Run(Tool, Params);
    if (!TestTrue(TEXT("Exact override preview succeeds"), Preview.Result.IsValid()))
    {
        if (Preview.Error.IsSet()) AddError(Preview.Error->Message);
        return false;
    }
    auto ReadChild = [&]() -> USceneComponent*
    {
        auto* CurrentHandler = Child->GetInheritableComponentHandler(false);
        return CurrentHandler ? Cast<USceneComponent>(CurrentHandler->GetOverridenComponentTemplate(Key)) : nullptr;
    };
    auto* Before = ReadChild();
    if (!TestNotNull(TEXT("Preview keeps override"), Before)) return false;
    TestEqual(TEXT("Preview preserves value"), Before->GetRelativeLocation().Z, 15.0);
    Params->SetBoolField(TEXT("dryRun"), false);
    const auto Applied = Run(Tool, Params);
    if (!TestTrue(TEXT("Exact override apply succeeds"), Applied.Result.IsValid()))
    {
        if (Applied.Error.IsSet()) AddError(Applied.Error->Message);
        return false;
    }
    auto* After = ReadChild();
    if (!TestNotNull(TEXT("Compiled override"), After)) return false;
    TestEqual(TEXT("Child override readback"), After->GetRelativeLocation().Z, 25.0);
    TestEqual(TEXT("Parent component unchanged"), CastChecked<USceneComponent>(Node->ComponentTemplate.Get())->GetRelativeLocation().Z, 0.0);
    TestFalse(TEXT("Fixture remains unsaved"), FPackageName::DoesPackageExist(Child->GetOutermost()->GetName()));

    auto* Broken = Fixture.Create(TEXT("BP_OverrideRollback"), Parent->GeneratedClass);
    UEdGraph* Graph = Broken->UbergraphPages[0];
    auto* Event = NewObject<UK2Node_CustomEvent>(Graph);
    Event->CustomFunctionName = TEXT("TriggerInvalidCast");
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Event, {});
    auto* CastNode = Place<UK2Node_DynamicCast>(Graph);
    if (!TestTrue(TEXT("Broken cast is reachable"),
        GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), CastNode->GetExecPin())))
        return false;
    Broken->GetOutermost()->SetDirtyFlag(false);
    auto RollbackParams = MakeShared<FJsonObject>();
    RollbackParams->SetStringField(TEXT("objectPath"), PathOf(Broken));
    RollbackParams->SetStringField(TEXT("componentName"), TEXT("FixtureComponent"));
    RollbackParams->SetStringField(TEXT("inheritedOverrideGuid"), Key.GetAssociatedGuid().ToString());
    RollbackParams->SetObjectField(TEXT("propertyDefaults"), Values);
    RollbackParams->SetBoolField(TEXT("createInheritedOverride"), true);
    RollbackParams->SetBoolField(TEXT("saveAfterEdit"), true);
    const auto Failed = Run(Tool, RollbackParams);
    if (!TestTrue(TEXT("Creation fails on compile and reports rollback"), Failed.Error.IsSet()
        && Failed.Error->Message.Contains(TEXT("compilation failed")) && Failed.Error->Message.Contains(TEXT("transaction rolled back"))))
    {
        if (Failed.Error.IsSet()) AddError(Failed.Error->Message);
        return false;
    }
    const auto* RolledBackHandler = Broken->GetInheritableComponentHandler(false);
    TestTrue(TEXT("Rollback removes the new editor override"),
        !RolledBackHandler || !RolledBackHandler->GetOverridenComponentTemplate(Key));
    auto* GeneratedClass = CastChecked<UBlueprintGeneratedClass>(Broken->GeneratedClass);
    const auto* GeneratedHandler = GeneratedClass->GetInheritableComponentHandler(false);
    TestTrue(TEXT("Rollback removes the new generated override"),
        !GeneratedHandler || !GeneratedHandler->GetOverridenComponentTemplate(Key));
    TestFalse(TEXT("Rollback restores clean child package"), Broken->GetOutermost()->IsDirty());
    TestFalse(TEXT("Failed creation never saves the child"), FPackageName::DoesPackageExist(Broken->GetOutermost()->GetName()));
    TestEqual(TEXT("Failed creation preserves the parent template"),
        CastChecked<USceneComponent>(Node->ComponentTemplate.Get())->GetRelativeLocation().Z, 0.0);
    auto BrokenVariable = MakeShared<FJsonObject>();
    BrokenVariable->SetStringField(TEXT("objectPath"), PathOf(Broken));
    BrokenVariable->SetStringField(TEXT("variableName"), TEXT("RollbackVariable"));
    BrokenVariable->SetStringField(TEXT("type"), TEXT("int"));
    BrokenVariable->SetBoolField(TEXT("compileAfterEdit"), true);
    BrokenVariable->SetBoolField(TEXT("saveAfterEdit"), true);
    const auto VariableFailure = Run(FAddBlueprintVariableTool(), BrokenVariable);
    TestTrue(TEXT("Failed variable compile reports rollback"), VariableFailure.Error.IsSet()
        && VariableFailure.Error->Message.Contains(TEXT("Variable transaction rolled back")));
    TestFalse(TEXT("Failed variable creation leaves no declaration"),
        Broken->NewVariables.ContainsByPredicate([](const auto& Variable) { return Variable.VarName == TEXT("RollbackVariable"); }));
    TestFalse(TEXT("Failed variable creation restores clean package"), Broken->GetOutermost()->IsDirty());
    TestFalse(TEXT("Failed variable creation never saves"), FPackageName::DoesPackageExist(Broken->GetOutermost()->GetName()));

    const FString TestDir = FPlatformMisc::GetEnvironmentVariable(TEXT("UNREALMCP_MERGE_TEST_DIR"));
    if (TestDir.IsEmpty()) return !HasAnyErrors();
    const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealMCP"));
    FPaths::NormalizeDirectoryName(TestRoot);
    FString NormalizedTestDir = TestDir;
    FPaths::NormalizeDirectoryName(NormalizedTestDir);
    if (!TestTrue(TEXT("Staged fixture requires an isolated test index and object store"),
        FPaths::GetPath(NormalizedTestDir).Equals(TestRoot, ESearchCase::IgnoreCase)
        && FPaths::GetCleanFilename(NormalizedTestDir).StartsWith(TEXT("InheritedMergeTest-"))
        && FPaths::IsSamePath(FPlatformMisc::GetEnvironmentVariable(TEXT("GIT_INDEX_FILE")), NormalizedTestDir / TEXT("index"))
        && FPaths::IsSamePath(FPlatformMisc::GetEnvironmentVariable(TEXT("GIT_OBJECT_DIRECTORY")), NormalizedTestDir / TEXT("objects"))))
        return false;
    auto Git = [&](const FString& Args, FString& Output)
    {
        const FString Command = FString::Printf(TEXT("-C \"%s\" %s"), *Project, *Args);
        FString Error;
        int32 ExitCode = -1;
        if (!FPlatformProcess::ExecProcess(TEXT("git"), *Command, &ExitCode, &Output, &Error) || ExitCode != 0)
        { AddError(FString::Printf(TEXT("Fixture Git command failed: %s"), *Error)); return false; }
        Output.TrimStartAndEndInline();
        return true;
    };

    UPackage* ChildPackage = Child->GetOutermost();
    const FString File = FPackageName::LongPackageNameToFilename(ChildPackage->GetName(), FPackageName::GetAssetPackageExtension());
    const FString Path = TEXT("Content/UnrealMCP_Automation/") + FPaths::GetCleanFilename(File);
    ON_SCOPE_EXIT
    {
        ResetLoaders(ChildPackage);
        if (IFileManager::Get().FileExists(*File))
            TestTrue(TEXT("Remove staged fixture asset"), IFileManager::Get().Delete(*File, false, true));
    };
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    auto SaveComparisonDependency = [&](UBlueprint* Asset, FString& OutFilename)
    {
        Compile(Asset);
        UPackage* Package = Asset->GetOutermost();
        OutFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        return UPackage::SavePackage(Package, Asset, *OutFilename, SaveArgs);
    };
    UBlueprint* CompareParent = Fixture.Create(TEXT("BP_CompareParent"), Parent->GeneratedClass);
    UBlueprint* CompareInterface = Fixture.Create(TEXT("BPI_CompareOnly"), UInterface::StaticClass(), BPTYPE_Interface);
    FString CompareParentFile, CompareInterfaceFile;
    ON_SCOPE_EXIT
    {
        for (const auto& Dependency : {TPair<UBlueprint*, FString>(CompareParent, CompareParentFile),
            TPair<UBlueprint*, FString>(CompareInterface, CompareInterfaceFile)})
            if (!Dependency.Value.IsEmpty() && IFileManager::Get().FileExists(*Dependency.Value))
            {
                ResetLoaders(Dependency.Key->GetOutermost());
                TestTrue(TEXT("Remove comparison dependency fixture"), IFileManager::Get().Delete(*Dependency.Value, false, true));
            }
    };
    if (!TestTrue(TEXT("Save comparison dependencies"),
        SaveComparisonDependency(CompareParent, CompareParentFile) && SaveComparisonDependency(CompareInterface, CompareInterfaceFile))) return false;
    auto* CompareFunction = FBlueprintEditorUtils::CreateNewGraph(
        Child, TEXT("CompareOnly"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Child, CompareFunction, true, nullptr);
    TArray<UK2Node_FunctionEntry*> CompareEntries;
    CompareFunction->GetNodesOfClass(CompareEntries);
    if (!TestEqual(TEXT("One staged signature entry"), CompareEntries.Num(), 1)) return false;
    FEdGraphPinType CompareInt;
    CompareInt.PinCategory = UEdGraphSchema_K2::PC_Int;
    auto* CompareEntry = CompareEntries[0];
    CompareEntry->CreateUserDefinedPin(TEXT("ChangeType"), CompareInt, EGPD_Output);
    auto* CompareBranch = Place<UK2Node_IfThenElse>(CompareFunction);
    auto* WideGraph = FBlueprintEditorUtils::CreateNewGraph(Child, TEXT("WideReview"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Child, WideGraph, true, nullptr);
    WideGraph->GraphGuid = FGuid(0xffffffff, 0xffffffff, 0xffffffff, 0xfffffffe);

    const bool bExerciseLfs = FPaths::IsSamePath(FPlatformMisc::GetEnvironmentVariable(TEXT("GIT_CONFIG_VALUE_0")),
        NormalizedTestDir / TEXT("lfs"));
    TArray<uint8> LfsBytes;
    FString LfsObjectFile;
    const FString PointerFile = NormalizedTestDir / TEXT("incoming-lfs-pointer");
    ON_SCOPE_EXIT
    {
        if (bExerciseLfs)
        {
            IFileManager::Get().Delete(*PointerFile, false, true);
            if (!LfsObjectFile.IsEmpty()) IFileManager::Get().Delete(*LfsObjectFile, false, true);
        }
    };
    FString Trees[3], StagedBlobIds[3], Blob, Ignored;
    TSet<FString> StageBlobs;
    if (!Git(TEXT("read-tree --empty"), Ignored)) return false;
    Params->SetBoolField(TEXT("saveAfterEdit"), true);
    for (int32 Side = 0; Side < 3; ++Side)
    {
        if (Side == 1)
        {
            Child->ParentClass = CompareParent->GeneratedClass;
            CompareEntry->UserDefinedPins[0]->PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
            CompareEntry->ReconstructNode();
        }
        if (Side == 2)
        {
            Child->ParentClass = Parent->GeneratedClass;
            CompareEntry->UserDefinedPins[0]->PinType = CompareInt;
            CompareEntry->ReconstructNode();
            if (!TestTrue(TEXT("Author incoming interface"), FBlueprintEditorUtils::ImplementNewInterface(Child,
                CompareInterface->GeneratedClass->GetClassPathName()))) return false;
            CompareEntry->FindPinChecked(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CompareBranch->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            for (int32 Index = 0; Index < 55; ++Index)
                Place<UK2Node_IfThenElse>(WideGraph)->NodeComment = FString::Printf(TEXT("Incoming branch %d"), Index);
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Child);
        Compile(Child);
        Values->SetStringField(TEXT("RelativeLocation"), FString::Printf(TEXT("(X=0,Y=0,Z=%d)"), 15 + 5 * Side));
        const auto Stage = Run(Tool, Params);
        if (!TestTrue(TEXT("Save inherited stage fixture"), Stage.Result.IsValid() && Stage.Result->GetBoolField(TEXT("saved"))))
        {
            if (Stage.Error.IsSet()) AddError(Stage.Error->Message);
            return false;
        }
        if (!Git(FString::Printf(TEXT("hash-object -w --no-filters -- \"%s\""), *Path), Blob)
            || !TestFalse(TEXT("Each stage has distinct saved bytes"), StageBlobs.Contains(Blob))) return false;
        if (Side == 2 && bExerciseLfs)
        {
            FString Pointer, Environment;
            if (!Git(FString::Printf(TEXT("lfs pointer --file=\"%s\""), *FPaths::ConvertRelativePathToFull(File)), Pointer)
                || !Git(TEXT("lfs env"), Environment)) return false;
            TArray<FString> PointerLines, EnvironmentLines;
            Pointer.ParseIntoArrayLines(PointerLines, true);
            Environment.ParseIntoArrayLines(EnvironmentLines, true);
            const FString* MediaLine = EnvironmentLines.FindByPredicate([](const FString& Line)
                { return Line.StartsWith(TEXT("LocalMediaDir=")); });
            const FString Media = MediaLine ? MediaLine->Mid(14) : FString();
            if (!TestTrue(TEXT("LFS fixture uses isolated media directory"), FPaths::IsSamePath(Media, NormalizedTestDir / TEXT("lfs/objects")))
                || !TestTrue(TEXT("LFS fixture produced object hash"), PointerLines.Num() == 3
                    && PointerLines[1].StartsWith(TEXT("oid sha256:")) && PointerLines[1].Len() == 75)) return false;
            const FString ObjectId = PointerLines[1].Mid(11);
            LfsObjectFile = Media / ObjectId.Left(2) / ObjectId.Mid(2, 2) / ObjectId;
            if (!FFileHelper::LoadFileToArray(LfsBytes, *File) || LfsBytes.IsEmpty()
                || !FFileHelper::SaveStringToFile(Pointer + TEXT("\n"), *PointerFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
                || !Git(FString::Printf(TEXT("hash-object -w --no-filters -- \"%s\""), *PointerFile), Blob)) return false;
        }
        StageBlobs.Add(Blob);
        StagedBlobIds[Side] = Blob;
        if (!Git(FString::Printf(TEXT("update-index --add --cacheinfo \"100644,%s,%s\""), *Blob, *Path), Ignored)
            || !Git(TEXT("write-tree"), Trees[Side])) return false;
    }
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *Trees[0], *Trees[1], *Trees[2]), Ignored)) return false;
    FString StagesBefore;
    if (!Git(FString::Printf(TEXT("ls-files -u -- \"%s\""), *Path), StagesBefore) || !TestFalse(TEXT("Three-way stages exist"), StagesBefore.IsEmpty())) return false;
    if (!VerifySavedReplay(*this, CompareEntry, Child, File, SaveArgs, bExerciseLfs, LfsObjectFile, LfsBytes, Path, StagedBlobIds, CompareFunction)) return false;
    FBlueprintEditorUtils::RemoveInterface(Child, CompareInterface->GeneratedClass->GetClassPathName(), false);
    CompareEntry->FindPinChecked(UEdGraphSchema_K2::PN_Then)->BreakAllPinLinks();
    FBlueprintEditorUtils::RemoveGraph(Child, CompareFunction);
    FBlueprintEditorUtils::RemoveGraph(Child, WideGraph);
    Child->GetInheritableComponentHandler(false)->RemoveOverridenComponentTemplate(Key);
    Compile(Child);
    if (!TestTrue(TEXT("Save working fixture without override"), UPackage::SavePackage(ChildPackage, Child, *File, SaveArgs))) return false;
    if (!Git(FString::Printf(TEXT("hash-object --no-filters -- \"%s\""), *Path), Blob)) return false;

    auto Comparison = MakeShared<FJsonObject>();
    Comparison->SetStringField(TEXT("path"), Path);
    Comparison->SetNumberField(TEXT("limit"), 200);
    const FCompareBlueprintMergeStructureTool ComparisonTool;
    if (bExerciseLfs)
    {
        const auto Missing = Run(ComparisonTool, Comparison);
        TestTrue(TEXT("Missing cached LFS object is unknown, never absent"), Missing.Result
            && !Missing.Result->GetBoolField(TEXT("evidenceComplete")) && Missing.Result->GetArrayField(TEXT("rows")).IsEmpty()
            && Missing.Result->GetObjectField(TEXT("stageStates"))->GetObjectField(TEXT("incoming"))->GetStringField(TEXT("error")).Contains(TEXT("missing locally")));
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(LfsObjectFile), true)) return false;
        LfsBytes[0] ^= 0xff;
        if (!FFileHelper::SaveArrayToFile(LfsBytes, *LfsObjectFile)) return false;
        const auto Corrupt = Run(ComparisonTool, Comparison);
        TestTrue(TEXT("Same-size corrupt LFS bytes fail SHA-256 verification"), Corrupt.Result
            && !Corrupt.Result->GetBoolField(TEXT("evidenceComplete"))
            && Corrupt.Result->GetObjectField(TEXT("stageStates"))->GetObjectField(TEXT("incoming"))->GetStringField(TEXT("error")).Contains(TEXT("SHA-256 verification")));
        LfsBytes[0] ^= 0xff;
        if (!FFileHelper::SaveArrayToFile(LfsBytes, *LfsObjectFile)) return false;
    }
    auto CommitTree = [&](const FString& Tree, const FString& ParentCommit, const TCHAR* Subject, FString& OutCommit)
    {
        const FString ParentArg = ParentCommit.IsEmpty() ? FString() : TEXT(" -p ") + ParentCommit;
        return Git(FString::Printf(TEXT("-c user.name=UnrealMCP -c user.email=unrealmcp@example.invalid -c commit.gpgSign=false commit-tree %s%s -m \"%s\""),
            *Tree, *ParentArg, Subject), OutCommit);
    };
    FString HistoricalCommit, RenamedCommit, HistoricalBase, HistoricalRenameTree;
    const FString HistoricalRenamePath = TEXT("Content/UnrealMCP_Automation/") + FPaths::GetBaseFilename(Path) + TEXT("_Historical.uasset");
    if (!CommitTree(Trees[0], FString(), TEXT("Historical base"), HistoricalBase)
        || !CommitTree(Trees[2], HistoricalBase, TEXT("Historical graph"), HistoricalCommit)
        || !Git(FString::Printf(TEXT("read-tree %s"), *Trees[2]), Ignored)
        || !Git(FString::Printf(TEXT("update-index --force-remove -- \"%s\""), *Path), Ignored)
        || !Git(FString::Printf(TEXT("update-index --add --cacheinfo \"100644,%s,%s\""), *StagedBlobIds[2], *HistoricalRenamePath), Ignored)
        || !Git(TEXT("write-tree"), HistoricalRenameTree)
        || !CommitTree(HistoricalRenameTree, HistoricalCommit, TEXT("Historical rename"), RenamedCommit)
        || !Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *Trees[0], *Trees[1], *Trees[2]), Ignored)) return false;

    using HistoryOp = FUnrealMCPGitHistoryTool::EOperation;
    const FUnrealMCPGitHistoryTool HistoryTool(HistoryOp::History);
    const FUnrealMCPGitHistoryTool ChangesTool(HistoryOp::Changes);
    const FUnrealMCPGitHistoryTool InspectTool(HistoryOp::BlueprintGraph);
    auto History = MakeShared<FJsonObject>();
    History->SetStringField(TEXT("commit"), RenamedCommit);
    History->SetNumberField(TEXT("limit"), 1);
    const auto HistoryFirst = Run(HistoryTool, History);
    if (HistoryFirst.Error.IsSet()) { AddError(HistoryFirst.Error->Message); return false; }
    TestTrue(TEXT("Historical commit has more rows"), HistoryFirst.Result->HasField(TEXT("nextOffset")));
    History->SetNumberField(TEXT("offset"), 1);
    const auto HistoryNext = Run(HistoryTool, History);
    if (HistoryNext.Error.IsSet()) { AddError(HistoryNext.Error->Message); return false; }
    if (TestEqual(TEXT("History returns one anchored row"), HistoryNext.Result->GetArrayField(TEXT("rows")).Num(), 1))
        TestEqual(TEXT("Anchored next row is the parent commit"),
            HistoryNext.Result->GetArrayField(TEXT("rows"))[0]->AsArray()[0]->AsString(), HistoricalCommit);
    auto Changes = MakeShared<FJsonObject>();
    Changes->SetStringField(TEXT("commit"), RenamedCommit);
    Changes->SetBoolField(TEXT("includePatch"), true);
    for (const FString& Endpoint : {Path, HistoricalRenamePath})
    {
        Changes->SetStringField(TEXT("path"), Endpoint);
        const auto RenamedChanges = Run(ChangesTool, Changes);
        if (RenamedChanges.Error.IsSet()) { AddError(RenamedChanges.Error->Message); return false; }
        const auto& RenameRows = RenamedChanges.Result->GetArrayField(TEXT("rows"));
        if (TestEqual(TEXT("Exact path filter returns one rename"), RenameRows.Num(), 1))
        {
            TestTrue(TEXT("Filtered rename retains both endpoints"), RenameRows[0]->AsArray()[0]->AsString().StartsWith(TEXT("R"))
                && RenameRows[0]->AsArray()[1]->AsString() == HistoricalRenamePath && RenameRows[0]->AsArray()[2]->AsString() == Path);
            TestTrue(TEXT("Exact-path patch retains rename headers"), RenamedChanges.Result->GetStringField(TEXT("patch")).Contains(TEXT("rename from")));
        }
    }
    auto Inspect = MakeShared<FJsonObject>();
    Inspect->SetStringField(TEXT("commit"), HistoricalCommit);
    Inspect->SetStringField(TEXT("path"), Path);
    const auto HistoricalOutline = Run(InspectTool, Inspect);
    if (HistoricalOutline.Error.IsSet()) { AddError(HistoricalOutline.Error->Message); return false; }
    TestTrue(TEXT("Commit reader returns complete graph evidence from the staged blob"), HistoricalOutline.Result->GetBoolField(TEXT("evidenceComplete"))
        && HistoricalOutline.Result->GetStringField(TEXT("blobId")) == StagedBlobIds[2]);
    Inspect->SetStringField(TEXT("graphGuid"), CompareFunction->GraphGuid.ToString());
    Inspect->SetStringField(TEXT("nodeGuid"), CompareEntry->NodeGuid.ToString());
    const auto HistoricalPins = Run(InspectTool, Inspect);
    if (HistoricalPins.Error.IsSet()) { AddError(HistoricalPins.Error->Message); return false; }
    const auto* HistoricalThen = HistoricalPins.Result->GetArrayField(TEXT("rows")).FindByPredicate([](const auto& Pin)
        { return Pin->AsArray()[1]->AsString() == UEdGraphSchema_K2::PN_Then.ToString(); });
    if (!TestNotNull(TEXT("Historical link source pin is listed"), HistoricalThen)) return false;
    Inspect->SetStringField(TEXT("pinId"), (*HistoricalThen)->AsArray()[0]->AsString());
    Inspect->SetStringField(TEXT("commit"), RenamedCommit);
    Inspect->SetStringField(TEXT("path"), HistoricalRenamePath);
    const auto HistoricalLinks = Run(InspectTool, Inspect);
    if (HistoricalLinks.Error.IsSet()) { AddError(HistoricalLinks.Error->Message); return false; }
    const auto& LinkRows = HistoricalLinks.Result->GetArrayField(TEXT("rows"));
    if (TestEqual(TEXT("Commit pin route finds the historical edge after a rename"), LinkRows.Num(), 1))
        TestEqual(TEXT("Commit link identifies the exact historic target"), LinkRows[0]->AsArray()[1]->AsString(), CompareBranch->NodeGuid.ToString());
    AddInfo(TEXT("Isolated commit history, exact-path rename and pin link review completed."));

    ChildPackage->SetDirtyFlag(true);
    const auto Structure = Run(ComparisonTool, Comparison);
    TestTrue(TEXT("Historical comparison preserves working dirty flag"), ChildPackage->IsDirty());
    ChildPackage->SetDirtyFlag(false);
    if (Structure.Error.IsSet()) { AddError(Structure.Error->Message); return false; }
    if (!TestNotNull(TEXT("Compare staged Blueprint structure"), Structure.Result.Get())) return false;
    const auto& StructureRows = Structure.Result->GetArrayField(TEXT("rows"));
    auto FindRow = [](const TArray<TSharedPtr<FJsonValue>>& Rows, const FString& Identity)
    {
        return Rows.FindByPredicate([&](const auto& Value) { return Value->AsArray()[0]->AsString() == Identity; });
    };
    const auto* ParentRow = FindRow(StructureRows, TEXT("parentClass"));
    if (TestNotNull(TEXT("Parent change row"), ParentRow))
        TestEqual(TEXT("Exact current parent class"), (*ParentRow)->AsArray()[2]->AsString(), CompareParent->GeneratedClass->GetPathName());
    const auto* InterfaceRow = FindRow(StructureRows, TEXT("interface/") + CompareInterface->GeneratedClass->GetPathName());
    if (TestNotNull(TEXT("Added interface row"), InterfaceRow))
        TestTrue(TEXT("Interface added on incoming only"), (*InterfaceRow)->AsArray()[1]->IsNull()
            && (*InterfaceRow)->AsArray()[2]->IsNull() && (*InterfaceRow)->AsArray()[3]->AsString() == TEXT("implemented"));
    const auto* SignatureRow = FindRow(StructureRows, TEXT("function/") + CompareFunction->GraphGuid.ToString() + TEXT("/input/ChangeType/type"));
    if (TestNotNull(TEXT("Parameter type row"), SignatureRow))
        TestTrue(TEXT("Exact signature types"), (*SignatureRow)->AsArray()[1]->AsString().Contains(TEXT("int"))
            && (*SignatureRow)->AsArray()[2]->AsString().Contains(TEXT("bool")));
    TestTrue(TEXT("Inherited component override row"), StructureRows.ContainsByPredicate([&](const auto& Value)
    {
        const auto& Row = Value->AsArray();
        return Row[0]->AsString().StartsWith(TEXT("componentOverride/")) && Row[0]->AsString().EndsWith(TEXT("/property/RelativeLocation"))
            && Row[1]->AsString().Contains(TEXT("15")) && Row[2]->AsString().Contains(TEXT("20")) && Row[3]->AsString().Contains(TEXT("25"));
    }));
    TestTrue(TEXT("Direct references could be compared"), Structure.Result->GetBoolField(TEXT("directReferencesCompared")));
    TestTrue(TEXT("Historical direct asset import row"), StructureRows.ContainsByPredicate([&](const auto& Value)
    {
        const auto& Row = Value->AsArray();
        return Row[0]->AsString().StartsWith(TEXT("reference/hard/"))
            && Row[0]->AsString().Contains(CompareInterface->GetOutermost()->GetName())
            && Row[1]->IsNull() && Row[3]->AsString() == TEXT("imported");
    }));
    TestFalse(TEXT("Structural mode excludes graph body rows"), Structure.Result->GetBoolField(TEXT("graphBodiesCompared")));
    Comparison->SetBoolField(TEXT("includeGraphBodies"), true);
    const auto Whole = Run(ComparisonTool, Comparison);
    if (Whole.Error.IsSet()) { AddError(Whole.Error->Message); return false; }
    TestTrue(TEXT("Graph mode includes exact new edge"), Whole.Result->GetArrayField(TEXT("rows")).ContainsByPredicate([&](const auto& Value)
    {
        const auto& Row = Value->AsArray();
        const FString Identity = Row[0]->AsString();
        return Identity.StartsWith(TEXT("graph/") + CompareFunction->GraphGuid.ToString()) && Identity.Contains(TEXT("/link/"))
            && Identity.Contains(CompareBranch->NodeGuid.ToString()) && Row[1]->IsNull() && Row[3]->AsString() == TEXT("connected");
    }));
    Comparison->SetNumberField(TEXT("limit"), 1);
    const auto FirstPage = Run(ComparisonTool, Comparison);
    if (FirstPage.Error.IsSet()) { AddError(FirstPage.Error->Message); return false; }
    Comparison->SetNumberField(TEXT("offset"), FirstPage.Result->GetIntegerField(TEXT("nextOffset")));
    Comparison->SetStringField(TEXT("expectedStageRevision"), TEXT("stale"));
    TestTrue(TEXT("Reject stale historical page"), Run(ComparisonTool, Comparison).Error.IsSet());
    Comparison->SetStringField(TEXT("expectedStageRevision"), FirstPage.Result->GetStringField(TEXT("stageRevision")));
    const auto Next = Run(ComparisonTool, Comparison);
    if (Next.Error.IsSet()) { AddError(Next.Error->Message); return false; }
    TestEqual(TEXT("Stable next historical row"), Next.Result->GetArrayField(TEXT("rows"))[0]->AsArray()[0]->AsString(),
        Whole.Result->GetArrayField(TEXT("rows"))[1]->AsArray()[0]->AsString());
    Comparison->RemoveField(TEXT("offset"));
    Comparison->RemoveField(TEXT("expectedStageRevision"));
    Comparison->SetNumberField(TEXT("limit"), 200);
    TestTrue(TEXT("Large review exceeds one page"), Whole.Result->GetIntegerField(TEXT("total")) > 200);
    TSet<FString> Identities;
    auto Page = Whole.Result;
    FString Cursor;
    while (true)
    {
        for (const auto& Row : Page->GetArrayField(TEXT("rows")))
        {
            const FString Identity = Row->AsArray()[0]->AsString();
            TestFalse(TEXT("Cursor never repeats a row"), Identities.Contains(Identity));
            Identities.Add(Identity);
        }
        if (!Page->TryGetStringField(TEXT("nextCursor"), Cursor)) break;
        Comparison->SetStringField(TEXT("cursor"), Cursor);
        const auto Continued = Run(ComparisonTool, Comparison);
        if (Continued.Error.IsSet()) { AddError(Continued.Error->Message); return false; }
        Page = Continued.Result;
    }
    TestEqual(TEXT("Cursor traverses the entire review"), Identities.Num(), Whole.Result->GetIntegerField(TEXT("total")));
    Comparison->SetBoolField(TEXT("includeGraphBodies"), false);
    TestTrue(TEXT("Changing graph coverage invalidates cursor"), Run(ComparisonTool, Comparison).Error.IsSet());
    Comparison->RemoveField(TEXT("cursor"));

    FString EmptyTree;
    if (!Git(TEXT("read-tree --empty"), Ignored) || !Git(TEXT("write-tree"), EmptyTree)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *Trees[0], *EmptyTree, *Trees[2]), Ignored)) return false;
    const auto Deleted = Run(ComparisonTool, Comparison);
    if (Deleted.Error.IsSet()) { AddError(Deleted.Error->Message); return false; }
    TestTrue(TEXT("Delete conflict requires explicit resolution"), Deleted.Result->GetBoolField(TEXT("requiresExplicitResolution")));
    TestEqual(TEXT("Deleted stage is explicitly absent"),
        Deleted.Result->GetObjectField(TEXT("stageStates"))->GetObjectField(TEXT("current"))->GetStringField(TEXT("state")), FString(TEXT("absent")));
    TestTrue(TEXT("Absent blob is null"), Deleted.Result->GetObjectField(TEXT("stageBlobs"))->TryGetField(TEXT("current"))->IsNull());
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *EmptyTree, *Trees[1], *Trees[2]), Ignored)) return false;
    const auto Added = Run(ComparisonTool, Comparison);
    if (Added.Error.IsSet()) { AddError(Added.Error->Message); return false; }
    TestEqual(TEXT("Add/add conflict explicitly lacks a base"),
        Added.Result->GetObjectField(TEXT("stageStates"))->GetObjectField(TEXT("base"))->GetStringField(TEXT("state")), FString(TEXT("absent")));
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *Trees[0], *Trees[1], *Trees[2]), Ignored)) return false;

    const FString RenamedPath = TEXT("Content/UnrealMCP_Automation/") + FPaths::GetBaseFilename(Path) + TEXT("_Renamed.uasset");
    FString RenamedTrees[3];
    for (int32 Side = 0; Side < 3; ++Side)
    {
        if (!Git(FString::Printf(TEXT("read-tree %s"), *Trees[Side]), Ignored)
            || !Git(FString::Printf(TEXT("update-index --add --cacheinfo \"100644,%s,%s\""), *StagedBlobIds[(Side + 1) % 3], *RenamedPath), Ignored)
            || !Git(TEXT("write-tree"), RenamedTrees[Side])) return false;
    }
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *RenamedTrees[0], *RenamedTrees[1], *RenamedTrees[2]), Ignored)) return false;
    auto AlternatePaths = MakeShared<FJsonObject>();
    AlternatePaths->SetStringField(TEXT("current"), RenamedPath);
    Comparison->SetObjectField(TEXT("stagePaths"), AlternatePaths);
    const auto RenamedReview = Run(ComparisonTool, Comparison);
    if (RenamedReview.Error.IsSet()) { AddError(RenamedReview.Error->Message); return false; }
    TestTrue(TEXT("Cross-path stages require an explicit resolution"), RenamedReview.Result->GetBoolField(TEXT("requiresExplicitResolution")));
    TestEqual(TEXT("Current stage resolves the caller-selected path"),
        RenamedReview.Result->GetObjectField(TEXT("stageBlobs"))->GetStringField(TEXT("current")), StagedBlobIds[2]);
    TestEqual(TEXT("Current stage reports its actual path"),
        RenamedReview.Result->GetObjectField(TEXT("stageStates"))->GetObjectField(TEXT("current"))->GetStringField(TEXT("path")), RenamedPath);
    Comparison->RemoveField(TEXT("stagePaths"));

    const FString MissingPackage = FPaths::GetPath(ChildPackage->GetName()) / TEXT("MissingReview_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString MissingObject = MissingPackage + TEXT(".MissingDependency");
    FEdGraphPinType SoftType = ObjectType(UObject::StaticClass());
    SoftType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
    if (!TestTrue(TEXT("Create soft dependency fixture"), FBlueprintEditorUtils::AddMemberVariable(Child, TEXT("SoftReviewDependency"), SoftType))) return false;
    Compile(Child);
    auto* SoftProperty = FindFProperty<FSoftObjectProperty>(Child->GeneratedClass, TEXT("SoftReviewDependency"));
    if (!TestNotNull(TEXT("Compiled soft dependency property"), SoftProperty)) return false;
    SoftProperty->SetPropertyValue_InContainer(Child->GeneratedClass->GetDefaultObject(), FSoftObjectPtr(FSoftObjectPath(MissingObject)));
    if (!TestTrue(TEXT("Save missing dependency fixture"), UPackage::SavePackage(ChildPackage, Child, *File, SaveArgs))) return false;
    FString MissingBlob, MissingTree;
    if (!Git(FString::Printf(TEXT("hash-object -w --no-filters -- \"%s\""), *Path), MissingBlob)
        || !Git(FString::Printf(TEXT("read-tree %s"), *Trees[2]), Ignored)
        || !Git(FString::Printf(TEXT("update-index --add --cacheinfo \"100644,%s,%s\""), *MissingBlob, *Path), Ignored)
        || !Git(TEXT("write-tree"), MissingTree)
        || !Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *Trees[0], *Trees[1], *MissingTree), Ignored)) return false;
    const auto IncompleteReview = Run(ComparisonTool, Comparison);
    if (IncompleteReview.Error.IsSet()) { AddError(IncompleteReview.Error->Message); return false; }
    TestFalse(TEXT("Missing dependency never appears as complete evidence"), IncompleteReview.Result->GetBoolField(TEXT("evidenceComplete")));
    const auto MissingSide = IncompleteReview.Result->GetObjectField(TEXT("stageStates"))->GetObjectField(TEXT("incoming"));
    const TArray<TSharedPtr<FJsonValue>>* MissingNames;
    TestTrue(TEXT("Missing dependency names the saved package"), MissingSide->TryGetArrayField(TEXT("missingDependencies"), MissingNames)
        && MissingNames->ContainsByPredicate([&](const auto& Value) { return Value->AsString() == MissingPackage; }));
    FString MissingCommit;
    if (!CommitTree(MissingTree, HistoricalCommit, TEXT("Missing historical dependency"), MissingCommit)) return false;
    auto MissingInspect = MakeShared<FJsonObject>();
    MissingInspect->SetStringField(TEXT("commit"), MissingCommit);
    MissingInspect->SetStringField(TEXT("path"), Path);
    const auto HistoricalMissing = Run(InspectTool, MissingInspect);
    TestTrue(TEXT("Historical commit names missing dependency and flags incomplete evidence"),
        HistoricalMissing.Error.IsSet() && HistoricalMissing.Error->Data
        && !HistoricalMissing.Error->Data->GetBoolField(TEXT("evidenceComplete"))
        && HistoricalMissing.Error->Data->GetArrayField(TEXT("missingDependencies")).ContainsByPredicate([&](const auto& Value)
            { return Value->AsString() == MissingPackage; }));
    FBlueprintEditorUtils::RemoveMemberVariable(Child, TEXT("SoftReviewDependency"));
    Compile(Child);
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *Trees[0], *Trees[1], *Trees[2]), Ignored)) return false;
    if (!TestTrue(TEXT("Restore clean working fixture"), UPackage::SavePackage(ChildPackage, Child, *File, SaveArgs))) return false;
    if (!Git(FString::Printf(TEXT("hash-object --no-filters -- \"%s\""), *Path), Blob)) return false;

    FString ComparedHash, ComparedStages;
    if (!Git(FString::Printf(TEXT("hash-object --no-filters -- \"%s\""), *Path), ComparedHash)
        || !Git(FString::Printf(TEXT("ls-files -u -- \"%s\""), *Path), ComparedStages)) return false;
    TestEqual(TEXT("Historical comparison preserves working asset bytes"), ComparedHash, Blob);
    TestEqual(TEXT("Historical comparison preserves Git stages"), ComparedStages, StagesBefore);
    AddInfo(TEXT("Isolated Git-stage whole Blueprint comparison completed."));
    auto Merge = MakeShared<FJsonObject>();
    auto MergeValues = MakeShared<FJsonObject>();
    MergeValues->SetStringField(TEXT("RelativeLocation"), TEXT("(X=0,Y=0,Z=35)"));
    Merge->SetObjectField(TEXT("values"), MergeValues);
    Merge->SetStringField(TEXT("path"), Path);
    Merge->SetStringField(TEXT("componentName"), TEXT("FixtureComponent"));
    Merge->SetStringField(TEXT("inheritedOverrideGuid"), Key.GetAssociatedGuid().ToString());
    Merge->SetBoolField(TEXT("createInheritedOverride"), true);
    Merge->SetBoolField(TEXT("save"), true);
    const FApplyBlueprintMergeTool MergeTool(FApplyBlueprintMergeTool::EOperation::ComponentDefaults);
    const auto MergePreview = Run(MergeTool, Merge);
    if (!TestTrue(TEXT("Staged inherited preview succeeds"), MergePreview.Result.IsValid()))
    {
        if (MergePreview.Error.IsSet()) AddError(MergePreview.Error->Message);
        return false;
    }
    TestTrue(TEXT("Merge preview reports creation"), MergePreview.Result->GetBoolField(TEXT("wouldCreateOverride")));
    TestNull(TEXT("Merge preview leaves override absent"), ReadChild());
    Merge->SetStringField(TEXT("previewToken"), MergePreview.Result->GetStringField(TEXT("previewToken")));
    Merge->SetBoolField(TEXT("dryRun"), false);
    Merge->SetBoolField(TEXT("confirm"), true);
    const auto MergeApply = Run(MergeTool, Merge);
    if (!TestTrue(TEXT("Staged inherited apply succeeds"), MergeApply.Result.IsValid()))
    {
        if (MergeApply.Error.IsSet()) AddError(MergeApply.Error->Message);
        return false;
    }
    TestTrue(TEXT("Merge receipt confirms compilation"), MergeApply.Result->GetBoolField(TEXT("compiled")));
    TestTrue(TEXT("Merge receipt confirms creation"), MergeApply.Result->GetBoolField(TEXT("createdOverride")));
    TestTrue(TEXT("Merge receipt confirms save"), MergeApply.Result->GetBoolField(TEXT("saved")));
    TestFalse(TEXT("Merge receipt confirms no staging"), MergeApply.Result->GetBoolField(TEXT("staged")));
    TestFalse(TEXT("Saved working child is clean"), ChildPackage->IsDirty());
    auto* MergedOverride = ReadChild();
    if (!TestNotNull(TEXT("Merged override exists"), MergedOverride)) return false;
    TestEqual(TEXT("Merge apply readback"), MergedOverride->GetRelativeLocation().Z, 35.0);
    TestEqual(TEXT("Merge preserves the parent's template"), CastChecked<USceneComponent>(Node->ComponentTemplate.Get())->GetRelativeLocation().Z, 0.0);
    FString StagesAfter, SavedHash;
    if (!Git(FString::Printf(TEXT("ls-files -u -- \"%s\""), *Path), StagesAfter)
        || !Git(FString::Printf(TEXT("hash-object --no-filters -- \"%s\""), *Path), SavedHash)) return false;
    TestEqual(TEXT("Merge leaves all Git stages unchanged"), StagesAfter, StagesBefore);
    TestNotEqual(TEXT("Save changes working file bytes"), SavedHash, Blob);
    AddInfo(TEXT("Isolated Git-stage inherited merge preview/apply completed."));

    auto* Interface = Fixture.Create(TEXT("BPI_Merge"), UInterface::StaticClass(), BPTYPE_Interface);
    UEdGraph* Signature = FBlueprintEditorUtils::CreateNewGraph(
        Interface, TEXT("MergeFixtureFunction"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Interface, Signature, true, nullptr);
    Compile(Interface);
    const FApplyBlueprintMergeTool InterfaceTool(FApplyBlueprintMergeTool::EOperation::AddInterface);
    auto InterfacePlan = MakeShared<FJsonObject>();
    InterfacePlan->SetStringField(TEXT("path"), Path);
    InterfacePlan->SetStringField(TEXT("interfaceClassPath"), Interface->GeneratedClass->GetPathName());
    InterfacePlan->SetBoolField(TEXT("save"), true);
    TestTrue(TEXT("Saving a merge with an unsaved interface is refused"), Run(InterfaceTool, InterfacePlan).Error.IsSet());
    UPackage* InterfacePackage = Interface->GetOutermost();
    const FString InterfaceFile = FPackageName::LongPackageNameToFilename(InterfacePackage->GetName(), FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT
    {
        ResetLoaders(InterfacePackage);
        if (IFileManager::Get().FileExists(*InterfaceFile))
            TestTrue(TEXT("Remove temporary interface fixture"), IFileManager::Get().Delete(*InterfaceFile, false, true));
    };
    if (!TestTrue(TEXT("Save temporary interface"), UPackage::SavePackage(InterfacePackage, Interface, *InterfaceFile, SaveArgs))) return false;
    const auto InterfacePreview = Run(InterfaceTool, InterfacePlan);
    if (!TestTrue(TEXT("Staged interface preview succeeds"), InterfacePreview.Result.IsValid()))
    {
        if (InterfacePreview.Error.IsSet()) AddError(InterfacePreview.Error->Message);
        return false;
    }
    TestTrue(TEXT("Interface preview reports addition"), InterfacePreview.Result->GetBoolField(TEXT("wouldAdd")));
    TestTrue(TEXT("Interface preview preserves declarations"), Child->ImplementedInterfaces.IsEmpty());
    InterfacePlan->SetBoolField(TEXT("dryRun"), false);
    InterfacePlan->SetBoolField(TEXT("confirm"), true);
    InterfacePlan->SetStringField(TEXT("previewToken"), InterfacePreview.Result->GetStringField(TEXT("previewToken")));
    const auto InterfaceApply = Run(InterfaceTool, InterfacePlan);
    if (!TestTrue(TEXT("Staged interface apply succeeds"), InterfaceApply.Result.IsValid()))
    {
        if (InterfaceApply.Error.IsSet()) AddError(InterfaceApply.Error->Message);
        return false;
    }
    TestTrue(TEXT("Interface added, compiled and saved"), InterfaceApply.Result->GetBoolField(TEXT("added"))
        && InterfaceApply.Result->GetBoolField(TEXT("compiled")) && InterfaceApply.Result->GetBoolField(TEXT("saved")));
    TestTrue(TEXT("Compiled child implements the interface"), Child->GeneratedClass->ImplementsInterface(Interface->GeneratedClass));
    TestFalse(TEXT("Interface merge leaves package clean"), ChildPackage->IsDirty());
    TestTrue(TEXT("Old interface token rejected after apply"), Run(InterfaceTool, InterfacePlan).Error.IsSet());
    if (!Git(FString::Printf(TEXT("ls-files -u -- \"%s\""), *Path), StagesAfter)) return false;
    TestEqual(TEXT("Interface merge preserves Git stages"), StagesAfter, StagesBefore);
    AddInfo(TEXT("Isolated Git-stage interface merge preview/apply completed."));

    const FApplyBlueprintMergeTool VariableMergeTool(FApplyBlueprintMergeTool::EOperation::AddVariable);
    auto VariablePlan = MakeShared<FJsonObject>();
    VariablePlan->SetStringField(TEXT("path"), Path);
    VariablePlan->SetBoolField(TEXT("save"), true);
    auto Declaration = MakeShared<FJsonObject>();
    Declaration->SetStringField(TEXT("variableName"), TEXT("MergeCounter"));
    Declaration->SetStringField(TEXT("type"), TEXT("int"));
    Declaration->SetStringField(TEXT("defaultValue"), TEXT("7"));
    VariablePlan->SetObjectField(TEXT("declaration"), Declaration);
    const auto VariablePreview = Run(VariableMergeTool, VariablePlan);
    if (!TestTrue(TEXT("Staged variable preview succeeds"), VariablePreview.Result.IsValid()))
    {
        if (VariablePreview.Error.IsSet()) AddError(VariablePreview.Error->Message);
        return false;
    }
    TestTrue(TEXT("Variable preview reports addition"), VariablePreview.Result->GetBoolField(TEXT("wouldAdd")));
    TestNull(TEXT("Preview leaves compiled property absent"), FindFProperty<FProperty>(Child->GeneratedClass, TEXT("MergeCounter")));
    VariablePlan->SetBoolField(TEXT("dryRun"), false);
    VariablePlan->SetBoolField(TEXT("confirm"), true);
    VariablePlan->SetStringField(TEXT("previewToken"), VariablePreview.Result->GetStringField(TEXT("previewToken")));
    const auto VariableApply = Run(VariableMergeTool, VariablePlan);
    if (!TestTrue(TEXT("Staged variable apply succeeds"), VariableApply.Result.IsValid()))
    {
        if (VariableApply.Error.IsSet()) AddError(VariableApply.Error->Message);
        return false;
    }
    TestTrue(TEXT("Member declaration added, compiled and saved"), VariableApply.Result->GetBoolField(TEXT("added"))
        && VariableApply.Result->GetBoolField(TEXT("compiled")) && VariableApply.Result->GetBoolField(TEXT("saved")));
    const auto* Counter = FindFProperty<FIntProperty>(Child->GeneratedClass, TEXT("MergeCounter"));
    if (!TestNotNull(TEXT("Compiled integer declaration"), Counter)) return false;
    TestEqual(TEXT("Compiled member default"), Counter->GetPropertyValue_InContainer(Child->GeneratedClass->GetDefaultObject()), 7);
    TestTrue(TEXT("Existing variable cannot be recreated"), Run(VariableMergeTool, VariablePlan).Error.IsSet());
    TestFalse(TEXT("Variable merge leaves package clean"), ChildPackage->IsDirty());
    if (!Git(FString::Printf(TEXT("ls-files -u -- \"%s\""), *Path), StagesAfter)) return false;
    TestEqual(TEXT("Variable merge preserves Git stages"), StagesAfter, StagesBefore);
    const FApplyBlueprintMergeTool StructureTool(FApplyBlueprintMergeTool::EOperation::Structure);
    auto StructurePlan = MakeShared<FJsonObject>();
    StructurePlan->SetStringField(TEXT("path"), Path);
    StructurePlan->SetBoolField(TEXT("save"), true);
    auto Rename = MakeShared<FJsonObject>();
    Rename->SetStringField(TEXT("kind"), TEXT("renameVariable"));
    Rename->SetStringField(TEXT("variableName"), TEXT("MergeCounter"));
    Rename->SetStringField(TEXT("newName"), TEXT("MergedCounter"));
    auto RemoveInterface = MakeShared<FJsonObject>();
    RemoveInterface->SetStringField(TEXT("kind"), TEXT("removeInterface"));
    RemoveInterface->SetStringField(TEXT("interfaceClassPath"), Interface->GeneratedClass->GetPathName());
    StructurePlan->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Rename), MakeShared<FJsonValueObject>(RemoveInterface)});
    const auto StructurePreview = Run(StructureTool, StructurePlan);
    if (StructurePreview.Error.IsSet()) { AddError(StructurePreview.Error->Message); return false; }
    TestNotNull(TEXT("Structure preview leaves member intact"), FindFProperty<FProperty>(Child->GeneratedClass, TEXT("MergeCounter")));
    TestTrue(TEXT("Structure preview leaves interface intact"), Child->GeneratedClass->ImplementsInterface(Interface->GeneratedClass));
    StructurePlan->SetBoolField(TEXT("dryRun"), false);
    StructurePlan->SetBoolField(TEXT("confirm"), true);
    StructurePlan->SetStringField(TEXT("previewToken"), StructurePreview.Result->GetStringField(TEXT("previewToken")));
    Rename->SetStringField(TEXT("newName"), TEXT("DifferentCounter"));
    TestTrue(TEXT("Changed operation invalidates preview"), Run(StructureTool, StructurePlan).Error.IsSet());
    Rename->SetStringField(TEXT("newName"), TEXT("MergedCounter"));
    const auto StructureApply = Run(StructureTool, StructurePlan);
    if (StructureApply.Error.IsSet()) { AddError(StructureApply.Error->Message); return false; }
    TestTrue(TEXT("Mixed plan applied"), StructureApply.Result->GetBoolField(TEXT("applied")));
    const auto& Receipt = StructureApply.Result->GetArrayField(TEXT("packages"))[0]->AsObject();
    TestTrue(TEXT("Mixed plan compiled and saved"), Receipt->GetBoolField(TEXT("compiled")) && Receipt->GetBoolField(TEXT("saved")));
    TestFalse(TEXT("Mixed plan removed interface"), Child->GeneratedClass->ImplementsInterface(Interface->GeneratedClass));
    const auto* Renamed = FindFProperty<FIntProperty>(Child->GeneratedClass, TEXT("MergedCounter"));
    if (!TestNotNull(TEXT("Mixed plan renamed variable"), Renamed)) return false;
    TestEqual(TEXT("Renamed variable retains default"), Renamed->GetPropertyValue_InContainer(Child->GeneratedClass->GetDefaultObject()), 7);
    if (!Git(FString::Printf(TEXT("ls-files -u -- \"%s\""), *Path), StagesAfter)) return false;
    TestEqual(TEXT("Mixed plan preserves Git stages"), StagesAfter, StagesBefore);
    AddInfo(TEXT("Isolated Git-stage structural merge preview/apply completed."));

    auto TypeEdit = MakeShared<FJsonObject>();
    TypeEdit->SetStringField(TEXT("kind"), TEXT("setVariableType"));
    TypeEdit->SetStringField(TEXT("variableName"), TEXT("MergedCounter"));
    TypeEdit->SetStringField(TEXT("type"), TEXT("string"));
    TypeEdit->SetStringField(TEXT("defaultValue"), TEXT("preserved"));
    StructurePlan->SetBoolField(TEXT("dryRun"), true);
    StructurePlan->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(TypeEdit)});
    const auto TypePreview = Run(StructureTool, StructurePlan);
    if (TypePreview.Error.IsSet()) { AddError(TypePreview.Error->Message); return false; }
    StructurePlan->SetStringField(TEXT("previewToken"), TypePreview.Result->GetStringField(TEXT("previewToken")));
    StructurePlan->SetBoolField(TEXT("dryRun"), false);
    const auto TypeApply = Run(StructureTool, StructurePlan);
    if (TypeApply.Error.IsSet()) { AddError(TypeApply.Error->Message); return false; }
    const auto* StringVariable = FindFProperty<FStrProperty>(Child->GeneratedClass, TEXT("MergedCounter"));
    if (!TestNotNull(TEXT("Variable type changed"), StringVariable)) return false;
    TestEqual(TEXT("Type change applies explicit default"),
        StringVariable->GetPropertyValue_InContainer(Child->GeneratedClass->GetDefaultObject()), FString(TEXT("preserved")));

    auto Removal = MakeShared<FJsonObject>();
    Removal->SetStringField(TEXT("kind"), TEXT("removeVariable"));
    Removal->SetStringField(TEXT("variableName"), TEXT("MergedCounter"));
    StructurePlan->SetBoolField(TEXT("dryRun"), true);
    StructurePlan->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Removal)});
    const auto RemovalPreview = Run(StructureTool, StructurePlan);
    if (RemovalPreview.Error.IsSet()) { AddError(RemovalPreview.Error->Message); return false; }
    StructurePlan->SetStringField(TEXT("previewToken"), RemovalPreview.Result->GetStringField(TEXT("previewToken")));
    StructurePlan->SetBoolField(TEXT("dryRun"), false);
    const auto RemovalApply = Run(StructureTool, StructurePlan);
    if (RemovalApply.Error.IsSet()) { AddError(RemovalApply.Error->Message); return false; }
    TestNull(TEXT("Variable removed"), FindFProperty<FProperty>(Child->GeneratedClass, TEXT("MergedCounter")));

    UBlueprint* NewParent = Fixture.Create(TEXT("BP_CompatibleParent"), Parent->GeneratedClass);
    Compile(NewParent);
    UPackage* NewParentPackage = NewParent->GetOutermost();
    const FString NewParentFile = FPackageName::LongPackageNameToFilename(NewParentPackage->GetName(), FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT
    {
        ResetLoaders(NewParentPackage);
        if (IFileManager::Get().FileExists(*NewParentFile))
            TestTrue(TEXT("Remove staged parent fixture"), IFileManager::Get().Delete(*NewParentFile, false, true));
    };
    if (!TestTrue(TEXT("Save compatible parent"), UPackage::SavePackage(NewParentPackage, NewParent, *NewParentFile, SaveArgs))) return false;
    auto Reparent = MakeShared<FJsonObject>();
    Reparent->SetStringField(TEXT("kind"), TEXT("reparent"));
    Reparent->SetStringField(TEXT("newParentClassPath"), NewParent->GeneratedClass->GetPathName());

    auto* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
        Child, TEXT("MergeSignature"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Child, FunctionGraph, true, nullptr);
    TArray<UK2Node_FunctionEntry*> Entries;
    FunctionGraph->GetNodesOfClass(Entries);
    if (!TestEqual(TEXT("One function entry"), Entries.Num(), 1)) return false;
    FEdGraphPinType IntegerPin;
    IntegerPin.PinCategory = UEdGraphSchema_K2::PC_Int;
    Entries[0]->CreateUserDefinedPin(TEXT("Input"), IntegerPin, EGPD_Output);
    auto* LocalRoot = Child->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("MergeLocalRoot"));
    auto* LocalOther = Child->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("MergeLocalOther"));
    Child->SimpleConstructionScript->AddNode(LocalRoot);
    Child->SimpleConstructionScript->AddNode(LocalOther);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Child);
    Compile(Child);
    if (!TestTrue(TEXT("Save signature owner fixture"), UPackage::SavePackage(ChildPackage, Child, *File, SaveArgs))) return false;
    UFunction* InitialFunction = Child->GeneratedClass->FindFunctionByName(TEXT("MergeSignature"));
    if (!TestNotNull(TEXT("Fixture signature compiled"), InitialFunction)) return false;

    UBlueprint* Caller = Fixture.Create(TEXT("BP_MergeDescendant"), Child->GeneratedClass);
    VariablePlan->SetBoolField(TEXT("dryRun"), true);
    Declaration->SetStringField(TEXT("variableName"), TEXT("SecondCounter"));
    const auto DescendantGuard = Run(VariableMergeTool, VariablePlan);
    TestTrue(TEXT("Variable merge refuses known descendants"), DescendantGuard.Error.IsSet()
        && DescendantGuard.Error->Message.Contains(TEXT("known descendants")));
    AddInfo(TEXT("Isolated Git-stage variable merge preview/apply completed."));
    UPackage* CallerPackage = Caller->GetOutermost();
    const FString CallerFile = FPackageName::LongPackageNameToFilename(CallerPackage->GetName(), FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT
    {
        ResetLoaders(CallerPackage);
        if (IFileManager::Get().FileExists(*CallerFile))
            TestTrue(TEXT("Remove staged caller fixture"), IFileManager::Get().Delete(*CallerFile, false, true));
    };
    FEdGraphPinType BooleanPin;
    BooleanPin.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    FBlueprintEditorUtils::AddMemberVariable(Caller, TEXT("Feed"), BooleanPin);
    UEdGraph* CallerGraph = Caller->UbergraphPages[0];
    auto* CallerEvent = NewObject<UK2Node_CustomEvent>(CallerGraph);
    CallerEvent->CustomFunctionName = TEXT("RunMergeSignature");
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(CallerGraph, CallerEvent, {});
    auto* Call = UnrealMCP::BlueprintGraphEditToolUtils::CreateFunctionCallNode(CallerGraph, InitialFunction, false);
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(CallerGraph, Call, {});
    auto* Feed = UnrealMCP::BlueprintGraphEditToolUtils::CreateVariableGetNode(CallerGraph, TEXT("Feed"), {}, false);
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(CallerGraph, Feed, {});
    CallerEvent->FindPinChecked(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(Call->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
    Compile(Caller);
    if (!TestTrue(TEXT("Save loaded caller fixture"), UPackage::SavePackage(CallerPackage, Caller, *CallerFile, SaveArgs))) return false;

    auto Parameter = MakeShared<FJsonObject>();
    Parameter->SetStringField(TEXT("kind"), TEXT("setParameterType"));
    Parameter->SetStringField(TEXT("nodeGuid"), Entries[0]->NodeGuid.ToString());
    Parameter->SetStringField(TEXT("parameterName"), TEXT("Input"));
    Parameter->SetStringField(TEXT("type"), TEXT("bool"));
    auto Move = MakeShared<FJsonObject>();
    Move->SetStringField(TEXT("kind"), TEXT("moveComponent"));
    Move->SetStringField(TEXT("componentName"), TEXT("MergeLocalOther"));
    Move->SetStringField(TEXT("parentComponentName"), TEXT("MergeLocalRoot"));
    StructurePlan->SetBoolField(TEXT("dryRun"), true);
    StructurePlan->SetArrayField(TEXT("operations"), {
        MakeShared<FJsonValueObject>(Parameter), MakeShared<FJsonValueObject>(Move), MakeShared<FJsonValueObject>(Reparent)});
    const auto SignaturePreview = Run(StructureTool, StructurePlan);
    if (SignaturePreview.Error.IsSet()) { AddError(SignaturePreview.Error->Message); return false; }
    TestEqual(TEXT("Signature preview lists caller"), SignaturePreview.Result->GetArrayField(TEXT("packages")).Num(), 2);
    StructurePlan->SetBoolField(TEXT("dryRun"), false);
    StructurePlan->SetStringField(TEXT("previewToken"), SignaturePreview.Result->GetStringField(TEXT("previewToken")));
    CallerPackage->SetDirtyFlag(true);
    TestTrue(TEXT("Dirty caller blocks apply"), Run(StructureTool, StructurePlan).Error.IsSet());
    CallerPackage->SetDirtyFlag(false);
    const auto SignatureApply = Run(StructureTool, StructurePlan);
    if (SignatureApply.Error.IsSet()) { AddError(SignatureApply.Error->Message); return false; }
    TestTrue(TEXT("Caller was compiled"), SignatureApply.Result->GetArrayField(TEXT("packages"))[1]->AsObject()->GetBoolField(TEXT("compiled")));
    TestEqual(TEXT("Caller parameter reconstructed"), Call->FindPinChecked(TEXT("Input"))->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
    TestEqual(TEXT("Scene component moved under new parent"),
        Child->SimpleConstructionScript->FindParentNode(LocalOther), LocalRoot);

    TestEqual(TEXT("Target reparented within the mixed batch"), Child->GeneratedClass->GetSuperClass(), NewParent->GeneratedClass.Get());

    Call->FindPinChecked(TEXT("Input"))->MakeLinkTo(Feed->FindPinChecked(TEXT("Feed")));
    Compile(Caller);
    if (!TestTrue(TEXT("Save linked caller"), UPackage::SavePackage(CallerPackage, Caller, *CallerFile, SaveArgs))) return false;
    Parameter->SetStringField(TEXT("type"), TEXT("object"));
    Parameter->SetStringField(TEXT("typeObjectPath"), TEXT("/Script/Engine.Actor"));
    StructurePlan->SetBoolField(TEXT("dryRun"), true);
    StructurePlan->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Parameter)});
    const auto FailurePreview = Run(StructureTool, StructurePlan);
    if (FailurePreview.Error.IsSet()) { AddError(FailurePreview.Error->Message); return false; }
    StructurePlan->SetStringField(TEXT("previewToken"), FailurePreview.Result->GetStringField(TEXT("previewToken")));
    StructurePlan->SetBoolField(TEXT("dryRun"), false);
    AddExpectedError(TEXT("Can.t connect pins.*not compatible"), EAutomationExpectedErrorFlags::Contains, 0);
    const auto FailedSignature = Run(StructureTool, StructurePlan);
    TestTrue(TEXT("Incompatible caller refuses the edit"), FailedSignature.Error.IsSet());
    TestTrue(TEXT("Failed caller restores clean packages"), !ChildPackage->IsDirty() && !CallerPackage->IsDirty());
    TestEqual(TEXT("Failed caller restores parameter type"), Call->FindPinChecked(TEXT("Input"))->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
    TestTrue(TEXT("Failed caller restores parameter links"), Call->FindPinChecked(TEXT("Input"))->LinkedTo.Contains(Feed->FindPinChecked(TEXT("Feed"))));
    if (!Git(FString::Printf(TEXT("ls-files -u -- \"%s\""), *Path), StagesAfter)) return false;
    TestEqual(TEXT("Failed signature preserves Git stages"), StagesAfter, StagesBefore);
    AddInfo(TEXT("Isolated Git-stage caller reconstruction and rollback completed."));

    const FApplyBlueprintMergePlanTool PlanTool;
    auto Plan = MakeShared<FJsonObject>();
    const FString MergePlanId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Journal = FPaths::ProjectSavedDir() / TEXT("UnrealMCP/MergePlans") / MergePlanId;
    ON_SCOPE_EXIT
    {
        for (int32 Step = 0; Step < 3; ++Step)
            for (const TCHAR* Suffix : {TEXT("intent"), TEXT("receipt")})
                IFileManager::Get().Delete(*(Journal / FString::Printf(TEXT("%02d.%s.json"), Step, Suffix)), false, true);
        IFileManager::Get().DeleteDirectory(*Journal, false, false);
    };
    Plan->SetStringField(TEXT("path"), Path);
    Plan->SetStringField(TEXT("planId"), MergePlanId);
    Plan->SetStringField(TEXT("stageRevision"), Structure.Result->GetStringField(TEXT("stageRevision")));
    auto GraphStep = [&](const UEdGraph* Graph, const TCHAR* PatchId)
    {
        auto Step = MakeShared<FJsonObject>();
        auto Args = MakeShared<FJsonObject>();
        auto Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("id"), TEXT("branch"));
        Node->SetStringField(TEXT("kind"), TEXT("branch"));
        Args->SetStringField(TEXT("patchId"), PatchId);
        Args->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString());
        Args->SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(Node)});
        Step->SetStringField(TEXT("kind"), TEXT("graphPatch"));
        Step->SetObjectField(TEXT("arguments"), Args);
        return MakeShared<FJsonValueObject>(Step);
    };
    auto StructureStep = MakeShared<FJsonObject>();
    auto StructureArgs = MakeShared<FJsonObject>();
    StructureArgs->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Parameter)});
    StructureStep->SetStringField(TEXT("kind"), TEXT("structure"));
    StructureStep->SetObjectField(TEXT("arguments"), StructureArgs);
    Plan->SetArrayField(TEXT("steps"), {GraphStep(Child->UbergraphPages[0], TEXT("plan-first")),
        GraphStep(FunctionGraph, TEXT("plan-second")), MakeShared<FJsonValueObject>(StructureStep)});
    Plan->SetNumberField(TEXT("stepIndex"), 0);
    const auto FirstCheckpoint = Run(PlanTool, Plan);
    if (FirstCheckpoint.Error.IsSet()) { AddError(FirstCheckpoint.Error->Message); return false; }
    Plan->SetStringField(TEXT("previewToken"), FirstCheckpoint.Result->GetStringField(TEXT("previewToken")));
    Plan->SetBoolField(TEXT("dryRun"), false);
    Plan->SetBoolField(TEXT("confirm"), true);
    ChildPackage->SetDirtyFlag(true);
    TestTrue(TEXT("Dirty working Blueprint invalidates checkpoint"), Run(PlanTool, Plan).Error.IsSet());
    ChildPackage->SetDirtyFlag(false);
    const auto FirstSaved = Run(PlanTool, Plan);
    if (FirstSaved.Error.IsSet()) { AddError(FirstSaved.Error->Message); return false; }
    TestTrue(TEXT("First checkpoint actually saved"), FirstSaved.Result->GetObjectField(TEXT("receipt"))->GetBoolField(TEXT("workingFileChanged")));
    const int32 FirstGraphNodes = Child->UbergraphPages[0]->Nodes.Num();
    const auto FirstReplay = Run(FApplyBlueprintMergePlanTool(), Plan);
    if (FirstReplay.Error.IsSet()) { AddError(FirstReplay.Error->Message); return false; }
    TestTrue(TEXT("Replay reads durable receipt"), FirstReplay.Result->GetBoolField(TEXT("replayed")));
    TestEqual(TEXT("Replay never duplicates graph edits"), Child->UbergraphPages[0]->Nodes.Num(), FirstGraphNodes);
    Plan->SetNumberField(TEXT("stepIndex"), 1);
    Plan->SetBoolField(TEXT("dryRun"), true);
    const auto SecondCheckpoint = Run(FApplyBlueprintMergePlanTool(), Plan);
    if (SecondCheckpoint.Error.IsSet()) { AddError(SecondCheckpoint.Error->Message); return false; }
    Plan->SetStringField(TEXT("previewToken"), SecondCheckpoint.Result->GetStringField(TEXT("previewToken")));
    Plan->SetBoolField(TEXT("dryRun"), false);
    TArray<uint8> ExactWorking;
    if (!FFileHelper::LoadFileToArray(ExactWorking, *File)) return false;
    auto ExternalEdit = ExactWorking;
    ExternalEdit.Add(0);
    if (!FFileHelper::SaveArrayToFile(ExternalEdit, *File)) return false;
    const auto DiskGuard = Run(PlanTool, Plan);
    if (!FFileHelper::SaveArrayToFile(ExactWorking, *File)) return false;
    TestTrue(TEXT("External disk change invalidates checkpoint"), DiskGuard.Error.IsSet());
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *EmptyTree, *Trees[1], *Trees[2]), Ignored)) return false;
    const auto StageGuard = Run(PlanTool, Plan);
    if (!Git(TEXT("read-tree --empty"), Ignored)
        || !Git(FString::Printf(TEXT("read-tree -i -m %s %s %s"), *Trees[0], *Trees[1], *Trees[2]), Ignored)) return false;
    TestTrue(TEXT("Changed stage identities invalidate checkpoint"), StageGuard.Error.IsSet());
    const auto SecondSaved = Run(PlanTool, Plan);
    if (SecondSaved.Error.IsSet()) { AddError(SecondSaved.Error->Message); return false; }
    TestEqual(TEXT("Two independent saved transactions"), SecondSaved.Result->GetArrayField(TEXT("completedSteps")).Num(), 2);
    FString BeforeFailedStep;
    if (!Git(FString::Printf(TEXT("hash-object --no-filters -- \"%s\""), *Path), BeforeFailedStep)) return false;
    Plan->SetNumberField(TEXT("stepIndex"), 2);
    Plan->SetBoolField(TEXT("dryRun"), true);
    const auto FinalPreflight = Run(PlanTool, Plan);
    if (FinalPreflight.Error.IsSet()) { AddError(FinalPreflight.Error->Message); return false; }
    Plan->SetStringField(TEXT("previewToken"), FinalPreflight.Result->GetStringField(TEXT("previewToken")));
    Plan->SetBoolField(TEXT("dryRun"), false);
    const auto FinalFailure = Run(PlanTool, Plan);
    TestTrue(TEXT("Later apply failure retains both saved checkpoint receipts"), FinalFailure.Error.IsSet()
        && FinalFailure.Error->Data->GetArrayField(TEXT("completedSteps")).Num() == 2
        && FinalFailure.Error->Data->GetObjectField(TEXT("receipt"))->GetStringField(TEXT("kind")) == TEXT("structure"));
    const auto FailureReplay = Run(FApplyBlueprintMergePlanTool(), Plan);
    TestTrue(TEXT("Failed checkpoint replays its durable failure"), FailureReplay.Error.IsSet()
        && FailureReplay.Error->Data->GetBoolField(TEXT("replayed")));
    FString AfterFailedStep;
    if (!Git(FString::Printf(TEXT("hash-object --no-filters -- \"%s\""), *Path), AfterFailedStep)) return false;
    TestEqual(TEXT("Later failure preserves earlier saved changes"), AfterFailedStep, BeforeFailedStep);
    TestTrue(TEXT("Simulate interrupted terminal receipt"), IFileManager::Get().Delete(*(Journal / TEXT("01.receipt.json")), true));
    const auto Unknown = Run(PlanTool, Plan);
    TestTrue(TEXT("Interrupted checkpoint cannot be retried blindly"), Unknown.Error.IsSet()
        && Unknown.Error->Data->GetStringField(TEXT("state")) == TEXT("indeterminate"));
    AddInfo(TEXT("Isolated Git-stage multi-graph checkpoint and durable replay completed."));
    if (!VerifyConflictStaging(*this, Git, Path, StagesBefore, Structure.Result->GetStringField(TEXT("stageRevision")), ChildPackage, Trees, EmptyTree, RenamedTrees, RenamedPath)) return false;
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPGraphPreservationTest, "UnrealMCP.Blueprint.Authoring.GraphPreservation.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPGraphPreservationTest::RunTest(const FString& Parameters)
{
    using namespace UnrealMCPToolingP1;
    FFixture Fixture;
    UBlueprint* Blueprint = Fixture.Create(TEXT("Preservation"));
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    auto* Other = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("Untouched"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Other, true, nullptr);
    auto* Branch = Place<UK2Node_IfThenElse>(Graph);
    auto* Destination = Place<UK2Node_IfThenElse>(Graph);
    auto* Exec = Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
    auto* Target = Destination->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
    Exec->MakeLinkTo(Target);
    const FCompareBlueprintGraphPreservationTool Tool;
    auto Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
    const auto Before = Run(Tool, Args);
    if (Before.Error.IsSet()) { AddError(Before.Error->Message); return false; }
    Args->SetObjectField(TEXT("baseline"), Before.Result->GetObjectField(TEXT("snapshot")));
    const auto Same = Run(Tool, Args);
    if (!TestTrue(TEXT("Unchanged wiring has an empty diff"), Same.Result && Same.Result->GetIntegerField(TEXT("total")) == 0)) return false;
    auto* Knot = Place<UK2Node_Knot>(Graph);
    Exec->BreakLinkTo(Target);
    Exec->MakeLinkTo(Knot->GetInputPin());
    Knot->GetOutputPin()->MakeLinkTo(Target);
    const auto Rerouted = Run(Tool, Args);
    if (Rerouted.Error.IsSet()) { AddError(Rerouted.Error->Message); return false; }
    const FString TargetSource = TEXT("node/") + UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Destination)
        + TEXT("/pin/") + UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Target) + TEXT("/source/0");
    TestFalse(TEXT("Reroute preserves the normalized destination source"), Rerouted.Result->GetArrayField(TEXT("rows")).ContainsByPredicate(
        [&](const auto& Row) { return Row->AsArray()[1]->AsString() == TargetSource; }));
    Knot->GetOutputPin()->BreakLinkTo(Target);
    auto* Condition = Branch->FindPinChecked(UEdGraphSchema_K2::PN_Condition);
    Condition->DefaultValue = Condition->DefaultValue == TEXT("true") ? TEXT("false") : TEXT("true");
    const FString UpdatedLiteral = Condition->DefaultValue;
    const auto Diff = Run(Tool, Args);
    if (Diff.Error.IsSet()) { AddError(Diff.Error->Message); return false; }
    const auto& Changed = Diff.Result->GetArrayField(TEXT("changedGraphs"));
    TestTrue(TEXT("Only the edited graph is listed"), Changed.Num() == 1 && Changed[0]->AsArray()[0]->AsString() == Graph->GraphGuid.ToString());
    bool bRemoved = false, bLiteral = false;
    for (const auto& Value : Diff.Result->GetArrayField(TEXT("rows")))
    {
        const auto& Row = Value->AsArray();
        bRemoved |= Row[1]->AsString().Contains(TEXT("/exec/")) && Row[3]->IsNull();
        bLiteral |= Row[1]->AsString().EndsWith(TEXT("/default")) && Row[3]->AsString() == UpdatedLiteral;
    }
    TestTrue(TEXT("Removed branch destination is reported"), bRemoved);
    TestTrue(TEXT("Changed literal is reported"), bLiteral);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPClassDefaultsTest, "UnrealMCP.Blueprint.Authoring.ClassDefaults.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPClassDefaultsTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Parent = Fixture.Create(TEXT("BP_DefaultsParent"));
    FEdGraphPinType ScaleType;
    ScaleType.PinCategory = UEdGraphSchema_K2::PC_Real;
    ScaleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    TestTrue(TEXT("Parent scale added"), FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("ModelScale"), ScaleType, TEXT("1.0")));
    TestTrue(TEXT("Parent flag added"), FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("ShouldSample"), BoolType, TEXT("True")));
    FEdGraphPinType MapType;
    MapType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    MapType.PinSubCategoryObject = LoadObject<UEnum>(nullptr, TEXT("/Script/Engine.ECollisionChannel"));
    MapType.ContainerType = EPinContainerType::Map;
    MapType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Class;
    MapType.PinValueType.TerminalSubCategoryObject = AActor::StaticClass();
    TestTrue(TEXT("Parent map added"), FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("TagMap"), MapType));
    Compile(Parent);
    UBlueprint* Child = Fixture.Create(TEXT("BP_DefaultsChild"), Parent->GeneratedClass);
    Compile(Child);
    UBlueprint* Shadow = Fixture.Create(TEXT("BP_DefaultsShadow"));
    TestTrue(TEXT("Shadow scale added"), FBlueprintEditorUtils::AddMemberVariable(Shadow, TEXT("ModelScale"), ScaleType, TEXT("0.0")));
    Compile(Shadow);

    auto GetOne = [&](UBlueprint* Blueprint, const TCHAR* Name)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
        Params->SetArrayField(TEXT("propertyNames"), {MakeShared<FJsonValueString>(Name)});
        const UnrealMCP::FMCPResponse Response = Run(FGetBlueprintClassDefaultsTool(), Params);
        if (Response.Error.IsSet()) AddError(Response.Error->Message);
        return Response.Result.IsValid() && Response.Result->GetArrayField(TEXT("properties")).Num() == 1
            ? Response.Result->GetArrayField(TEXT("properties"))[0]->AsObject() : TSharedPtr<FJsonObject>();
    };
    const TSharedPtr<FJsonObject> ParentScale = GetOne(Parent, TEXT("ModelScale"));
    const TSharedPtr<FJsonObject> ShadowScale = GetOne(Shadow, TEXT("ModelScale"));
    const TSharedPtr<FJsonObject> ChildFlag = GetOne(Child, TEXT("ShouldSample"));
    TestTrue(TEXT("Parent scale is one"), ParentScale.IsValid() && FCString::Atod(*ParentScale->GetStringField(TEXT("value"))) == 1.0);
    TestTrue(TEXT("Shadow scale is zero"), ShadowScale.IsValid() && FCString::Atod(*ShadowScale->GetStringField(TEXT("value"))) == 0.0);
    TestTrue(TEXT("Child initially inherits true"), ChildFlag.IsValid() && ChildFlag->GetStringField(TEXT("value")).Equals(TEXT("True"), ESearchCase::IgnoreCase));
    TestTrue(TEXT("Read leaves parent dirty state unchanged"), Parent->GetPackage()->IsDirty());
    TSharedRef<FJsonObject> CompareParams = MakeShared<FJsonObject>();
    CompareParams->SetStringField(TEXT("objectPathA"), PathOf(Parent));
    CompareParams->SetStringField(TEXT("objectPathB"), PathOf(Shadow));
    const UnrealMCP::FMCPResponse Compared = Run(FCompareBlueprintClassDefaultsTool(), CompareParams);
    if (Compared.Error.IsSet()) AddError(Compared.Error->Message);
    TestTrue(TEXT("Compare finds ModelScale regression"), Compared.Result.IsValid()
        && Compared.Result->GetArrayField(TEXT("properties")).ContainsByPredicate([](const TSharedPtr<FJsonValue>& Item)
            { return Item->AsObject()->GetStringField(TEXT("name")) == TEXT("ModelScale")
                && Item->AsObject()->GetStringField(TEXT("valueA")) != Item->AsObject()->GetStringField(TEXT("valueB")); }));

    auto SetOne = [&](UBlueprint* Blueprint, const TCHAR* Name, const TCHAR* Value, bool bAllowDirty)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
        TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetStringField(Name, Value);
        Params->SetObjectField(TEXT("values"), Values);
        Params->SetBoolField(TEXT("confirm"), true);
        Params->SetBoolField(TEXT("allowDirty"), bAllowDirty);
        return Run(FSetBlueprintClassDefaultsTool(), Params);
    };
    TestTrue(TEXT("Dirty guard rejects edit"), SetOne(Shadow, TEXT("ModelScale"), TEXT("1.0"), false).Error.IsSet());
    const UnrealMCP::FMCPResponse ChangedShadow = SetOne(Shadow, TEXT("ModelScale"), TEXT("1.0"), true);
    if (ChangedShadow.Error.IsSet()) AddError(ChangedShadow.Error->Message);
    TestTrue(TEXT("Own default changed"), ChangedShadow.Result.IsValid() && ChangedShadow.Result->GetBoolField(TEXT("compiled")));
    const UnrealMCP::FMCPResponse ChangedChild = SetOne(Child, TEXT("ShouldSample"), TEXT("False"), true);
    if (ChangedChild.Error.IsSet()) AddError(ChangedChild.Error->Message);
    TestTrue(TEXT("Inherited default changed"), ChangedChild.Result.IsValid() && ChangedChild.Result->GetBoolField(TEXT("compiled")));
    const TSharedPtr<FJsonObject> NewChildFlag = GetOne(Child, TEXT("ShouldSample"));
    if (NewChildFlag.IsValid()) AddInfo(FString::Printf(TEXT("Child default=%s parent=%s overridden=%s"),
        *NewChildFlag->GetStringField(TEXT("value")), *NewChildFlag->GetStringField(TEXT("parentValue")),
        NewChildFlag->GetBoolField(TEXT("isOverridden")) ? TEXT("true") : TEXT("false")));
    TestTrue(TEXT("Child override readback"), NewChildFlag.IsValid() && NewChildFlag->GetBoolField(TEXT("isOverridden"))
        && NewChildFlag->GetStringField(TEXT("value")).Equals(TEXT("False"), ESearchCase::IgnoreCase)
        && NewChildFlag->GetStringField(TEXT("parentValue")).Equals(TEXT("True"), ESearchCase::IgnoreCase));
    const TSharedPtr<FJsonObject> NewShadowScale = GetOne(Shadow, TEXT("ModelScale"));
    TestTrue(TEXT("Shadow scale readback"), NewShadowScale.IsValid() && FCString::Atod(*NewShadowScale->GetStringField(TEXT("value"))) == 1.0);

    TSharedRef<FJsonObject> Reset = MakeShared<FJsonObject>();
    Reset->SetStringField(TEXT("objectPath"), PathOf(Child));
    Reset->SetArrayField(TEXT("propertyNames"), {MakeShared<FJsonValueString>(TEXT("ShouldSample"))});
    Reset->SetBoolField(TEXT("confirm"), true);
    Reset->SetBoolField(TEXT("allowDirty"), true);
    const UnrealMCP::FMCPResponse ResetResult = Run(FResetBlueprintClassDefaultsTool(), Reset);
    if (ResetResult.Error.IsSet()) AddError(ResetResult.Error->Message);
    const TSharedPtr<FJsonObject> ResetFlag = GetOne(Child, TEXT("ShouldSample"));
    TestTrue(TEXT("Inherited default reset"), ResetFlag.IsValid() && !ResetFlag->GetBoolField(TEXT("isOverridden"))
        && ResetFlag->GetStringField(TEXT("value")).Equals(TEXT("True"), ESearchCase::IgnoreCase));

    const UnrealMCP::FMCPResponse Invalid = SetOne(Shadow, TEXT("ModelScale"), TEXT("not-a-double"), true);
    TestTrue(TEXT("Invalid import rejected"), Invalid.Error.IsSet());
    const TSharedPtr<FJsonObject> StillShadowScale = GetOne(Shadow, TEXT("ModelScale"));
    TestTrue(TEXT("Invalid import left CDO intact"), StillShadowScale.IsValid()
        && FCString::Atod(*StillShadowScale->GetStringField(TEXT("value"))) == 1.0);
    const FString MapExport = TEXT("((ECC_WorldStatic,Class'/Script/Engine.Actor'),(ECC_WorldDynamic,Class'/Script/Engine.Actor'),(ECC_Pawn,Class'/Script/Engine.Actor'),(ECC_Visibility,Class'/Script/Engine.Actor'))");
    const UnrealMCP::FMCPResponse MapSet = SetOne(Parent, TEXT("TagMap"), *MapExport, true);
    if (MapSet.Error.IsSet()) AddError(MapSet.Error->Message);
    const TSharedPtr<FJsonObject> MapRead = GetOne(Parent, TEXT("TagMap"));
    TestTrue(TEXT("Map default set and read"), MapSet.Result.IsValid() && MapSet.Result->GetBoolField(TEXT("compiled"))
        && MapRead.IsValid() && MapRead->GetStringField(TEXT("value")).Contains(TEXT("ECC_Visibility")));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPLocalVariablesTest, "UnrealMCP.Blueprint.Authoring.LocalVariables.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPLocalVariablesTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Blueprint = Fixture.Create(TEXT("BP_Locals"));
    UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("DoWork"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);

    auto Params = [&]()
    {
        TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
        P->SetStringField(TEXT("graphGuid"), Function->GraphGuid.ToString());
        return P;
    };
    auto Count = [&]()
    {
        const UnrealMCP::FMCPResponse R = Run(FListBlueprintLocalVariablesTool(), Params());
        return R.Result.IsValid() ? static_cast<int32>(R.Result->GetNumberField(TEXT("count"))) : -1;
    };

    TestEqual(TEXT("No locals at start"), Count(), 0);
    TSharedRef<FJsonObject> Add = Params();
    Add->SetStringField(TEXT("variableName"), TEXT("Count"));
    Add->SetStringField(TEXT("type"), TEXT("int"));
    Add->SetStringField(TEXT("defaultValue"), TEXT("3"));
    Add->SetBoolField(TEXT("compileAfterEdit"), true);
    const UnrealMCP::FMCPResponse Added = Run(FAddBlueprintLocalVariableTool(), Add);
    if (Added.Error.IsSet()) AddError(Added.Error->Message);
    TestTrue(TEXT("Local added"), Added.Result.IsValid() && Added.Result->GetBoolField(TEXT("added")));
    TestTrue(TEXT("Compiles with the local"), Added.Result.IsValid() && Added.Result->GetBoolField(TEXT("compileSucceeded")));
    TestEqual(TEXT("One local listed"), Count(), 1);
    const UnrealMCP::FMCPResponse Again = Run(FAddBlueprintLocalVariableTool(), Add);
    TestTrue(TEXT("Second add is idempotent"), Again.Result.IsValid() && Again.Result->GetBoolField(TEXT("alreadyExists")));
    TestEqual(TEXT("Still one local"), Count(), 1);

    TSharedRef<FJsonObject> SetDefault = Params();
    SetDefault->SetStringField(TEXT("variableName"), TEXT("Count"));
    SetDefault->SetStringField(TEXT("defaultValue"), TEXT("7"));
    SetDefault->SetBoolField(TEXT("compileAfterEdit"), true);
    const UnrealMCP::FMCPResponse Updated = Run(FSetBlueprintLocalVariableDefaultTool(), SetDefault);
    if (Updated.Error.IsSet()) AddError(Updated.Error->Message);
    TestTrue(TEXT("Local default updated and compiled"), Updated.Result.IsValid() && Updated.Result->GetBoolField(TEXT("compileSucceeded")));
    const FBPVariableDescription* CountLocal = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Function, TEXT("Count"));
    TestTrue(TEXT("New local default stored"), CountLocal && CountLocal->DefaultValue == TEXT("7"));

    TSharedRef<FJsonObject> Map = Params();
    Map->SetStringField(TEXT("variableName"), TEXT("ClassByChannel"));
    Map->SetStringField(TEXT("type"), TEXT("enum"));
    Map->SetStringField(TEXT("typeObjectPath"), TEXT("/Script/Engine.ECollisionChannel"));
    Map->SetStringField(TEXT("containerType"), TEXT("map"));
    Map->SetStringField(TEXT("valueType"), TEXT("class"));
    Map->SetStringField(TEXT("valueTypeObjectPath"), AActor::StaticClass()->GetPathName());
    Map->SetBoolField(TEXT("compileAfterEdit"), true);
    const UnrealMCP::FMCPResponse MapAdded = Run(FAddBlueprintLocalVariableTool(), Map);
    if (MapAdded.Error.IsSet()) AddError(MapAdded.Error->Message);
    TestTrue(TEXT("Enum to class map local compiles"), MapAdded.Result.IsValid() && MapAdded.Result->GetBoolField(TEXT("compileSucceeded")));
    const FBPVariableDescription* MapLocal = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Function, TEXT("ClassByChannel"));
    TestTrue(TEXT("Map key and value retain types"), MapLocal
        && MapLocal->VarType.ContainerType == EPinContainerType::Map
        && MapLocal->VarType.PinCategory == UEdGraphSchema_K2::PC_Byte
        && MapLocal->VarType.PinSubCategoryObject == LoadObject<UEnum>(nullptr, TEXT("/Script/Engine.ECollisionChannel"))
        && MapLocal->VarType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Class);

    const FString MapDefault = TEXT("((ECC_WorldStatic,Class'/Script/Engine.Actor'),(ECC_WorldDynamic,Class'/Script/Engine.Actor'),(ECC_Pawn,Class'/Script/Engine.Actor'),(ECC_Visibility,Class'/Script/Engine.Actor'))");
    TSharedRef<FJsonObject> SetMap = Params();
    SetMap->SetStringField(TEXT("variableName"), TEXT("ClassByChannel"));
    SetMap->SetStringField(TEXT("defaultValue"), MapDefault);
    SetMap->SetBoolField(TEXT("compileAfterEdit"), true);
    const UnrealMCP::FMCPResponse MapUpdated = Run(FSetBlueprintLocalVariableDefaultTool(), SetMap);
    if (MapUpdated.Error.IsSet()) AddError(MapUpdated.Error->Message);
    TestTrue(TEXT("Populated map local compiles"), MapUpdated.Result.IsValid() && MapUpdated.Result->GetBoolField(TEXT("compileSucceeded")));
    MapLocal = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Function, TEXT("ClassByChannel"));
    TestTrue(TEXT("Four map entries stored"), MapLocal && MapLocal->DefaultValue == MapDefault);

    TSharedRef<FJsonObject> Remove = Params();
    Remove->SetStringField(TEXT("variableName"), TEXT("Count"));
    TestTrue(TEXT("Remove without confirm is refused"), Run(FRemoveBlueprintLocalVariableTool(), Remove).Error.IsSet());
    Remove->SetBoolField(TEXT("confirm"), true);
    const UnrealMCP::FMCPResponse Removed = Run(FRemoveBlueprintLocalVariableTool(), Remove);
    if (Removed.Error.IsSet()) AddError(Removed.Error->Message);
    TestEqual(TEXT("Only map local after remove"), Count(), 1);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPFindClassUsagesTest, "UnrealMCP.Blueprint.Query.FindClassUsages.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPFindClassUsagesTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Target = Fixture.Create(TEXT("BP_UsageTarget"));
    Compile(Target);
    UBlueprint* Blueprint = Fixture.Create(TEXT("BP_UsageUser"));
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("MyPawn"), ObjectType(APawn::StaticClass()));
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("MyTarget"), ObjectType(Target->GeneratedClass));
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
    CastNode->TargetType = APawn::StaticClass();
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, CastNode, {});
    UK2Node_CallFunction* Destroy = Place<UK2Node_CallFunction>(Graph);
    Destroy->FunctionReference.SetExternalMember(TEXT("K2_DestroyActor"), AActor::StaticClass());
    Destroy->ReconstructNode();

    FEdGraphPinType AssetArray = ObjectType(UObject::StaticClass());
    AssetArray.ContainerType = EPinContainerType::Array;
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("AssetDefaults"), AssetArray);
    Compile(Blueprint);
    const auto* ArrayProperty = FindFProperty<FArrayProperty>(Blueprint->GeneratedClass, TEXT("AssetDefaults"));
    if (!TestNotNull(TEXT("Asset default array property"), ArrayProperty)) return false;
    auto* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
    FScriptArrayHelper Array(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(DefaultObject));
    CastFieldChecked<FObjectProperty>(ArrayProperty->Inner)->SetObjectPropertyValue(Array.GetRawPtr(Array.AddValue()), Target);

    auto Find = [&](const FString& Path, int32 Offset = 0, int32 Limit = 100)
    {
        TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
        P->SetStringField(TEXT("classOrAssetPath"), Path);
        P->SetNumberField(TEXT("offset"), Offset);
        P->SetNumberField(TEXT("limit"), Limit);
        return Run(FFindClassUsagesTool(), P);
    };

    const UnrealMCP::FMCPResponse Pawn = Find(PathOf(APawn::StaticClass()));
    const TSet<FString> PawnKinds = Kinds(Pawn);
    TestTrue(TEXT("Pawn: member variable found"), PawnKinds.Contains(TEXT("member_variable")));
    TestTrue(TEXT("Pawn: cast target found"), PawnKinds.Contains(TEXT("cast_target")));
    TestFalse(TEXT("Pawn: Actor parent not matched"), PawnKinds.Contains(TEXT("parent_class")));
    TestFalse(TEXT("Pawn: Actor function call not matched"), PawnKinds.Contains(TEXT("function_call")));
    TestFalse(TEXT("Pawn: no false stale import"), PawnKinds.Contains(TEXT("stale_import_only")));

    TestTrue(TEXT("Actor: parent class found"), Kinds(Find(PathOf(AActor::StaticClass()))).Contains(TEXT("parent_class")));
    TestTrue(TEXT("Blueprint target: member variable found by asset path"), Kinds(Find(PathOf(Target))).Contains(TEXT("member_variable")));
    TestTrue(TEXT("Bare /Script package rejected"), Find(TEXT("/Script/Engine")).Error.IsSet());
    const auto FirstPage = Find(PathOf(Target), 0, 1);
    if (!TestTrue(TEXT("First page succeeded"), FirstPage.Result.IsValid())) return false;
    TestEqual(TEXT("Next result offset"), FirstPage.Result->GetIntegerField(TEXT("nextOffset")), 1);
    const auto SecondPage = Find(PathOf(Target), 1, 1);
    TestTrue(TEXT("Object in a broad-typed CDO array found on second page"), Kinds(SecondPage).Contains(TEXT("class_default")));
    TestEqual(TEXT("Unsaved Blueprint reports import evidence unavailable"), FirstPage.Result->GetStringField(TEXT("importEvidence")), FString(TEXT("unavailable")));

    UEdGraphPin* LiteralPin = CastNode->GetCastSourcePin();
    if (!TestNotNull(TEXT("Cast input pin"), LiteralPin)) return false;
    LiteralPin->DefaultValue = FString::Printf(TEXT("BlueprintGeneratedClass'%s_C'"), *PathOf(Target));
    TestTrue(TEXT("Export-text path in a pin is located"), Kinds(Find(PathOf(Target))).Contains(TEXT("pin_path_literal")));
    LiteralPin->DefaultValue.Reset();

    auto Candidates = MakeShared<FJsonObject>();
    Candidates->SetStringField(TEXT("classOrAssetPath"), PathOf(Target));
    const auto CandidateSearch = Run(FFindClassUsagesTool(), Candidates);
    if (!TestTrue(TEXT("Registry candidate search returns a result"), CandidateSearch.Result.IsValid())) return false;
    TestEqual(TEXT("Registry candidate scope"), CandidateSearch.Result->GetStringField(TEXT("scope")), FString(TEXT("registry_candidates")));
    TestEqual(TEXT("Registry candidates are not live hits"), CandidateSearch.Result->GetIntegerField(TEXT("liveHits")), 0);

    using ENodeSearch = FFindBlueprintNodesTool::ESearch;
    auto Search = MakeShared<FJsonObject>();
    Search->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
    Search->SetStringField(TEXT("typePath"), PathOf(APawn::StaticClass()));
    Search->SetNumberField(TEXT("limit"), 1);
    const auto PinSearch = Run(FFindBlueprintNodesTool(ENodeSearch::PinType), Search);
    if (!TestTrue(TEXT("Pin search returns a result"), PinSearch.Result.IsValid())) return false;
    const auto& PinHits = PinSearch.Result->GetArrayField(TEXT("hits"));
    if (!TestEqual(TEXT("One exact Pawn pin"), PinHits.Num(), 1)) return false;
    TestEqual(TEXT("Pin hit identifies cast node"), PinHits[0]->AsObject()->GetStringField(TEXT("nodeGuid")), CastNode->NodeGuid.ToString());
    TestFalse(TEXT("Pin hit identifies the pin"), PinHits[0]->AsObject()->GetStringField(TEXT("pinId")).IsEmpty());

    Search->RemoveField(TEXT("typePath"));
    Search->SetStringField(TEXT("memberName"), TEXT("K2_DestroyActor"));
    const auto MemberSearch = Run(FFindBlueprintNodesTool(ENodeSearch::Member), Search);
    if (!TestTrue(TEXT("Member search returns a result"), MemberSearch.Result.IsValid())) return false;
    const auto& MemberHits = MemberSearch.Result->GetArrayField(TEXT("hits"));
    if (!TestEqual(TEXT("One exact function call"), MemberHits.Num(), 1)) return false;
    TestEqual(TEXT("Member hit identifies the call"), MemberHits[0]->AsObject()->GetStringField(TEXT("nodeGuid")), Destroy->NodeGuid.ToString());
    Search->SetStringField(TEXT("memberName"), TEXT("DestroyActor"));
    const auto Substring = Run(FFindBlueprintNodesTool(ENodeSearch::Member), Search);
    if (!TestTrue(TEXT("Substring search returns a result"), Substring.Result.IsValid())) return false;
    TestEqual(TEXT("Member search does not match substrings"), Substring.Result->GetIntegerField(TEXT("totalHits")), 0);

    UPackage* Package = Blueprint->GetOutermost();
    const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT { TestTrue(TEXT("Saved import fixture can be deleted"), IFileManager::Get().Delete(*Filename, false, true)); };
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    if (!TestTrue(TEXT("Usage fixture saved"), UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs))) return false;
    const auto Saved = Find(PathOf(APawn::StaticClass()));
    if (!TestTrue(TEXT("Saved import scan succeeds"), Saved.Result.IsValid())) return false;
    TestEqual(TEXT("Saved package read without an attached linker"), Saved.Result->GetStringField(TEXT("importEvidence")), FString(TEXT("saved_package")));
    TestTrue(TEXT("Exact saved Pawn import found"), Kinds(Saved).Contains(TEXT("package_import")));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPCompileResultTest, "UnrealMCP.Blueprint.Authoring.CompileResult.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPCompileResultTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Blueprint = Fixture.Create(TEXT("BP_Compile"));
    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("objectPath"), PathOf(Blueprint));

    const UnrealMCP::FMCPResponse Clean = Run(FCompileBlueprintTool(), Params);
    if (!TestTrue(TEXT("Clean compile returns a result"), Clean.Result.IsValid())) return false;
    TestEqual(TEXT("Clean compile has no errors"), static_cast<int32>(Clean.Result->GetNumberField(TEXT("errorCount"))), 0);
    TestTrue(TEXT("Clean compile has an errors array"), Clean.Result->HasTypedField<EJson::Array>(TEXT("errors")));
    TestTrue(TEXT("Clean compile reports a status"), !Clean.Result->GetStringField(TEXT("status")).IsEmpty());

    // An executed cast with no target class is a compile error.
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
    Event->CustomFunctionName = TEXT("Go");
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Event, {});
    UK2Node_DynamicCast* BadCast = NewObject<UK2Node_DynamicCast>(Graph);
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, BadCast, {});
    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), BadCast->GetExecPin());

    const UnrealMCP::FMCPResponse Broken = Run(FCompileBlueprintTool(), Params);
    if (!TestTrue(TEXT("Failed compile still returns a structured result"), Broken.Result.IsValid())) return false;
    const TArray<TSharedPtr<FJsonValue>> Errors = Broken.Result->GetArrayField(TEXT("errors"));
    TestTrue(TEXT("Errors reported"), Errors.Num() > 0);
    const FString BadGuid = BadCast->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
    TestTrue(TEXT("Error points at the bad cast node"), Errors.ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Error)
    {
        return Error->AsObject()->GetStringField(TEXT("nodeGuid")).Equals(BadGuid, ESearchCase::IgnoreCase)
            || Error->AsObject()->GetStringField(TEXT("nodeGuid")).Equals(BadCast->NodeGuid.ToString(), ESearchCase::IgnoreCase);
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPConsolidateTest, "UnrealMCP.Asset.ConsolidateAndDelete.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPConsolidateTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Old = Fixture.Create(TEXT("BP_Old"));
    UBlueprint* Replacement = Fixture.Create(TEXT("BP_New"));
    Compile(Old);
    Compile(Replacement);
    UBlueprint* User = Fixture.Create(TEXT("BP_OldUser"));
    FBlueprintEditorUtils::AddMemberVariable(User, TEXT("Ref"), ObjectType(Old->GeneratedClass));
    Compile(User);
    const FString OldPath = PathOf(Old);

    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("oldAssetPath"), OldPath);
    Params->SetStringField(TEXT("replacementAssetPath"), PathOf(Replacement));
    TestTrue(TEXT("Consolidate without confirm is refused"), Run(FConsolidateAndDeleteAssetTool(), Params).Error.IsSet());
    Params->SetBoolField(TEXT("dryRun"), true);
    TestTrue(TEXT("Dry-run succeeds"), Run(FConsolidateAndDeleteAssetTool(), Params).Result.IsValid());
    TestNotNull(TEXT("Dry-run keeps the old asset"), FindObject<UBlueprint>(nullptr, *OldPath));

    Params->SetBoolField(TEXT("dryRun"), false);
    Params->SetBoolField(TEXT("confirm"), true);
    const UnrealMCP::FMCPResponse Response = Run(FConsolidateAndDeleteAssetTool(), Params);
    if (Response.Error.IsSet()) AddError(Response.Error->Message);
    if (!TestTrue(TEXT("Consolidate returns a result"), Response.Result.IsValid())) return false;
    TestTrue(TEXT("Consolidate reports success"), Response.Result->GetBoolField(TEXT("success")));
    TestFalse(TEXT("Old asset is gone"), Response.Result->GetBoolField(TEXT("oldAssetStillExists")));

    const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(User, TEXT("Ref"));
    if (Index != INDEX_NONE) AddInfo(FString::Printf(TEXT("Ref type after consolidate: %s (replacement %s)"),
        *GetPathNameSafe(User->NewVariables[Index].VarType.PinSubCategoryObject.Get()), *GetPathNameSafe(Replacement->GeneratedClass)));
    TestTrue(TEXT("User variable now points at the replacement class"), Index != INDEX_NONE
        && User->NewVariables[Index].VarType.PinSubCategoryObject.Get() == Replacement->GeneratedClass.Get());
    Compile(User);
    TestTrue(TEXT("Referencer still compiles"), User->Status == BS_UpToDate || User->Status == BS_UpToDateWithWarnings);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
