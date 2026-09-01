#include "Tools/GetBlueprintInfoTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Dom/JsonObject.h"
#include "Tools/BlueprintToolUtils.h"

FGetBlueprintInfoTool::FGetBlueprintInfoTool()
    : FMCPToolBase(TEXT("GetBlueprintInfo"), TEXT("Returns summary information about a Blueprint asset."))
{
}

UnrealMCP::FMCPResponse FGetBlueprintInfoTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintInfo requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintInfo requires a non-empty params.objectPath."));
    }

    FString ParentClassPath, GeneratedClassPath, SkeletonClassPath, IsDataOnly, BlueprintType;
    bool bHasSimpleConstructionScript = false;
    int32 VariableCount = 0, FunctionGraphCount = 0, ComponentCount = 0, ImplementedInterfaceCount = 0;
    TSharedPtr<FJsonObject> AssetJson;
    FString ExecutionError;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            UBlueprint* Blueprint = nullptr;
            FAssetData AssetData;
            if (!UnrealMCP::BlueprintToolUtils::ResolveBlueprintAssetData(AssetRegistryModule.Get(), ObjectPath, Blueprint, AssetData))
            {
                OutError = TEXT("GetBlueprintInfo could not load a Blueprint from params.objectPath.");
                return false;
            }

            const TArray<USCS_Node*>& ComponentNodes = Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->GetAllNodes() : TArray<USCS_Node*>();

            AssetJson = UnrealMCP::BlueprintToolUtils::SerializeBlueprintAssetReference(AssetData);
            ParentClassPath = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString();
            GeneratedClassPath = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString();
            SkeletonClassPath = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass->GetPathName() : FString();
            bHasSimpleConstructionScript = Blueprint->SimpleConstructionScript != nullptr;
            VariableCount = Blueprint->NewVariables.Num();
            FunctionGraphCount = Blueprint->FunctionGraphs.Num();
            ComponentCount = ComponentNodes.Num();
            ImplementedInterfaceCount = Blueprint->ImplementedInterfaces.Num();
            IsDataOnly = AssetData.GetTagValueRef<FString>(FBlueprintTags::IsDataOnly);
            BlueprintType = AssetData.GetTagValueRef<FString>(FBlueprintTags::BlueprintType);
            return true;
        },
        ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError.IsEmpty() ? TEXT("GetBlueprintInfo could not load a Blueprint from params.objectPath.") : ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetObjectField(TEXT("asset"), AssetJson.ToSharedRef());
    Result->SetStringField(TEXT("parentClassPath"), ParentClassPath);
    Result->SetStringField(TEXT("generatedClassPath"), GeneratedClassPath);
    Result->SetStringField(TEXT("skeletonClassPath"), SkeletonClassPath);
    Result->SetBoolField(TEXT("hasSimpleConstructionScript"), bHasSimpleConstructionScript);
    Result->SetNumberField(TEXT("variableCount"), VariableCount);
    Result->SetNumberField(TEXT("functionGraphCount"), FunctionGraphCount);
    Result->SetNumberField(TEXT("componentCount"), ComponentCount);
    Result->SetNumberField(TEXT("implementedInterfaceCount"), ImplementedInterfaceCount);
    Result->SetStringField(TEXT("isDataOnly"), IsDataOnly);
    Result->SetStringField(TEXT("blueprintType"), BlueprintType);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetBlueprintInfoTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path, for example /Game/BP_MyActor.BP_MyActor"));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
