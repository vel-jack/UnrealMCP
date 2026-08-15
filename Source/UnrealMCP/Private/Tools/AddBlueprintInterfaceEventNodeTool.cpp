#include "Tools/AddBlueprintInterfaceEventNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintInterfaceEventNodeTool::FAddBlueprintInterfaceEventNodeTool()
    : FMCPToolBase(
          TEXT("AddBlueprintInterfaceEventNode"),
          TEXT("Adds an implemented Blueprint interface function as an event node in an exact Ubergraph, with idempotent duplicate detection, dry-run, and optional save."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintInterfaceEventNodeTool::Execute(
    const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
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
            TEXT("AddBlueprintInterfaceEventNode requires objectPath, interfaceClassPath, functionName, and graphName or graphGuid."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintInterfaceEventNode requires graphName or graphGuid."));
    }

    double NodeX = 0.0;
    double NodeY = 0.0;
    const bool bHasNodeX = Request.Params->TryGetNumberField(TEXT("nodeX"), NodeX);
    const bool bHasNodeY = Request.Params->TryGetNumberField(TEXT("nodeY"), NodeY);
    if (bHasNodeX != bHasNodeY)
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("nodeX and nodeY must be provided together."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("saveAfterEdit"), false);

    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;
    FString NodeGuid;
    FString ResolvedGraphName;
    FString ResolvedGraphGuid;
    FString ResolvedInterfaceClassPath;
    FString FunctionOwnerClassPath;
    FString ImplementationSource;
    FString SavedFilename;
    FString IndexError;
    FString ExecutionError;
    bool bAlreadyExists = false;
    bool bIndexRefreshed = false;
    TArray<TSharedPtr<FJsonValue>> Pins;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            UBlueprint* Blueprint = nullptr;
            if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(
                    ObjectPath, Blueprint, OutError))
            {
                return false;
            }

            UEdGraph* Graph = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(
                    Blueprint, GraphName, GraphGuid, Graph, OutError))
            {
                return false;
            }
            if (Graph->GetSchema() == nullptr
                || Graph->GetSchema()->GetGraphType(Graph) != EGraphType::GT_Ubergraph)
            {
                OutError = TEXT("Interface event nodes can only be added to an Ubergraph event graph.");
                return false;
            }
            ResolvedGraphName = Graph->GetName();
            ResolvedGraphGuid = Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);

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
            UClass* MatchedInterface = nullptr;
            for (UClass* Candidate : ImplementedInterfaces)
            {
                if (Candidate == InterfaceClass
                    || (Candidate != nullptr
                        && Candidate->GetAuthoritativeClass() == InterfaceClass->GetAuthoritativeClass()))
                {
                    MatchedInterface = Candidate;
                    break;
                }
            }
            if (MatchedInterface == nullptr)
            {
                OutError = FString::Printf(
                    TEXT("Blueprint '%s' does not directly or inheritedly implement interface '%s'."),
                    *ObjectPath,
                    *ResolvedInterfaceClassPath);
                return false;
            }

            ImplementationSource = TEXT("inherited");
            for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
            {
                if (Description.Interface == InterfaceClass
                    || (Description.Interface != nullptr
                        && Description.Interface->GetAuthoritativeClass()
                            == InterfaceClass->GetAuthoritativeClass()))
                {
                    ImplementationSource = TEXT("direct");
                    break;
                }
            }

            UFunction* Function = InterfaceClass->FindFunctionByName(*FunctionName);
            if (Function == nullptr)
            {
                OutError = FString::Printf(
                    TEXT("Function '%s' was not found on interface '%s'."),
                    *FunctionName,
                    *ResolvedInterfaceClassPath);
                return false;
            }
            if (UEdGraphSchema_K2::HasFunctionAnyOutputParameter(Function))
            {
                OutError = FString::Printf(
                    TEXT("Interface function '%s' has an output, return, or non-const output reference parameter and must be implemented as a function graph, not an event node."),
                    *FunctionName);
                return false;
            }
            if (!Function->HasAnyFunctionFlags(FUNC_BlueprintEvent)
                || !UEdGraphSchema_K2::CanKismetOverrideFunction(Function)
                || !UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function))
            {
                OutError = FString::Printf(
                    TEXT("Interface function '%s' cannot be implemented as a Blueprint event node; it may be forced as a function, static, const, thread-safe, internal, final, or otherwise non-overrideable."),
                    *FunctionName);
                return false;
            }

            UClass* SignatureClass = Function->GetOwnerClass()->GetAuthoritativeClass();
            FunctionOwnerClassPath = SignatureClass->GetPathName();
            if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(
                    Blueprint, SignatureClass, Function->GetFName()))
            {
                if (Existing->GetGraph() != Graph)
                {
                    const UEdGraph* ExistingGraph = Existing->GetGraph();
                    const FString ExistingGraphName = ExistingGraph ? ExistingGraph->GetName() : FString();
                    const FString ExistingGraphGuid = ExistingGraph
                        ? ExistingGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
                        : FString();
                    OutError = FString::Printf(
                        TEXT("Interface event '%s' already exists in graph '%s' (graphGuid=%s, nodeGuid=%s), not the selected graph '%s'."),
                        *FunctionName,
                        *ExistingGraphName,
                        *ExistingGraphGuid,
                        *UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Existing),
                        *ResolvedGraphName);
                    return false;
                }

                bAlreadyExists = true;
                NodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Existing);
                Placement.X = Existing->NodePosX;
                Placement.Y = Existing->NodePosY;
                Pins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Existing);
                return true;
            }

            TSharedRef<FJsonObject> PlacementParams = MakeShared<FJsonObject>();
            if (bHasNodeX)
            {
                PlacementParams->SetNumberField(TEXT("positionX"), NodeX);
                PlacementParams->SetNumberField(TEXT("positionY"), NodeY);
            }
            Placement = UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(
                Graph, PlacementParams, OutError);
            if (!OutError.IsEmpty() || bDryRun)
            {
                return OutError.IsEmpty();
            }

            const FScopedTransaction Transaction(
                NSLOCTEXT("UnrealMCP", "AddInterfaceEvent", "UnrealMCP Add Interface Event"));
            Blueprint->Modify();
            Graph->Modify();

            UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
            Node->EventReference.SetExternalMember(Function->GetFName(), SignatureClass);
            Node->bOverrideFunction = true;
            UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, Placement);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

            NodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
            Pins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Node);
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
    Result->SetStringField(TEXT("graphName"), ResolvedGraphName);
    Result->SetStringField(TEXT("graphGuid"), ResolvedGraphGuid);
    Result->SetStringField(TEXT("interfaceClassPath"), ResolvedInterfaceClassPath);
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetStringField(TEXT("functionOwnerClass"), FunctionOwnerClassPath);
    Result->SetStringField(TEXT("implementationSource"), ImplementationSource);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyExists"), bAlreadyExists);
    Result->SetBoolField(TEXT("added"), !bDryRun && !bAlreadyExists);
    Result->SetBoolField(TEXT("wouldAdd"), bDryRun && !bAlreadyExists);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("nodeType"), TEXT("interfaceEvent"));
    Result->SetNumberField(TEXT("positionX"), Placement.X);
    Result->SetNumberField(TEXT("positionY"), Placement.Y);
    Result->SetBoolField(TEXT("autoPlaced"), Placement.bAutoPlaced);
    Result->SetBoolField(TEXT("collisionAdjusted"), Placement.bCollisionAdjusted);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bAlreadyExists);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintInterfaceEventNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(
        TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(
        TEXT("graphName"), BuildStringProperty(TEXT("Exact Ubergraph name; graphGuid is preferred when available.")));
    Properties->SetObjectField(
        TEXT("graphGuid"), BuildStringProperty(TEXT("Exact stable Ubergraph GUID.")));
    Properties->SetObjectField(
        TEXT("interfaceClassPath"),
        BuildStringProperty(TEXT("Exact /Script/... or generated Blueprint interface class path.")));
    Properties->SetObjectField(
        TEXT("functionName"), BuildStringProperty(TEXT("Exact interface function name.")));

    TSharedRef<FJsonObject> NodeXProperty = MakeShared<FJsonObject>();
    NodeXProperty->SetStringField(TEXT("type"), TEXT("integer"));
    NodeXProperty->SetStringField(TEXT("description"), TEXT("Optional requested node X position."));
    Properties->SetObjectField(TEXT("nodeX"), NodeXProperty);

    TSharedRef<FJsonObject> NodeYProperty = MakeShared<FJsonObject>();
    NodeYProperty->SetStringField(TEXT("type"), TEXT("integer"));
    NodeYProperty->SetStringField(TEXT("description"), TEXT("Optional requested node Y position."));
    Properties->SetObjectField(TEXT("nodeY"), NodeYProperty);

    Properties->SetObjectField(
        TEXT("dryRun"),
        BuildBoolProperty(TEXT("Validate the interface event and preview placement without mutation.")));
    Properties->SetObjectField(
        TEXT("saveAfterEdit"),
        BuildBoolProperty(TEXT("Save and partially refresh this Blueprint after adding.")));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("interfaceClassPath")),
        MakeShared<FJsonValueString>(TEXT("functionName"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
