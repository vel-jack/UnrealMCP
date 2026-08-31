#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UFunction;
class UK2Node_CallFunction;
class UK2Node_IfThenElse;
class UK2Node_Knot;
class UK2Node_VariableGet;
class UK2Node_VariableSet;

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
    bool ResolveTypedOperatorFunction(
        const FString& RequestedOperator,
        FString& OutCanonicalName,
        UFunction*& OutFunction,
        bool& bOutSupportsTolerance,
        FString& OutError);
    FString ComputeGraphRevision(const UEdGraph* Graph);
    UK2Node_CallFunction* CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, bool bDetached);
    UK2Node_IfThenElse* CreateBranchNode(UEdGraph* Graph, bool bDetached);
    UK2Node_VariableGet* CreateVariableGetNode(UEdGraph* Graph, FName VariableName, const FGuid& VariableGuid, bool bDetached);
    UK2Node_VariableSet* CreateVariableSetNode(UEdGraph* Graph, FName VariableName, const FGuid& VariableGuid, bool bDetached);
    UK2Node_Knot* CreateRerouteNode(UEdGraph* Graph, bool bDetached);
    void PlaceNewNode(UEdGraph* Graph, UEdGraphNode* Node, const FPlacement& Placement);
    bool SaveAndRefreshIfRequested(UBlueprint* Blueprint, const FString& ObjectPath, bool bSave, FString& OutFilename, bool& bOutIndexRefreshed, FString& OutIndexError, FString& OutError);

    // Shared 8-step sequence used by the AddBlueprint*NodeTool family: resolve blueprint/graph,
    // resolve placement, create a detached preview node on dry-run (or the real node inside one
    // transaction otherwise), place it, mark the Blueprint modified, and save/refresh if requested.
    // CreateNode receives the resolved Blueprint and Graph so factories needing extra Blueprint-level
    // resolution (e.g. a member variable GUID lookup) can do it without a separate pass.
    struct FNodeAdditionResult
    {
        bool bSucceeded = false;
        FString ErrorMessage;
        FString NodeGuid;
        FPlacement Placement;
        TArray<TSharedPtr<FJsonValue>> Pins;
        bool bAdded = false;
        bool bSaved = false;
        FString SavedFilename;
        bool bIndexRefreshed = false;
        FString IndexRefreshError;
    };

    using FGraphNodeFactory = TFunctionRef<UEdGraphNode* (UBlueprint* Blueprint, UEdGraph* Graph, bool bDetached, FString& OutError)>;

    FNodeAdditionResult AddSimpleGraphNode(
        const FString& ObjectPath,
        const FString& GraphName,
        const FString& GraphGuid,
        const TSharedPtr<FJsonObject>& Params,
        bool bDryRun,
        bool bSave,
        const FText& TransactionDescription,
        FGraphNodeFactory CreateNode);
}
