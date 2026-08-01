#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UnrealMCPSettings.generated.h"

UCLASS(Config=EditorPerProjectUserSettings, DefaultConfig, meta=(DisplayName="Unreal MCP"))
class UNREALMCP_API UUnrealMCPSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UUnrealMCPSettings();

    virtual FName GetCategoryName() const override;

    UPROPERTY(Config, EditAnywhere, Category="Server")
    bool bEnableServer = true;

    UPROPERTY(Config, EditAnywhere, Category="Server")
    FString ServerName = TEXT("unreal-mcp");

    UPROPERTY(Config, EditAnywhere, Category="Server")
    FString ServerVersion = TEXT("0.1.0");

    UPROPERTY(Config, EditAnywhere, Category="Server")
    FString ProtocolVersion = TEXT("2026-07-31");

    UPROPERTY(Config, EditAnywhere, Category="Transport")
    bool bEnableStdIOTransport = false;

    UPROPERTY(Config, EditAnywhere, Category="Python")
    bool bEnablePythonFallback = false;
};
