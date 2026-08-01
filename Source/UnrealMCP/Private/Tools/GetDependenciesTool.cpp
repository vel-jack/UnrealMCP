#include "Tools/GetDependenciesTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Tools/AssetRegistryToolUtils.h"

FGetDependenciesTool::FGetDependenciesTool()
    : FMCPToolBase(TEXT("GetDependencies"), TEXT("Returns Asset Registry package dependencies for one asset or package."))
{
}

UnrealMCP::FMCPResponse FGetDependenciesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    FString ObjectPath;
    FName PackageName;
    if (!UnrealMCP::AssetRegistryToolUtils::ResolvePackageName(Request.Params, AssetRegistryModule.Get(), ObjectPath, PackageName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetDependencies requires params.objectPath or params.packageName, and the asset must exist when objectPath is used."));
    }

    TArray<FName> Dependencies;
    AssetRegistryModule.Get().GetDependencies(PackageName, Dependencies);
    UnrealMCP::AssetRegistryToolUtils::SortNames(Dependencies);

    const int32 Limit = UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params);
    if (Dependencies.Num() > Limit)
    {
        Dependencies.SetNum(Limit);
    }

    TArray<TSharedPtr<FJsonValue>> DependencyValues;
    DependencyValues.Reserve(Dependencies.Num());
    for (const FName& DependencyPackage : Dependencies)
    {
        DependencyValues.Add(MakeShared<FJsonValueObject>(UnrealMCP::AssetRegistryToolUtils::SerializePackageReference(AssetRegistryModule.Get(), DependencyPackage)));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("packageName"), PackageName.ToString());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetNumberField(TEXT("count"), Dependencies.Num());
    Result->SetArrayField(TEXT("dependencies"), DependencyValues);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetDependenciesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Optional asset object path to resolve into a package, for example /Game/MyFolder/MyAsset.MyAsset."));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> PackageNameProperty = MakeShared<FJsonObject>();
    PackageNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackageNameProperty->SetStringField(TEXT("description"), TEXT("Optional package name when objectPath is not provided, for example /Game/MyFolder/MyAsset."));
    Properties->SetObjectField(TEXT("packageName"), PackageNameProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional max result count. Defaults to 200."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
