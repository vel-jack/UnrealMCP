#include "Tools/HealthCheckTool.h"

#include "Dom/JsonObject.h"
#include "Misc/App.h"
#include "UnrealMCPSettings.h"

FHealthCheckTool::FHealthCheckTool()
    : FMCPToolBase(TEXT("HealthCheck"), TEXT("Returns plugin health and readiness information."))
{
}

UnrealMCP::FMCPResponse FHealthCheckTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(Settings->bEnableServer);
    Result->SetStringField(TEXT("status"), Settings->bEnableServer ? TEXT("ready") : TEXT("disabled"));
    Result->SetStringField(TEXT("projectName"), FApp::GetProjectName());
    Result->SetBoolField(TEXT("serverEnabled"), Settings->bEnableServer);
    Result->SetBoolField(TEXT("pythonFallbackEnabled"), Settings->bEnablePythonFallback);

    Response.Result = Result;
    return Response;
}
