#include "Tools/InspectLiveBlueprintTool.h"

#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/LiveBlueprintToolUtils.h"
#include "Tools/WorldToolUtils.h"

FInspectLiveBlueprintTool::FInspectLiveBlueprintTool()
    : FMCPToolBase(TEXT("InspectLiveBlueprint"), TEXT("Read-only live graph/node/pin inspection, including a map's embedded Level Blueprint. Defaults to the current editor level. No index, loading, PIE, compilation or saving."))
{
}

UnrealMCP::FMCPResponse FInspectLiveBlueprintTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    FString Error;
    if (!UnrealMCP::WorldToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        return UnrealMCP::LiveBlueprintToolUtils::Inspect(Request.Params, Response.Result, OutError);
    }, Error)) return UnrealMCP::LiveBlueprintToolUtils::ReadError(Request, Error);
    return Response;
}

TSharedPtr<FJsonObject> FInspectLiveBlueprintTool::BuildInputSchema() const
{
    using namespace UnrealMCP;
    auto Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    auto Properties = LiveBlueprintToolUtils::TargetSchema();
    Properties->SetObjectField(TEXT("mode"), BlueprintEditToolUtils::BuildStringProperty(TEXT("graphs (default) or nodes. Node results include exact pins and links.")));
    Properties->SetObjectField(TEXT("nodeGuid"), BlueprintEditToolUtils::BuildStringProperty(TEXT("Optional exact node GUID; implies nodes mode.")));
    Properties->SetObjectField(TEXT("query"), BlueprintEditToolUtils::BuildStringProperty(TEXT("Optional member/title/name substring; implies nodes mode.")));
    for (const TCHAR* FieldName : {TEXT("offset"), TEXT("limit")})
    {
        auto Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), TEXT("integer"));
        Property->SetStringField(TEXT("description"), FString(FieldName) == TEXT("limit") ? TEXT("Page size, default 50, maximum 200.") : TEXT("Zero-based page offset, default 0."));
        Properties->SetObjectField(FieldName, Property);
    }
    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
