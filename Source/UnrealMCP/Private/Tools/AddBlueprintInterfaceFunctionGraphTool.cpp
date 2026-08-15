#include "Tools/AddBlueprintInterfaceFunctionGraphTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    bool IsSameInterface(const UClass* Candidate, const UClass* Requested)
    {
        return Candidate != nullptr
            && Requested != nullptr
            && (Candidate == Requested
                || Candidate->GetAuthoritativeClass() == Requested->GetAuthoritativeClass());
    }

    bool ImplementsRequestedInterface(const UClass* Candidate, const UClass* Requested)
    {
        return IsSameInterface(Candidate, Requested)
            || (Candidate != nullptr && Requested != nullptr && Candidate->IsChildOf(Requested));
    }

    UEdGraph* FindInterfaceGraph(
        const UBlueprint* Blueprint,
        const UClass* InterfaceClass,
        const FName FunctionName)
    {
        if (Blueprint == nullptr)
        {
            return nullptr;
        }

        for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
        {
            if (!ImplementsRequestedInterface(Description.Interface, InterfaceClass))
            {
                continue;
            }

            for (UEdGraph* Graph : Description.Graphs)
            {
                if (Graph != nullptr && Graph->GetFName() == FunctionName)
                {
                    return Graph;
                }
            }
        }

        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph != nullptr && Graph->GetFName() == FunctionName)
            {
                return Graph;
            }
        }
        return nullptr;
    }

    bool IsSignatureCorrectInterfaceGraph(
        UEdGraph* Graph,
        const UFunction* RequestedFunction)
    {
        if (Graph == nullptr || RequestedFunction == nullptr)
        {
            return false;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                const UFunction* Signature = Entry->FindSignatureFunction();
                return Signature != nullptr
                    && Signature->GetFName() == RequestedFunction->GetFName()
                    && Signature->GetOwnerClass()->GetAuthoritativeClass()
                        == RequestedFunction->GetOwnerClass()->GetAuthoritativeClass();
            }
        }
        return false;
    }

    void CollectTerminatorInformation(
        UEdGraph* Graph,
        TSharedPtr<FJsonObject>& OutEntryNode,
        TArray<TSharedPtr<FJsonValue>>& OutResultNodes)
    {
        if (Graph == nullptr)
        {
            return;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                TSharedRef<FJsonObject> EntryJson = MakeShared<FJsonObject>();
                EntryJson->SetStringField(
                    TEXT("nodeGuid"),
                    UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Entry));
                EntryJson->SetArrayField(
                    TEXT("pins"),
                    UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Entry));
                OutEntryNode = EntryJson;
            }
            else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
            {
                TSharedRef<FJsonObject> ResultJson = MakeShared<FJsonObject>();
                ResultJson->SetStringField(
                    TEXT("nodeGuid"),
                    UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Result));
                ResultJson->SetArrayField(
                    TEXT("pins"),
                    UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Result));
                OutResultNodes.Add(MakeShared<FJsonValueObject>(ResultJson));
            }
        }
    }
}

FAddBlueprintInterfaceFunctionGraphTool::FAddBlueprintInterfaceFunctionGraphTool()
    : FMCPToolBase(
          TEXT("AddBlueprintInterfaceFunctionGraph"),
          TEXT("Creates the signature-correct function-graph implementation for a non-event Blueprint interface function, including output and return signatures."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintInterfaceFunctionGraphTool::Execute(
    const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString InterfaceClassPath;
    FString FunctionName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("interfaceClassPath"), InterfaceClassPath)
        || !Request.Params->TryGetStringField(TEXT("functionName"), FunctionName)
        || ObjectPath.IsEmpty()
        || InterfaceClassPath.IsEmpty()
        || FunctionName.IsEmpty())
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintInterfaceFunctionGraph requires objectPath, interfaceClassPath, and functionName."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("saveAfterEdit"), false);

    FString ResolvedInterfaceClassPath;
    FString FunctionOwnerClassPath;
    FString ImplementationSource;
    FString GraphName;
    FString GraphGuid;
    FString SavedFilename;
    FString IndexError;
    FString ExecutionError;
    bool bAlreadyExists = false;
    bool bIndexRefreshed = false;
    TSharedPtr<FJsonObject> EntryNode;
    TArray<TSharedPtr<FJsonValue>> ResultNodes;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            UBlueprint* Blueprint = nullptr;
            if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(
                    ObjectPath, Blueprint, OutError))
            {
                return false;
            }

            UClass* InterfaceClass = LoadObject<UClass>(nullptr, *InterfaceClassPath);
            if (InterfaceClass == nullptr)
            {
                OutError = FString::Printf(
                    TEXT("Could not load exact interface class path '%s'."),
                    *InterfaceClassPath);
                return false;
            }
            if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface))
            {
                OutError = FString::Printf(
                    TEXT("Class '%s' is not a Blueprint interface class."),
                    *InterfaceClass->GetPathName());
                return false;
            }
            ResolvedInterfaceClassPath = InterfaceClass->GetPathName();

            TArray<UClass*> ImplementedInterfaces;
            FBlueprintEditorUtils::FindImplementedInterfaces(Blueprint, true, ImplementedInterfaces);
            if (!ImplementedInterfaces.ContainsByPredicate(
                    [InterfaceClass](const UClass* Candidate)
                    {
                        return ImplementsRequestedInterface(Candidate, InterfaceClass);
                    }))
            {
                OutError = FString::Printf(
                    TEXT("Blueprint '%s' does not directly or inheritedly implement interface '%s'."),
                    *ObjectPath,
                    *ResolvedInterfaceClassPath);
                return false;
            }

            FBPInterfaceDescription* DirectDescription = nullptr;
            for (FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
            {
                if (ImplementsRequestedInterface(Description.Interface, InterfaceClass))
                {
                    DirectDescription = &Description;
                    break;
                }
            }
            ImplementationSource = DirectDescription != nullptr ? TEXT("direct") : TEXT("inherited");

            UFunction* Function = InterfaceClass->FindFunctionByName(*FunctionName);
            if (Function == nullptr
                || !Function->GetName().Equals(FunctionName, ESearchCase::CaseSensitive))
            {
                OutError = FString::Printf(
                    TEXT("Exact function '%s' was not found on interface '%s'."),
                    *FunctionName,
                    *ResolvedInterfaceClassPath);
                return false;
            }
            FunctionOwnerClassPath = Function->GetOwnerClass()->GetPathName();

            if (!Function->HasAnyFunctionFlags(FUNC_BlueprintEvent)
                || !UEdGraphSchema_K2::CanKismetOverrideFunction(Function))
            {
                OutError = FString::Printf(
                    TEXT("Interface function '%s' is not a Blueprint-implementable function."),
                    *FunctionName);
                return false;
            }
            if (UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function))
            {
                OutError = FString::Printf(
                    TEXT("Interface function '%s' is event-compatible. Use AddBlueprintInterfaceEventNode instead."),
                    *FunctionName);
                return false;
            }

            const FName ExactFunctionName = Function->GetFName();
            if (UEdGraph* ExistingGraph = FindInterfaceGraph(
                    Blueprint, InterfaceClass, ExactFunctionName))
            {
                if (!IsSignatureCorrectInterfaceGraph(
                        ExistingGraph, Function))
                {
                    OutError = FString::Printf(
                        TEXT("Graph '%s' conflicts with the requested interface function but does not implement its signature."),
                        *ExistingGraph->GetName());
                    return false;
                }
                bAlreadyExists = true;
                GraphName = ExistingGraph->GetName();
                GraphGuid = ExistingGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
                CollectTerminatorInformation(ExistingGraph, EntryNode, ResultNodes);
                if (!EntryNode.IsValid())
                {
                    OutError = FString::Printf(
                        TEXT("Existing graph '%s' does not contain a function entry node and is not a valid interface implementation graph."),
                        *GraphName);
                    return false;
                }
                return true;
            }

            if (!FBlueprintEditorUtils::IsGraphNameUnique(Blueprint, ExactFunctionName))
            {
                OutError = FString::Printf(
                    TEXT("Function graph name '%s' conflicts with an existing Blueprint graph or member."),
                    *FunctionName);
                return false;
            }
            if (bDryRun)
            {
                GraphName = FunctionName;
                return true;
            }

            const FScopedTransaction Transaction(
                NSLOCTEXT(
                    "UnrealMCP",
                    "AddInterfaceFunctionGraph",
                    "UnrealMCP Add Interface Function Graph"));
            Blueprint->Modify();
            UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
                Blueprint,
                ExactFunctionName,
                UEdGraph::StaticClass(),
                UEdGraphSchema_K2::StaticClass());
            if (Graph == nullptr)
            {
                OutError = TEXT("Unreal failed to create the interface implementation graph.");
                return false;
            }
            Graph->Modify();

            if (DirectDescription != nullptr)
            {
                Graph->bAllowDeletion = false;
                Graph->InterfaceGuid = FBlueprintEditorUtils::FindInterfaceFunctionGuid(
                    Function, DirectDescription->Interface);
                DirectDescription->Graphs.Add(Graph);
                FBlueprintEditorUtils::AddInterfaceGraph(
                    Blueprint, Graph, DirectDescription->Interface);
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            }
            else
            {
                UClass* SignatureClass = Function->GetOwnerClass()->GetAuthoritativeClass();
                FBlueprintEditorUtils::AddFunctionGraph<UClass>(
                    Blueprint, Graph, false, SignatureClass);
            }

            GraphName = Graph->GetName();
            GraphGuid = Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
            CollectTerminatorInformation(Graph, EntryNode, ResultNodes);
            if (!EntryNode.IsValid())
            {
                OutError = TEXT("Unreal created the graph without a function entry node.");
                return false;
            }

            return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
                Blueprint,
                ObjectPath,
                bSave,
                SavedFilename,
                bIndexRefreshed,
                IndexError,
                OutError);
        },
        ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("interfaceClassPath"), ResolvedInterfaceClassPath);
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetStringField(TEXT("functionOwnerClass"), FunctionOwnerClassPath);
    Result->SetStringField(TEXT("implementationSource"), ImplementationSource);
    Result->SetStringField(TEXT("graphName"), GraphName);
    Result->SetStringField(TEXT("graphGuid"), GraphGuid);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyExists"), bAlreadyExists);
    Result->SetBoolField(TEXT("created"), !bDryRun && !bAlreadyExists);
    Result->SetBoolField(TEXT("wouldCreate"), bDryRun && !bAlreadyExists);
    Result->SetObjectField(
        TEXT("entryNode"),
        EntryNode.IsValid() ? EntryNode.ToSharedRef() : MakeShared<FJsonObject>());
    Result->SetArrayField(TEXT("resultNodes"), ResultNodes);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bAlreadyExists);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintInterfaceFunctionGraphTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(
        TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(
        TEXT("interfaceClassPath"),
        BuildStringProperty(TEXT("Exact /Script/... or generated Blueprint interface class path.")));
    Properties->SetObjectField(
        TEXT("functionName"), BuildStringProperty(TEXT("Exact interface function name.")));
    Properties->SetObjectField(
        TEXT("dryRun"),
        BuildBoolProperty(TEXT("Validate and report whether the interface graph would be created.")));
    Properties->SetObjectField(
        TEXT("saveAfterEdit"),
        BuildBoolProperty(TEXT("Save and partially refresh the Blueprint after creation.")));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("interfaceClassPath")),
        MakeShared<FJsonValueString>(TEXT("functionName"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
