#pragma once

#include "Algo/Sort.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"

namespace UnrealMCP::AssetRegistryToolUtils
{
    inline int32 GetOptionalLimit(const TSharedPtr<FJsonObject>& Params, int32 DefaultLimit = 200)
    {
        if (!Params.IsValid())
        {
            return DefaultLimit;
        }

        int32 Limit = DefaultLimit;
        if (Params->TryGetNumberField(TEXT("limit"), Limit) && Limit > 0)
        {
            return Limit;
        }

        return DefaultLimit;
    }

    inline bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue)
    {
        if (!Params.IsValid())
        {
            return DefaultValue;
        }

        bool Value = DefaultValue;
        return Params->TryGetBoolField(FieldName, Value) ? Value : DefaultValue;
    }

    inline FString GetOptionalString(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FString& DefaultValue = FString())
    {
        if (!Params.IsValid())
        {
            return DefaultValue;
        }

        FString Value;
        return Params->TryGetStringField(FieldName, Value) && !Value.IsEmpty() ? Value : DefaultValue;
    }

    inline TSharedRef<FJsonObject> SerializeAssetData(const FAssetData& Asset, bool bIncludeTags = false)
    {
        TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
        AssetObject->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        AssetObject->SetStringField(TEXT("classPath"), Asset.AssetClassPath.ToString());
        AssetObject->SetStringField(TEXT("className"), Asset.AssetClassPath.GetAssetName().ToString());
        AssetObject->SetStringField(TEXT("objectPath"), Asset.GetObjectPathString());
        AssetObject->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
        AssetObject->SetStringField(TEXT("packagePath"), Asset.PackagePath.ToString());
        AssetObject->SetBoolField(TEXT("isRedirector"), Asset.IsRedirector());
        AssetObject->SetNumberField(TEXT("packageFlags"), static_cast<double>(Asset.PackageFlags));

        TArray<TSharedPtr<FJsonValue>> ChunkIds;
        for (const int32 ChunkId : Asset.GetChunkIDs())
        {
            ChunkIds.Add(MakeShared<FJsonValueNumber>(ChunkId));
        }
        AssetObject->SetArrayField(TEXT("chunkIds"), ChunkIds);

        if (bIncludeTags)
        {
            TSharedRef<FJsonObject> TagsObject = MakeShared<FJsonObject>();
            Asset.EnumerateTags([&TagsObject](const TPair<FName, FAssetTagValueRef>& TagValue)
            {
                TagsObject->SetStringField(TagValue.Key.ToString(), TagValue.Value.AsString());
            });
            AssetObject->SetObjectField(TEXT("tags"), TagsObject);
        }

        return AssetObject;
    }

    inline TArray<TSharedPtr<FJsonValue>> SerializeAssetArray(const TArray<FAssetData>& Assets, bool bIncludeTags = false)
    {
        TArray<TSharedPtr<FJsonValue>> AssetValues;
        AssetValues.Reserve(Assets.Num());

        for (const FAssetData& Asset : Assets)
        {
            AssetValues.Add(MakeShared<FJsonValueObject>(SerializeAssetData(Asset, bIncludeTags)));
        }

        return AssetValues;
    }

    inline void SortAssets(TArray<FAssetData>& Assets)
    {
        Algo::SortBy(Assets, [](const FAssetData& Asset)
        {
            return Asset.GetObjectPathString();
        });
    }

    inline void SortNames(TArray<FName>& Names)
    {
        Algo::SortBy(Names, [](const FName& Name)
        {
            return Name.ToString();
        });
    }

    inline TSharedRef<FJsonObject> SerializePackageReference(IAssetRegistry& AssetRegistry, FName PackageName)
    {
        TArray<FAssetData> PackageAssets;
        AssetRegistry.GetAssetsByPackageName(PackageName, PackageAssets, true);
        SortAssets(PackageAssets);

        TSharedRef<FJsonObject> PackageObject = MakeShared<FJsonObject>();
        PackageObject->SetStringField(TEXT("packageName"), PackageName.ToString());
        PackageObject->SetNumberField(TEXT("assetCount"), PackageAssets.Num());
        PackageObject->SetArrayField(TEXT("assets"), SerializeAssetArray(PackageAssets));
        return PackageObject;
    }

    inline bool ResolvePackageName(const TSharedPtr<FJsonObject>& Params, IAssetRegistry& AssetRegistry, FString& OutObjectPath, FName& OutPackageName)
    {
        OutObjectPath = GetOptionalString(Params, TEXT("objectPath"));
        if (!OutObjectPath.IsEmpty())
        {
            const FAssetData Asset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(OutObjectPath));
            if (!Asset.IsValid())
            {
                return false;
            }

            OutPackageName = Asset.PackageName;
            return true;
        }

        const FString PackageNameString = GetOptionalString(Params, TEXT("packageName"));
        if (!PackageNameString.IsEmpty())
        {
            OutPackageName = FName(*PackageNameString);
            return true;
        }

        return false;
    }
}
