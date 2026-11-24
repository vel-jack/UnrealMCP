#include "UnrealMCPModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

#include "Index/UnrealMCPProjectIndex.h"
#include "MCP/MCPServer.h"
#include "Tools/Assets/AssetExistsTool.h"
#include "Tools/Blueprint/Graph/ApplyBlueprintInteractionPlanTool.h"
#include "Tools/Blueprint/Graph/ApplyBlueprintGraphPatchTool.h"
#include "Tools/Blueprint/Members/AddBlueprintComponentTool.h"
#include "Tools/Blueprint/Members/AddBlueprintComponentsTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintBranchNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintMacroNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintArrayOperationNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintSetOperationNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintMapOperationNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintTypedOperatorNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintCastNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintCommentNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintCustomEventNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintFunctionCallNodeTool.h"
#include "Tools/Blueprint/Members/AddBlueprintFunctionParameterTool.h"
#include "Tools/Blueprint/Members/SetBlueprintParameterTool.h"
#include "Tools/Blueprint/Members/ReparentBlueprintTool.h"
#include "Tools/Blueprint/Members/BlueprintMemberRemovalTools.h"
#include "Tools/Data/UserDefinedStructMemberTools.h"
#include "Tools/Data/StructMemberValueMigrationTool.h"
#include "Tools/Assets/AssetManagementTools.h"
#include "Tools/Blueprint/Graph/AddBlueprintInterfaceEventNodeTool.h"
#include "Tools/Blueprint/Members/AddBlueprintInterfaceFunctionGraphTool.h"
#include "Tools/Blueprint/Members/AddBlueprintInterfaceTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintOverrideEventNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintDelegateNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintDelegateEventNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintDelegateBroadcastNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintRerouteNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintSequenceNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintVariableGetNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintVariableSetNodeTool.h"
#include "Tools/Blueprint/Members/AddBlueprintVariableTool.h"
#include "Tools/Input/AddEnhancedInputActionNodeTool.h"
#include "Tools/Index/AnalyzeBlueprintCouplingTool.h"
#include "Tools/Index/AnalyzeFeatureBoundaryTool.h"
#include "Tools/Index/AnalyzeProjectArchitectureTool.h"
#include "Tools/Index/BuildProjectIndexTool.h"
#include "Tools/Blueprint/Inspection/CompileAllBlueprintsTool.h"
#include "Tools/Input/AddInputMappingContextMappingTool.h"
#include "Tools/Blueprint/Graph/ConnectBlueprintPinsTool.h"
#include "Tools/Blueprint/Inspection/CompileBlueprintTool.h"
#include "Tools/Blueprint/Inspection/FindClassUsagesTool.h"
#include "Tools/Git/CompareBlueprintMergeStructureTool.h"
#include "Tools/Blueprint/Graph/CompareBlueprintGraphPreservationTool.h"
#include "Tools/Blueprint/Members/RetargetBlueprintNodeTool.h"
#include "Tools/Blueprint/Members/BlueprintMemberGraphTools.h"
#include "Tools/Editor/EditorVisualTools.h"
#include "Tools/Blueprint/Members/BlueprintLocalVariableTools.h"
#include "Tools/Blueprint/Members/BlueprintClassDefaultsTools.h"
#include "Tools/Editor/DiscardDirtyPackagesForShutdownTool.h"
#include "Tools/Assets/ConsolidateAndDeleteAssetTool.h"
#include "Tools/Assets/CreateBlueprintAssetTool.h"
#include "Tools/Blueprint/Members/CreateBlueprintFunctionGraphTool.h"
#include "Tools/Input/CreateInputActionTool.h"
#include "Tools/Input/CreateInputMappingContextTool.h"
#include "Tools/Blueprint/Graph/DeleteBlueprintNodeTool.h"
#include "Tools/Blueprint/Graph/PruneUnreachableNodesTool.h"
#include "Tools/Blueprint/Graph/RefreshBlueprintNodesTool.h"
#include "Tools/Blueprint/Graph/BlueprintGraphDslTool.h"
#include "Tools/Blueprint/Graph/WrapWithIsValidTool.h"
#include "Tools/Input/GetInputMappingContextMappingsTool.h"
#include "Tools/Input/SetInputMappingContextMappingKeyTool.h"
#include "Tools/Input/RemoveInputMappingContextMappingTool.h"
#include "Tools/Blueprint/Graph/DisconnectBlueprintPinsTool.h"
#include "Tools/Blueprint/Inspection/ExplainBlueprintRoleTool.h"
#include "Tools/Blueprint/Inspection/ExplainBlueprintTraceTool.h"
#include "Tools/Index/ExplainFeatureWorkflowTool.h"
#include "Tools/Editor/FindActorsUsingBlueprintTool.h"
#include "Tools/Assets/FindAssetsByClassTool.h"
#include "Tools/Assets/FindAssetsByPathTool.h"
#include "Tools/Blueprint/Inspection/FindBlueprintNodeReferencesTool.h"
#include "Tools/Blueprint/Inspection/FindBlueprintTraceStartPointsTool.h"
#include "Tools/Index/FindCrossBlueprintCallsTool.h"
#include "Tools/Index/FindCircularDependenciesTool.h"
#include "Tools/Blueprint/Inspection/FindBlueprintVariableUsageTool.h"
#include "Tools/Index/FindBrokenBlueprintReferencesTool.h"
#include "Tools/Index/FindFeatureEntryPointsTool.h"
#include "Tools/Index/FindUnusedBlueprintAssetsTool.h"
#include "Tools/Editor/GetActorInfoTool.h"
#include "Tools/Assets/GetAssetInfoTool.h"
#include "Tools/Data/DataTableRowMutationTool.h"
#include "Tools/Data/FindDataTableReferencesTool.h"
#include "Tools/Data/SaveDataTableTool.h"
#include "Tools/Data/InspectDataTableRowsTool.h"
#include "Tools/Index/GetAssetReferencersTool.h"
#include "Tools/Index/GetAssetReferencesTool.h"
#include "Tools/Blueprint/Inspection/GetBlueprintComponentHierarchyTool.h"
#include "Tools/Blueprint/Members/GetBlueprintComponentDefaultsTool.h"
#include "Tools/Index/GetBlueprintDependenciesTool.h"
#include "Tools/Blueprint/Inspection/GetBlueprintInfoTool.h"
#include "Tools/Blueprint/Inspection/GetBlueprintOverviewTool.h"
#include "Tools/Blueprint/Inspection/GetBlueprintInterfacesTool.h"
#include "Tools/Assets/GetDependenciesTool.h"
#include "Tools/Index/GetIndexStatusTool.h"
#include "Tools/Blueprint/Inspection/InspectBlueprintNodeTool.h"
#include "Tools/Input/InspectEnhancedInputActionWiringTool.h"
#include "Tools/Index/GetLevelActorDependenciesTool.h"
#include "Tools/System/GetMutationRequestStatusTool.h"
#include "Tools/Blueprint/Inspection/GetParentBlueprintTool.h"
#include "Tools/Assets/GetReferencersTool.h"
#include "Tools/System/GetServerInfoTool.h"
#include "Tools/System/HealthCheckTool.h"
#include "Tools/Blueprint/Members/SetBlueprintComponentDefaultsTool.h"
#include "Tools/Editor/ListActorsTool.h"
#include "Tools/Assets/ListAssetsTool.h"
#include "Tools/Blueprint/Inspection/ListBlueprintComponentsTool.h"
#include "Tools/Blueprint/Inspection/ListBlueprintFunctionsTool.h"
#include "Tools/Blueprint/Inspection/ListBlueprintGraphsTool.h"
#include "Tools/Blueprint/Inspection/InspectLiveBlueprintTool.h"
#include "Tools/Blueprint/Inspection/TraceLiveBlueprintFlowTool.h"
#include "Tools/Blueprint/Inspection/ListBlueprintNodePinsTool.h"
#include "Tools/Blueprint/Inspection/ListBlueprintVariablesTool.h"
#include "Tools/Blueprint/Inspection/ListChildBlueprintsTool.h"
#include "Tools/Assets/ListFoldersTool.h"
#include "Tools/Editor/ListUnrealMCPAutomationTestsTool.h"
#include "Tools/Editor/ListSelectedActorsTool.h"
#include "Tools/System/ListToolsTool.h"
#include "Tools/Blueprint/Graph/LayoutBlueprintNodesTool.h"
#include "Tools/Blueprint/Graph/MoveBlueprintNodeTool.h"
#include "Tools/Index/PlanProjectRefactorTool.h"
#include "Tools/Blueprint/Members/RefreshBlueprintCallSitesTool.h"
#include "Tools/Index/RefreshProjectIndexTool.h"
#include "Tools/Assets/SearchAssetsTool.h"
#include "Tools/Blueprint/Graph/SetBlueprintPinDefaultObjectTool.h"
#include "Tools/Blueprint/Graph/SetBlueprintPinDefaultValueTool.h"
#include "Tools/Blueprint/Graph/SetBlueprintPinSplitTool.h"
#include "Tools/Blueprint/Members/SetBlueprintFunctionMetadataTool.h"
#include "Tools/Blueprint/Graph/SetBlueprintNodeCommentTool.h"
#include "Tools/Blueprint/Graph/SetBlueprintSequenceOutputsTool.h"
#include "Tools/Blueprint/Graph/SpliceBlueprintExecFlowTool.h"
#include "Tools/Blueprint/Inspection/SaveBlueprintTool.h"
#include "Tools/Blueprint/Inspection/SaveValidatedBlueprintsTool.h"
#include "Tools/Editor/SaveAllDirtyPackagesTool.h"
#include "Tools/Editor/RunUnrealMCPAutomationTestTool.h"
#include "Tools/Editor/RunUnrealMCPAutomationTestsTool.h"
#include "Tools/Index/SummarizeBlueprintClusterTool.h"
#include "Tools/Blueprint/Inspection/TraceBlueprintFlowTool.h"
#include "Tools/Index/TraceFeatureFlowTool.h"
#include "Tools/Blueprint/Inspection/ValidateBlueprintTool.h"
#include "Tools/Blueprint/Graph/WireBlueprintEventToFunctionTool.h"
#include "Tools/Input/WireEnhancedInputActionToComponentTool.h"
#include "Tools/Blueprint/Graph/WireSelectionWorkflowTool.h"
#include "Transport/NamedPipeMCPTransport.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"

#include "Misc/App.h"
#include "Misc/EngineVersion.h"

DEFINE_LOG_CATEGORY(LogUnrealMCP);

IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP)

FUnrealMCPModule& FUnrealMCPModule::Get()
{
    return FModuleManager::LoadModuleChecked<FUnrealMCPModule>(TEXT("UnrealMCP"));
}

bool FUnrealMCPModule::IsAvailable()
{
    return FModuleManager::Get().IsModuleLoaded(TEXT("UnrealMCP"));
}

void FUnrealMCPModule::StartupModule()
{
    Server = MakeUnique<FMCPServer>();
    ProjectIndex = MakeUnique<FUnrealMCPProjectIndex>();
    RegisterCoreTools();
    RegisterTestCommands();
    RegisterEditorControls();

    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();

    UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP started. Enabled=%s ServerName=%s Version=%s Engine=%s IndexInitOnStartup=%s LiveIndexTracking=%s"),
        Settings->bEnableServer ? TEXT("true") : TEXT("false"),
        *Settings->ServerName,
        *Settings->ServerVersion,
        *FEngineVersion::Current().ToString(),
        Settings->bInitializeProjectIndexOnStartup ? TEXT("true") : TEXT("false"),
        Settings->bEnableLiveIndexTracking ? TEXT("true") : TEXT("false"));

    if (Settings->bInitializeProjectIndexOnStartup)
    {
        EnsureProjectIndexInitialized();
    }
    else
    {
        UE_LOG(LogUnrealMCP, Log, TEXT("Project index initialization deferred until first use."));
    }

    StartConfiguredTransport();
}

void FUnrealMCPModule::ShutdownModule()
{
    StopTransport();
    UnregisterEditorControls();
    UnregisterTestCommands();
    ProjectIndex.Reset();
    Server.Reset();
    UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP shut down."));
}

FMCPServer& FUnrealMCPModule::GetServer()
{
    check(Server.IsValid());
    return *Server;
}

const FMCPServer& FUnrealMCPModule::GetServer() const
{
    check(Server.IsValid());
    return *Server;
}

FUnrealMCPProjectIndex& FUnrealMCPModule::GetProjectIndex()
{
    check(ProjectIndex.IsValid());
    EnsureProjectIndexInitialized();
    return *ProjectIndex;
}

const FUnrealMCPProjectIndex& FUnrealMCPModule::GetProjectIndex() const
{
    check(ProjectIndex.IsValid());
    EnsureProjectIndexInitialized();
    return *ProjectIndex;
}

bool FUnrealMCPModule::EnsureProjectIndexInitialized() const
{
    check(ProjectIndex.IsValid());
    if (ProjectIndex->GetStatusSnapshot().bDatabaseOpen)
    {
        return true;
    }

    const bool bInitialized = ProjectIndex->Initialize();
    if (!bInitialized)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Deferred project index initialization failed."));
    }
    return bInitialized;
}

FString FUnrealMCPModule::GetEffectiveNamedPipeName() const
{
    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
    const FString ExplicitPipeName = SanitizeNamedPipeToken(Settings->NamedPipeName);
    if (!ExplicitPipeName.IsEmpty())
    {
        return ExplicitPipeName;
    }

    const FString BasePipeName = TEXT("UnrealMCP");
    if (!Settings->bUseProjectSpecificNamedPipe)
    {
        return BasePipeName;
    }

    const FString ProjectToken = SanitizeNamedPipeToken(FApp::GetProjectName());
    return ProjectToken.IsEmpty()
        ? BasePipeName
        : FString::Printf(TEXT("%s_%s"), *BasePipeName, *ProjectToken);
}

FString FUnrealMCPModule::SanitizeNamedPipeToken(const FString& Value)
{
    FString Result;
    Result.Reserve(Value.Len());

    for (const TCHAR Character : Value)
    {
        if (FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-'))
        {
            Result.AppendChar(Character);
        }
    }

    return Result;
}

void FUnrealMCPModule::RegisterCoreTools()
{
    FMCPToolRegistry& Registry = GetServer().GetToolRegistry();
    Registry.RegisterTool(MakeShared<FHealthCheckTool>());
    Registry.RegisterTool(MakeShared<FGetServerInfoTool>());
    Registry.RegisterTool(MakeShared<FGetMutationRequestStatusTool>());
    Registry.RegisterTool(MakeShared<FGetIndexStatusTool>());
    Registry.RegisterTool(MakeShared<FBuildProjectIndexTool>());
    Registry.RegisterTool(MakeShared<FRefreshProjectIndexTool>());
    Registry.RegisterTool(MakeShared<FGetAssetReferencesTool>());
    Registry.RegisterTool(MakeShared<FGetAssetReferencersTool>());
    Registry.RegisterTool(MakeShared<FExplainBlueprintRoleTool>());
    Registry.RegisterTool(MakeShared<FExplainBlueprintTraceTool>());
    Registry.RegisterTool(MakeShared<FExplainFeatureWorkflowTool>());
    Registry.RegisterTool(MakeShared<FFindFeatureEntryPointsTool>());
    Registry.RegisterTool(MakeShared<FSummarizeBlueprintClusterTool>());
    Registry.RegisterTool(MakeShared<FTraceBlueprintFlowTool>());
    Registry.RegisterTool(MakeShared<FTraceFeatureFlowTool>());
    Registry.RegisterTool(MakeShared<FFindCircularDependenciesTool>());
    Registry.RegisterTool(MakeShared<FAnalyzeBlueprintCouplingTool>());
    Registry.RegisterTool(MakeShared<FFindBrokenBlueprintReferencesTool>());
    Registry.RegisterTool(MakeShared<FFindUnusedBlueprintAssetsTool>());
    Registry.RegisterTool(MakeShared<FAnalyzeFeatureBoundaryTool>());
    Registry.RegisterTool(MakeShared<FAnalyzeProjectArchitectureTool>());
    Registry.RegisterTool(MakeShared<FPlanProjectRefactorTool>());
    Registry.RegisterTool(MakeShared<FSearchAssetsTool>());
    Registry.RegisterTool(MakeShared<FAssetExistsTool>());
    Registry.RegisterTool(MakeShared<FGetAssetInfoTool>());
    Registry.RegisterTool(MakeShared<FInspectDataTableRowsTool>());
    Registry.RegisterTool(MakeShared<FFindDataTableReferencesTool>());
    Registry.RegisterTool(MakeShared<FDataTableRowMutationTool>(FDataTableRowMutationTool::EAction::Set));
    Registry.RegisterTool(MakeShared<FDataTableRowMutationTool>(FDataTableRowMutationTool::EAction::Add));
    Registry.RegisterTool(MakeShared<FDataTableRowMutationTool>(FDataTableRowMutationTool::EAction::Remove));
    Registry.RegisterTool(MakeShared<FSaveDataTableTool>());
    Registry.RegisterTool(MakeShared<FListAssetsTool>());
    Registry.RegisterTool(MakeShared<FListFoldersTool>());
    Registry.RegisterTool(MakeShared<FGetDependenciesTool>());
    Registry.RegisterTool(MakeShared<FGetReferencersTool>());
    Registry.RegisterTool(MakeShared<FFindAssetsByClassTool>());
    Registry.RegisterTool(MakeShared<FFindAssetsByPathTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintInfoTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintOverviewTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintDependenciesTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintComponentHierarchyTool>());
    Registry.RegisterTool(MakeShared<FFindBlueprintNodeReferencesTool>());
    Registry.RegisterTool(MakeShared<FInspectBlueprintNodeTool>());
    Registry.RegisterTool(MakeShared<FFindBlueprintTraceStartPointsTool>());
    Registry.RegisterTool(MakeShared<FFindCrossBlueprintCallsTool>());
    Registry.RegisterTool(MakeShared<FFindBlueprintVariableUsageTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintVariablesTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintFunctionsTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintGraphsTool>());
    Registry.RegisterTool(MakeShared<FInspectLiveBlueprintTool>());
    Registry.RegisterTool(MakeShared<FTraceLiveBlueprintFlowTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintComponentsTool>());
    Registry.RegisterTool(MakeShared<FGetParentBlueprintTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintInterfacesTool>());
    Registry.RegisterTool(MakeShared<FListChildBlueprintsTool>());
    Registry.RegisterTool(MakeShared<FListActorsTool>());
    Registry.RegisterTool(MakeShared<FGetActorInfoTool>());
    Registry.RegisterTool(MakeShared<FFindActorsUsingBlueprintTool>());
    Registry.RegisterTool(MakeShared<FGetLevelActorDependenciesTool>());
    Registry.RegisterTool(MakeShared<FListSelectedActorsTool>());
    Registry.RegisterTool(MakeShared<FCompileBlueprintTool>());
    Registry.RegisterTool(MakeShared<FFindClassUsagesTool>());
    Registry.RegisterTool(MakeShared<FCompareBlueprintMergeStructureTool>());
    Registry.RegisterTool(MakeShared<FCompareBlueprintGraphPreservationTool>());
    Registry.RegisterTool(MakeShared<FApplyBlueprintMergePlanTool>());
    Registry.RegisterTool(MakeShared<FBlueprintGitConflictTool>(false));
    Registry.RegisterTool(MakeShared<FBlueprintGitConflictTool>(true));
    Registry.RegisterTool(MakeShared<FPreviewBlueprintSideReplayTool>());
    Registry.RegisterTool(MakeShared<FUnrealMCPGitHistoryTool>(FUnrealMCPGitHistoryTool::EOperation::History));
    Registry.RegisterTool(MakeShared<FUnrealMCPGitHistoryTool>(FUnrealMCPGitHistoryTool::EOperation::Changes));
    Registry.RegisterTool(MakeShared<FUnrealMCPGitHistoryTool>(FUnrealMCPGitHistoryTool::EOperation::BlueprintGraph));
    Registry.RegisterTool(MakeShared<FApplyBlueprintMergeTool>());
    Registry.RegisterTool(MakeShared<FApplyBlueprintMergeTool>(FApplyBlueprintMergeTool::EOperation::ComponentDefaults));
    Registry.RegisterTool(MakeShared<FApplyBlueprintMergeTool>(FApplyBlueprintMergeTool::EOperation::AddInterface));
    Registry.RegisterTool(MakeShared<FApplyBlueprintMergeTool>(FApplyBlueprintMergeTool::EOperation::AddVariable));
    Registry.RegisterTool(MakeShared<FApplyBlueprintMergeTool>(FApplyBlueprintMergeTool::EOperation::Structure));
    Registry.RegisterTool(MakeShared<FFindBlueprintNodesTool>(FFindBlueprintNodesTool::ESearch::PinType));
    Registry.RegisterTool(MakeShared<FFindBlueprintNodesTool>(FFindBlueprintNodesTool::ESearch::Member));
    Registry.RegisterTool(MakeShared<FRetargetBlueprintNodeTool>());
    Registry.RegisterTool(MakeShared<FBlueprintMemberGraphTool>(EBlueprintMemberGraphEdit::RetargetMember));
    Registry.RegisterTool(MakeShared<FBlueprintMemberGraphTool>(EBlueprintMemberGraphEdit::SwitchCases));
    Registry.RegisterTool(MakeShared<FOverrideBlueprintFunctionTool>());
    Registry.RegisterTool(MakeShared<FGetEditorWidgetSnapshotTool>());
    Registry.RegisterTool(MakeShared<FCaptureEditorWidgetTool>());
    Registry.RegisterTool(MakeShared<FFrameBlueprintGraphTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintLocalVariablesTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintLocalVariableTool>());
    Registry.RegisterTool(MakeShared<FRemoveBlueprintLocalVariableTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintLocalVariableDefaultTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintClassDefaultsTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintClassDefaultsTool>());
    Registry.RegisterTool(MakeShared<FResetBlueprintClassDefaultsTool>());
    Registry.RegisterTool(MakeShared<FCompareBlueprintClassDefaultsTool>());
    Registry.RegisterTool(MakeShared<FDiscardDirtyPackagesForShutdownTool>());
    Registry.RegisterTool(MakeShared<FConsolidateAndDeleteAssetTool>());
    Registry.RegisterTool(MakeShared<FCompileAllBlueprintsTool>());
    Registry.RegisterTool(MakeShared<FCreateBlueprintAssetTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintComponentTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintComponentsTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintComponentDefaultsTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintComponentDefaultsTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintVariableTool>());
    Registry.RegisterTool(MakeShared<FSaveBlueprintTool>());
    Registry.RegisterTool(MakeShared<FSaveValidatedBlueprintsTool>());
    Registry.RegisterTool(MakeShared<FSaveAllDirtyPackagesTool>());
    Registry.RegisterTool(MakeShared<FListUnrealMCPAutomationTestsTool>());
    Registry.RegisterTool(MakeShared<FRunUnrealMCPAutomationTestTool>());
    Registry.RegisterTool(MakeShared<FRunUnrealMCPAutomationTestsTool>());
    Registry.RegisterTool(MakeShared<FValidateBlueprintTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintBranchNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintMacroNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintArrayOperationNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintSetOperationNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintMapOperationNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintTypedOperatorNodeTool>());
    Registry.RegisterTool(MakeShared<FMoveBlueprintNodeTool>());
    Registry.RegisterTool(MakeShared<FConnectBlueprintPinsTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintPinDefaultObjectTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintPinDefaultValueTool>());
    Registry.RegisterTool(MakeShared<FDeleteBlueprintNodeTool>());
    Registry.RegisterTool(MakeShared<FPruneUnreachableNodesTool>());
    Registry.RegisterTool(MakeShared<FRefreshBlueprintNodesTool>());
    Registry.RegisterTool(MakeShared<FBlueprintGraphDslTool>(false));
    Registry.RegisterTool(MakeShared<FBlueprintGraphDslTool>(true));
    Registry.RegisterTool(MakeShared<FWrapWithIsValidTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintSequenceNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintCustomEventNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintFunctionCallNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintFunctionParameterTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintParameterTool>());
    Registry.RegisterTool(MakeShared<FRemoveBlueprintMemberTool>(false));
    Registry.RegisterTool(MakeShared<FRemoveBlueprintMemberTool>(true));
    Registry.RegisterTool(MakeShared<FStructMemberValueMigrationTool>(false));
    Registry.RegisterTool(MakeShared<FStructMemberValueMigrationTool>(true));
    Registry.RegisterTool(MakeShared<FUserDefinedStructMemberTool>(EUserStructMemberOperation::Inspect));
    Registry.RegisterTool(MakeShared<FUserDefinedStructMemberTool>(EUserStructMemberOperation::Add));
    Registry.RegisterTool(MakeShared<FUserDefinedStructMemberTool>(EUserStructMemberOperation::Remove));
    Registry.RegisterTool(MakeShared<FUserDefinedStructMemberTool>(EUserStructMemberOperation::Rename));
    Registry.RegisterTool(MakeShared<FUserDefinedStructMemberTool>(EUserStructMemberOperation::SetDefault));
    Registry.RegisterTool(MakeShared<FUserDefinedStructMemberTool>(EUserStructMemberOperation::InspectEnum));
    Registry.RegisterTool(MakeShared<FUserDefinedStructMemberTool>(EUserStructMemberOperation::AddEnum));
    Registry.RegisterTool(MakeShared<FUserDefinedStructMemberTool>(EUserStructMemberOperation::SetEnumDisplayName));
    Registry.RegisterTool(MakeShared<FReparentBlueprintTool>());
    Registry.RegisterTool(MakeShared<FMoveAssetTool>());
    Registry.RegisterTool(MakeShared<FFixupRedirectorsTool>());
    Registry.RegisterTool(MakeShared<FDuplicateAssetTool>());
    Registry.RegisterTool(MakeShared<FDeleteAssetTool>());
    Registry.RegisterTool(MakeShared<FRefreshBlueprintCallSitesTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintInterfaceEventNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintInterfaceFunctionGraphTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintInterfaceTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintOverrideEventNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintDelegateNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintDelegateEventNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintDelegateBroadcastNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintVariableGetNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintVariableSetNodeTool>());
    Registry.RegisterTool(MakeShared<FDisconnectBlueprintPinsTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintNodePinsTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintCastNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintRerouteNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintCommentNodeTool>());
    Registry.RegisterTool(MakeShared<FCreateBlueprintFunctionGraphTool>());
    Registry.RegisterTool(MakeShared<FAddEnhancedInputActionNodeTool>());
    Registry.RegisterTool(MakeShared<FInspectEnhancedInputActionWiringTool>());
    Registry.RegisterTool(MakeShared<FCreateInputActionTool>());
    Registry.RegisterTool(MakeShared<FCreateInputMappingContextTool>());
    Registry.RegisterTool(MakeShared<FAddInputMappingContextMappingTool>());
    Registry.RegisterTool(MakeShared<FRemoveInputMappingContextMappingTool>());
    Registry.RegisterTool(MakeShared<FGetInputMappingContextMappingsTool>());
    Registry.RegisterTool(MakeShared<FSetInputMappingContextMappingKeyTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintPinSplitTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintFunctionMetadataTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintNodeCommentTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintSequenceOutputsTool>());
    Registry.RegisterTool(MakeShared<FLayoutBlueprintNodesTool>());
    Registry.RegisterTool(MakeShared<FApplyBlueprintInteractionPlanTool>());
    Registry.RegisterTool(MakeShared<FApplyBlueprintGraphPatchTool>());
    Registry.RegisterTool(MakeShared<FSpliceBlueprintExecFlowTool>());
    Registry.RegisterTool(MakeShared<FWireBlueprintEventToFunctionTool>());
    Registry.RegisterTool(MakeShared<FWireEnhancedInputActionToComponentTool>());
    Registry.RegisterTool(MakeShared<FWireSelectionWorkflowTool>());
    Registry.RegisterTool(MakeShared<FListToolsTool>(Registry));
}

void FUnrealMCPModule::StartConfiguredTransport()
{
    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
    if (!Settings->bEnableServer)
    {
        UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP transport startup skipped because the server is disabled."));
        return;
    }

    // Cooking/packaging runs UnrealEditor-Cmd (still the Editor target, so TargetAllowList does not
    // exclude it) and would contend with the open editor for the same pipe name (Win32 231, pipe busy),
    // failing the package with an Error. Only an interactive editor serves agents.
    if (IsRunningCommandlet() || !GIsEditor)
    {
        UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP transport not started: commandlet or non-editor process."));
        return;
    }

#if PLATFORM_WINDOWS
    if (Settings->bEnableNamedPipeTransport)
    {
        const FString EffectivePipeName = GetEffectiveNamedPipeName();
        Transport = MakeUnique<FNamedPipeMCPTransport>(EffectivePipeName);
        if (Transport->Start(GetServer()))
        {
            UE_LOG(LogUnrealMCP, Log, TEXT("Started %s transport. EffectivePipe=%s"), *Transport->GetTransportName(), *EffectivePipeName);
            return;
        }

        UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to start named pipe transport '%s'."), *EffectivePipeName);
        Transport.Reset();
    }
#endif

    if (Settings->bEnableStdIOTransport)
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("StdIO transport is enabled in settings but not implemented yet."));
    }
}

void FUnrealMCPModule::StopTransport()
{
    if (Transport.IsValid())
    {
        Transport->Stop();
        UE_LOG(LogUnrealMCP, Log, TEXT("Stopped %s transport."), *Transport->GetTransportName());
        Transport.Reset();
    }
}

FString FUnrealMCPModule::ExecuteToolJson(const FString& ToolName, const FString& ParamsJson)
{
    // The integration uses the same dispatch, operation tracking and tool validation as the adapter.
    TSharedPtr<FJsonObject> Params;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ParamsJson), Params) || !Params.IsValid())
        return TEXT("{\"error\":{\"message\":\"Invalid tool parameters JSON\"}}");
    UnrealMCP::FMCPRequest Request; Request.Id = FGuid::NewGuid().ToString(); Request.Method = ToolName; Request.Params = Params;
    FMCPServer& Server = Get().GetServer();
    return Server.SerializeResponse(Server.HandleRequest(Request));
}
