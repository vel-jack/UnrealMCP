#include "Tools/AddBlueprintSetOperationNodeTool.h"

#include "Blueprint/UnrealMCPTypedContainerFunctionNode.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet/BlueprintSetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    bool ResolveSetOperation(const FString& Requested, FString& OutCanonical, FName& OutFunction, FString& OutError)
    {
        FString Normalized = Requested;
        Normalized.TrimStartAndEndInline();
        Normalized.ReplaceInline(TEXT("_"), TEXT(""));
        Normalized.ReplaceInline(TEXT(" "), TEXT(""));
        Normalized = Normalized.ToLower();

        if (Normalized == TEXT("contains") || Normalized == TEXT("containsitem"))
        {
            OutCanonical = TEXT("Contains");
            OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintSetLibrary, Set_Contains);
        }
        else if (Normalized == TEXT("add"))
        {
            OutCanonical = TEXT("Add");
            OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintSetLibrary, Set_Add);
        }
        else if (Normalized == TEXT("remove") || Normalized == TEXT("removeitem"))
        {
            OutCanonical = TEXT("Remove");
            OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintSetLibrary, Set_Remove);
        }
        else if (Normalized == TEXT("clear") || Normalized == TEXT("empty"))
        {
            OutCanonical = TEXT("Clear");
            OutFunction = GET_FUNCTION_NAME_CHECKED(UBlueprintSetLibrary, Set_Clear);
        }
        else
        {
            OutError = FString::Printf(
                TEXT("Unsupported set operation '%s'. Use Contains, Add, Remove, or Clear."), *Requested);
            return false;
        }
        return true;
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

FAddBlueprintSetOperationNodeTool::FAddBlueprintSetOperationNodeTool()
    : FMCPToolBase(
        TEXT("AddBlueprintSetOperationNode"),
        TEXT("Adds a persistent typed Blueprint Set operation node with resolved element pins."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintSetOperationNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, GraphName, GraphGuid, RequestedOperation, ElementTypeName, TypeObjectPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("operation"), RequestedOperation)
        || !Request.Params->TryGetStringField(TEXT("elementType"), ElementTypeName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintSetOperationNode requires objectPath, operation, elementType, and graphName or graphGuid."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("typeObjectPath"), TypeObjectPath);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    FString CanonicalOperation, ValidationError;
    FName FunctionName;
    if (!ResolveSetOperation(RequestedOperation, CanonicalOperation, FunctionName, ValidationError))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ValidationError);
    }

    FEdGraphPinType ElementType;
    if (!UnrealMCP::BlueprintEditToolUtils::BuildPinType(
            ElementTypeName, TypeObjectPath, false, ElementType, ValidationError))
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
        UEdGraph* Graph = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError)) return false;

        UFunction* Function = UBlueprintSetLibrary::StaticClass()->FindFunctionByName(FunctionName);
        if (Function == nullptr)
        {
            OutError = FString::Printf(TEXT("Could not resolve Blueprint Set function '%s'."), *FunctionName.ToString());
            return false;
        }

        Placement = UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(Graph, Request.Params, OutError);
        if (!OutError.IsEmpty() || bDryRun) return OutError.IsEmpty();

        const FScopedTransaction Transaction(NSLOCTEXT(
            "UnrealMCP", "AddSetOperationNode", "UnrealMCP Add Set Operation Node"));
        Blueprint->Modify();
        Graph->Modify();
        UUnrealMCPTypedContainerFunctionNode* Node =
            NewObject<UUnrealMCPTypedContainerFunctionNode>(Graph);
        Node->SetFromFunction(Function);
        Node->ConfigureSet(ElementType);
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, Placement);
        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

        NodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
        NodeClass = Node->GetClass()->GetPathName();
        WildcardPinCount = CountWildcardPins(Node);
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

TSharedPtr<FJsonObject> FAddBlueprintSetOperationNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Target graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Stable target graph GUID.")));
    TSharedRef<FJsonObject> Operation = BuildStringProperty(TEXT("Set operation to add."));
    Operation->SetArrayField(TEXT("enum"), {
        MakeShared<FJsonValueString>(TEXT("Contains")), MakeShared<FJsonValueString>(TEXT("Add")),
        MakeShared<FJsonValueString>(TEXT("Remove")), MakeShared<FJsonValueString>(TEXT("Clear"))});
    Properties->SetObjectField(TEXT("operation"), Operation);
    Properties->SetObjectField(TEXT("elementType"), BuildStringProperty(
        TEXT("Set element type: bool, float, int, string, name, text, object, or class.")));
    Properties->SetObjectField(TEXT("typeObjectPath"), BuildStringProperty(
        TEXT("Required class path when elementType is object or class.")));
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
        MakeShared<FJsonValueString>(TEXT("elementType"))});
    return Schema;
}
