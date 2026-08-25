#include "Tools/ConnectBlueprintPinsTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    TSharedRef<FJsonObject> SerializePinType(const UEdGraphPin* Pin)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        if (Pin == nullptr) return Result;
        Result->SetStringField(TEXT("pinId"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Pin));
        Result->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
        Result->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
        Result->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
        Result->SetStringField(TEXT("subCategoryObjectPath"),
            Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : FString());
        Result->SetStringField(TEXT("containerType"),
            StaticEnum<EPinContainerType>()->GetNameStringByValue(static_cast<int64>(Pin->PinType.ContainerType)));
        Result->SetStringField(TEXT("valueCategory"), Pin->PinType.PinValueType.TerminalCategory.ToString());
        Result->SetStringField(TEXT("valueSubCategory"), Pin->PinType.PinValueType.TerminalSubCategory.ToString());
        Result->SetStringField(TEXT("valueSubCategoryObjectPath"),
            Pin->PinType.PinValueType.TerminalSubCategoryObject.IsValid()
                ? Pin->PinType.PinValueType.TerminalSubCategoryObject->GetPathName()
                : FString());
        Result->SetNumberField(TEXT("linkedPinCount"), Pin->LinkedTo.Num());
        return Result;
    }

    bool IsUnresolvedWildcard(const UEdGraphPin* Pin)
    {
        return Pin != nullptr
            && (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
                || (Pin->PinType.ContainerType == EPinContainerType::Map
                    && Pin->PinType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Wildcard));
    }
}

FConnectBlueprintPinsTool::FConnectBlueprintPinsTool()
    : FMCPToolBase(
        TEXT("ConnectBlueprintPins"),
        TEXT("Validates and transactionally connects Blueprint pins, returning their resolved post-connection type model."))
{
}

UnrealMCP::FMCPResponse FConnectBlueprintPinsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, GraphName, GraphGuid;
    FString SourceNodeGuid, SourcePinId, SourcePinName;
    FString TargetNodeGuid, TargetPinId, TargetPinName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("sourceNodeGuid"), SourceNodeGuid)
        || !Request.Params->TryGetStringField(TEXT("targetNodeGuid"), TargetNodeGuid))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("ConnectBlueprintPins requires objectPath, sourceNodeGuid, targetNodeGuid, graphName/graphGuid, and pin ids or names."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("sourcePinId"), SourcePinId);
    Request.Params->TryGetStringField(TEXT("sourcePinName"), SourcePinName);
    Request.Params->TryGetStringField(TEXT("targetPinId"), TargetPinId);
    Request.Params->TryGetStringField(TEXT("targetPinName"), TargetPinName);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("ConnectBlueprintPins requires graphName or graphGuid."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool bAlreadyConnected = false;
    bool bIndexRefreshed = false;
    bool bWildcardResolved = false;
    FString SavedFilename, IndexError, ExecutionError, CompatibilityMessage;
    TSharedPtr<FJsonObject> SourceBefore, TargetBefore, SourceAfter, TargetAfter;
    TArray<TSharedPtr<FJsonValue>> SourceNodePinsAfter, TargetNodePinsAfter;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        UEdGraph* Graph = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError)) return false;
        UEdGraphNode* SourceNode = nullptr;
        UEdGraphNode* TargetNode = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Graph, SourceNodeGuid, SourceNode, OutError)
            || !UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Graph, TargetNodeGuid, TargetNode, OutError))
        {
            return false;
        }
        UEdGraphPin* SourcePin = nullptr;
        UEdGraphPin* TargetPin = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolvePin(
                SourceNode, SourcePinId, SourcePinName, TEXT("output"), SourcePin, OutError)
            || !UnrealMCP::BlueprintGraphEditToolUtils::ResolvePin(
                TargetNode, TargetPinId, TargetPinName, TEXT("input"), TargetPin, OutError))
        {
            return false;
        }

        SourceBefore = SerializePinType(SourcePin);
        TargetBefore = SerializePinType(TargetPin);
        const bool bHadWildcard = IsUnresolvedWildcard(SourcePin) || IsUnresolvedWildcard(TargetPin);
        bAlreadyConnected = SourcePin->LinkedTo.Contains(TargetPin);
        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (Schema == nullptr)
        {
            OutError = TEXT("Graph does not use the K2 schema.");
            return false;
        }
        const FPinConnectionResponse Compatibility = Schema->CanCreateConnection(SourcePin, TargetPin);
        CompatibilityMessage = Compatibility.Message.ToString();
        if (Compatibility.Response == CONNECT_RESPONSE_DISALLOW)
        {
            OutError = FString::Printf(TEXT("Pins are incompatible: %s"), *CompatibilityMessage);
            return false;
        }

        if (!bDryRun && !bAlreadyConnected)
        {
            const FScopedTransaction Transaction(NSLOCTEXT(
                "UnrealMCP", "ConnectBlueprintPins", "UnrealMCP Connect Blueprint Pins"));
            Blueprint->Modify();
            Graph->Modify();
            SourceNode->Modify();
            TargetNode->Modify();
            if (!Schema->TryCreateConnection(SourcePin, TargetPin))
            {
                OutError = TEXT("Unreal rejected the pin connection.");
                return false;
            }
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        }

        SourceAfter = SerializePinType(SourcePin);
        TargetAfter = SerializePinType(TargetPin);
        SourceNodePinsAfter = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(SourceNode);
        TargetNodePinsAfter = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(TargetNode);
        bWildcardResolved = bHadWildcard
            && !IsUnresolvedWildcard(SourcePin)
            && !IsUnresolvedWildcard(TargetPin);

        if (bDryRun || bAlreadyConnected) return true;
        return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
            Blueprint, ObjectPath, bSave, SavedFilename, bIndexRefreshed, IndexError, OutError);
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("compatible"), true);
    Result->SetBoolField(TEXT("alreadyConnected"), bAlreadyConnected);
    Result->SetBoolField(TEXT("connected"), !bDryRun && !bAlreadyConnected);
    Result->SetBoolField(TEXT("wildcardResolved"), bWildcardResolved);
    Result->SetStringField(TEXT("compatibilityMessage"), CompatibilityMessage);
    Result->SetStringField(TEXT("sourceNodeGuid"), SourceNodeGuid);
    Result->SetStringField(TEXT("sourcePinId"), SourcePinId);
    Result->SetStringField(TEXT("targetNodeGuid"), TargetNodeGuid);
    Result->SetStringField(TEXT("targetPinId"), TargetPinId);
    Result->SetObjectField(TEXT("sourcePinBefore"), SourceBefore.ToSharedRef());
    Result->SetObjectField(TEXT("targetPinBefore"), TargetBefore.ToSharedRef());
    Result->SetObjectField(TEXT("sourcePinAfter"), SourceAfter.ToSharedRef());
    Result->SetObjectField(TEXT("targetPinAfter"), TargetAfter.ToSharedRef());
    Result->SetArrayField(TEXT("sourceNodePinsAfter"), SourceNodePinsAfter);
    Result->SetArrayField(TEXT("targetNodePinsAfter"), TargetNodePinsAfter);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bAlreadyConnected);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FConnectBlueprintPinsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"),
        TEXT("sourceNodeGuid"), TEXT("sourcePinId"), TEXT("sourcePinName"),
        TEXT("targetNodeGuid"), TEXT("targetPinId"), TEXT("targetPinName")})
    {
        Properties->SetObjectField(FieldName, BuildStringProperty(TEXT("Stable Blueprint graph/node/pin selector.")));
    }
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without connection.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh after connection.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("sourceNodeGuid")),
        MakeShared<FJsonValueString>(TEXT("targetNodeGuid"))});
    return Schema;
}
