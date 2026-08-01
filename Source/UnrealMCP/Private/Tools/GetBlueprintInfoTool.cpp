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

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    UBlueprint* Blueprint = nullptr;
    FAssetData AssetData;
    if (!UnrealMCP::BlueprintToolUtils::ResolveBlueprintAssetData(AssetRegistryModule.Get(), ObjectPath, Blueprint, AssetData))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintInfo could not load a Blueprint from params.objectPath."));
    }

    const TArray<USCS_Node*>& ComponentNodes = Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->GetAllNodes() : TArray<USCS_Node*>();

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetObjectField(TEXT("asset"), UnrealMCP::BlueprintToolUtils::SerializeBlueprintAssetReference(AssetData));
    Result->SetStringField(TEXT("parentClassPath"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
    Result->SetStringField(TEXT("generatedClassPath"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());
    Result->SetStringField(TEXT("skeletonClassPath"), Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass->GetPathName() : FString());
    Result->SetBoolField(TEXT("hasSimpleConstructionScript"), Blueprint->SimpleConstructionScript != nullptr);
    Result->SetNumberField(TEXT("variableCount"), Blueprint->NewVariables.Num());
    Result->SetNumberField(TEXT("functionGraphCount"), Blueprint->FunctionGraphs.Num());
    Result->SetNumberField(TEXT("componentCount"), ComponentNodes.Num());
    Result->SetNumberField(TEXT("implementedInterfaceCount"), Blueprint->ImplementedInterfaces.Num());
    Result->SetStringField(TEXT("isDataOnly"), AssetData.GetTagValueRef<FString>(FBlueprintTags::IsDataOnly));
    Result->SetStringField(TEXT("blueprintType"), AssetData.GetTagValueRef<FString>(FBlueprintTags::BlueprintType));
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
