#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

namespace UnrealMCP::BlueprintGraphEditToolUtils
{
    struct FPlacement
    {
        int32 X = 0;
        int32 Y = 0;
        bool bAutoPlaced = false;
        bool bCollisionAdjusted = false;
    };

    bool ResolveGraph(UBlueprint* Blueprint, const FString& GraphName, const FString& GraphGuid, UEdGraph*& OutGraph, FString& OutError);
    bool ResolveNode(UEdGraph* Graph, const FString& NodeGuid, UEdGraphNode*& OutNode, FString& OutError);
    bool ResolvePin(UEdGraphNode* Node, const FString& PinId, const FString& PinName, const FString& Direction, UEdGraphPin*& OutPin, FString& OutError);
    FPlacement ResolvePlacement(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params, FString& OutError);
    FString GetNodeGuid(const UEdGraphNode* Node);
    FString GetPinId(const UEdGraphPin* Pin);
    FString GetPinDefaultObjectPath(const UEdGraphPin* Pin);
    FString GetPinDefaultTextValue(const UEdGraphPin* Pin);
    FString GetEffectivePinDefaultValue(const UEdGraphPin* Pin);
    FString GetPinDefaultValueSource(const UEdGraphPin* Pin);
    TArray<TSharedPtr<FJsonValue>> SerializePins(const UEdGraphNode* Node);
    void PlaceNewNode(UEdGraph* Graph, UEdGraphNode* Node, const FPlacement& Placement);
    bool SaveAndRefreshIfRequested(UBlueprint* Blueprint, const FString& ObjectPath, bool bSave, FString& OutFilename, bool& bOutIndexRefreshed, FString& OutIndexError, FString& OutError);
}
