#include "Tools/AddBlueprintDelegateEventNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UnrealType.h"

FAddBlueprintDelegateEventNodeTool::FAddBlueprintDelegateEventNodeTool()
    : FMCPToolBase(
        TEXT("AddBlueprintDelegateEventNode"),
        TEXT("Adds a uniquely named Custom Event whose parameter pins exactly match a reflected Blueprint-assignable multicast delegate signature."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintDelegateEventNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
    FString OwnerClassPath;
    FString DelegateName;
    FString EventName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("ownerClassPath"), OwnerClassPath)
        || !Request.Params->TryGetStringField(TEXT("delegateName"), DelegateName)
        || !Request.Params->TryGetStringField(TEXT("eventName"), EventName)
        || EventName.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("AddBlueprintDelegateEventNode requires objectPath, ownerClassPath, delegateName, eventName, and graph selector."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool bIndexRefreshed = false;
    FString ResolvedOwnerClassPath;
    FString SignatureFunctionPath;
    FString NodeGuid;
    FString SavedFilename;
    FString IndexRefreshError;
    FString ExecutionError;
    BlueprintGraphEditToolUtils::FPlacement Placement;
    TArray<TSharedPtr<FJsonValue>> Pins;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
        {
            return false;
        }
        UEdGraph* Graph = nullptr;
        if (!BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError))
        {
            return false;
        }
        UClass* OwnerClass = nullptr;
        if (!BlueprintEditToolUtils::ResolveClass(OwnerClassPath, OwnerClass, OutError))
        {
            return false;
        }
        FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(OwnerClass, *DelegateName);
        if (DelegateProperty == nullptr || !DelegateProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable))
        {
            OutError = FString::Printf(TEXT("BlueprintAssignable multicast delegate '%s' was not found on '%s'."), *DelegateName, *OwnerClass->GetPathName());
            return false;
        }
        if (DelegateProperty->SignatureFunction == nullptr)
        {
            OutError = TEXT("Delegate signature function is unavailable.");
            return false;
        }
        for (UEdGraphNode* ExistingNode : Graph->Nodes)
        {
            const UK2Node_CustomEvent* ExistingEvent = Cast<UK2Node_CustomEvent>(ExistingNode);
            if (ExistingEvent != nullptr
                && ExistingEvent->CustomFunctionName.ToString().Equals(EventName, ESearchCase::IgnoreCase))
            {
                OutError = FString::Printf(
                    TEXT("Custom Event '%s' already exists (nodeGuid=%s); choose a unique name to avoid signature ambiguity."),
                    *EventName,
                    *BlueprintGraphEditToolUtils::GetNodeGuid(ExistingEvent));
                return false;
            }
        }

        ResolvedOwnerClassPath = OwnerClass->GetPathName();
        SignatureFunctionPath = DelegateProperty->SignatureFunction->GetPathName();
        Placement = BlueprintGraphEditToolUtils::ResolvePlacement(Graph, Request.Params, OutError);
        if (!OutError.IsEmpty() || bDryRun)
        {
            return OutError.IsEmpty();
        }

        const FScopedTransaction Transaction(
            NSLOCTEXT("UnrealMCP", "AddBlueprintDelegateEventNode", "UnrealMCP Add Blueprint Delegate Event"));
        Blueprint->Modify();
        Graph->Modify();
        UK2Node_CustomEvent* Node = UK2Node_CustomEvent::CreateFromFunction(
            FVector2D(Placement.X, Placement.Y),
            Graph,
            EventName,
            DelegateProperty->SignatureFunction,
            false);
        if (Node == nullptr)
        {
            OutError = TEXT("Unreal failed to create the signature-matched Custom Event.");
            return false;
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        NodeGuid = BlueprintGraphEditToolUtils::GetNodeGuid(Node);
        Pins = BlueprintGraphEditToolUtils::SerializePins(Node);
        return BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
            Blueprint,
            ObjectPath,
            bSave,
            SavedFilename,
            bIndexRefreshed,
            IndexRefreshError,
            OutError);
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);
    }

    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("ownerClassPath"), ResolvedOwnerClassPath);
    Result->SetStringField(TEXT("delegateName"), DelegateName);
    Result->SetStringField(TEXT("signatureFunctionPath"), SignatureFunctionPath);
    Result->SetStringField(TEXT("eventName"), EventName);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("added"), !bDryRun);
    Result->SetNumberField(TEXT("positionX"), Placement.X);
    Result->SetNumberField(TEXT("positionY"), Placement.Y);
    Result->SetBoolField(TEXT("collisionAdjusted"), Placement.bCollisionAdjusted);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintDelegateEventNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable event graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact event graph GUID.")));
    Properties->SetObjectField(TEXT("ownerClassPath"), BuildStringProperty(TEXT("Exact class that declares the delegate.")));
    Properties->SetObjectField(TEXT("delegateName"), BuildStringProperty(TEXT("Exact BlueprintAssignable multicast delegate property name.")));
    Properties->SetObjectField(TEXT("eventName"), BuildStringProperty(TEXT("Unique Custom Event name to create.")));
    Properties->SetObjectField(TEXT("relativeToNodeGuid"), BuildStringProperty(TEXT("Optional placement anchor.")));
    TSharedRef<FJsonObject> IntegerProperty = MakeShared<FJsonObject>();
    IntegerProperty->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("positionX"), IntegerProperty);
    Properties->SetObjectField(TEXT("positionY"), IntegerProperty);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate the exact delegate signature and placement.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("ownerClassPath")),
        MakeShared<FJsonValueString>(TEXT("delegateName")),
        MakeShared<FJsonValueString>(TEXT("eventName"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
