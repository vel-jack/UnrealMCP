#include "Tools/FindAssetsByPathTool.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Tools/AssetRegistryToolUtils.h"

FFindAssetsByPathTool::FFindAssetsByPathTool()
    : FMCPToolBase(TEXT("FindAssetsByPath"), TEXT("Finds assets under a package path, optionally filtered by class."))
{
}

UnrealMCP::FMCPResponse FFindAssetsByPathTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindAssetsByPath requires params.packagePath."));
    }

    FString PackagePath;
    if (!Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath) || PackagePath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindAssetsByPath requires a non-empty params.packagePath."));
    }

    const bool bRecursive = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("recursive"), true);
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

TSharedPtr<FJsonObject> FFindAssetsByPathTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> PackagePathProperty = MakeShared<FJsonObject>();
    PackagePathProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackagePathProperty->SetStringField(TEXT("description"), TEXT("Package path to search under, for example /Game/MyFolder."));
    Properties->SetObjectField(TEXT("packagePath"), PackagePathProperty);

    TSharedRef<FJsonObject> RecursiveProperty = MakeShared<FJsonObject>();
    RecursiveProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    RecursiveProperty->SetStringField(TEXT("description"), TEXT("Whether to recurse subfolders. Defaults to true."));
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

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("packagePath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
