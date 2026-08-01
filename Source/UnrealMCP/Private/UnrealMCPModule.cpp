#include "UnrealMCPModule.h"

#include "Index/UnrealMCPProjectIndex.h"
#include "MCP/MCPServer.h"
#include "Tools/AssetExistsTool.h"
#include "Tools/BuildProjectIndexTool.h"
#include "Tools/CompileAllBlueprintsTool.h"
#include "Tools/CompileBlueprintTool.h"
#include "Tools/ExplainBlueprintRoleTool.h"
#include "Tools/FindAssetsByClassTool.h"
#include "Tools/FindAssetsByPathTool.h"
#include "Tools/FindFeatureEntryPointsTool.h"
#include "Tools/GetAssetReferencesTool.h"
#include "Tools/GetAssetReferencersTool.h"
#include "Tools/GetBlueprintInfoTool.h"
#include "Tools/GetBlueprintDependenciesTool.h"
#include "Tools/GetBlueprintComponentHierarchyTool.h"
#include "Tools/GetBlueprintInterfacesTool.h"
#include "Tools/GetIndexStatusTool.h"
#include "Tools/GetParentBlueprintTool.h"
#include "Tools/GetAssetInfoTool.h"
#include "Tools/GetDependenciesTool.h"
#include "Tools/GetReferencersTool.h"
#include "Tools/GetServerInfoTool.h"
#include "Tools/HealthCheckTool.h"
#include "Tools/ListBlueprintComponentsTool.h"
#include "Tools/ListBlueprintFunctionsTool.h"
#include "Tools/ListBlueprintVariablesTool.h"
#include "Tools/ListChildBlueprintsTool.h"
#include "Tools/ListAssetsTool.h"
#include "Tools/ListFoldersTool.h"
#include "Tools/ListToolsTool.h"
#include "Tools/SearchAssetsTool.h"
#include "Tools/SummarizeBlueprintClusterTool.h"
#include "Tools/TraceFeatureFlowTool.h"
#include "Transport/NamedPipeMCPTransport.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
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
    ProjectIndex->Initialize();
    RegisterCoreTools();
    RegisterTestCommands();

    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();

    UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP started. Enabled=%s ServerName=%s Version=%s Engine=%s"),
        Settings->bEnableServer ? TEXT("true") : TEXT("false"),
        *Settings->ServerName,
        *Settings->ServerVersion,
        *FEngineVersion::Current().ToString());

    StartConfiguredTransport();
}

void FUnrealMCPModule::ShutdownModule()
{
    StopTransport();
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
    return *ProjectIndex;
}

const FUnrealMCPProjectIndex& FUnrealMCPModule::GetProjectIndex() const
{
    check(ProjectIndex.IsValid());
    return *ProjectIndex;
}

void FUnrealMCPModule::RegisterCoreTools()
{
    FMCPToolRegistry& Registry = GetServer().GetToolRegistry();
    Registry.RegisterTool(MakeShared<FHealthCheckTool>());
    Registry.RegisterTool(MakeShared<FGetServerInfoTool>());
    Registry.RegisterTool(MakeShared<FGetIndexStatusTool>());
    Registry.RegisterTool(MakeShared<FBuildProjectIndexTool>());
    Registry.RegisterTool(MakeShared<FGetAssetReferencesTool>());
    Registry.RegisterTool(MakeShared<FGetAssetReferencersTool>());
    Registry.RegisterTool(MakeShared<FExplainBlueprintRoleTool>());
    Registry.RegisterTool(MakeShared<FFindFeatureEntryPointsTool>());
    Registry.RegisterTool(MakeShared<FSummarizeBlueprintClusterTool>());
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
    Registry.RegisterTool(MakeShared<FListBlueprintVariablesTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintFunctionsTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintComponentsTool>());
    Registry.RegisterTool(MakeShared<FGetParentBlueprintTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintInterfacesTool>());
    Registry.RegisterTool(MakeShared<FListChildBlueprintsTool>());
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
        Transport = MakeUnique<FNamedPipeMCPTransport>(Settings->NamedPipeName);
        if (Transport->Start(GetServer()))
        {
            UE_LOG(LogUnrealMCP, Log, TEXT("Started %s transport."), *Transport->GetTransportName());
            return;
        }

        UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to start named pipe transport '%s'."), *Settings->NamedPipeName);
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

void FUnrealMCPModule::RegisterTestCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();

    RegisteredConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("UnrealMCP.TestHealth"),
        TEXT("Runs the HealthCheck MCP tool and logs the JSON response."),
        FConsoleCommandDelegate::CreateLambda([this]()
        {
            RunTestCommand(TEXT("HealthCheck"));
        }),
        ECVF_Default));

    RegisteredConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("UnrealMCP.TestServerInfo"),
        TEXT("Runs the GetServerInfo MCP tool and logs the JSON response."),
        FConsoleCommandDelegate::CreateLambda([this]()
        {
            RunTestCommand(TEXT("GetServerInfo"));
        }),
        ECVF_Default));

    RegisteredConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("UnrealMCP.TestListTools"),
        TEXT("Runs the ListTools MCP tool and logs the JSON response."),
        FConsoleCommandDelegate::CreateLambda([this]()
        {
            RunTestCommand(TEXT("ListTools"));
        }),
        ECVF_Default));

    RegisteredConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("UnrealMCP.TestSearchAssets"),
        TEXT("Runs the SearchAssets MCP tool and logs the JSON response. Usage: UnrealMCP.TestSearchAssets Door"),
        FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
        {
            HandleSearchAssetsCommand(Args);
        }),
        ECVF_Default));
}

void FUnrealMCPModule::UnregisterTestCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();
    for (IConsoleObject* Command : RegisteredConsoleCommands)
    {
        if (Command != nullptr)
        {
            ConsoleManager.UnregisterConsoleObject(Command, false);
        }
    }

    RegisteredConsoleCommands.Reset();
}

void FUnrealMCPModule::RunTestCommand(const FString& Method, TSharedPtr<FJsonObject> Params) const
{
    if (!Server.IsValid())
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Cannot run MCP test command '%s' because the server is not initialized."), *Method);
        return;
    }

    UnrealMCP::FMCPRequest Request;
    Request.Id = FString::Printf(TEXT("test-%s"), *Method);
    Request.Method = Method;
    Request.Params = Params;

    const UnrealMCP::FMCPResponse Response = GetServer().HandleRequest(Request);
    const FString SerializedResponse = GetServer().SerializeResponse(Response);

    UE_LOG(LogUnrealMCP, Log, TEXT("MCP test response for %s: %s"), *Method, *SerializedResponse);
}

void FUnrealMCPModule::HandleSearchAssetsCommand(const TArray<FString>& Args) const
{
    if (Args.IsEmpty())
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("Usage: UnrealMCP.TestSearchAssets <Query>"));
        return;
    }

    TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("query"), FString::Join(Args, TEXT(" ")));
    RunTestCommand(TEXT("SearchAssets"), Params);
}
