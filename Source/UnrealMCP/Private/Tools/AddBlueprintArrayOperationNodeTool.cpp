#include "Tools/AddBlueprintArrayOperationNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_GetArrayItem.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    struct FArrayOperation
    {
        FString CanonicalName;
        FName FunctionName;
        bool bUsesGetNode = false;
    };

    bool ResolveArrayOperation(const FString& RequestedOperation, FArrayOperation& OutOperation, FString& OutError)
    {
        FString Normalized = RequestedOperation;
        Normalized.TrimStartAndEndInline();
        Normalized.ReplaceInline(TEXT("_"), TEXT(""));
        Normalized.ReplaceInline(TEXT(" "), TEXT(""));
        Normalized = Normalized.ToLower();

        if (Normalized == TEXT("contains") || Normalized == TEXT("containsitem"))
        {
            OutOperation = {TEXT("Contains"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Contains), false};
        }
        else if (Normalized == TEXT("add"))
        {
            OutOperation = {TEXT("Add"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Add), false};
        }
        else if (Normalized == TEXT("addunique"))
        {
            OutOperation = {TEXT("AddUnique"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_AddUnique), false};
        }
        else if (Normalized == TEXT("removeitem") || Normalized == TEXT("remove"))
        {
            OutOperation = {TEXT("RemoveItem"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_RemoveItem), false};
        }
        else if (Normalized == TEXT("clear") || Normalized == TEXT("empty"))
        {
            OutOperation = {TEXT("Clear"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Clear), false};
        }
        else if (Normalized == TEXT("length") || Normalized == TEXT("num"))
        {
            OutOperation = {TEXT("Length"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Length), false};
        }
        else if (Normalized == TEXT("get") || Normalized == TEXT("getitem"))
        {
            OutOperation = {TEXT("Get"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Get), true};
        }
        else
        {
            OutError = FString::Printf(
                TEXT("Unsupported array operation '%s'. Use Contains, Add, AddUnique, RemoveItem, Clear, Length, or Get."),
                *RequestedOperation);
            return false;
        }
        return true;
    }

    void ApplyElementType(UK2Node_CallArrayFunction* Node, const FEdGraphPinType& ElementType)
    {
        if (Node == nullptr)
        {
            return;
        }

        // Array custom thunks expose the target array and dependent item pins as wildcards.
        // Preserve each pin's container/reference flags while applying the chosen element type.
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
            {
                Pin->PinType.PinCategory = ElementType.PinCategory;
                Pin->PinType.PinSubCategory = ElementType.PinSubCategory;
                Pin->PinType.PinSubCategoryObject = ElementType.PinSubCategoryObject;
            }
        }
    }

    void ApplyElementType(UK2Node_GetArrayItem* Node, const FEdGraphPinType& ElementType)
    {
        if (Node == nullptr)
        {
            return;
        }

        UEdGraphPin* TargetArrayPin = Node->GetTargetArrayPin();
        UEdGraphPin* ResultPin = Node->GetResultPin();
        TargetArrayPin->PinType = ElementType;
        TargetArrayPin->PinType.ContainerType = EPinContainerType::Array;
        TargetArrayPin->PinType.bIsReference = true;
        ResultPin->PinType = ElementType;
        ResultPin->PinType.ContainerType = EPinContainerType::None;
    }

    int32 CountWildcardPins(const UEdGraphNode* Node)
    {
        int32 Count = 0;
        if (Node != nullptr)
        {
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
                {
                    ++Count;
                }
            }
        }
        return Count;
    }
}

FAddBlueprintArrayOperationNodeTool::FAddBlueprintArrayOperationNodeTool()
    : FMCPToolBase(
        TEXT("AddBlueprintArrayOperationNode"),
        TEXT("Adds a typed Blueprint array operation node and resolves its wildcard array and item pins."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintArrayOperationNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, GraphName, GraphGuid, RequestedOperation, ElementTypeName, TypeObjectPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("operation"), RequestedOperation)
        || !Request.Params->TryGetStringField(TEXT("elementType"), ElementTypeName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintArrayOperationNode requires objectPath, operation, elementType, and graphName or graphGuid."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("typeObjectPath"), TypeObjectPath);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    FArrayOperation Operation;
    FString ValidationError;
    if (!ResolveArrayOperation(RequestedOperation, Operation, ValidationError))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ValidationError);
    }

    FEdGraphPinType ElementPinType;
    if (!UnrealMCP::BlueprintEditToolUtils::BuildPinType(
            ElementTypeName, TypeObjectPath, false, ElementPinType, ValidationError))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ValidationError);
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;
    FString NodeGuid, NodeClass, SavedFilename, IndexError, ExecutionError;
    bool bIndexRefreshed = false;
    int32 WildcardPinCount = 0;
    TArray<TSharedPtr<FJsonValue>> Pins;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;

        UEdGraph* TargetGraph = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, TargetGraph, OutError)) return false;

        Placement = UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(TargetGraph, Request.Params, OutError);
        if (!OutError.IsEmpty() || bDryRun) return OutError.IsEmpty();

        const FScopedTransaction Transaction(NSLOCTEXT(
            "UnrealMCP", "AddArrayOperationNode", "UnrealMCP Add Array Operation Node"));
        Blueprint->Modify();
        TargetGraph->Modify();

        UEdGraphNode* AddedNode = nullptr;
        if (Operation.bUsesGetNode)
        {
            UK2Node_GetArrayItem* GetNode = NewObject<UK2Node_GetArrayItem>(TargetGraph);
            UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(TargetGraph, GetNode, Placement);
            ApplyElementType(GetNode, ElementPinType);
            AddedNode = GetNode;
        }
        else
        {
            UFunction* Function = UKismetArrayLibrary::StaticClass()->FindFunctionByName(Operation.FunctionName);
            if (Function == nullptr)
            {
                OutError = FString::Printf(TEXT("Could not resolve Kismet array function '%s'."), *Operation.FunctionName.ToString());
                return false;
            }

            UK2Node_CallArrayFunction* ArrayNode = NewObject<UK2Node_CallArrayFunction>(TargetGraph);
            ArrayNode->SetFromFunction(Function);
            UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(TargetGraph, ArrayNode, Placement);
            ApplyElementType(ArrayNode, ElementPinType);
            AddedNode = ArrayNode;
        }

        AddedNode->NodeConnectionListChanged();
        TargetGraph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        NodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(AddedNode);
        NodeClass = AddedNode->GetClass()->GetPathName();
        WildcardPinCount = CountWildcardPins(AddedNode);
        Pins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(AddedNode);
        return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
            Blueprint, ObjectPath, bSave, SavedFilename, bIndexRefreshed, IndexError, OutError);
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("operation"), Operation.CanonicalName);
    Result->SetStringField(TEXT("elementType"), ElementTypeName);
    Result->SetStringField(TEXT("typeObjectPath"), TypeObjectPath);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("nodeClass"), NodeClass);
    Result->SetNumberField(TEXT("remainingWildcardPinCount"), WildcardPinCount);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("added"), !bDryRun);
    Result->SetNumberField(TEXT("positionX"), Placement.X);
    Result->SetNumberField(TEXT("positionY"), Placement.Y);
    Result->SetBoolField(TEXT("autoPlaced"), Placement.bAutoPlaced);
    Result->SetBoolField(TEXT("collisionAdjusted"), Placement.bCollisionAdjusted);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintArrayOperationNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Target graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Stable target graph GUID.")));

    TSharedRef<FJsonObject> Operation = BuildStringProperty(TEXT("Array operation to add."));
    Operation->SetArrayField(TEXT("enum"), {
        MakeShared<FJsonValueString>(TEXT("Contains")),
        MakeShared<FJsonValueString>(TEXT("Add")),
        MakeShared<FJsonValueString>(TEXT("AddUnique")),
        MakeShared<FJsonValueString>(TEXT("RemoveItem")),
        MakeShared<FJsonValueString>(TEXT("Clear")),
        MakeShared<FJsonValueString>(TEXT("Length")),
        MakeShared<FJsonValueString>(TEXT("Get"))});
    Properties->SetObjectField(TEXT("operation"), Operation);

    TSharedRef<FJsonObject> ElementType = BuildStringProperty(TEXT("Array element type."));
    ElementType->SetArrayField(TEXT("enum"), {
        MakeShared<FJsonValueString>(TEXT("bool")),
        MakeShared<FJsonValueString>(TEXT("float")),
        MakeShared<FJsonValueString>(TEXT("int")),
        MakeShared<FJsonValueString>(TEXT("string")),
        MakeShared<FJsonValueString>(TEXT("name")),
        MakeShared<FJsonValueString>(TEXT("text")),
        MakeShared<FJsonValueString>(TEXT("object")),
        MakeShared<FJsonValueString>(TEXT("class"))});
    Properties->SetObjectField(TEXT("elementType"), ElementType);
    Properties->SetObjectField(TEXT("typeObjectPath"), BuildStringProperty(
        TEXT("Required class path when elementType is object or class.")));
    Properties->SetObjectField(TEXT("relativeToNodeGuid"), BuildStringProperty(TEXT("Optional placement anchor node.")));

    TSharedRef<FJsonObject> Integer = MakeShared<FJsonObject>();
    Integer->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("positionX"), Integer);
    Properties->SetObjectField(TEXT("positionY"), Integer);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without changing the Blueprint.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh the index after adding.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("operation")),
        MakeShared<FJsonValueString>(TEXT("elementType"))});
    return Schema;
}
