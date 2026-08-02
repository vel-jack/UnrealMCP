#include "Tools/FindFeatureEntryPointsTool.h"

#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    struct FCandidateRow
    {
        FString ObjectPath;
        FString AssetName;
        FString ClassPath;
        FString PackageName;
        FString PackagePath;
        FString ContentScope;
        bool bIsBlueprint = false;
        int32 DependencyCount = 0;
        int32 ReferencerCount = 0;
        int32 ComponentCount = 0;
        int32 FunctionCount = 0;
        bool bContextDependsOn = false;
        bool bContextReferencedBy = false;
        int32 Score = 0;
        TArray<FString> Reasons;
    };

    void AddReason(TArray<FString>& Reasons, const FString& Reason)
    {
        if (!Reason.IsEmpty())
        {
            Reasons.AddUnique(Reason);
        }
    }

    int32 ScoreCandidate(const FCandidateRow& Candidate, const FString& Query, const FString& ContextPackagePath, TArray<FString>& OutReasons)
    {
        const FString LowerQuery = Query.ToLower();
        const FString LowerName = Candidate.AssetName.ToLower();
        const FString LowerObjectPath = Candidate.ObjectPath.ToLower();
        const FString LowerPackagePath = Candidate.PackagePath.ToLower();
        int32 Score = 0;

        if (LowerName == LowerQuery)
        {
            Score += 120;
            AddReason(OutReasons, TEXT("exact_asset_name_match"));
        }
        else if (LowerName.StartsWith(LowerQuery))
        {
            Score += 85;
            AddReason(OutReasons, TEXT("asset_name_prefix_match"));
        }
        else if (LowerName.Contains(LowerQuery))
        {
            Score += 60;
            AddReason(OutReasons, TEXT("asset_name_contains_query"));
        }

        if (LowerObjectPath.Contains(LowerQuery))
        {
            Score += 35;
            AddReason(OutReasons, TEXT("object_path_contains_query"));
        }

        if (LowerPackagePath.Contains(LowerQuery))
        {
            Score += 20;
            AddReason(OutReasons, TEXT("folder_path_contains_query"));
        }

        if (Candidate.bIsBlueprint)
        {
            Score += 20;
            AddReason(OutReasons, TEXT("blueprint_asset"));
        }

        if (Candidate.ReferencerCount > 0)
        {
            Score += FMath::Min(Candidate.ReferencerCount * 4, 28);
            AddReason(OutReasons, TEXT("used_by_other_assets"));
        }

        if (Candidate.FunctionCount > 0)
        {
            Score += FMath::Min(Candidate.FunctionCount * 2, 12);
            AddReason(OutReasons, TEXT("has_indexed_functions"));
        }

        if (Candidate.ComponentCount > 0)
        {
            Score += FMath::Min(Candidate.ComponentCount * 2, 12);
            AddReason(OutReasons, TEXT("has_components"));
        }

        if (Candidate.DependencyCount > 0)
        {
            Score += FMath::Min(Candidate.DependencyCount, 8);
            AddReason(OutReasons, TEXT("depends_on_other_assets"));
        }

        if (Candidate.bContextDependsOn || Candidate.bContextReferencedBy)
        {
            Score += 45;
            AddReason(OutReasons, TEXT("directly_linked_to_context_asset"));
        }

        if (!ContextPackagePath.IsEmpty() && Candidate.PackagePath.Equals(ContextPackagePath, ESearchCase::IgnoreCase))
        {
            Score += 18;
            AddReason(OutReasons, TEXT("same_folder_as_context_asset"));
        }

        return Score;
    }
}

FFindFeatureEntryPointsTool::FFindFeatureEntryPointsTool()
    : FMCPToolBase(TEXT("FindFeatureEntryPoints"), TEXT("Finds likely starting assets for a feature by ranking indexed assets that match a query, optionally boosted by a related context asset."))
{
}

UnrealMCP::FMCPResponse FFindFeatureEntryPointsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindFeatureEntryPoints requires params.query."));
    }

    FString Query;
    if (!Request.Params->TryGetStringField(TEXT("query"), Query) || Query.TrimStartAndEnd().IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindFeatureEntryPoints requires a non-empty params.query."));
    }
    Query = Query.TrimStartAndEnd();

    const bool bBlueprintsOnly = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("blueprintsOnly"), true);
    const int32 Limit = FMath::Clamp(UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 10), 1, 50);
    const FString PackagePathPrefix = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("packagePath"));

    FString ContextObjectPath = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("contextObjectPath"));
    FString ContextPackageName = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("contextPackageName"));
    FString ContextResolvedObjectPath;
    FString ContextResolvedPackagePath;
    if (!ContextObjectPath.IsEmpty() || !ContextPackageName.IsEmpty())
    {
        TSharedRef<FJsonObject> ContextParams = MakeShared<FJsonObject>();
        if (!ContextObjectPath.IsEmpty())
        {
            ContextParams->SetStringField(TEXT("objectPath"), ContextObjectPath);
        }
        if (!ContextPackageName.IsEmpty())
        {
            ContextParams->SetStringField(TEXT("packageName"), ContextPackageName);
        }

        FName ResolvedContextPackageName;
        if (!UnrealMCP::IndexedQueryToolUtils::ResolvePackageName(ContextParams, ContextResolvedObjectPath, ResolvedContextPackageName))
        {
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindFeatureEntryPoints could not resolve the optional context asset from params.contextObjectPath or params.contextPackageName."));
        }

        ContextPackageName = ResolvedContextPackageName.ToString();
    }

    TArray<FCandidateRow> CandidateRows;
    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!ContextPackageName.IsEmpty())
            {
                TSharedPtr<FJsonObject> ContextAsset;
                FString ContextError;
                if (UnrealMCP::IndexedQueryToolUtils::QueryAssetByPackageName(Database, ContextPackageName, ContextAsset, ContextError) && ContextAsset.IsValid())
                {
                    ContextResolvedPackagePath = ContextAsset->GetStringField(TEXT("packagePath"));
                }
            }

            FSQLitePreparedStatement Statement(
                Database,
                TEXT("SELECT a.object_path, a.asset_name, a.class_path, a.package_name, a.package_path, a.content_scope, a.is_blueprint, "
                     "COALESCE((SELECT COUNT(*) FROM asset_dependencies d1 WHERE d1.source_package_name = a.package_name), 0), "
                     "COALESCE((SELECT COUNT(*) FROM asset_dependencies d2 WHERE d2.target_package_name = a.package_name), 0), "
                     "COALESCE((SELECT COUNT(*) FROM blueprint_components bc WHERE bc.blueprint_object_path = a.object_path), 0), "
                     "COALESCE((SELECT COUNT(*) FROM blueprint_functions bf WHERE bf.blueprint_object_path = a.object_path), 0), "
                     "COALESCE((SELECT COUNT(*) FROM asset_dependencies dc1 WHERE dc1.source_package_name = ?2 AND dc1.target_package_name = a.package_name), 0), "
                     "COALESCE((SELECT COUNT(*) FROM asset_dependencies dc2 WHERE dc2.target_package_name = ?2 AND dc2.source_package_name = a.package_name), 0) "
                     "FROM assets a "
                     "WHERE (LOWER(a.asset_name) LIKE ?1 OR LOWER(a.object_path) LIKE ?1 OR LOWER(a.package_path) LIKE ?1) "
                     "AND (?3 = '' OR a.package_path LIKE ?3 || '%') "
                     "AND (?4 = 0 OR a.is_blueprint = 1) "
                     "ORDER BY a.asset_name ASC, a.object_path ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!Statement.IsValid()
                || !Statement.SetBindingValueByIndex(1, FString::Printf(TEXT("%%%s%%"), *Query.ToLower()))
                || !Statement.SetBindingValueByIndex(2, ContextPackageName)
                || !Statement.SetBindingValueByIndex(3, PackagePathPrefix)
                || !Statement.SetBindingValueByIndex(4, bBlueprintsOnly ? 1 : 0))
            {
                OutExecError = TEXT("FindFeatureEntryPoints could not prepare the project index query.");
                return false;
            }

            if (Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FCandidateRow Candidate;
                    int32 bIsBlueprint = 0;
                    int32 ContextDependsOnCount = 0;
                    int32 ContextReferencedByCount = 0;

                    if (!Row.GetColumnValueByIndex(0, Candidate.ObjectPath)
                        || !Row.GetColumnValueByIndex(1, Candidate.AssetName)
                        || !Row.GetColumnValueByIndex(2, Candidate.ClassPath)
                        || !Row.GetColumnValueByIndex(3, Candidate.PackageName)
                        || !Row.GetColumnValueByIndex(4, Candidate.PackagePath)
                        || !Row.GetColumnValueByIndex(5, Candidate.ContentScope)
                        || !Row.GetColumnValueByIndex(6, bIsBlueprint)
                        || !Row.GetColumnValueByIndex(7, Candidate.DependencyCount)
                        || !Row.GetColumnValueByIndex(8, Candidate.ReferencerCount)
                        || !Row.GetColumnValueByIndex(9, Candidate.ComponentCount)
                        || !Row.GetColumnValueByIndex(10, Candidate.FunctionCount)
                        || !Row.GetColumnValueByIndex(11, ContextDependsOnCount)
                        || !Row.GetColumnValueByIndex(12, ContextReferencedByCount))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }

                    Candidate.bIsBlueprint = bIsBlueprint != 0;
                    Candidate.bContextDependsOn = ContextDependsOnCount > 0;
                    Candidate.bContextReferencedBy = ContextReferencedByCount > 0;
                    Candidate.Score = ScoreCandidate(Candidate, Query, ContextResolvedPackagePath, Candidate.Reasons);
                    CandidateRows.Add(MoveTemp(Candidate));
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }) == INDEX_NONE)
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                return false;
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("FindFeatureEntryPoints failed to query the project index: %s"), *Error));
    }

    CandidateRows.Sort([](const FCandidateRow& A, const FCandidateRow& B)
    {
        if (A.Score != B.Score)
        {
            return A.Score > B.Score;
        }

        if (A.ReferencerCount != B.ReferencerCount)
        {
            return A.ReferencerCount > B.ReferencerCount;
        }

        return A.ObjectPath < B.ObjectPath;
    });

    if (CandidateRows.Num() > Limit)
    {
        CandidateRows.SetNum(Limit);
    }

    TArray<TSharedPtr<FJsonValue>> Candidates;
    int32 BlueprintCandidateCount = 0;
    int32 ProjectCandidateCount = 0;
    int32 EngineCandidateCount = 0;
    int32 PluginCandidateCount = 0;
    for (const FCandidateRow& Candidate : CandidateRows)
    {
        BlueprintCandidateCount += Candidate.bIsBlueprint ? 1 : 0;
        ProjectCandidateCount += Candidate.ContentScope.Equals(TEXT("project"), ESearchCase::IgnoreCase) ? 1 : 0;
        EngineCandidateCount += Candidate.ContentScope.Equals(TEXT("engine"), ESearchCase::IgnoreCase) ? 1 : 0;
        PluginCandidateCount += Candidate.ContentScope.Equals(TEXT("plugin"), ESearchCase::IgnoreCase) ? 1 : 0;

        TSharedRef<FJsonObject> CandidateObject = MakeShared<FJsonObject>();
        CandidateObject->SetStringField(TEXT("objectPath"), Candidate.ObjectPath);
        CandidateObject->SetStringField(TEXT("assetName"), Candidate.AssetName);
        CandidateObject->SetStringField(TEXT("classPath"), Candidate.ClassPath);
        CandidateObject->SetStringField(TEXT("packageName"), Candidate.PackageName);
        CandidateObject->SetStringField(TEXT("packagePath"), Candidate.PackagePath);
        CandidateObject->SetStringField(TEXT("contentScope"), Candidate.ContentScope);
        CandidateObject->SetBoolField(TEXT("isBlueprint"), Candidate.bIsBlueprint);
        CandidateObject->SetNumberField(TEXT("score"), Candidate.Score);
        CandidateObject->SetNumberField(TEXT("dependencyCount"), Candidate.DependencyCount);
        CandidateObject->SetNumberField(TEXT("referencerCount"), Candidate.ReferencerCount);
        CandidateObject->SetNumberField(TEXT("componentCount"), Candidate.ComponentCount);
        CandidateObject->SetNumberField(TEXT("functionCount"), Candidate.FunctionCount);
        CandidateObject->SetBoolField(TEXT("isContextDependency"), Candidate.bContextDependsOn);
        CandidateObject->SetBoolField(TEXT("isContextReferencer"), Candidate.bContextReferencedBy);

        TArray<TSharedPtr<FJsonValue>> ReasonValues;
        for (const FString& Reason : Candidate.Reasons)
        {
            ReasonValues.Add(MakeShared<FJsonValueString>(Reason));
        }
        CandidateObject->SetArrayField(TEXT("reasons"), ReasonValues);
        Candidates.Add(MakeShared<FJsonValueObject>(CandidateObject));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("query"), Query);
    Result->SetBoolField(TEXT("blueprintsOnly"), bBlueprintsOnly);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetStringField(TEXT("packagePath"), PackagePathPrefix);
    Result->SetStringField(TEXT("contextObjectPath"), ContextResolvedObjectPath);
    Result->SetStringField(TEXT("contextPackageName"), ContextPackageName);
    Result->SetNumberField(TEXT("count"), Candidates.Num());
    Result->SetNumberField(TEXT("blueprintCandidateCount"), BlueprintCandidateCount);
    Result->SetNumberField(TEXT("projectCandidateCount"), ProjectCandidateCount);
    Result->SetNumberField(TEXT("engineCandidateCount"), EngineCandidateCount);
    Result->SetNumberField(TEXT("pluginCandidateCount"), PluginCandidateCount);
    Result->SetBoolField(TEXT("hasStrongMatch"), CandidateRows.Num() > 0 && CandidateRows[0].Score >= 100);
    if (CandidateRows.Num() > 0)
    {
        TSharedRef<FJsonObject> BestCandidate = MakeShared<FJsonObject>();
        BestCandidate->SetStringField(TEXT("objectPath"), CandidateRows[0].ObjectPath);
        BestCandidate->SetStringField(TEXT("assetName"), CandidateRows[0].AssetName);
        BestCandidate->SetStringField(TEXT("packageName"), CandidateRows[0].PackageName);
        BestCandidate->SetStringField(TEXT("contentScope"), CandidateRows[0].ContentScope);
        BestCandidate->SetNumberField(TEXT("score"), CandidateRows[0].Score);
        if (CandidateRows[0].Reasons.Num() > 0)
        {
            BestCandidate->SetStringField(TEXT("primaryReason"), CandidateRows[0].Reasons[0]);
        }
        Result->SetObjectField(TEXT("bestCandidate"), BestCandidate);
    }
    Result->SetArrayField(TEXT("candidates"), Candidates);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindFeatureEntryPointsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> QueryProperty = MakeShared<FJsonObject>();
    QueryProperty->SetStringField(TEXT("type"), TEXT("string"));
    QueryProperty->SetStringField(TEXT("description"), TEXT("Feature name, asset name, or path fragment to search for, for example inventory or Door."));
    Properties->SetObjectField(TEXT("query"), QueryProperty);

    TSharedRef<FJsonObject> BlueprintsOnlyProperty = MakeShared<FJsonObject>();
    BlueprintsOnlyProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    BlueprintsOnlyProperty->SetStringField(TEXT("description"), TEXT("Whether to only return Blueprint assets. Defaults to true."));
    Properties->SetObjectField(TEXT("blueprintsOnly"), BlueprintsOnlyProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional max result count from 1 to 50. Defaults to 10."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    TSharedRef<FJsonObject> PackagePathProperty = MakeShared<FJsonObject>();
    PackagePathProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackagePathProperty->SetStringField(TEXT("description"), TEXT("Optional package path prefix such as /Game/UI to narrow the search."));
    Properties->SetObjectField(TEXT("packagePath"), PackagePathProperty);

    TSharedRef<FJsonObject> ContextObjectPathProperty = MakeShared<FJsonObject>();
    ContextObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ContextObjectPathProperty->SetStringField(TEXT("description"), TEXT("Optional related asset object path to boost directly connected candidates."));
    Properties->SetObjectField(TEXT("contextObjectPath"), ContextObjectPathProperty);

    TSharedRef<FJsonObject> ContextPackageNameProperty = MakeShared<FJsonObject>();
    ContextPackageNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    ContextPackageNameProperty->SetStringField(TEXT("description"), TEXT("Optional related asset package name when object path is not available."));
    Properties->SetObjectField(TEXT("contextPackageName"), ContextPackageNameProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("query")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
