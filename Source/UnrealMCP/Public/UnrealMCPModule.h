#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FMCPServer;
class IConsoleObject;

class FUnrealMCPModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    static FUnrealMCPModule& Get();
    static bool IsAvailable();

    FMCPServer& GetServer();
    const FMCPServer& GetServer() const;

private:
    void RegisterCoreTools();
    void RegisterTestCommands();
    void UnregisterTestCommands();
    void RunTestCommand(const FString& Method, TSharedPtr<class FJsonObject> Params = nullptr) const;
    void HandleSearchAssetsCommand(const TArray<FString>& Args) const;

    TUniquePtr<FMCPServer> Server;
    TArray<IConsoleObject*> RegisteredConsoleCommands;
};
