#include "UnrealMCPModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

#include "Index/UnrealMCPProjectIndex.h"
#include "MCP/MCPServer.h"
#include "Tools/AssetExistsTool.h"
#include "Tools/ApplyBlueprintInteractionPlanTool.h"
#include "Tools/ApplyBlueprintGraphPatchTool.h"
#include "Tools/AddBlueprintComponentTool.h"
#include "Tools/AddBlueprintComponentsTool.h"
#include "Tools/AddBlueprintBranchNodeTool.h"
#include "Tools/AddBlueprintMacroNodeTool.h"
#include "Tools/AddBlueprintArrayOperationNodeTool.h"
#include "Tools/AddBlueprintSetOperationNodeTool.h"
#include "Tools/AddBlueprintMapOperationNodeTool.h"
#include "Tools/AddBlueprintTypedOperatorNodeTool.h"
#include "Tools/AddBlueprintCastNodeTool.h"
#include "Tools/AddBlueprintCommentNodeTool.h"
#include "Tools/AddBlueprintCustomEventNodeTool.h"
#include "Tools/AddBlueprintFunctionCallNodeTool.h"
#include "Tools/AddBlueprintFunctionParameterTool.h"
#include "Tools/SetBlueprintParameterTool.h"
#include "Tools/AddBlueprintInterfaceEventNodeTool.h"
#include "Tools/AddBlueprintInterfaceFunctionGraphTool.h"
#include "Tools/AddBlueprintInterfaceTool.h"
#include "Tools/AddBlueprintOverrideEventNodeTool.h"
#include "Tools/AddBlueprintDelegateNodeTool.h"
#include "Tools/AddBlueprintDelegateEventNodeTool.h"
#include "Tools/AddBlueprintDelegateBroadcastNodeTool.h"
#include "Tools/AddBlueprintRerouteNodeTool.h"
#include "Tools/AddBlueprintSequenceNodeTool.h"
#include "Tools/AddBlueprintVariableGetNodeTool.h"
#include "Tools/AddBlueprintVariableSetNodeTool.h"
#include "Tools/AddBlueprintVariableTool.h"
#include "Tools/AddEnhancedInputActionNodeTool.h"
#include "Tools/AnalyzeBlueprintCouplingTool.h"
#include "Tools/AnalyzeFeatureBoundaryTool.h"
#include "Tools/AnalyzeProjectArchitectureTool.h"
#include "Tools/BuildProjectIndexTool.h"
#include "Tools/CompileAllBlueprintsTool.h"
#include "Tools/AddInputMappingContextMappingTool.h"
#include "Tools/ConnectBlueprintPinsTool.h"
#include "Tools/CompileBlueprintTool.h"
#include "Tools/FindClassUsagesTool.h"
#include "Tools/RetargetBlueprintNodeTool.h"
#include "Tools/BlueprintLocalVariableTools.h"
#include "Tools/BlueprintClassDefaultsTools.h"
#include "Tools/DiscardDirtyPackagesForShutdownTool.h"
#include "Tools/ConsolidateAndDeleteAssetTool.h"
#include "Tools/CreateBlueprintAssetTool.h"
#include "Tools/CreateBlueprintFunctionGraphTool.h"
#include "Tools/CreateInputActionTool.h"
#include "Tools/CreateInputMappingContextTool.h"
#include "Tools/DeleteBlueprintNodeTool.h"
#include "Tools/GetInputMappingContextMappingsTool.h"
#include "Tools/SetInputMappingContextMappingKeyTool.h"
#include "Tools/RemoveInputMappingContextMappingTool.h"
#include "Tools/DisconnectBlueprintPinsTool.h"
#include "Tools/ExplainBlueprintRoleTool.h"
#include "Tools/ExplainBlueprintTraceTool.h"
#include "Tools/ExplainFeatureWorkflowTool.h"
#include "Tools/FindActorsUsingBlueprintTool.h"
#include "Tools/FindAssetsByClassTool.h"
#include "Tools/FindAssetsByPathTool.h"
#include "Tools/FindBlueprintNodeReferencesTool.h"
#include "Tools/FindBlueprintTraceStartPointsTool.h"
#include "Tools/FindCrossBlueprintCallsTool.h"
#include "Tools/FindCircularDependenciesTool.h"
#include "Tools/FindBlueprintVariableUsageTool.h"
#include "Tools/FindBrokenBlueprintReferencesTool.h"
#include "Tools/FindFeatureEntryPointsTool.h"
#include "Tools/FindUnusedBlueprintAssetsTool.h"
#include "Tools/GetActorInfoTool.h"
#include "Tools/GetAssetInfoTool.h"
#include "Tools/DataTableRowMutationTool.h"
#include "Tools/FindDataTableReferencesTool.h"
#include "Tools/SaveDataTableTool.h"
#include "Tools/InspectDataTableRowsTool.h"
#include "Tools/GetAssetReferencersTool.h"
#include "Tools/GetAssetReferencesTool.h"
#include "Tools/GetBlueprintComponentHierarchyTool.h"
#include "Tools/GetBlueprintComponentDefaultsTool.h"
#include "Tools/GetBlueprintDependenciesTool.h"
#include "Tools/GetBlueprintInfoTool.h"
#include "Tools/GetBlueprintOverviewTool.h"
#include "Tools/GetBlueprintInterfacesTool.h"
#include "Tools/GetDependenciesTool.h"
#include "Tools/GetIndexStatusTool.h"
#include "Tools/InspectBlueprintNodeTool.h"
#include "Tools/InspectEnhancedInputActionWiringTool.h"
#include "Tools/GetLevelActorDependenciesTool.h"
#include "Tools/GetMutationRequestStatusTool.h"
#include "Tools/GetParentBlueprintTool.h"
#include "Tools/GetReferencersTool.h"
#include "Tools/GetServerInfoTool.h"
#include "Tools/HealthCheckTool.h"
#include "Tools/SetBlueprintComponentDefaultsTool.h"
#include "Tools/ListActorsTool.h"
#include "Tools/ListAssetsTool.h"
#include "Tools/ListBlueprintComponentsTool.h"
#include "Tools/ListBlueprintFunctionsTool.h"
#include "Tools/ListBlueprintGraphsTool.h"
#include "Tools/InspectLiveBlueprintTool.h"
#include "Tools/TraceLiveBlueprintFlowTool.h"
#include "Tools/ListBlueprintNodePinsTool.h"
#include "Tools/ListBlueprintVariablesTool.h"
#include "Tools/ListChildBlueprintsTool.h"
#include "Tools/ListFoldersTool.h"
#include "Tools/ListUnrealMCPAutomationTestsTool.h"
#include "Tools/ListSelectedActorsTool.h"
#include "Tools/ListToolsTool.h"
#include "Tools/LayoutBlueprintNodesTool.h"
#include "Tools/MoveBlueprintNodeTool.h"
#include "Tools/PlanProjectRefactorTool.h"
#include "Tools/RefreshBlueprintCallSitesTool.h"
#include "Tools/RefreshProjectIndexTool.h"
#include "Tools/SearchAssetsTool.h"
#include "Tools/SetBlueprintPinDefaultObjectTool.h"
#include "Tools/SetBlueprintPinDefaultValueTool.h"
#include "Tools/SetBlueprintPinSplitTool.h"
#include "Tools/SetBlueprintFunctionMetadataTool.h"
#include "Tools/SetBlueprintNodeCommentTool.h"
#include "Tools/SetBlueprintSequenceOutputsTool.h"
#include "Tools/SpliceBlueprintExecFlowTool.h"
#include "Tools/SaveBlueprintTool.h"
#include "Tools/SaveValidatedBlueprintsTool.h"
#include "Tools/SaveAllDirtyPackagesTool.h"
#include "Tools/RunUnrealMCPAutomationTestTool.h"
#include "Tools/RunUnrealMCPAutomationTestsTool.h"
#include "Tools/SummarizeBlueprintClusterTool.h"
#include "Tools/TraceBlueprintFlowTool.h"
#include "Tools/TraceFeatureFlowTool.h"
#include "Tools/ValidateBlueprintTool.h"
#include "Tools/WireBlueprintEventToFunctionTool.h"
#include "Tools/WireEnhancedInputActionToComponentTool.h"
#include "Tools/WireSelectionWorkflowTool.h"
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
    Registry.RegisterTool(MakeShared<FRetargetBlueprintNodeTool>());
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
    Registry.RegisterTool(MakeShared<FAddBlueprintSequenceNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintCustomEventNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintFunctionCallNodeTool>());
    Registry.RegisterTool(MakeShared<FAddBlueprintFunctionParameterTool>());
    Registry.RegisterTool(MakeShared<FSetBlueprintParameterTool>());
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
