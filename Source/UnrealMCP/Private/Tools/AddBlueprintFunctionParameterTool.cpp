#include "Tools/AddBlueprintFunctionParameterTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    bool HasUserParameterNamed(const UEdGraph* Graph, const FString& ParameterName)
    {
        if (Graph == nullptr)
        {
            return false;
        }

        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_EditablePinBase* EditableNode = Cast<UK2Node_EditablePinBase>(Node);
            if (EditableNode == nullptr)
            {
                continue;
            }

            for (const TSharedPtr<FUserPinInfo>& UserPin : EditableNode->UserDefinedPins)
            {
                if (UserPin.IsValid() && UserPin->PinName.ToString().Equals(ParameterName, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
        }

        return false;
    }

    UK2Node_FunctionEntry* FindFunctionEntry(UEdGraph* Graph)
    {
        if (Graph == nullptr)
        {
            return nullptr;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                return Entry;
            }
        }

        return nullptr;
    }
}

FAddBlueprintFunctionParameterTool::FAddBlueprintFunctionParameterTool()
    : FMCPToolBase(
        TEXT("AddBlueprintFunctionParameter"),
        TEXT("Transactionally adds one typed input or output parameter to a Blueprint function graph identified by graphGuid."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintFunctionParameterTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString GraphGuid;
    FString ParameterName;
    FString Direction;
    FString TypeName;
    FString TypeObjectPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid)
        || !Request.Params->TryGetStringField(TEXT("parameterName"), ParameterName)
        || !Request.Params->TryGetStringField(TEXT("direction"), Direction)
        || !Request.Params->TryGetStringField(TEXT("type"), TypeName)
        || ObjectPath.IsEmpty()
        || GraphGuid.IsEmpty()
        || ParameterName.IsEmpty())
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintFunctionParameter requires objectPath, graphGuid, parameterName, direction, and type."));
    }

    const bool bIsInput = Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase);
    const bool bIsOutput = Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase);
    if (!bIsInput && !bIsOutput)
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("direction must be 'input' or 'output'."));
    }

    Request.Params->TryGetStringField(TEXT("typeObjectPath"), TypeObjectPath);
    const bool bIsArray = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("isArray"), false);
    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    FString FunctionName;
    FString EntryNodeGuid;
    FString ResultNodeGuid;
    FString TargetNodeGuid;
    FString PinId;
    FString SavedFilename;
    FString IndexRefreshError;
    FString ExecutionError;
    bool bIndexRefreshed = false;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            UBlueprint* Blueprint = nullptr;
            if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
            {
                return false;
            }

            UEdGraph* Graph = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, FString(), GraphGuid, Graph, OutError))
            {
                return false;
            }

            if (!Blueprint->FunctionGraphs.Contains(Graph))
            {
                OutError = TEXT("graphGuid does not identify a Blueprint function graph.");
                return false;
            }

            UK2Node_FunctionEntry* EntryNode = FindFunctionEntry(Graph);
            if (EntryNode == nullptr)
            {
                OutError = TEXT("The function graph has no function entry node.");
                return false;
            }

            FEdGraphPinType PinType;
            if (!UnrealMCP::BlueprintEditToolUtils::BuildPinType(TypeName, TypeObjectPath, bIsArray, PinType, OutError))
            {
                return false;
            }

            if (HasUserParameterNamed(Graph, ParameterName))
            {
                OutError = FString::Printf(TEXT("Function parameter '%s' already exists."), *ParameterName);
                return false;
            }

            FunctionName = Graph->GetName();
            EntryNodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(EntryNode);
            TargetNodeGuid = EntryNodeGuid;

            UK2Node_FunctionResult* ExistingResultNode = nullptr;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
                {
                    ExistingResultNode = ResultNode;
                    ResultNodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(ResultNode);
                    break;
                }
            }

            if (bIsInput)
            {
                FText ValidationError;
                if (!EntryNode->CanCreateUserDefinedPin(PinType, EGPD_Output, ValidationError))
                {
                    OutError = ValidationError.IsEmpty() ? TEXT("The function entry rejected this input parameter type.") : ValidationError.ToString();
                    return false;
                }
            }

            if (bDryRun)
            {
                if (bIsOutput && ExistingResultNode != nullptr)
                {
                    TargetNodeGuid = ResultNodeGuid;
                }
                return true;
            }

            const FScopedTransaction Transaction(NSLOCTEXT(
                "UnrealMCP",
                "AddBlueprintFunctionParameter",
                "UnrealMCP Add Blueprint Function Parameter"));
            Blueprint->Modify();
            Graph->Modify();
            EntryNode->Modify();

            UEdGraphPin* CreatedPin = nullptr;
            if (bIsInput)
            {
                CreatedPin = EntryNode->CreateUserDefinedPin(*ParameterName, PinType, EGPD_Output, false);
            }
            else
            {
                UK2Node_FunctionResult* ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
                if (ResultNode == nullptr)
                {
                    OutError = TEXT("Unreal failed to create or resolve the function result node.");
                    return false;
                }

                ResultNodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(ResultNode);
                TargetNodeGuid = ResultNodeGuid;
                TArray<UK2Node_FunctionResult*> ResultNodes = ResultNode->GetAllResultNodes();
                if (ResultNodes.IsEmpty())
                {
                    ResultNodes.Add(ResultNode);
                }

                for (UK2Node_FunctionResult* CurrentResult : ResultNodes)
                {
                    FText ValidationError;
                    if (CurrentResult == nullptr || !CurrentResult->CanCreateUserDefinedPin(PinType, EGPD_Input, ValidationError))
                    {
                        OutError = ValidationError.IsEmpty() ? TEXT("A function result node rejected this output parameter type.") : ValidationError.ToString();
                        return false;
                    }
                }

                for (UK2Node_FunctionResult* CurrentResult : ResultNodes)
                {
                    CurrentResult->Modify();
                    UEdGraphPin* CurrentPin = CurrentResult->CreateUserDefinedPin(*ParameterName, PinType, EGPD_Input, false);
                    if (CurrentPin == nullptr)
                    {
                        OutError = TEXT("Unreal failed to create the output parameter pin on every function result node.");
                        return false;
                    }
                    if (CurrentResult == ResultNode)
                    {
                        CreatedPin = CurrentPin;
                    }
                }
            }

            if (CreatedPin == nullptr)
            {
                OutError = TEXT("Unreal failed to create the function parameter pin.");
                return false;
            }

            PinId = UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(CreatedPin);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
                Blueprint,
                ObjectPath,
                bSave,
                SavedFilename,
                bIndexRefreshed,
                IndexRefreshError,
                OutError);
        },
        ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetStringField(TEXT("graphGuid"), GraphGuid);
    Result->SetStringField(TEXT("entryNodeGuid"), EntryNodeGuid);
    Result->SetStringField(TEXT("resultNodeGuid"), ResultNodeGuid);
    Result->SetStringField(TEXT("targetNodeGuid"), TargetNodeGuid);
    Result->SetStringField(TEXT("pinId"), PinId);
    Result->SetStringField(TEXT("parameterName"), ParameterName);
    Result->SetStringField(TEXT("direction"), bIsInput ? TEXT("input") : TEXT("output"));
    Result->SetStringField(TEXT("type"), TypeName);
    Result->SetStringField(TEXT("typeObjectPath"), TypeObjectPath);
    Result->SetBoolField(TEXT("isArray"), bIsArray);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("added"), !bDryRun);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintFunctionParameterTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Stable GUID of the target function graph.")));
    Properties->SetObjectField(TEXT("parameterName"), BuildStringProperty(TEXT("Unique function parameter name.")));
    Properties->SetObjectField(TEXT("direction"), BuildStringProperty(TEXT("Parameter direction: input or output.")));
    Properties->SetObjectField(TEXT("type"), BuildStringProperty(TEXT("bool, float, int, string, name, text, object, or class.")));
    Properties->SetObjectField(TEXT("typeObjectPath"), BuildStringProperty(TEXT("Required class path for object/class types.")));
    Properties->SetObjectField(TEXT("isArray"), BuildBoolProperty(TEXT("Create an array parameter.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without mutation.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index after mutation.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(
        TEXT("required"),
        {
            MakeShared<FJsonValueString>(TEXT("objectPath")),
            MakeShared<FJsonValueString>(TEXT("graphGuid")),
            MakeShared<FJsonValueString>(TEXT("parameterName")),
            MakeShared<FJsonValueString>(TEXT("direction")),
            MakeShared<FJsonValueString>(TEXT("type"))
        });
    return Schema;
}
