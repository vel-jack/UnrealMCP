#include "Tools/ExplainFeatureWorkflowToolInternal.h"

#include "Tools/IndexedQueryToolUtils.h"

void AddUniqueString(TArray<FString>& Items, const FString& Item)
{
    if (!Item.IsEmpty())
    {
        Items.AddUnique(Item);
    }
}

void AddRoleHintsFromClassPath(const FString& ClassPath, TArray<FString>& RoleHints)
{
    const FString LoweredClassPath = ClassPath.ToLower();
    if (LoweredClassPath.Contains(TEXT("character")))
    {
        AddUniqueString(RoleHints, TEXT("character-like"));
    }
    if (LoweredClassPath.Contains(TEXT("pawn")))
    {
        AddUniqueString(RoleHints, TEXT("pawn-like"));
    }
    if (LoweredClassPath.Contains(TEXT("controller")))
    {
        AddUniqueString(RoleHints, TEXT("controller-like"));
    }
    if (LoweredClassPath.Contains(TEXT("gamemode")))
    {
        AddUniqueString(RoleHints, TEXT("game-rule-owner"));
    }
    if (LoweredClassPath.Contains(TEXT("gamestate")))
    {
        AddUniqueString(RoleHints, TEXT("shared-game-state"));
    }
    if (LoweredClassPath.Contains(TEXT("widget")))
    {
        AddUniqueString(RoleHints, TEXT("ui-related"));
    }
    if (LoweredClassPath.Contains(TEXT("actorcomponent")))
    {
        AddUniqueString(RoleHints, TEXT("reusable-component"));
    }
}

void AddRoleHintsFromComponentClass(const FString& ComponentClassPath, TArray<FString>& RoleHints)
{
    const FString LoweredComponentClassPath = ComponentClassPath.ToLower();
    if (LoweredComponentClassPath.Contains(TEXT("camera")))
    {
        AddUniqueString(RoleHints, TEXT("camera-related"));
    }
    if (LoweredComponentClassPath.Contains(TEXT("springarm")))
    {
        AddUniqueString(RoleHints, TEXT("camera-rig"));
    }
    if (LoweredComponentClassPath.Contains(TEXT("widget")))
    {
        AddUniqueString(RoleHints, TEXT("ui-related"));
    }
    if (LoweredComponentClassPath.Contains(TEXT("audio")))
    {
        AddUniqueString(RoleHints, TEXT("audio-related"));
    }
    if (LoweredComponentClassPath.Contains(TEXT("skeletalmesh")) || LoweredComponentClassPath.Contains(TEXT("staticmesh")))
    {
        AddUniqueString(RoleHints, TEXT("visual-mesh-owner"));
    }
    if (LoweredComponentClassPath.Contains(TEXT("boxcomponent"))
        || LoweredComponentClassPath.Contains(TEXT("spherecomponent"))
        || LoweredComponentClassPath.Contains(TEXT("capsulecomponent")))
    {
        AddUniqueString(RoleHints, TEXT("collision-related"));
    }
}

void AddReason(TArray<FString>& Reasons, const FString& Reason)
{
    AddUniqueString(Reasons, Reason);
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

TSharedRef<FJsonObject> MakeLinkedAssetObject(
    const FString& PackageName,
    const FString& ObjectPath,
    const FString& AssetName,
    const FString& ClassPath,
    const FString& ContentScope,
    const bool bIsBlueprint)
{
    TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
    AssetObject->SetStringField(TEXT("packageName"), PackageName);
    AssetObject->SetStringField(TEXT("objectPath"), ObjectPath);
    AssetObject->SetStringField(TEXT("assetName"), AssetName);
    AssetObject->SetStringField(TEXT("classPath"), ClassPath);
    AssetObject->SetStringField(TEXT("contentScope"), ContentScope.IsEmpty() ? UnrealMCP::IndexedQueryToolUtils::GetContentScopeFromPackageName(PackageName) : ContentScope);
    AssetObject->SetBoolField(TEXT("isBlueprint"), bIsBlueprint);
    return AssetObject;
}

bool QueryEntryCandidates(
    FSQLiteDatabase& Database,
    const FString& Query,
    const bool bBlueprintsOnly,
    const int32 Limit,
    const FString& PackagePathPrefix,
    const FString& ContextPackageName,
    const FString& ContextPackagePath,
    TArray<FCandidateRow>& OutCandidates,
    FString& OutError)
{
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
        OutError = TEXT("ExplainFeatureWorkflow could not prepare the entry-point query.");
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
            Candidate.Score = ScoreCandidate(Candidate, Query, ContextPackagePath, Candidate.Reasons);
            OutCandidates.Add(MoveTemp(Candidate));
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        }) == INDEX_NONE)
    {
        OutError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
        return false;
    }

    OutCandidates.Sort([](const FCandidateRow& A, const FCandidateRow& B)
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

    if (OutCandidates.Num() > Limit)
    {
        OutCandidates.SetNum(Limit);
    }

    return true;
}

bool QueryBlueprintProfile(FSQLiteDatabase& Database, const FString& ObjectPath, TSharedPtr<FJsonObject>& OutProfile, FString& OutError)
{
    FSQLitePreparedStatement Statement(
        Database,
        TEXT("SELECT generated_class_path, parent_class_path, native_parent_class_path, blueprint_type, is_data_only, blueprint_status, variable_count, function_count, component_count, interface_count "
             "FROM assets WHERE object_path = ?1;"),
        ESQLitePreparedStatementFlags::None);
    if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
    {
        OutError = TEXT("ExplainFeatureWorkflow could not prepare the Blueprint profile query.");
        return false;
    }

    const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
    {
        FString GeneratedClassPath;
        FString ParentClassPath;
        FString NativeParentClassPath;
        FString BlueprintType;
        int32 bIsDataOnly = 0;
        FString BlueprintStatus;
        int32 VariableCount = 0;
        int32 FunctionCount = 0;
        int32 ComponentCount = 0;
        int32 InterfaceCount = 0;

        if (!Row.GetColumnValueByIndex(0, GeneratedClassPath)
            || !Row.GetColumnValueByIndex(1, ParentClassPath)
            || !Row.GetColumnValueByIndex(2, NativeParentClassPath)
            || !Row.GetColumnValueByIndex(3, BlueprintType)
            || !Row.GetColumnValueByIndex(4, bIsDataOnly)
            || !Row.GetColumnValueByIndex(5, BlueprintStatus)
            || !Row.GetColumnValueByIndex(6, VariableCount)
            || !Row.GetColumnValueByIndex(7, FunctionCount)
            || !Row.GetColumnValueByIndex(8, ComponentCount)
            || !Row.GetColumnValueByIndex(9, InterfaceCount))
        {
            return ESQLitePreparedStatementExecuteRowResult::Error;
        }

        TSharedRef<FJsonObject> ProfileObject = MakeShared<FJsonObject>();
        ProfileObject->SetStringField(TEXT("generatedClassPath"), GeneratedClassPath);
        ProfileObject->SetStringField(TEXT("parentClassPath"), ParentClassPath);
        ProfileObject->SetStringField(TEXT("nativeParentClassPath"), NativeParentClassPath);
        ProfileObject->SetStringField(TEXT("blueprintType"), BlueprintType);
        ProfileObject->SetBoolField(TEXT("isDataOnly"), bIsDataOnly != 0);
        ProfileObject->SetStringField(TEXT("blueprintStatus"), BlueprintStatus);
        ProfileObject->SetNumberField(TEXT("variableCount"), VariableCount);
        ProfileObject->SetNumberField(TEXT("functionCount"), FunctionCount);
        ProfileObject->SetNumberField(TEXT("componentCount"), ComponentCount);
        ProfileObject->SetNumberField(TEXT("interfaceCount"), InterfaceCount);
        OutProfile = ProfileObject;
        return ESQLitePreparedStatementExecuteRowResult::Stop;
    });

    if (QueryResult == INDEX_NONE)
    {
        OutError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
        return false;
    }

    return true;
}

bool QueryRelatedAssets(
    FSQLiteDatabase& Database,
    const TCHAR* Sql,
    const FString& PackageName,
    const int32 Limit,
    TArray<TSharedPtr<FJsonValue>>& OutRows,
    int32& OutCount,
    FString& OutError)
{
    FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
    if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, PackageName))
    {
        OutError = TEXT("ExplainFeatureWorkflow could not prepare the related-assets query.");
        return false;
    }

    if (Statement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            FString RelatedPackageName;
            FString RelatedObjectPath;
            FString RelatedAssetName;
            FString RelatedClassPath;
            FString RelatedContentScope;
            int32 bRelatedIsBlueprint = 0;

            if (!Row.GetColumnValueByIndex(0, RelatedPackageName)
                || !Row.GetColumnValueByIndex(1, RelatedObjectPath)
                || !Row.GetColumnValueByIndex(2, RelatedAssetName)
                || !Row.GetColumnValueByIndex(3, RelatedClassPath)
                || !Row.GetColumnValueByIndex(4, RelatedContentScope)
                || !Row.GetColumnValueByIndex(5, bRelatedIsBlueprint))
            {
                return ESQLitePreparedStatementExecuteRowResult::Error;
            }

            ++OutCount;
            if (OutRows.Num() < Limit)
            {
                OutRows.Add(MakeShared<FJsonValueObject>(
                    MakeLinkedAssetObject(
                        RelatedPackageName,
                        RelatedObjectPath,
                        RelatedAssetName,
                        RelatedClassPath,
                        RelatedContentScope,
                        bRelatedIsBlueprint != 0)));
            }

            return ESQLitePreparedStatementExecuteRowResult::Continue;
        }) == INDEX_NONE)
    {
        OutError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
        return false;
    }

    return true;
}

bool QueryStringPreview(
    FSQLiteDatabase& Database,
    const TCHAR* Sql,
    const FString& ObjectPath,
    const int32 Limit,
    TArray<TSharedPtr<FJsonValue>>& OutRows,
    FString& OutError,
    TFunctionRef<void(const FSQLitePreparedStatement&, TSharedRef<FJsonObject>)> Populate)
{
    FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
    if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
    {
        OutError = TEXT("ExplainFeatureWorkflow could not prepare a preview query.");
        return false;
    }

    if (Statement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            if (OutRows.Num() >= Limit)
            {
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            }

            TSharedRef<FJsonObject> ItemObject = MakeShared<FJsonObject>();
            Populate(Row, ItemObject);
            OutRows.Add(MakeShared<FJsonValueObject>(ItemObject));
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        }) == INDEX_NONE)
    {
        OutError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
        return false;
    }

    return true;
}

bool QueryFlowSummary(
    FSQLiteDatabase& Database,
    const FString& StartPackageName,
    const int32 MaxDepth,
    const int32 MaxNodes,
    TArray<TSharedPtr<FJsonValue>>& OutNodes,
    TArray<TSharedPtr<FJsonValue>>& OutEdges,
    FString& OutError)
{
    TArray<FString> Frontier;
    Frontier.Add(StartPackageName);

    TSet<FString> VisitedPackages;
    VisitedPackages.Add(StartPackageName);

    for (int32 Depth = 0; Depth < MaxDepth && Frontier.Num() > 0 && VisitedPackages.Num() < MaxNodes; ++Depth)
    {
        TArray<FString> NextFrontier;

        for (const FString& CurrentPackage : Frontier)
        {
            if (VisitedPackages.Num() > MaxNodes)
            {
                break;
            }

            TSharedPtr<FJsonObject> CurrentAsset;
            FString AssetError;
            if (UnrealMCP::IndexedQueryToolUtils::QueryAssetByPackageName(Database, CurrentPackage, CurrentAsset, AssetError) && CurrentAsset.IsValid())
            {
                TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
                NodeObject->SetStringField(TEXT("packageName"), CurrentPackage);
                NodeObject->SetNumberField(TEXT("depth"), Depth);
                NodeObject->SetObjectField(TEXT("asset"), CurrentAsset.ToSharedRef());
                OutNodes.Add(MakeShared<FJsonValueObject>(NodeObject));
            }

            auto ExpandQuery = [&](const TCHAR* Sql, const TCHAR* EdgeKind) -> bool
            {
                FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, CurrentPackage))
                {
                    return false;
                }

                return Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FString RelatedPackage;
                    if (!Row.GetColumnValueByIndex(0, RelatedPackage))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }

                    TSharedRef<FJsonObject> EdgeObject = MakeShared<FJsonObject>();
                    EdgeObject->SetStringField(TEXT("fromPackage"), CurrentPackage);
                    EdgeObject->SetStringField(TEXT("toPackage"), RelatedPackage);
                    EdgeObject->SetStringField(TEXT("kind"), EdgeKind);
                    EdgeObject->SetNumberField(TEXT("depth"), Depth);
                    OutEdges.Add(MakeShared<FJsonValueObject>(EdgeObject));

                    if (VisitedPackages.Num() < MaxNodes && !VisitedPackages.Contains(RelatedPackage))
                    {
                        VisitedPackages.Add(RelatedPackage);
                        NextFrontier.Add(RelatedPackage);
                    }

                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }) != INDEX_NONE;
            };

            if (!ExpandQuery(TEXT("SELECT target_package_name FROM asset_dependencies WHERE source_package_name = ?1 ORDER BY target_package_name ASC;"), TEXT("depends_on"))
                || !ExpandQuery(TEXT("SELECT source_package_name FROM asset_dependencies WHERE target_package_name = ?1 ORDER BY source_package_name ASC;"), TEXT("referenced_by")))
            {
                OutError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                return false;
            }
        }

        Frontier = MoveTemp(NextFrontier);
    }

    return true;
}
