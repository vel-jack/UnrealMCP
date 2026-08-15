#include "Tools/SetBlueprintPinDefaultObjectTool.h"

#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    bool ResolveCompatibleDefaultObject(
        const FString& DefaultObjectPath,
        const UEdGraphPin* Pin,
        UObject*& OutObject,
        FString& OutExpectedType,
        FString& OutError)
    {
        OutObject = nullptr;
        OutExpectedType.Reset();

        if (Pin->PinType.ContainerType != EPinContainerType::None)
        {
            OutError = TEXT("Object defaults are not supported on container pins.");
            return false;
        }

        const FName Category = Pin->PinType.PinCategory;
        if (Category != UEdGraphSchema_K2::PC_Object && Category != UEdGraphSchema_K2::PC_Class)
        {
            OutError = FString::Printf(
                TEXT("Pin category '%s' is not object/class-compatible."),
                *Category.ToString());
            return false;
        }

        OutObject = LoadObject<UObject>(nullptr, *DefaultObjectPath);
        if (OutObject == nullptr)
        {
            OutError = FString::Printf(
                TEXT("Could not load exact default object/class path '%s'. Class pins require a /Script/... class path or an exact generated Blueprint class path ending in _C."),
                *DefaultObjectPath);
            return false;
        }

        UClass* ExpectedClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
        OutExpectedType = ExpectedClass != nullptr ? ExpectedClass->GetPathName() : FString();

        if (Category == UEdGraphSchema_K2::PC_Class)
        {
            UClass* RequestedClass = Cast<UClass>(OutObject);
            if (RequestedClass == nullptr)
            {
                OutError = FString::Printf(
                    TEXT("Class pin requires a UClass, but '%s' resolved to '%s'."),
                    *DefaultObjectPath,
                    *OutObject->GetClass()->GetPathName());
                return false;
            }
            if (ExpectedClass != nullptr && !RequestedClass->IsChildOf(ExpectedClass))
            {
                OutError = FString::Printf(
                    TEXT("Class '%s' is not derived from the pin's required class '%s'."),
                    *RequestedClass->GetPathName(),
                    *ExpectedClass->GetPathName());
                return false;
            }
        }
        else if (ExpectedClass != nullptr && !OutObject->IsA(ExpectedClass))
        {
            OutError = FString::Printf(
                TEXT("Object '%s' is not compatible with the pin's required class '%s'."),
                *OutObject->GetPathName(),
                *ExpectedClass->GetPathName());
            return false;
        }

        return true;
    }
}

FSetBlueprintPinDefaultObjectTool::FSetBlueprintPinDefaultObjectTool()
    : FMCPToolBase(
        TEXT("SetBlueprintPinDefaultObject"),
        TEXT("Sets an exact UObject or UClass default on an unconnected Blueprint input pin using stable graph, node, and pin identity. Supports dry-run and optional save."))
{
}

UnrealMCP::FMCPResponse FSetBlueprintPinDefaultObjectTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
    FString NodeGuid;
    FString PinId;
    FString PinName;
    FString RequestedDefaultObjectPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("nodeGuid"), NodeGuid)
        || !Request.Params->TryGetStringField(TEXT("defaultObjectPath"), RequestedDefaultObjectPath))
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("SetBlueprintPinDefaultObject requires objectPath, nodeGuid, defaultObjectPath, a graph selector, and a pin selector."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("pinId"), PinId);
    Request.Params->TryGetStringField(TEXT("pinName"), PinName);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SetBlueprintPinDefaultObject requires graphName or graphGuid."));
    }
    if (PinId.IsEmpty() && PinName.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SetBlueprintPinDefaultObject requires pinId or pinName."));
    }
    if (RequestedDefaultObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("defaultObjectPath must be an exact, non-empty UObject or UClass path."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSaveAfterEdit = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    FString ResolvedPinId;
    FString ResolvedPinName;
    FString PinCategory;
    FString ExpectedTypePath;
    FString OldEffectiveDefault;
    FString OldDefaultObjectPath;
    FString NewEffectiveDefault;
    FString NewDefaultObjectPath;
    FString ResolvedDefaultObjectPath;
    FString Filename;
    FString IndexError;
    FString ExecutionError;
    bool bIndexRefreshed = false;
    bool bChanged = false;

    const bool bExecuted = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            UBlueprint* Blueprint = nullptr;
            if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;

            UEdGraph* Graph = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError)) return false;

            UEdGraphNode* Node = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Graph, NodeGuid, Node, OutError)) return false;

            UEdGraphPin* Pin = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolvePin(Node, PinId, PinName, TEXT("input"), Pin, OutError)) return false;
            if (Pin->LinkedTo.Num() > 0)
            {
                OutError = TEXT("Cannot set an object default on a connected input pin.");
                return false;
            }

            const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
            if (Schema == nullptr)
            {
                OutError = TEXT("Graph does not use the K2 schema.");
                return false;
            }

            UObject* RequestedObject = nullptr;
            if (!ResolveCompatibleDefaultObject(RequestedDefaultObjectPath, Pin, RequestedObject, ExpectedTypePath, OutError)) return false;

            const FString ValidationError = Schema->IsPinDefaultValid(Pin, FString(), RequestedObject, FText::GetEmpty());
            if (!ValidationError.IsEmpty())
            {
                OutError = FString::Printf(TEXT("K2 schema rejected the requested object default: %s"), *ValidationError);
                return false;
            }

            ResolvedPinId = UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Pin);
            ResolvedPinName = Pin->PinName.ToString();
            PinCategory = Pin->PinType.PinCategory.ToString();
            OldEffectiveDefault = UnrealMCP::BlueprintGraphEditToolUtils::GetEffectivePinDefaultValue(Pin);
            OldDefaultObjectPath = UnrealMCP::BlueprintGraphEditToolUtils::GetPinDefaultObjectPath(Pin);
            ResolvedDefaultObjectPath = RequestedObject->GetPathName();
            bChanged = Pin->DefaultObject != RequestedObject;

            if (bDryRun)
            {
                NewEffectiveDefault = ResolvedDefaultObjectPath;
                NewDefaultObjectPath = ResolvedDefaultObjectPath;
                return true;
            }

            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetPinDefaultObject", "UnrealMCP Set Blueprint Pin Default Object"));
            Blueprint->Modify();
            Graph->Modify();
            Node->Modify();
            Schema->TrySetDefaultObject(*Pin, RequestedObject);

            NewEffectiveDefault = UnrealMCP::BlueprintGraphEditToolUtils::GetEffectivePinDefaultValue(Pin);
            NewDefaultObjectPath = UnrealMCP::BlueprintGraphEditToolUtils::GetPinDefaultObjectPath(Pin);
            if (Pin->DefaultObject != RequestedObject)
            {
                OutError = FString::Printf(
                    TEXT("Unreal did not apply the requested object default. Requested='%s', applied='%s'."),
                    *ResolvedDefaultObjectPath,
                    *NewDefaultObjectPath);
                return false;
            }

            if (bChanged)
            {
                FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            }
            return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
                Blueprint,
                ObjectPath,
                bSaveAfterEdit,
                Filename,
                bIndexRefreshed,
                IndexError,
                OutError);
        },
        ExecutionError);

    if (!bExecuted)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("pinId"), ResolvedPinId);
    Result->SetStringField(TEXT("pinName"), ResolvedPinName);
    Result->SetStringField(TEXT("pinCategory"), PinCategory);
    Result->SetStringField(TEXT("expectedTypePath"), ExpectedTypePath);
    Result->SetStringField(TEXT("requestedDefaultObjectPath"), RequestedDefaultObjectPath);
    Result->SetStringField(TEXT("resolvedDefaultObjectPath"), ResolvedDefaultObjectPath);
    Result->SetStringField(TEXT("oldEffectiveDefault"), OldEffectiveDefault);
    Result->SetStringField(TEXT("oldDefaultObjectPath"), OldDefaultObjectPath);
    Result->SetStringField(TEXT("newEffectiveDefault"), NewEffectiveDefault);
    Result->SetStringField(TEXT("newDefaultObjectPath"), NewDefaultObjectPath);
    Result->SetStringField(TEXT("mutationStatus"), bDryRun ? TEXT("validated") : (bChanged ? TEXT("applied") : TEXT("unchanged")));
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("changed"), !bDryRun && bChanged);
    Result->SetBoolField(TEXT("wouldChange"), bDryRun && bChanged);
    Result->SetBoolField(TEXT("saved"), bSaveAfterEdit && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), Filename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSetBlueprintPinDefaultObjectTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Exact Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Graph name; graphGuid is preferred when available.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Stable graph GUID.")));
    Properties->SetObjectField(TEXT("nodeGuid"), BuildStringProperty(TEXT("Stable node GUID.")));
    Properties->SetObjectField(TEXT("pinId"), BuildStringProperty(TEXT("Stable input pin GUID; preferred over pinName.")));
    Properties->SetObjectField(TEXT("pinName"), BuildStringProperty(TEXT("Input pin name when pinId is unavailable.")));
    Properties->SetObjectField(
        TEXT("defaultObjectPath"),
        BuildStringProperty(TEXT("Exact UObject path, /Script/... UClass path, or generated Blueprint class path ending in _C.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate resolution and compatibility without mutating the Blueprint.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save the Blueprint and partially refresh its project-index entry after mutation.")));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Required.Add(MakeShared<FJsonValueString>(TEXT("nodeGuid")));
    Required.Add(MakeShared<FJsonValueString>(TEXT("defaultObjectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
