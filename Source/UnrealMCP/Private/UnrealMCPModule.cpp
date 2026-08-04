#include "UnrealMCPModule.h"

#include "Index/UnrealMCPProjectIndex.h"
#include "MCP/MCPServer.h"
#include "Tools/AssetExistsTool.h"
#include "Tools/BuildProjectIndexTool.h"
#include "Tools/CompileAllBlueprintsTool.h"
#include "Tools/CompileBlueprintTool.h"
#include "Tools/ExplainBlueprintRoleTool.h"
#include "Tools/ExplainBlueprintTraceTool.h"
#include "Tools/ExplainFeatureWorkflowTool.h"
#include "Tools/FindActorsUsingBlueprintTool.h"
#include "Tools/FindAssetsByClassTool.h"
#include "Tools/FindAssetsByPathTool.h"
#include "Tools/FindBlueprintNodeReferencesTool.h"
#include "Tools/FindBlueprintTraceStartPointsTool.h"
#include "Tools/FindCrossBlueprintCallsTool.h"
#include "Tools/FindBlueprintVariableUsageTool.h"
#include "Tools/FindFeatureEntryPointsTool.h"
#include "Tools/GetActorInfoTool.h"
#include "Tools/GetAssetInfoTool.h"
#include "Tools/GetAssetReferencersTool.h"
#include "Tools/GetAssetReferencesTool.h"
#include "Tools/GetBlueprintComponentHierarchyTool.h"
#include "Tools/GetBlueprintDependenciesTool.h"
#include "Tools/GetBlueprintInfoTool.h"
#include "Tools/GetBlueprintInterfacesTool.h"
#include "Tools/GetDependenciesTool.h"
#include "Tools/GetIndexStatusTool.h"
#include "Tools/InspectBlueprintNodeTool.h"
#include "Tools/GetLevelActorDependenciesTool.h"
#include "Tools/GetParentBlueprintTool.h"
#include "Tools/GetReferencersTool.h"
#include "Tools/GetServerInfoTool.h"
#include "Tools/HealthCheckTool.h"
#include "Tools/ListActorsTool.h"
#include "Tools/ListAssetsTool.h"
#include "Tools/ListBlueprintComponentsTool.h"
#include "Tools/ListBlueprintFunctionsTool.h"
#include "Tools/ListBlueprintVariablesTool.h"
#include "Tools/ListChildBlueprintsTool.h"
#include "Tools/ListFoldersTool.h"
#include "Tools/ListSelectedActorsTool.h"
#include "Tools/ListToolsTool.h"
#include "Tools/RefreshProjectIndexTool.h"
#include "Tools/SearchAssetsTool.h"
#include "Tools/SummarizeBlueprintClusterTool.h"
#include "Tools/TraceBlueprintFlowTool.h"
#include "Tools/TraceFeatureFlowTool.h"
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
    Registry.RegisterTool(MakeShared<FSearchAssetsTool>());
    Registry.RegisterTool(MakeShared<FAssetExistsTool>());
    Registry.RegisterTool(MakeShared<FGetAssetInfoTool>());
    Registry.RegisterTool(MakeShared<FListAssetsTool>());
    Registry.RegisterTool(MakeShared<FListFoldersTool>());
    Registry.RegisterTool(MakeShared<FGetDependenciesTool>());
    Registry.RegisterTool(MakeShared<FGetReferencersTool>());
    Registry.RegisterTool(MakeShared<FFindAssetsByClassTool>());
    Registry.RegisterTool(MakeShared<FFindAssetsByPathTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintInfoTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintDependenciesTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintComponentHierarchyTool>());
    Registry.RegisterTool(MakeShared<FFindBlueprintNodeReferencesTool>());
    Registry.RegisterTool(MakeShared<FInspectBlueprintNodeTool>());
    Registry.RegisterTool(MakeShared<FFindBlueprintTraceStartPointsTool>());
    Registry.RegisterTool(MakeShared<FFindCrossBlueprintCallsTool>());
    Registry.RegisterTool(MakeShared<FFindBlueprintVariableUsageTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintVariablesTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintFunctionsTool>());
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
    Registry.RegisterTool(MakeShared<FCompileAllBlueprintsTool>());
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
