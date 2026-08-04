#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FMCPServer;
class IConsoleObject;
class IMCPTransport;
class FUnrealMCPProjectIndex;

class FUnrealMCPModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    static FUnrealMCPModule& Get();
    static bool IsAvailable();

    FMCPServer& GetServer();
    const FMCPServer& GetServer() const;
    FUnrealMCPProjectIndex& GetProjectIndex();
    const FUnrealMCPProjectIndex& GetProjectIndex() const;
    FString GetEffectiveNamedPipeName() const;

private:
    bool EnsureProjectIndexInitialized() const;
    static FString SanitizeNamedPipeToken(const FString& Value);
    void RegisterCoreTools();
    void RegisterEditorControls();
    void UnregisterEditorControls();
    void StartConfiguredTransport();
    void StopTransport();
    void RegisterTestCommands();
    void UnregisterTestCommands();
    void RunTestCommand(const FString& Method, TSharedPtr<class FJsonObject> Params = nullptr) const;
    void HandleSearchAssetsCommand(const TArray<FString>& Args) const;
    void HandleInitializeProjectIndex() const;
    void HandleRefreshProjectIndex() const;
    void HandleRebuildProjectIndex() const;
    void HandleLogIndexStatus() const;
    void HandleOpenIndexFolder() const;
    void HandleOpenHelp() const;
    void HandleOpenPromptExamples() const;
    void HandleStartTransport();
    void HandleStopTransportCommand();

    TUniquePtr<FMCPServer> Server;
    TUniquePtr<FUnrealMCPProjectIndex> ProjectIndex;
    TUniquePtr<IMCPTransport> Transport;
    TArray<IConsoleObject*> RegisteredConsoleCommands;
    bool bEditorControlsRegistered = false;
};
