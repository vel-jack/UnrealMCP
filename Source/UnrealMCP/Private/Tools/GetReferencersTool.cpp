#include "Tools/GetReferencersTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Tools/AssetRegistryToolUtils.h"

FGetReferencersTool::FGetReferencersTool()
    : FMCPToolBase(TEXT("GetReferencers"), TEXT("Returns Asset Registry package referencers for one asset or package."))
{
}

UnrealMCP::FMCPResponse FGetReferencersTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    FString ObjectPath;
    FName PackageName;
    if (!UnrealMCP::AssetRegistryToolUtils::ResolvePackageName(Request.Params, AssetRegistryModule.Get(), ObjectPath, PackageName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetReferencers requires params.objectPath or params.packageName, and the asset must exist when objectPath is used."));
    }

    TArray<FName> Referencers;
    AssetRegistryModule.Get().GetReferencers(PackageName, Referencers);
    UnrealMCP::AssetRegistryToolUtils::SortNames(Referencers);

    const int32 Limit = UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params);
    if (Referencers.Num() > Limit)
    {
        Referencers.SetNum(Limit);
    }

    TArray<TSharedPtr<FJsonValue>> ReferencerValues;
    ReferencerValues.Reserve(Referencers.Num());
    for (const FName& ReferencerPackage : Referencers)
    {
        ReferencerValues.Add(MakeShared<FJsonValueObject>(UnrealMCP::AssetRegistryToolUtils::SerializePackageReference(AssetRegistryModule.Get(), ReferencerPackage)));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("packageName"), PackageName.ToString());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetNumberField(TEXT("count"), Referencers.Num());
    Result->SetArrayField(TEXT("referencers"), ReferencerValues);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetReferencersTool::BuildInputSchema() const
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
