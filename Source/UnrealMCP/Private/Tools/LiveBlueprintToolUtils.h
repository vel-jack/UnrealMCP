#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCP/MCPProtocol.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class ULevel;
class UWorld;

namespace UnrealMCP::LiveBlueprintToolUtils
{
    struct FTarget
    {
        UBlueprint* Blueprint = nullptr;
        UWorld* World = nullptr;
        ULevel* Level = nullptr;
        TArray<UEdGraph*> Graphs;
    };

    FString String(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name);
    int32 Bound(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, int32 Default, int32 Min, int32 Max);
    FMCPResponse ReadError(const FMCPRequest& Request, const FString& Message);
    bool Resolve(const TSharedPtr<FJsonObject>& Params, FTarget& Target, FString& Error);
    TSharedRef<FJsonObject> DescribeTarget(const FTarget& Target);
    TSharedRef<FJsonObject> DescribeGraph(UEdGraph* Graph);
    TSharedRef<FJsonObject> DescribeNode(UEdGraphNode* Node, bool bIncludePins);
    TSharedRef<FJsonObject> Endpoint(const UEdGraphPin* Pin);
    FString MemberName(const UEdGraphNode* Node);
    TSharedPtr<FJsonObject> TargetSchema();
    bool Inspect(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& Result, FString& Error);
    bool Trace(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& Result, FString& Error);
}
