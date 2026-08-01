#include "Tools/FindAssetsByClassTool.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Tools/AssetRegistryToolUtils.h"

FFindAssetsByClassTool::FFindAssetsByClassTool()
    : FMCPToolBase(TEXT("FindAssetsByClass"), TEXT("Finds assets by Asset Registry class path, optionally scoped to a package path."))
{
}

UnrealMCP::FMCPResponse FFindAssetsByClassTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindAssetsByClass requires params.classPath."));
    }

    FString ClassPath;
    if (!Request.Params->TryGetStringField(TEXT("classPath"), ClassPath) || ClassPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindAssetsByClass requires a non-empty params.classPath."));
    }

    const FString PackagePath = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("packagePath"));
    const bool bRecursivePaths = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("recursivePaths"), true);
    const bool bIncludeDerivedClasses = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("includeDerivedClasses"), true);
    const int32 Limit = UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params);

    FARFilter Filter;
    Filter.ClassPaths.Add(FTopLevelAssetPath(ClassPath));
    Filter.bRecursiveClasses = bIncludeDerivedClasses;
    Filter.bIncludeOnlyOnDiskAssets = true;
    if (!PackagePath.IsEmpty())
    {
        Filter.PackagePaths.Add(FName(*PackagePath));
        Filter.bRecursivePaths = bRecursivePaths;
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
    Result->SetStringField(TEXT("classPath"), ClassPath);
    Result->SetStringField(TEXT("packagePath"), PackagePath);
    Result->SetBoolField(TEXT("recursivePaths"), bRecursivePaths);
    Result->SetBoolField(TEXT("includeDerivedClasses"), bIncludeDerivedClasses);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetNumberField(TEXT("count"), Assets.Num());
    Result->SetArrayField(TEXT("assets"), UnrealMCP::AssetRegistryToolUtils::SerializeAssetArray(Assets));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindAssetsByClassTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ClassPathProperty = MakeShared<FJsonObject>();
    ClassPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ClassPathProperty->SetStringField(TEXT("description"), TEXT("Class path to search for, for example /Script/Engine.Blueprint."));
    Properties->SetObjectField(TEXT("classPath"), ClassPathProperty);

    TSharedRef<FJsonObject> PackagePathProperty = MakeShared<FJsonObject>();
    PackagePathProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackagePathProperty->SetStringField(TEXT("description"), TEXT("Optional package path scope, for example /Game."));
    Properties->SetObjectField(TEXT("packagePath"), PackagePathProperty);

    TSharedRef<FJsonObject> RecursivePathsProperty = MakeShared<FJsonObject>();
    RecursivePathsProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    RecursivePathsProperty->SetStringField(TEXT("description"), TEXT("Whether to recurse package subpaths when packagePath is supplied. Defaults to true."));
    Properties->SetObjectField(TEXT("recursivePaths"), RecursivePathsProperty);

    TSharedRef<FJsonObject> IncludeDerivedProperty = MakeShared<FJsonObject>();
    IncludeDerivedProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    IncludeDerivedProperty->SetStringField(TEXT("description"), TEXT("Whether to include derived classes. Defaults to true."));
    Properties->SetObjectField(TEXT("includeDerivedClasses"), IncludeDerivedProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional max result count. Defaults to 200."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("classPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
