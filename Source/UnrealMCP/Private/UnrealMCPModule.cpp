#include "UnrealMCPModule.h"

#include "MCP/MCPServer.h"
#include "Tools/AssetExistsTool.h"
#include "Tools/GetServerInfoTool.h"
#include "Tools/HealthCheckTool.h"
#include "Tools/ListToolsTool.h"
#include "Tools/SearchAssetsTool.h"
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
    RegisterCoreTools();
    RegisterTestCommands();

    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();

    UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP started. Enabled=%s ServerName=%s Version=%s Engine=%s"),
        Settings->bEnableServer ? TEXT("true") : TEXT("false"),
        *Settings->ServerName,
        *Settings->ServerVersion,
        *FEngineVersion::Current().ToString());
}

void FUnrealMCPModule::ShutdownModule()
{
    UnregisterTestCommands();
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

void FUnrealMCPModule::RegisterCoreTools()
{
    FMCPToolRegistry& Registry = GetServer().GetToolRegistry();
    Registry.RegisterTool(MakeShared<FHealthCheckTool>());
    Registry.RegisterTool(MakeShared<FGetServerInfoTool>());
    Registry.RegisterTool(MakeShared<FSearchAssetsTool>());
    Registry.RegisterTool(MakeShared<FAssetExistsTool>());
    Registry.RegisterTool(MakeShared<FListToolsTool>(Registry));
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
