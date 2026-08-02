#include "Tools/GetServerInfoTool.h"

#include "Dom/JsonObject.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "UnrealMCPModule.h"
#include "UnrealMCPSettings.h"

FGetServerInfoTool::FGetServerInfoTool()
    : FMCPToolBase(TEXT("GetServerInfo"), TEXT("Returns plugin, protocol, and engine version information."))
{
}

UnrealMCP::FMCPResponse FGetServerInfoTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("serverName"), Settings->ServerName);
    Result->SetStringField(TEXT("serverVersion"), Settings->ServerVersion);
    Result->SetStringField(TEXT("protocolVersion"), Settings->ProtocolVersion);
    Result->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
    Result->SetStringField(TEXT("projectName"), FApp::GetProjectName());
    Result->SetStringField(TEXT("namedPipeName"), FUnrealMCPModule::Get().GetEffectiveNamedPipeName());
    Result->SetBoolField(TEXT("pythonFallbackEnabled"), Settings->bEnablePythonFallback);

    Response.Result = Result;
    return Response;
}
