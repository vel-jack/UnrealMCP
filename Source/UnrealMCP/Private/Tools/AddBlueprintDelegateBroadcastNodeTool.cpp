#include "Tools/AddBlueprintDelegateBroadcastNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallDelegate.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UnrealType.h"

namespace
{
    bool IsUnconnected(const UEdGraphNode* Node)
    {
        if (Node == nullptr)
        {
            return false;
        }

        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin != nullptr && !Pin->LinkedTo.IsEmpty())
            {
                return false;
            }
        }
        return true;
    }

    UK2Node_CallDelegate* FindEquivalentUnconnectedNodeAtPosition(
        UEdGraph* Graph,
        const FMulticastDelegateProperty* DelegateProperty,
        int32 PositionX,
        int32 PositionY)
    {
        if (Graph == nullptr || DelegateProperty == nullptr)
        {
            return nullptr;
        }

        for (UEdGraphNode* ExistingNode : Graph->Nodes)
        {
            UK2Node_CallDelegate* ExistingCall = Cast<UK2Node_CallDelegate>(ExistingNode);
            if (ExistingCall == nullptr
                || ExistingCall->NodePosX != PositionX
                || ExistingCall->NodePosY != PositionY
                || !IsUnconnected(ExistingCall))
            {
                continue;
            }

            // Pointer equality proves both the exact property and its declaring class.
            if (ExistingCall->GetProperty() == DelegateProperty)
            {
                return ExistingCall;
            }
        }
        return nullptr;
    }

    bool ValidateBroadcastAccess(
        const UBlueprint* Blueprint,
        const UClass* OwnerClass,
        const FMulticastDelegateProperty* DelegateProperty,
        FString& OutError)
    {
        if (!DelegateProperty->HasAnyPropertyFlags(CPF_BlueprintCallable))
        {
            OutError = FString::Printf(
                TEXT("Delegate '%s' is not BlueprintCallable and cannot be broadcast from Blueprint."),
                *DelegateProperty->GetName());
            return false;
        }
        if (DelegateProperty->HasAnyPropertyFlags(CPF_Parm))
        {
            OutError = TEXT("Delegate parameters cannot be used as broadcastable member properties.");
            return false;
        }
        if (DelegateProperty->SignatureFunction == nullptr)
        {
            OutError = TEXT("Delegate signature function is unavailable.");
            return false;
        }

        const UClass* ContextClass = Blueprint != nullptr
            ? (Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass)
            : nullptr;
        const bool bContextDerivesFromOwner = ContextClass != nullptr && ContextClass->IsChildOf(OwnerClass);
        if (DelegateProperty->GetBoolMetaData(TEXT("BlueprintPrivate")) && ContextClass != OwnerClass)
        {
            OutError = FString::Printf(
                TEXT("Delegate '%s' is Blueprint-private and can only be broadcast by its declaring Blueprint."),
                *DelegateProperty->GetName());
            return false;
        }
        if (DelegateProperty->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPrivate))
        {
            const bool bAllowsBlueprintAccess = DelegateProperty->GetBoolMetaData(TEXT("AllowPrivateAccess"));
            if (!bContextDerivesFromOwner || !bAllowsBlueprintAccess)
            {
                OutError = FString::Printf(
                    TEXT("Delegate '%s' is a private native property and is not accessible from Blueprint '%s'."),
                    *DelegateProperty->GetName(),
                    Blueprint != nullptr ? *Blueprint->GetPathName() : TEXT("<unknown>"));
                return false;
            }
        }
        else if (DelegateProperty->HasAnyPropertyFlags(CPF_NativeAccessSpecifierProtected)
            && !bContextDerivesFromOwner)
        {
            OutError = FString::Printf(
                TEXT("Delegate '%s' is protected and the target Blueprint does not derive from '%s'."),
                *DelegateProperty->GetName(),
                *OwnerClass->GetPathName());
            return false;
        }
        return true;
    }
}

FAddBlueprintDelegateBroadcastNodeTool::FAddBlueprintDelegateBroadcastNodeTool()
    : FMCPToolBase(
        TEXT("AddBlueprintDelegateBroadcastNode"),
        TEXT("Adds a Call Delegate node for an exact Blueprint-callable multicast delegate property."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintDelegateBroadcastNodeTool::Execute(
    const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
    FString OwnerClassPath;
    FString DelegateName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("ownerClassPath"), OwnerClassPath)
        || !Request.Params->TryGetStringField(TEXT("delegateName"), DelegateName))
    {
        return BuildError(
            Request,
            EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintDelegateBroadcastNode requires objectPath, ownerClassPath, delegateName, and a graph selector."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool bAlreadyExists = false;
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
        if (OwnerClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            OutError = FString::Printf(TEXT("Delegate owner class '%s' is deprecated or superseded."), *OwnerClass->GetPathName());
            return false;
        }

        FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(OwnerClass, *DelegateName);
        if (DelegateProperty == nullptr)
        {
            OutError = FString::Printf(
                TEXT("Multicast delegate property '%s' was not found on '%s'."),
                *DelegateName,
                *OwnerClass->GetPathName());
            return false;
        }
        if (!DelegateProperty->GetName().Equals(DelegateName, ESearchCase::CaseSensitive))
        {
            OutError = FString::Printf(
                TEXT("Delegate property names are case-sensitive; resolved '%s' instead of exact name '%s'."),
                *DelegateProperty->GetName(),
                *DelegateName);
            return false;
        }
        if (DelegateProperty->GetOwnerClass() != OwnerClass)
        {
            OutError = FString::Printf(
                TEXT("Delegate '%s' is declared by '%s', not the exact owner class '%s'."),
                *DelegateName,
                DelegateProperty->GetOwnerClass() != nullptr
                    ? *DelegateProperty->GetOwnerClass()->GetPathName()
                    : TEXT("<unknown>"),
                *OwnerClass->GetPathName());
            return false;
        }
        if (!ValidateBroadcastAccess(Blueprint, OwnerClass, DelegateProperty, OutError))
        {
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

        double RequestedX = 0.0;
        double RequestedY = 0.0;
        const bool bHasExactPlacement = Request.Params->TryGetNumberField(TEXT("positionX"), RequestedX)
            && Request.Params->TryGetNumberField(TEXT("positionY"), RequestedY);
        if (bHasExactPlacement)
        {
            UK2Node_CallDelegate* ExistingNode = FindEquivalentUnconnectedNodeAtPosition(
                Graph,
                DelegateProperty,
                FMath::RoundToInt(RequestedX),
                FMath::RoundToInt(RequestedY));
            if (ExistingNode != nullptr)
            {
                bAlreadyExists = true;
                Placement.X = ExistingNode->NodePosX;
                Placement.Y = ExistingNode->NodePosY;
                NodeGuid = BlueprintGraphEditToolUtils::GetNodeGuid(ExistingNode);
                Pins = BlueprintGraphEditToolUtils::SerializePins(ExistingNode);
                return true;
            }
        }

        Placement = BlueprintGraphEditToolUtils::ResolvePlacement(Graph, Request.Params, OutError);
        if (!OutError.IsEmpty() || bDryRun)
        {
            return OutError.IsEmpty();
        }

        UK2Node_CallDelegate* Node = NewObject<UK2Node_CallDelegate>(Graph);
        const bool bSelfContext = Blueprint->GeneratedClass != nullptr
            && Blueprint->GeneratedClass->IsChildOf(OwnerClass);
        Node->SetFromProperty(DelegateProperty, bSelfContext, OwnerClass);
        if (!Node->IsCompatibleWithGraph(Graph))
        {
            OutError = FString::Printf(
                TEXT("Call Delegate node for '%s' is not compatible with graph '%s'."),
                *DelegateName,
                *Graph->GetName());
            return false;
        }

        const FScopedTransaction Transaction(
            NSLOCTEXT("UnrealMCP", "AddBlueprintDelegateBroadcastNode", "UnrealMCP Add Blueprint Delegate Broadcast Node"));
        Blueprint->Modify();
        Graph->Modify();
        BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, Placement);
        if (Node->GetDelegateSignature() == nullptr)
        {
            Graph->RemoveNode(Node);
            OutError = TEXT("Unreal failed to resolve the delegate signature while creating the broadcast node.");
            return false;
        }

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
    Result->SetStringField(TEXT("signatureFunctionPath"), SignatureFunctionPath);
    Result->SetArrayField(TEXT("signatureParameters"), SignatureParameters);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyExists"), bAlreadyExists);
    Result->SetBoolField(TEXT("added"), !bDryRun && !bAlreadyExists);
    Result->SetNumberField(TEXT("positionX"), Placement.X);
    Result->SetNumberField(TEXT("positionY"), Placement.Y);
    Result->SetBoolField(TEXT("collisionAdjusted"), Placement.bCollisionAdjusted);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bAlreadyExists);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintDelegateBroadcastNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact graph GUID.")));
    Properties->SetObjectField(TEXT("ownerClassPath"), BuildStringProperty(TEXT("Exact class that declares the BlueprintCallable multicast delegate.")));
    Properties->SetObjectField(TEXT("delegateName"), BuildStringProperty(TEXT("Exact multicast delegate property name.")));
    Properties->SetObjectField(TEXT("relativeToNodeGuid"), BuildStringProperty(TEXT("Optional placement anchor.")));
    TSharedRef<FJsonObject> IntegerProperty = MakeShared<FJsonObject>();
    IntegerProperty->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("positionX"), IntegerProperty);
    Properties->SetObjectField(TEXT("positionY"), IntegerProperty);
    Properties->SetObjectField(TEXT("horizontalSpacing"), IntegerProperty);
    Properties->SetObjectField(TEXT("verticalOffset"), IntegerProperty);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate access, signature, graph compatibility, and placement without editing.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save the Blueprint and partially refresh its index entry.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("ownerClassPath")),
        MakeShared<FJsonValueString>(TEXT("delegateName"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
