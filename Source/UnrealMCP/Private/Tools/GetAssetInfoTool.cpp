#include "Tools/GetAssetInfoTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Tools/AssetRegistryToolUtils.h"

FGetAssetInfoTool::FGetAssetInfoTool()
    : FMCPToolBase(TEXT("GetAssetInfo"), TEXT("Returns detailed Asset Registry information for one asset without loading it."))
{
}

UnrealMCP::FMCPResponse FGetAssetInfoTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetAssetInfo requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetAssetInfo requires a non-empty params.objectPath."));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    const FAssetData Asset = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetBoolField(TEXT("exists"), Asset.IsValid());
    if (Asset.IsValid())
    {
        Result->SetObjectField(TEXT("asset"), UnrealMCP::AssetRegistryToolUtils::SerializeAssetData(Asset, true));
    }

    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetAssetInfoTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Asset object path, for example /Game/MyFolder/MyAsset.MyAsset"));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
