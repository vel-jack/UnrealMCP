#include "Tools/TraceLiveBlueprintFlowTool.h"

#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/LiveBlueprintToolUtils.h"
#include "Tools/WorldToolUtils.h"

FTraceLiveBlueprintFlowTool::FTraceLiveBlueprintFlowTool()
    : FMCPToolBase(TEXT("TraceLiveBlueprintFlow"), TEXT("Trace live Blueprint execution and data links from an event or node, including embedded Level Blueprints, delays, and resident function/macro bodies. Read-only; no asset index or runtime evaluation."))
{
}

UnrealMCP::FMCPResponse FTraceLiveBlueprintFlowTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    FString Error;
    if (!UnrealMCP::WorldToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        return UnrealMCP::LiveBlueprintToolUtils::Trace(Request.Params, Response.Result, OutError);
    }, Error)) return UnrealMCP::LiveBlueprintToolUtils::ReadError(Request, Error);
    return Response;
}

TSharedPtr<FJsonObject> FTraceLiveBlueprintFlowTool::BuildInputSchema() const
{
    using namespace UnrealMCP;
    auto Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    auto Properties = LiveBlueprintToolUtils::TargetSchema();
    Properties->SetObjectField(TEXT("startNodeGuid"), BlueprintEditToolUtils::BuildStringProperty(TEXT("Exact start node GUID. Exclusive with startEvent; use graphPath if ambiguous.")));
    Properties->SetObjectField(TEXT("startEvent"), BlueprintEditToolUtils::BuildStringProperty(TEXT("Exact event member name. Defaults to BeginPlay (ReceiveBeginPlay).")));
    Properties->SetObjectField(TEXT("includePins"), BlueprintEditToolUtils::BuildBoolProperty(TEXT("Include complete authored pin defaults and links for each node; default true.")));
    for (const TCHAR* FieldName : {TEXT("maxNodes"), TEXT("maxDepth"), TEXT("maxCallDepth")})
    {
        auto Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), TEXT("integer"));
        Property->SetStringField(TEXT("description"), TEXT("Traversal bound: maxNodes default 100/max 1000; maxDepth default 128/max 512; maxCallDepth default 4/max 16. Limits return explicit frontier evidence."));
        Properties->SetObjectField(FieldName, Property);
    }
    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
