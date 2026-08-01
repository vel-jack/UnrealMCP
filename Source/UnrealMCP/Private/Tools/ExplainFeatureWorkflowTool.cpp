#include "Tools/ExplainFeatureWorkflowTool.h"

#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    constexpr int32 MaxEntryPointLimit = 5;
    constexpr int32 PreviewLimit = 6;

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
}

FExplainFeatureWorkflowTool::FExplainFeatureWorkflowTool()
    : FMCPToolBase(TEXT("ExplainFeatureWorkflow"), TEXT("Explains a feature query by ranking likely entry assets, then summarizing each candidate's role and nearby indexed flow."))
{
}

UnrealMCP::FMCPResponse FExplainFeatureWorkflowTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ExplainFeatureWorkflow requires params.query."));
    }

    FString Query;
    if (!Request.Params->TryGetStringField(TEXT("query"), Query) || Query.TrimStartAndEnd().IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ExplainFeatureWorkflow requires a non-empty params.query."));
    }
    Query = Query.TrimStartAndEnd();

    const bool bBlueprintsOnly = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("blueprintsOnly"), true);
    const int32 EntryPointLimit = FMath::Clamp(UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 3), 1, MaxEntryPointLimit);
    const FString PackagePathPrefix = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("packagePath"));

    int32 FlowMaxDepth = 2;
    int32 FlowMaxNodes = 12;
    Request.Params->TryGetNumberField(TEXT("flowMaxDepth"), FlowMaxDepth);
    Request.Params->TryGetNumberField(TEXT("flowMaxNodes"), FlowMaxNodes);
    FlowMaxDepth = FMath::Clamp(FlowMaxDepth, 1, 3);
    FlowMaxNodes = FMath::Clamp(FlowMaxNodes, 1, 30);

    FString ContextObjectPath = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("contextObjectPath"));
    FString ContextPackageName = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("contextPackageName"));
    FString ContextResolvedObjectPath;

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
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ExplainFeatureWorkflow could not resolve the optional context asset from params.contextObjectPath or params.contextPackageName."));
        }

        ContextPackageName = ResolvedContextPackageName.ToString();
    }

    TArray<TSharedPtr<FJsonValue>> WorkflowEntries;
    TArray<TSharedPtr<FJsonValue>> SuggestedSequence;
    FString Error;

    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            FString ContextPackagePath;
            if (!ContextPackageName.IsEmpty())
            {
                TSharedPtr<FJsonObject> ContextAsset;
                FString ContextError;
                if (UnrealMCP::IndexedQueryToolUtils::QueryAssetByPackageName(Database, ContextPackageName, ContextAsset, ContextError) && ContextAsset.IsValid())
                {
                    ContextPackagePath = ContextAsset->GetStringField(TEXT("packagePath"));
                }
            }

            TArray<FCandidateRow> Candidates;
            if (!QueryEntryCandidates(Database, Query, bBlueprintsOnly, EntryPointLimit, PackagePathPrefix, ContextPackageName, ContextPackagePath, Candidates, OutExecError))
            {
                return false;
            }

            for (const FCandidateRow& Candidate : Candidates)
            {
                TSharedPtr<FJsonObject> AssetObject;
                FString AssetError;
                if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByObjectPath(Database, Candidate.ObjectPath, AssetObject, AssetError) || !AssetObject.IsValid())
                {
                    continue;
                }

                TSharedPtr<FJsonObject> BlueprintProfile;
                if (!QueryBlueprintProfile(Database, Candidate.ObjectPath, BlueprintProfile, OutExecError))
                {
                    return false;
                }

                TArray<FString> RoleHints;
                if (BlueprintProfile.IsValid())
                {
                    AddRoleHintsFromClassPath(BlueprintProfile->GetStringField(TEXT("parentClassPath")), RoleHints);
                }

                TArray<TSharedPtr<FJsonValue>> DependencyPreview;
                TArray<TSharedPtr<FJsonValue>> ReferencerPreview;
                TArray<TSharedPtr<FJsonValue>> ComponentPreview;
                TArray<TSharedPtr<FJsonValue>> VariablePreview;
                TArray<TSharedPtr<FJsonValue>> FunctionPreview;
                TArray<TSharedPtr<FJsonValue>> FlowNodes;
                TArray<TSharedPtr<FJsonValue>> FlowEdges;
                int32 DependencyCount = 0;
                int32 ReferencerCount = 0;

                if (!QueryRelatedAssets(
                        Database,
                        TEXT("SELECT d.target_package_name, COALESCE(a.object_path, ''), COALESCE(a.asset_name, ''), COALESCE(a.class_path, ''), COALESCE(a.content_scope, ''), COALESCE(a.is_blueprint, 0) "
                             "FROM asset_dependencies d "
                             "LEFT JOIN assets a ON a.package_name = d.target_package_name "
                             "WHERE d.source_package_name = ?1 "
                             "ORDER BY d.target_package_name ASC;"),
                        Candidate.PackageName,
                        PreviewLimit,
                        DependencyPreview,
                        DependencyCount,
                        OutExecError)
                    || !QueryRelatedAssets(
                        Database,
                        TEXT("SELECT d.source_package_name, COALESCE(a.object_path, ''), COALESCE(a.asset_name, ''), COALESCE(a.class_path, ''), COALESCE(a.content_scope, ''), COALESCE(a.is_blueprint, 0) "
                             "FROM asset_dependencies d "
                             "LEFT JOIN assets a ON a.package_name = d.source_package_name "
                             "WHERE d.target_package_name = ?1 "
                             "ORDER BY d.source_package_name ASC;"),
                        Candidate.PackageName,
                        PreviewLimit,
                        ReferencerPreview,
                        ReferencerCount,
                        OutExecError))
                {
                    return false;
                }

                if (!QueryStringPreview(
                        Database,
                        TEXT("SELECT variable_name, component_class_path, parent_variable_name, is_default_scene_root, child_count "
                             "FROM blueprint_components WHERE blueprint_object_path = ?1 ORDER BY variable_name ASC;"),
                        Candidate.ObjectPath,
                        PreviewLimit,
                        ComponentPreview,
                        OutExecError,
                        [&](const FSQLitePreparedStatement& Row, TSharedRef<FJsonObject> ItemObject)
                        {
                            FString VariableName;
                            FString ComponentClassPath;
                            FString ParentVariableName;
                            int32 bIsDefaultSceneRoot = 0;
                            int32 ChildCount = 0;
                            Row.GetColumnValueByIndex(0, VariableName);
                            Row.GetColumnValueByIndex(1, ComponentClassPath);
                            Row.GetColumnValueByIndex(2, ParentVariableName);
                            Row.GetColumnValueByIndex(3, bIsDefaultSceneRoot);
                            Row.GetColumnValueByIndex(4, ChildCount);
                            AddRoleHintsFromComponentClass(ComponentClassPath, RoleHints);
                            ItemObject->SetStringField(TEXT("variableName"), VariableName);
                            ItemObject->SetStringField(TEXT("componentClassPath"), ComponentClassPath);
                            ItemObject->SetStringField(TEXT("parentVariableName"), ParentVariableName);
                            ItemObject->SetBoolField(TEXT("isDefaultSceneRoot"), bIsDefaultSceneRoot != 0);
                            ItemObject->SetNumberField(TEXT("childCount"), ChildCount);
                        })
                    || !QueryStringPreview(
                        Database,
                        TEXT("SELECT name, type_category, type_subcategory, type_object_path, container_type, is_reference, is_const "
                             "FROM blueprint_variables WHERE blueprint_object_path = ?1 ORDER BY name ASC;"),
                        Candidate.ObjectPath,
                        PreviewLimit,
                        VariablePreview,
                        OutExecError,
                        [&](const FSQLitePreparedStatement& Row, TSharedRef<FJsonObject> ItemObject)
                        {
                            FString Name;
                            FString TypeCategory;
                            FString TypeSubcategory;
                            FString TypeObjectPath;
                            FString ContainerType;
                            int32 bIsReference = 0;
                            int32 bIsConst = 0;
                            Row.GetColumnValueByIndex(0, Name);
                            Row.GetColumnValueByIndex(1, TypeCategory);
                            Row.GetColumnValueByIndex(2, TypeSubcategory);
                            Row.GetColumnValueByIndex(3, TypeObjectPath);
                            Row.GetColumnValueByIndex(4, ContainerType);
                            Row.GetColumnValueByIndex(5, bIsReference);
                            Row.GetColumnValueByIndex(6, bIsConst);
                            ItemObject->SetStringField(TEXT("name"), Name);
                            ItemObject->SetStringField(TEXT("typeCategory"), TypeCategory);
                            ItemObject->SetStringField(TEXT("typeSubcategory"), TypeSubcategory);
                            ItemObject->SetStringField(TEXT("typeObjectPath"), TypeObjectPath);
                            ItemObject->SetStringField(TEXT("containerType"), ContainerType);
                            ItemObject->SetBoolField(TEXT("isReference"), bIsReference != 0);
                            ItemObject->SetBoolField(TEXT("isConst"), bIsConst != 0);
                        })
                    || !QueryStringPreview(
                        Database,
                        TEXT("SELECT name, source, interface_path FROM blueprint_functions WHERE blueprint_object_path = ?1 ORDER BY name ASC;"),
                        Candidate.ObjectPath,
                        PreviewLimit,
                        FunctionPreview,
                        OutExecError,
                        [&](const FSQLitePreparedStatement& Row, TSharedRef<FJsonObject> ItemObject)
                        {
                            FString Name;
                            FString Source;
                            FString InterfacePath;
                            Row.GetColumnValueByIndex(0, Name);
                            Row.GetColumnValueByIndex(1, Source);
                            Row.GetColumnValueByIndex(2, InterfacePath);
                            ItemObject->SetStringField(TEXT("name"), Name);
                            ItemObject->SetStringField(TEXT("source"), Source);
                            ItemObject->SetStringField(TEXT("interfacePath"), InterfacePath);
                        }))
                {
                    return false;
                }

                if (ReferencerCount == 0)
                {
                    AddUniqueString(RoleHints, TEXT("leaf-or-manually-placed"));
                }
                else
                {
                    AddUniqueString(RoleHints, TEXT("used-by-other-assets"));
                }
                if (DependencyCount > 0)
                {
                    AddUniqueString(RoleHints, TEXT("depends-on-other-assets"));
                }

                if (!QueryFlowSummary(Database, Candidate.PackageName, FlowMaxDepth, FlowMaxNodes, FlowNodes, FlowEdges, OutExecError))
                {
                    return false;
                }

                TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
                EntryObject->SetObjectField(TEXT("entryAsset"), AssetObject.ToSharedRef());
                EntryObject->SetNumberField(TEXT("score"), Candidate.Score);

                TArray<TSharedPtr<FJsonValue>> ReasonValues;
                for (const FString& Reason : Candidate.Reasons)
                {
                    ReasonValues.Add(MakeShared<FJsonValueString>(Reason));
                }
                EntryObject->SetArrayField(TEXT("entryReasons"), ReasonValues);

                if (BlueprintProfile.IsValid())
                {
                    EntryObject->SetObjectField(TEXT("blueprintProfile"), BlueprintProfile.ToSharedRef());
                }

                TSharedRef<FJsonObject> RoleSummary = MakeShared<FJsonObject>();
                RoleSummary->SetNumberField(TEXT("dependencyCount"), DependencyCount);
                RoleSummary->SetNumberField(TEXT("referencerCount"), ReferencerCount);
                RoleSummary->SetNumberField(TEXT("componentPreviewCount"), ComponentPreview.Num());
                RoleSummary->SetNumberField(TEXT("variablePreviewCount"), VariablePreview.Num());
                RoleSummary->SetNumberField(TEXT("functionPreviewCount"), FunctionPreview.Num());
                EntryObject->SetObjectField(TEXT("roleSummary"), RoleSummary);

                TArray<TSharedPtr<FJsonValue>> RoleHintValues;
                for (const FString& RoleHint : RoleHints)
                {
                    RoleHintValues.Add(MakeShared<FJsonValueString>(RoleHint));
                }
                EntryObject->SetArrayField(TEXT("roleHints"), RoleHintValues);
                EntryObject->SetArrayField(TEXT("dependencyPreview"), DependencyPreview);
                EntryObject->SetArrayField(TEXT("referencerPreview"), ReferencerPreview);
                EntryObject->SetArrayField(TEXT("componentPreview"), ComponentPreview);
                EntryObject->SetArrayField(TEXT("variablePreview"), VariablePreview);
                EntryObject->SetArrayField(TEXT("functionPreview"), FunctionPreview);

                TSharedRef<FJsonObject> FlowSummary = MakeShared<FJsonObject>();
                FlowSummary->SetNumberField(TEXT("maxDepth"), FlowMaxDepth);
                FlowSummary->SetNumberField(TEXT("maxNodes"), FlowMaxNodes);
                FlowSummary->SetNumberField(TEXT("nodeCount"), FlowNodes.Num());
                FlowSummary->SetNumberField(TEXT("edgeCount"), FlowEdges.Num());
                FlowSummary->SetArrayField(TEXT("nodes"), FlowNodes);
                FlowSummary->SetArrayField(TEXT("edges"), FlowEdges);
                EntryObject->SetObjectField(TEXT("flowSummary"), FlowSummary);

                WorkflowEntries.Add(MakeShared<FJsonValueObject>(EntryObject));

                TSharedRef<FJsonObject> SequenceObject = MakeShared<FJsonObject>();
                SequenceObject->SetStringField(TEXT("objectPath"), Candidate.ObjectPath);
                SequenceObject->SetStringField(TEXT("packageName"), Candidate.PackageName);
                SequenceObject->SetStringField(TEXT("whyStartHere"), Candidate.Reasons.Num() > 0 ? Candidate.Reasons[0] : TEXT("query_match"));
                SequenceObject->SetStringField(TEXT("nextStep"), TEXT("Use ExplainBlueprintRole or TraceFeatureFlow on this entry asset for deeper follow-up."));
                SuggestedSequence.Add(MakeShared<FJsonValueObject>(SequenceObject));
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("ExplainFeatureWorkflow failed to query the project index: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("query"), Query);
    Result->SetBoolField(TEXT("blueprintsOnly"), bBlueprintsOnly);
    Result->SetNumberField(TEXT("entryPointLimit"), EntryPointLimit);
    Result->SetStringField(TEXT("packagePath"), PackagePathPrefix);
    Result->SetStringField(TEXT("contextObjectPath"), ContextResolvedObjectPath);
    Result->SetStringField(TEXT("contextPackageName"), ContextPackageName);
    Result->SetNumberField(TEXT("entryPointCount"), WorkflowEntries.Num());
    Result->SetArrayField(TEXT("entryPoints"), WorkflowEntries);
    Result->SetArrayField(TEXT("suggestedSequence"), SuggestedSequence);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FExplainFeatureWorkflowTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> QueryProperty = MakeShared<FJsonObject>();
    QueryProperty->SetStringField(TEXT("type"), TEXT("string"));
    QueryProperty->SetStringField(TEXT("description"), TEXT("Feature name, asset name, or path fragment to explain, for example inventory, Door, or NiceActor."));
    Properties->SetObjectField(TEXT("query"), QueryProperty);

    TSharedRef<FJsonObject> BlueprintsOnlyProperty = MakeShared<FJsonObject>();
    BlueprintsOnlyProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    BlueprintsOnlyProperty->SetStringField(TEXT("description"), TEXT("Whether to only use Blueprint assets as workflow entry points. Defaults to true."));
    Properties->SetObjectField(TEXT("blueprintsOnly"), BlueprintsOnlyProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional max entry points from 1 to 5. Defaults to 3."));
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

    TSharedRef<FJsonObject> FlowMaxDepthProperty = MakeShared<FJsonObject>();
    FlowMaxDepthProperty->SetStringField(TEXT("type"), TEXT("integer"));
    FlowMaxDepthProperty->SetStringField(TEXT("description"), TEXT("Optional flow traversal depth from 1 to 3. Defaults to 2."));
    Properties->SetObjectField(TEXT("flowMaxDepth"), FlowMaxDepthProperty);

    TSharedRef<FJsonObject> FlowMaxNodesProperty = MakeShared<FJsonObject>();
    FlowMaxNodesProperty->SetStringField(TEXT("type"), TEXT("integer"));
    FlowMaxNodesProperty->SetStringField(TEXT("description"), TEXT("Optional flow traversal node cap from 1 to 30. Defaults to 12."));
    Properties->SetObjectField(TEXT("flowMaxNodes"), FlowMaxNodesProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("query")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
