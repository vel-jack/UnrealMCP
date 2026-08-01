#include "Tools/GetAssetReferencersTool.h"

#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

FGetAssetReferencersTool::FGetAssetReferencersTool()
    : FMCPToolBase(TEXT("GetAssetReferencers"), TEXT("Returns indexed package referencers for one asset or package.")) 
{
}

UnrealMCP::FMCPResponse FGetAssetReferencersTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FName PackageName;
    if (!UnrealMCP::IndexedQueryToolUtils::ResolvePackageName(Request.Params, ObjectPath, PackageName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetAssetReferencers requires params.objectPath or params.packageName, and the asset must exist when objectPath is used."));
    }

    const int32 Limit = UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params);
    TArray<TSharedPtr<FJsonValue>> Referencers;
    int32 Count = 0;
    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            FSQLitePreparedStatement Statement(
                Database,
                TEXT("SELECT source_package_name FROM asset_dependencies WHERE target_package_name = ?1 ORDER BY source_package_name ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, PackageName.ToString()))
            {
                OutExecError = TEXT("GetAssetReferencers could not prepare the project index query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&Referencers, &Count, Limit](const FSQLitePreparedStatement& Row)
            {
                FString SourcePackageName;
                if (!Row.GetColumnValueByIndex(0, SourcePackageName))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                if (Count < Limit)
                {
                    Referencers.Add(MakeShared<FJsonValueObject>(UnrealMCP::IndexedQueryToolUtils::SerializePackageSummary(SourcePackageName)));
                }

                ++Count;
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });

            if (QueryResult == INDEX_NONE)
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                return false;
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("GetAssetReferencers failed to query the project index: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("packageName"), PackageName.ToString());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetNumberField(TEXT("count"), Count);
    Result->SetArrayField(TEXT("referencers"), Referencers);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetAssetReferencersTool::BuildInputSchema() const
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
