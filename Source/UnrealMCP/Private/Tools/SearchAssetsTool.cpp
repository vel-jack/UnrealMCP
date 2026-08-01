#include "Tools/SearchAssetsTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"

FSearchAssetsTool::FSearchAssetsTool()
    : FMCPToolBase(TEXT("SearchAssets"), TEXT("Searches the Asset Registry without loading matching assets."))
{
}

UnrealMCP::FMCPResponse FSearchAssetsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SearchAssets requires params.query."));
    }

    FString Query;
    if (!Request.Params->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SearchAssets requires a non-empty params.query."));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    TArray<FAssetData> AllAssets;
    AssetRegistryModule.Get().GetAllAssets(AllAssets, true);

    TArray<TSharedPtr<FJsonValue>> Assets;
    const FString LowerQuery = Query.ToLower();

    for (const FAssetData& Asset : AllAssets)
    {
        const FString AssetName = Asset.AssetName.ToString();
        const FString ObjectPath = Asset.GetObjectPathString();
        const FString PackagePath = Asset.PackagePath.ToString();

        if (AssetName.ToLower().Contains(LowerQuery) || ObjectPath.ToLower().Contains(LowerQuery) || PackagePath.ToLower().Contains(LowerQuery))
        {
            TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
            AssetObject->SetStringField(TEXT("name"), AssetName);
            AssetObject->SetStringField(TEXT("classPath"), Asset.AssetClassPath.ToString());
            AssetObject->SetStringField(TEXT("objectPath"), ObjectPath);
            AssetObject->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
            AssetObject->SetStringField(TEXT("packagePath"), PackagePath);
            Assets.Add(MakeShared<FJsonValueObject>(AssetObject));
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("query"), Query);
    Result->SetArrayField(TEXT("assets"), Assets);
    Result->SetNumberField(TEXT("count"), Assets.Num());
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSearchAssetsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> QueryProperty = MakeShared<FJsonObject>();
    QueryProperty->SetStringField(TEXT("type"), TEXT("string"));
    QueryProperty->SetStringField(TEXT("description"), TEXT("Case-insensitive asset name or path fragment."));
    Properties->SetObjectField(TEXT("query"), QueryProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("query")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
