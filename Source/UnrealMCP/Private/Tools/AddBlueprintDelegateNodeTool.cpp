#include "Tools/AddBlueprintDelegateNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UnrealType.h"

FAddBlueprintDelegateNodeTool::FAddBlueprintDelegateNodeTool()
    : FMCPToolBase(
        TEXT("AddBlueprintDelegateNode"),
        TEXT("Adds a Bind or Unbind node for an exact Blueprint-assignable multicast delegate property on a reflected owner class."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintDelegateNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
    FString OwnerClassPath;
    FString DelegateName;
    FString Operation = TEXT("bind");
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("ownerClassPath"), OwnerClassPath)
        || !Request.Params->TryGetStringField(TEXT("delegateName"), DelegateName))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("AddBlueprintDelegateNode requires objectPath, ownerClassPath, delegateName, and graph selector."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("operation"), Operation);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));
    }
    const bool bBind = Operation.Equals(TEXT("bind"), ESearchCase::IgnoreCase);
    if (!bBind && !Operation.Equals(TEXT("unbind"), ESearchCase::IgnoreCase))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("operation must be bind or unbind."));
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
    TArray<TSharedPtr<FJsonValue>> SignatureParameters;

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
        if (DelegateProperty == nullptr)
        {
            OutError = FString::Printf(TEXT("Multicast delegate property '%s' was not found on '%s'."), *DelegateName, *OwnerClass->GetPathName());
            return false;
        }
        if (!DelegateProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable))
        {
            OutError = FString::Printf(TEXT("Delegate '%s' is not BlueprintAssignable."), *DelegateName);
            return false;
        }
        if (DelegateProperty->SignatureFunction == nullptr)
        {
            OutError = TEXT("Delegate signature function is unavailable.");
            return false;
        }
        ResolvedOwnerClassPath = OwnerClass->GetPathName();
        SignatureFunctionPath = DelegateProperty->SignatureFunction->GetPathName();
        for (TFieldIterator<FProperty> It(DelegateProperty->SignatureFunction); It; ++It)
        {
            FProperty* Parameter = *It;
            if (!Parameter->HasAnyPropertyFlags(CPF_Parm) || Parameter->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                continue;
            }
            TSharedRef<FJsonObject> ParameterJson = MakeShared<FJsonObject>();
            ParameterJson->SetStringField(TEXT("name"), Parameter->GetName());
            ParameterJson->SetStringField(TEXT("cppType"), Parameter->GetCPPType());
            ParameterJson->SetBoolField(TEXT("out"), Parameter->HasAnyPropertyFlags(CPF_OutParm));
            SignatureParameters.Add(MakeShared<FJsonValueObject>(ParameterJson));
        }

        Placement = BlueprintGraphEditToolUtils::ResolvePlacement(Graph, Request.Params, OutError);
        if (!OutError.IsEmpty() || bDryRun)
        {
            return OutError.IsEmpty();
        }

        const FScopedTransaction Transaction(
            NSLOCTEXT("UnrealMCP", "AddBlueprintDelegateNode", "UnrealMCP Add Blueprint Delegate Node"));
        Blueprint->Modify();
        Graph->Modify();
        UK2Node_BaseMCDelegate* Node = bBind
            ? static_cast<UK2Node_BaseMCDelegate*>(NewObject<UK2Node_AddDelegate>(Graph))
            : static_cast<UK2Node_BaseMCDelegate*>(NewObject<UK2Node_RemoveDelegate>(Graph));
        const bool bSelfContext = Blueprint->GeneratedClass != nullptr
            && Blueprint->GeneratedClass->IsChildOf(OwnerClass);
        Node->SetFromProperty(DelegateProperty, bSelfContext, OwnerClass);
        BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, Placement);
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
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
    Result->SetStringField(TEXT("operation"), bBind ? TEXT("bind") : TEXT("unbind"));
    Result->SetStringField(TEXT("signatureFunctionPath"), SignatureFunctionPath);
    Result->SetArrayField(TEXT("signatureParameters"), SignatureParameters);
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

TSharedPtr<FJsonObject> FAddBlueprintDelegateNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact graph GUID.")));
    Properties->SetObjectField(TEXT("ownerClassPath"), BuildStringProperty(TEXT("Exact class that declares the BlueprintAssignable delegate.")));
    Properties->SetObjectField(TEXT("delegateName"), BuildStringProperty(TEXT("Exact multicast delegate property name.")));
    Properties->SetObjectField(TEXT("operation"), BuildStringProperty(TEXT("bind or unbind; defaults to bind.")));
    Properties->SetObjectField(TEXT("relativeToNodeGuid"), BuildStringProperty(TEXT("Optional placement anchor.")));
    TSharedRef<FJsonObject> IntegerProperty = MakeShared<FJsonObject>();
    IntegerProperty->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("positionX"), IntegerProperty);
    Properties->SetObjectField(TEXT("positionY"), IntegerProperty);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate the delegate signature and placement.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("ownerClassPath")),
        MakeShared<FJsonValueString>(TEXT("delegateName"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
