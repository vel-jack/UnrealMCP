#include "Tools/AddBlueprintTypedOperatorNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    struct FTypedOperator
    {
        FString CanonicalName;
        FName FunctionName;
        bool bSupportsTolerance = false;
    };

    bool ResolveOperator(const FString& Requested, FTypedOperator& OutOperator, FString& OutError)
    {
        FString Normalized = Requested;
        Normalized.TrimStartAndEndInline();
        Normalized.ReplaceInline(TEXT("_"), TEXT(""));
        Normalized.ReplaceInline(TEXT(" "), TEXT(""));
        Normalized = Normalized.ToLower();

        if (Normalized == TEXT("objectequal") || Normalized == TEXT("objectequals"))
            OutOperator = {TEXT("ObjectEqual"), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_ObjectObject), false};
        else if (Normalized == TEXT("objectnotequal"))
            OutOperator = {TEXT("ObjectNotEqual"), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_ObjectObject), false};
        else if (Normalized == TEXT("booleanand") || Normalized == TEXT("booland"))
            OutOperator = {TEXT("BooleanAnd"), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanAND), false};
        else if (Normalized == TEXT("booleanor") || Normalized == TEXT("boolor"))
            OutOperator = {TEXT("BooleanOr"), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanOR), false};
        else if (Normalized == TEXT("booleannot") || Normalized == TEXT("boolnot") || Normalized == TEXT("not"))
            OutOperator = {TEXT("BooleanNot"), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Not_PreBool), false};
        else if (Normalized == TEXT("vectoradd"))
            OutOperator = {TEXT("VectorAdd"), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_VectorVector), false};
        else if (Normalized == TEXT("vectorsubtract") || Normalized == TEXT("vectorsub"))
            OutOperator = {TEXT("VectorSubtract"), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Subtract_VectorVector), false};
        else if (Normalized == TEXT("vectornearlyequal") || Normalized == TEXT("vectorequal"))
            OutOperator = {TEXT("VectorNearlyEqual"), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_VectorVector), true};
        else
        {
            OutError = FString::Printf(
                TEXT("Unsupported typed operator '%s'. Use ObjectEqual, ObjectNotEqual, BooleanAnd, BooleanOr, BooleanNot, VectorAdd, VectorSubtract, or VectorNearlyEqual."),
                *Requested);
            return false;
        }
        return true;
    }
}

FAddBlueprintTypedOperatorNodeTool::FAddBlueprintTypedOperatorNodeTool()
    : FMCPToolBase(
        TEXT("AddBlueprintTypedOperatorNode"),
        TEXT("Adds an exact object, Boolean, or Vector operator node without ambiguous function lookup."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintTypedOperatorNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, GraphName, GraphGuid, RequestedOperator;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("operator"), RequestedOperator))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintTypedOperatorNode requires objectPath, operator, and graphName or graphGuid."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    FTypedOperator Operator;
    FString ValidationError;
    if (!ResolveOperator(RequestedOperator, Operator, ValidationError))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ValidationError);
    }

    double RequestedTolerance = 0.0;
    const bool bHasTolerance = Request.Params->TryGetNumberField(TEXT("tolerance"), RequestedTolerance);
    if (bHasTolerance && (!Operator.bSupportsTolerance || RequestedTolerance < 0.0))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            Operator.bSupportsTolerance
                ? TEXT("tolerance must be zero or greater.")
                : TEXT("tolerance is only valid for VectorNearlyEqual."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;
    FString NodeGuid, NodeClass, SavedFilename, IndexError, ExecutionError, AppliedTolerance;
    bool bIndexRefreshed = false;
    TArray<TSharedPtr<FJsonValue>> Pins;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        UEdGraph* Graph = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError)) return false;
        UFunction* Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(Operator.FunctionName);
        if (Function == nullptr)
        {
            OutError = FString::Printf(TEXT("Could not resolve Kismet operator function '%s'."), *Operator.FunctionName.ToString());
            return false;
        }

        Placement = UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(Graph, Request.Params, OutError);
        if (!OutError.IsEmpty() || bDryRun) return OutError.IsEmpty();

        const FScopedTransaction Transaction(NSLOCTEXT(
            "UnrealMCP", "AddTypedOperatorNode", "UnrealMCP Add Typed Operator Node"));
        Blueprint->Modify();
        Graph->Modify();
        UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
        Node->SetFromFunction(Function);
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, Placement);

        if (bHasTolerance)
        {
            UEdGraphPin* TolerancePin = Node->FindPin(TEXT("ErrorTolerance"), EGPD_Input);
            const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
            if (TolerancePin == nullptr || Schema == nullptr)
            {
                OutError = TEXT("VectorNearlyEqual did not expose its ErrorTolerance input.");
                return false;
            }
            const FString ToleranceText = FString::SanitizeFloat(RequestedTolerance);
            Schema->TrySetDefaultValue(*TolerancePin, ToleranceText);
            AppliedTolerance = TolerancePin->DefaultValue;
        }

        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        NodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
        NodeClass = Node->GetClass()->GetPathName();
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
    Result->SetStringField(TEXT("operator"), Operator.CanonicalName);
    Result->SetStringField(TEXT("functionName"), Operator.FunctionName.ToString());
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("nodeClass"), NodeClass);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("added"), !bDryRun);
    Result->SetNumberField(TEXT("positionX"), Placement.X);
    Result->SetNumberField(TEXT("positionY"), Placement.Y);
    Result->SetBoolField(TEXT("autoPlaced"), Placement.bAutoPlaced);
    Result->SetBoolField(TEXT("collisionAdjusted"), Placement.bCollisionAdjusted);
    Result->SetBoolField(TEXT("hasExplicitTolerance"), bHasTolerance);
    Result->SetStringField(TEXT("appliedTolerance"), AppliedTolerance);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintTypedOperatorNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Target graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Stable target graph GUID.")));
    TSharedRef<FJsonObject> Operator = BuildStringProperty(TEXT("Exact operator to add."));
    Operator->SetArrayField(TEXT("enum"), {
        MakeShared<FJsonValueString>(TEXT("ObjectEqual")), MakeShared<FJsonValueString>(TEXT("ObjectNotEqual")),
        MakeShared<FJsonValueString>(TEXT("BooleanAnd")), MakeShared<FJsonValueString>(TEXT("BooleanOr")),
        MakeShared<FJsonValueString>(TEXT("BooleanNot")), MakeShared<FJsonValueString>(TEXT("VectorAdd")),
        MakeShared<FJsonValueString>(TEXT("VectorSubtract")), MakeShared<FJsonValueString>(TEXT("VectorNearlyEqual"))});
    Properties->SetObjectField(TEXT("operator"), Operator);
    TSharedRef<FJsonObject> Number = MakeShared<FJsonObject>();
    Number->SetStringField(TEXT("type"), TEXT("number"));
    Number->SetStringField(TEXT("description"), TEXT("Optional non-negative VectorNearlyEqual tolerance."));
    Properties->SetObjectField(TEXT("tolerance"), Number);
    Properties->SetObjectField(TEXT("relativeToNodeGuid"), BuildStringProperty(TEXT("Optional placement anchor node.")));
    TSharedRef<FJsonObject> Integer = MakeShared<FJsonObject>();
    Integer->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("positionX"), Integer);
    Properties->SetObjectField(TEXT("positionY"), Integer);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without changing the Blueprint.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh after adding.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {
        MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("operator"))});
    return Schema;
}
