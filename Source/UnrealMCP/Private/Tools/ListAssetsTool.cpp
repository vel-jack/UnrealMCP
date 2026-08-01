#include "Tools/ListAssetsTool.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Tools/AssetRegistryToolUtils.h"

FListAssetsTool::FListAssetsTool()
    : FMCPToolBase(TEXT("ListAssets"), TEXT("Lists assets under a package path without loading them."))
{
}

UnrealMCP::FMCPResponse FListAssetsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString PackagePath = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("packagePath"), TEXT("/Game"));
    const bool bRecursive = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("recursive"), false);
    const FString ClassPath = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("classPath"));
    const int32 Limit = UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params);

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*PackagePath));
    Filter.bRecursivePaths = bRecursive;
    Filter.bIncludeOnlyOnDiskAssets = true;
    if (!ClassPath.IsEmpty())
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(ClassPath));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets;
    AssetRegistryModule.Get().GetAssets(Filter, Assets);

    UnrealMCP::AssetRegistryToolUtils::SortAssets(Assets);
    if (Assets.Num() > Limit)
    {
        Assets.SetNum(Limit);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("packagePath"), PackagePath);
    Result->SetBoolField(TEXT("recursive"), bRecursive);
    Result->SetStringField(TEXT("classPath"), ClassPath);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetNumberField(TEXT("count"), Assets.Num());
    Result->SetArrayField(TEXT("assets"), UnrealMCP::AssetRegistryToolUtils::SerializeAssetArray(Assets));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FListAssetsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> PackagePathProperty = MakeShared<FJsonObject>();
    PackagePathProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackagePathProperty->SetStringField(TEXT("description"), TEXT("Package path to list, for example /Game or /Game/MyFolder. Defaults to /Game."));
    Properties->SetObjectField(TEXT("packagePath"), PackagePathProperty);

    TSharedRef<FJsonObject> RecursiveProperty = MakeShared<FJsonObject>();
    RecursiveProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    RecursiveProperty->SetStringField(TEXT("description"), TEXT("Whether to recurse into subfolders. Defaults to false."));
    Properties->SetObjectField(TEXT("recursive"), RecursiveProperty);

    TSharedRef<FJsonObject> ClassPathProperty = MakeShared<FJsonObject>();
    ClassPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ClassPathProperty->SetStringField(TEXT("description"), TEXT("Optional class path filter, for example /Script/Engine.Blueprint."));
    Properties->SetObjectField(TEXT("classPath"), ClassPathProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional max result count. Defaults to 200."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
