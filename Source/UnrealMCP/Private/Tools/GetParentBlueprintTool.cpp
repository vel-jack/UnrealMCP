#include "Tools/GetParentBlueprintTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Dom/JsonObject.h"
#include "Tools/BlueprintToolUtils.h"

FGetParentBlueprintTool::FGetParentBlueprintTool()
    : FMCPToolBase(TEXT("GetParentBlueprint"), TEXT("Returns parent class information and resolves the parent Blueprint asset when applicable."))
{
}

UnrealMCP::FMCPResponse FGetParentBlueprintTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetParentBlueprint requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetParentBlueprint requires a non-empty params.objectPath."));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    UBlueprint* Blueprint = nullptr;
    FAssetData AssetData;
    if (!UnrealMCP::BlueprintToolUtils::ResolveBlueprintAssetData(AssetRegistryModule.Get(), ObjectPath, Blueprint, AssetData))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetParentBlueprint could not load a Blueprint from params.objectPath."));
    }

    const FString ParentClassTag = AssetData.GetTagValueRef<FString>(FBlueprintTags::ParentClassPath);
    const FString ParentClassObjectPath = ParentClassTag.IsEmpty() ? FString() : FPackageName::ExportTextPathToObjectPath(ParentClassTag);

    TSharedPtr<FJsonObject> ParentBlueprintObject;
    if (!ParentClassTag.IsEmpty())
    {
        TArray<FAssetData> AllAssets;
        AssetRegistryModule.Get().GetAllAssets(AllAssets, true);
        for (const FAssetData& Candidate : AllAssets)
        {
            if (Candidate.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath) == ParentClassTag)
            {
                ParentBlueprintObject = UnrealMCP::BlueprintToolUtils::SerializeBlueprintAssetReference(Candidate);
                break;
            }
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("parentClassPath"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
    Result->SetStringField(TEXT("parentClassTag"), ParentClassTag);
    Result->SetStringField(TEXT("parentClassObjectPath"), ParentClassObjectPath);
    Result->SetBoolField(TEXT("isNativeParent"), ParentBlueprintObject == nullptr);
    if (ParentBlueprintObject.IsValid())
    {
        Result->SetObjectField(TEXT("parentBlueprint"), ParentBlueprintObject.ToSharedRef());
    }
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetParentBlueprintTool::BuildInputSchema() const
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
