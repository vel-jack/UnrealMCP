#include "Tools/AddBlueprintMapOperationNodeTool.h"

#include "Blueprint/UnrealMCPTypedContainerFunctionNode.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet/BlueprintMapLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    bool ResolveMapOperation(const FString& Requested, FString& OutCanonical, FName& OutFunction, FString& OutError)
    {
        FString Normalized = Requested;
        Normalized.TrimStartAndEndInline();
        Normalized.ReplaceInline(TEXT("_"), TEXT(""));
        Normalized.ReplaceInline(TEXT(" "), TEXT(""));
        Normalized = Normalized.ToLower();

        if (Normalized == TEXT("add")) { OutCanonical = TEXT("Add"); OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintMapLibrary, Map_Add); }
        else if (Normalized == TEXT("find")) { OutCanonical = TEXT("Find"); OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintMapLibrary, Map_Find); }
        else if (Normalized == TEXT("contains")) { OutCanonical = TEXT("Contains"); OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintMapLibrary, Map_Contains); }
        else if (Normalized == TEXT("remove")) { OutCanonical = TEXT("Remove"); OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintMapLibrary, Map_Remove); }
        else if (Normalized == TEXT("clear") || Normalized == TEXT("empty")) { OutCanonical = TEXT("Clear"); OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintMapLibrary, Map_Clear); }
        else if (Normalized == TEXT("keys")) { OutCanonical = TEXT("Keys"); OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintMapLibrary, Map_Keys); }
        else if (Normalized == TEXT("values")) { OutCanonical = TEXT("Values"); OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintMapLibrary, Map_Values); }
        else
        {
            OutError = FString::Printf(
                TEXT("Unsupported map operation '%s'. Use Add, Find, Contains, Remove, Clear, Keys, or Values."), *Requested);
            return false;
        }
        return true;
    }

    int32 CountUnresolvedPins(const UEdGraphNode* Node)
    {
        int32 Count = 0;
        if (Node != nullptr)
        {
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin != nullptr
                    && (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
                        || (Pin->PinType.ContainerType == EPinContainerType::Map
                            && Pin->PinType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Wildcard)))
                {
                    ++Count;
                }
            }
        }
        return Count;
    }
}

FAddBlueprintMapOperationNodeTool::FAddBlueprintMapOperationNodeTool()
    : FMCPToolBase(
        TEXT("AddBlueprintMapOperationNode"),
        TEXT("Adds a persistent typed Blueprint Map operation node with independently resolved key and value pins."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintMapOperationNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, GraphName, GraphGuid, RequestedOperation;
    FString KeyTypeName, KeyTypeObjectPath, ValueTypeName, ValueTypeObjectPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("operation"), RequestedOperation)
        || !Request.Params->TryGetStringField(TEXT("keyType"), KeyTypeName)
        || !Request.Params->TryGetStringField(TEXT("valueType"), ValueTypeName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintMapOperationNode requires objectPath, operation, keyType, valueType, and graphName or graphGuid."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("keyTypeObjectPath"), KeyTypeObjectPath);
    Request.Params->TryGetStringField(TEXT("valueTypeObjectPath"), ValueTypeObjectPath);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    FString CanonicalOperation, ValidationError;
    FName FunctionName;
    if (!ResolveMapOperation(RequestedOperation, CanonicalOperation, FunctionName, ValidationError))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ValidationError);
    }

    FEdGraphPinType KeyType, ValueType;
    if (!UnrealMCP::BlueprintEditToolUtils::BuildPinType(
            KeyTypeName, KeyTypeObjectPath, false, KeyType, ValidationError)
        || !UnrealMCP::BlueprintEditToolUtils::BuildPinType(
            ValueTypeName, ValueTypeObjectPath, false, ValueType, ValidationError))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ValidationError);
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;
    FString NodeGuid, NodeClass, SavedFilename, IndexError, ExecutionError;
    bool bIndexRefreshed = false;
    int32 UnresolvedPinCount = 0;
    TArray<TSharedPtr<FJsonValue>> Pins;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        UEdGraph* Graph = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError)) return false;

        UFunction* Function = UBlueprintMapLibrary::StaticClass()->FindFunctionByName(FunctionName);
        if (Function == nullptr)
        {
            OutError = FString::Printf(TEXT("Could not resolve Blueprint Map function '%s'."), *FunctionName.ToString());
            return false;
        }

        Placement = UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(Graph, Request.Params, OutError);
        if (!OutError.IsEmpty() || bDryRun) return OutError.IsEmpty();

        const FScopedTransaction Transaction(NSLOCTEXT(
            "UnrealMCP", "AddMapOperationNode", "UnrealMCP Add Map Operation Node"));
        Blueprint->Modify();
        Graph->Modify();
        UUnrealMCPTypedContainerFunctionNode* Node =
            NewObject<UUnrealMCPTypedContainerFunctionNode>(Graph);
        Node->SetFromFunction(Function);
        Node->ConfigureMap(KeyType, ValueType);
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, Placement);
        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

        NodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
        NodeClass = Node->GetClass()->GetPathName();
        UnresolvedPinCount = CountUnresolvedPins(Node);
        Pins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Node);
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
    Result->SetStringField(TEXT("operation"), CanonicalOperation);
    Result->SetStringField(TEXT("keyType"), KeyTypeName);
    Result->SetStringField(TEXT("keyTypeObjectPath"), KeyTypeObjectPath);
    Result->SetStringField(TEXT("valueType"), ValueTypeName);
    Result->SetStringField(TEXT("valueTypeObjectPath"), ValueTypeObjectPath);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("nodeClass"), NodeClass);
    Result->SetNumberField(TEXT("remainingUnresolvedPinCount"), UnresolvedPinCount);
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

TSharedPtr<FJsonObject> FAddBlueprintMapOperationNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Target graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Stable target graph GUID.")));
    TSharedRef<FJsonObject> Operation = BuildStringProperty(TEXT("Map operation to add."));
    Operation->SetArrayField(TEXT("enum"), {
        MakeShared<FJsonValueString>(TEXT("Add")), MakeShared<FJsonValueString>(TEXT("Find")),
        MakeShared<FJsonValueString>(TEXT("Contains")), MakeShared<FJsonValueString>(TEXT("Remove")),
        MakeShared<FJsonValueString>(TEXT("Clear")), MakeShared<FJsonValueString>(TEXT("Keys")),
        MakeShared<FJsonValueString>(TEXT("Values"))});
    Properties->SetObjectField(TEXT("operation"), Operation);
    Properties->SetObjectField(TEXT("keyType"), BuildStringProperty(TEXT("Map key type.")));
    Properties->SetObjectField(TEXT("keyTypeObjectPath"), BuildStringProperty(TEXT("Class path for object/class key types.")));
    Properties->SetObjectField(TEXT("valueType"), BuildStringProperty(TEXT("Map value type.")));
    Properties->SetObjectField(TEXT("valueTypeObjectPath"), BuildStringProperty(TEXT("Class path for object/class value types.")));
    Properties->SetObjectField(TEXT("relativeToNodeGuid"), BuildStringProperty(TEXT("Optional placement anchor node.")));
    TSharedRef<FJsonObject> Integer = MakeShared<FJsonObject>();
    Integer->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("positionX"), Integer);
    Properties->SetObjectField(TEXT("positionY"), Integer);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without changing the Blueprint.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh after adding.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {
        MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("operation")),
        MakeShared<FJsonValueString>(TEXT("keyType")), MakeShared<FJsonValueString>(TEXT("valueType"))});
    return Schema;
}
