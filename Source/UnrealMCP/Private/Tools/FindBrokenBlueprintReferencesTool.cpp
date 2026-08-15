#include "Tools/FindBrokenBlueprintReferencesTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SQLitePreparedStatement.h"
#include "Tools/FindBrokenBlueprintReferencesToolInternal.h"
#include "Tools/IndexedQueryToolUtils.h"

using namespace UnrealMCP::FindBrokenBlueprintReferencesInternal;

FFindBrokenBlueprintReferencesTool::FFindBrokenBlueprintReferencesTool()
    : FMCPToolBase(
        TEXT("FindBrokenBlueprintReferences"),
        TEXT("Finds project Blueprint references that are provably unresolved in the current project index. This is static index evidence, not a live compiler check."))
{
}

UnrealMCP::FMCPResponse FFindBrokenBlueprintReferencesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString RootPath;
    FString PackagePath;
    if (Request.Params.IsValid())
    {
        Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->TryGetStringField(TEXT("rootPath"), RootPath);
        Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath);
    }

    ObjectPath.TrimStartAndEndInline();
    RootPath = NormalizePackagePath(RootPath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!RootPath.IsEmpty() && !PackagePath.IsEmpty() && !RootPath.Equals(PackagePath, ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindBrokenBlueprintReferences rootPath and packagePath must match when both are supplied."));
    }

    const FString ScopePath = !PackagePath.IsEmpty() ? PackagePath : RootPath;
    if (!ScopePath.IsEmpty() && !ScopePath.StartsWith(TEXT("/Game"), ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindBrokenBlueprintReferences rootPath/packagePath must begin with /Game."));
    }

    int32 MaxResults = DefaultMaxResults;
    TSet<FString> EnabledCategories;
    FString ParameterError;
    if (!ReadPositiveInteger(Request.Params, TEXT("maxResults"), DefaultMaxResults, MaximumMaxResults, MaxResults, ParameterError)
        || !ParseCategories(Request.Params, EnabledCategories, ParameterError))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ParameterError);
    }

    TArray<FBrokenReferenceFinding> Findings;
    TSet<FString> IndexedPackages;
    bool bRequestedBlueprintFound = ObjectPath.IsEmpty();
    FString QueryError;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutError)
        {
            FSQLitePreparedStatement PackageStatement(
                Database,
                TEXT("SELECT DISTINCT package_name FROM assets WHERE content_scope = 'project' ORDER BY package_name ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!PackageStatement.IsValid())
            {
                OutError = TEXT("FindBrokenBlueprintReferences could not prepare the indexed package query.");
                return false;
            }

            const int64 PackageResult = PackageStatement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FString PackageName;
                if (!Row.GetColumnValueByIndex(0, PackageName))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }
                IndexedPackages.Add(PackageName);
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });
            if (PackageResult == INDEX_NONE)
            {
                OutError = GetQueryError(Database, TEXT("FindBrokenBlueprintReferences package query failed."));
                return false;
            }

            FSQLitePreparedStatement BlueprintCheck(
                Database,
                TEXT("SELECT COUNT(*) FROM assets a WHERE a.content_scope = 'project' AND a.is_blueprint = 1 "
                     "AND (?1 = '' OR a.object_path = ?1 COLLATE NOCASE) "
                     "AND (?2 = '' OR a.package_path = ?2 OR a.package_path LIKE ?2 || '/%');"),
                ESQLitePreparedStatementFlags::None);
            if (!BlueprintCheck.IsValid() || !BindScope(BlueprintCheck, ObjectPath, ScopePath))
            {
                OutError = TEXT("FindBrokenBlueprintReferences could not prepare the Blueprint scope query.");
                return false;
            }
            int32 MatchingBlueprintCount = 0;
            const int64 BlueprintCheckResult = BlueprintCheck.Execute([&](const FSQLitePreparedStatement& Row)
            {
                return Row.GetColumnValueByIndex(0, MatchingBlueprintCount)
                    ? ESQLitePreparedStatementExecuteRowResult::Stop
                    : ESQLitePreparedStatementExecuteRowResult::Error;
            });
            if (BlueprintCheckResult == INDEX_NONE)
            {
                OutError = GetQueryError(Database, TEXT("FindBrokenBlueprintReferences Blueprint scope query failed."));
                return false;
            }
            bRequestedBlueprintFound = ObjectPath.IsEmpty() || MatchingBlueprintCount > 0;

            if (EnabledCategories.Contains(AssetDependencyCategory))
            {
                FSQLitePreparedStatement Statement(
                    Database,
                    TEXT("SELECT a.object_path, d.target_package_name FROM asset_dependencies d "
                         "JOIN assets a ON a.package_name = d.source_package_name "
                         "WHERE a.content_scope = 'project' AND a.is_blueprint = 1 "
                         "AND d.target_package_name LIKE '/Game%' "
                         "AND NOT EXISTS (SELECT 1 FROM assets target WHERE target.package_name = d.target_package_name) "
                         "AND (?1 = '' OR a.object_path = ?1 COLLATE NOCASE) "
                         "AND (?2 = '' OR a.package_path = ?2 OR a.package_path LIKE ?2 || '/%') "
                         "ORDER BY a.object_path ASC, d.target_package_name ASC;"),
                    ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !BindScope(Statement, ObjectPath, ScopePath))
                {
                    OutError = TEXT("FindBrokenBlueprintReferences could not prepare the asset dependency query.");
                    return false;
                }
                const int64 Result = Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FBrokenReferenceFinding Finding;
                    Finding.Category = AssetDependencyCategory;
                    Finding.Severity = TEXT("warning");
                    if (!Row.GetColumnValueByIndex(0, Finding.BlueprintObjectPath)
                        || !Row.GetColumnValueByIndex(1, Finding.ReferencedPath))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }
                    Finding.Evidence = TEXT("The indexed Blueprint package depends on a /Game package that has no row in assets.");
                    Finding.RecommendedAction = TEXT("Verify whether the referenced asset was renamed or deleted, then repair the Blueprint reference and refresh its index entry.");
                    Findings.Add(MoveTemp(Finding));
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                });
                if (Result == INDEX_NONE)
                {
                    OutError = GetQueryError(Database, TEXT("FindBrokenBlueprintReferences asset dependency query failed."));
                    return false;
                }
            }

            auto QueryPathReferences = [&](const TCHAR* Sql, const FString& Category, const FString& Evidence, const FString& Action, const bool bHasGraph, const bool bHasNode, const bool bHasPin, const bool bHasMember)
            {
                FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !BindScope(Statement, ObjectPath, ScopePath))
                {
                    OutError = FString::Printf(TEXT("FindBrokenBlueprintReferences could not prepare the %s query."), *Category);
                    return false;
                }
                const int64 Result = Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FBrokenReferenceFinding Finding;
                    Finding.Category = Category;
                    Finding.Severity = TEXT("warning");
                    int32 Column = 0;
                    if (!Row.GetColumnValueByIndex(Column++, Finding.BlueprintObjectPath))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }
                    if (bHasGraph && !Row.GetColumnValueByIndex(Column++, Finding.GraphName))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }
                    if (bHasNode && !Row.GetColumnValueByIndex(Column++, Finding.NodeGuid))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }
                    if (bHasPin && !Row.GetColumnValueByIndex(Column++, Finding.PinId))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }
                    if (bHasMember && !Row.GetColumnValueByIndex(Column++, Finding.MemberName))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }
                    if (!Row.GetColumnValueByIndex(Column, Finding.ReferencedPath))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }
                    if (IsMissingProjectReference(Finding.ReferencedPath, IndexedPackages))
                    {
                        Finding.Evidence = Evidence;
                        Finding.RecommendedAction = Action;
                        Findings.Add(MoveTemp(Finding));
                    }
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                });
                if (Result == INDEX_NONE)
                {
                    OutError = GetQueryError(Database, *FString::Printf(TEXT("FindBrokenBlueprintReferences %s query failed."), *Category));
                    return false;
                }
                return true;
            };

            const TCHAR* ScopeClause =
                TEXT(" JOIN assets a ON a.object_path = x.blueprint_object_path "
                     "WHERE a.content_scope = 'project' AND a.is_blueprint = 1 "
                     "AND (?1 = '' OR a.object_path = ?1 COLLATE NOCASE) "
                     "AND (?2 = '' OR a.package_path = ?2 OR a.package_path LIKE ?2 || '/%') ");

            if (EnabledCategories.Contains(ComponentBlueprintCategory))
            {
                const FString Sql = FString(TEXT("SELECT x.blueprint_object_path, x.variable_name, COALESCE(x.component_blueprint_path, '') FROM blueprint_components x"))
                    + ScopeClause + TEXT("AND COALESCE(x.component_blueprint_path, '') <> '' ORDER BY x.blueprint_object_path, x.variable_name;");
                if (!QueryPathReferences(*Sql, ComponentBlueprintCategory,
                    TEXT("The component's indexed Blueprint class path resolves to no indexed /Game asset package."),
                    TEXT("Open the component template, select a valid Blueprint component class, and refresh this Blueprint's index entry."),
                    false, false, false, true))
                {
                    return false;
                }
            }

            if (EnabledCategories.Contains(VariableTypeCategory))
            {
                const FString Sql = FString(TEXT("SELECT x.blueprint_object_path, x.name, COALESCE(x.type_object_path, '') FROM blueprint_variables x"))
                    + ScopeClause + TEXT("AND COALESCE(x.type_object_path, '') <> '' ORDER BY x.blueprint_object_path, x.name;");
                if (!QueryPathReferences(*Sql, VariableTypeCategory,
                    TEXT("The variable's indexed type object path resolves to no indexed /Game asset package."),
                    TEXT("Replace the missing variable type or restore the referenced project asset, then refresh this Blueprint's index entry."),
                    false, false, false, true))
                {
                    return false;
                }
            }

            if (EnabledCategories.Contains(PinTypeCategory))
            {
                const FString Sql = FString(TEXT("SELECT x.blueprint_object_path, x.graph_name, x.node_guid, x.pin_id, COALESCE(x.subcategory_object_path, '') FROM blueprint_pins x"))
                    + ScopeClause + TEXT("AND COALESCE(x.subcategory_object_path, '') <> '' ORDER BY x.blueprint_object_path, x.graph_name, x.node_guid, x.pin_id;");
                if (!QueryPathReferences(*Sql, PinTypeCategory,
                    TEXT("The pin's indexed subcategory object path resolves to no indexed /Game asset package."),
                    TEXT("Reconstruct or replace the affected node/pin after restoring the intended project type, then refresh this Blueprint's index entry."),
                    true, true, true, false))
                {
                    return false;
                }
            }

            if (EnabledCategories.Contains(MemberParentCategory))
            {
                const FString Sql = FString(TEXT("SELECT x.blueprint_object_path, x.graph_name, x.node_guid, COALESCE(x.member_name, ''), COALESCE(x.member_parent_path, '') FROM blueprint_nodes x"))
                    + ScopeClause + TEXT("AND COALESCE(x.member_parent_path, '') <> '' ORDER BY x.blueprint_object_path, x.graph_name, x.node_guid;");
                if (!QueryPathReferences(*Sql, MemberParentCategory,
                    TEXT("The node's indexed member parent path resolves to no indexed /Game asset package."),
                    TEXT("Replace or reconstruct the node against a valid class/member, then refresh this Blueprint's index entry."),
                    true, true, false, true))
                {
                    return false;
                }
            }

            if (EnabledCategories.Contains(EdgeSourceNodeCategory) || EnabledCategories.Contains(EdgeTargetNodeCategory))
            {
                FSQLitePreparedStatement Statement(
                    Database,
                    TEXT("SELECT e.blueprint_object_path, e.source_graph_name, e.source_node_guid, e.source_pin_id, "
                         "e.target_graph_name, e.target_node_guid, e.target_pin_id, "
                         "CASE WHEN source.node_guid IS NULL THEN 1 ELSE 0 END, "
                         "CASE WHEN target.node_guid IS NULL THEN 1 ELSE 0 END "
                         "FROM blueprint_edges e "
                         "JOIN assets a ON a.object_path = e.blueprint_object_path "
                         "LEFT JOIN blueprint_nodes source ON source.blueprint_object_path = e.blueprint_object_path "
                         " AND source.graph_name = e.source_graph_name AND source.node_guid = e.source_node_guid "
                         "LEFT JOIN blueprint_nodes target ON target.blueprint_object_path = e.blueprint_object_path "
                         " AND target.graph_name = e.target_graph_name AND target.node_guid = e.target_node_guid "
                         "WHERE a.content_scope = 'project' AND a.is_blueprint = 1 "
                         "AND (?1 = '' OR a.object_path = ?1 COLLATE NOCASE) "
                         "AND (?2 = '' OR a.package_path = ?2 OR a.package_path LIKE ?2 || '/%') "
                         "AND (source.node_guid IS NULL OR target.node_guid IS NULL) "
                         "ORDER BY e.blueprint_object_path, e.source_graph_name, e.source_node_guid, e.source_pin_id, e.target_graph_name, e.target_node_guid, e.target_pin_id;"),
                    ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !BindScope(Statement, ObjectPath, ScopePath))
                {
                    OutError = TEXT("FindBrokenBlueprintReferences could not prepare the Blueprint edge query.");
                    return false;
                }
                const int64 Result = Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FString BlueprintPath;
                    FString SourceGraph;
                    FString SourceNode;
                    FString SourcePin;
                    FString TargetGraph;
                    FString TargetNode;
                    FString TargetPin;
                    int32 bMissingSource = 0;
                    int32 bMissingTarget = 0;
                    if (!Row.GetColumnValueByIndex(0, BlueprintPath)
                        || !Row.GetColumnValueByIndex(1, SourceGraph)
                        || !Row.GetColumnValueByIndex(2, SourceNode)
                        || !Row.GetColumnValueByIndex(3, SourcePin)
                        || !Row.GetColumnValueByIndex(4, TargetGraph)
                        || !Row.GetColumnValueByIndex(5, TargetNode)
                        || !Row.GetColumnValueByIndex(6, TargetPin)
                        || !Row.GetColumnValueByIndex(7, bMissingSource)
                        || !Row.GetColumnValueByIndex(8, bMissingTarget))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }

                    if (bMissingSource != 0 && EnabledCategories.Contains(EdgeSourceNodeCategory))
                    {
                        FBrokenReferenceFinding Finding;
                        Finding.Category = EdgeSourceNodeCategory;
                        Finding.Severity = TEXT("error");
                        Finding.BlueprintObjectPath = BlueprintPath;
                        Finding.GraphName = SourceGraph;
                        Finding.NodeGuid = SourceNode;
                        Finding.PinId = SourcePin;
                        Finding.ReferencedPath = FString::Printf(TEXT("%s#%s/%s"), *BlueprintPath, *SourceGraph, *SourceNode);
                        Finding.Evidence = TEXT("An indexed edge names a source graph/node key that has no matching blueprint_nodes row.");
                        Finding.RecommendedAction = TEXT("Refresh this Blueprint's index entry; if the finding remains, reconstruct the stale graph connection.");
                        Findings.Add(MoveTemp(Finding));
                    }
                    if (bMissingTarget != 0 && EnabledCategories.Contains(EdgeTargetNodeCategory))
                    {
                        FBrokenReferenceFinding Finding;
                        Finding.Category = EdgeTargetNodeCategory;
                        Finding.Severity = TEXT("error");
                        Finding.BlueprintObjectPath = BlueprintPath;
                        Finding.GraphName = TargetGraph;
                        Finding.NodeGuid = TargetNode;
                        Finding.PinId = TargetPin;
                        Finding.ReferencedPath = FString::Printf(TEXT("%s#%s/%s"), *BlueprintPath, *TargetGraph, *TargetNode);
                        Finding.Evidence = TEXT("An indexed edge names a target graph/node key that has no matching blueprint_nodes row.");
                        Finding.RecommendedAction = TEXT("Refresh this Blueprint's index entry; if the finding remains, reconstruct the stale graph connection.");
                        Findings.Add(MoveTemp(Finding));
                    }
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                });
                if (Result == INDEX_NONE)
                {
                    OutError = GetQueryError(Database, TEXT("FindBrokenBlueprintReferences Blueprint edge query failed."));
                    return false;
                }
            }

            return true;
        },
        QueryError);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError,
            FString::Printf(TEXT("FindBrokenBlueprintReferences failed to query the project index: %s"), *QueryError));
    }
    if (!bRequestedBlueprintFound)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("FindBrokenBlueprintReferences could not find objectPath as an indexed project Blueprint in the selected scope."));
    }

    Findings.Sort([](const FBrokenReferenceFinding& A, const FBrokenReferenceFinding& B)
    {
        if (A.Category != B.Category) return A.Category < B.Category;
        if (A.BlueprintObjectPath != B.BlueprintObjectPath) return A.BlueprintObjectPath < B.BlueprintObjectPath;
        if (A.GraphName != B.GraphName) return A.GraphName < B.GraphName;
        if (A.NodeGuid != B.NodeGuid) return A.NodeGuid < B.NodeGuid;
        if (A.PinId != B.PinId) return A.PinId < B.PinId;
        if (A.MemberName != B.MemberName) return A.MemberName < B.MemberName;
        return A.ReferencedPath < B.ReferencedPath;
    });

    TMap<FString, int32> CountsByCategory;
    for (const FString& Category : GetAllCategories())
    {
        CountsByCategory.Add(Category, 0);
    }
    for (const FBrokenReferenceFinding& Finding : Findings)
    {
        ++CountsByCategory.FindChecked(Finding.Category);
    }

    const int32 TotalFindingCount = Findings.Num();
    const int32 ReturnedFindingCount = FMath::Min(TotalFindingCount, MaxResults);
    TArray<TSharedPtr<FJsonValue>> FindingValues;
    FindingValues.Reserve(ReturnedFindingCount);
    for (int32 Index = 0; Index < ReturnedFindingCount; ++Index)
    {
        FindingValues.Add(MakeShared<FJsonValueObject>(SerializeFinding(Findings[Index])));
    }

    TSharedRef<FJsonObject> CategoryCounts = MakeShared<FJsonObject>();
    TArray<FString> SortedCategories;
    CountsByCategory.GetKeys(SortedCategories);
    SortedCategories.Sort();
    for (const FString& Category : SortedCategories)
    {
        CategoryCounts->SetNumberField(Category, CountsByCategory.FindChecked(Category));
    }

    TArray<FString> EnabledCategoryNames = EnabledCategories.Array();
    EnabledCategoryNames.Sort();
    TArray<TSharedPtr<FJsonValue>> EnabledCategoryValues;
    for (const FString& Category : EnabledCategoryNames)
    {
        EnabledCategoryValues.Add(MakeShared<FJsonValueString>(Category));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("analysisType"), TEXT("indexed_broken_blueprint_reference_evidence"));
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("rootPath"), ScopePath);
    Result->SetArrayField(TEXT("categories"), EnabledCategoryValues);
    Result->SetNumberField(TEXT("total"), TotalFindingCount);
    Result->SetNumberField(TEXT("returned"), ReturnedFindingCount);
    Result->SetBoolField(TEXT("truncated"), ReturnedFindingCount < TotalFindingCount);
    Result->SetObjectField(TEXT("countsByCategory"), CategoryCounts);
    Result->SetArrayField(TEXT("findings"), FindingValues);
    Result->SetStringField(TEXT("limitation"), TEXT("Index evidence does not prove all live Blueprint compiler errors. Compile or validate the Blueprint separately when runtime/editor confirmation is required."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindBrokenBlueprintReferencesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    auto AddStringProperty = [&](const TCHAR* PropertyName, const TCHAR* PropertyDescription)
    {
        TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), TEXT("string"));
        Property->SetStringField(TEXT("description"), PropertyDescription);
        Properties->SetObjectField(PropertyName, Property);
    };
    AddStringProperty(TEXT("objectPath"), TEXT("Optional exact indexed project Blueprint object path."));
    AddStringProperty(TEXT("rootPath"), TEXT("Optional /Game package-path scope. Alias of packagePath."));
    AddStringProperty(TEXT("packagePath"), TEXT("Optional /Game package-path scope. Alias of rootPath."));

    TSharedRef<FJsonObject> Categories = MakeShared<FJsonObject>();
    Categories->SetStringField(TEXT("type"), TEXT("array"));
    Categories->SetStringField(TEXT("description"), TEXT("Optional evidence categories to include. Omit to run every category."));
    Categories->SetBoolField(TEXT("uniqueItems"), true);
    TSharedRef<FJsonObject> CategoryItems = MakeShared<FJsonObject>();
    CategoryItems->SetStringField(TEXT("type"), TEXT("string"));
    TArray<TSharedPtr<FJsonValue>> CategoryEnums;
    TArray<FString> CategoryNames = GetAllCategories().Array();
    CategoryNames.Sort();
    for (const FString& Category : CategoryNames)
    {
        CategoryEnums.Add(MakeShared<FJsonValueString>(Category));
    }
    CategoryItems->SetArrayField(TEXT("enum"), CategoryEnums);
    Categories->SetObjectField(TEXT("items"), CategoryItems);
    Properties->SetObjectField(TEXT("categories"), Categories);

    TSharedRef<FJsonObject> MaxResults = MakeShared<FJsonObject>();
    MaxResults->SetStringField(TEXT("type"), TEXT("integer"));
    MaxResults->SetStringField(TEXT("description"), TEXT("Maximum findings to return after deterministic sorting. Defaults to 100."));
    MaxResults->SetNumberField(TEXT("default"), DefaultMaxResults);
    MaxResults->SetNumberField(TEXT("minimum"), 1);
    MaxResults->SetNumberField(TEXT("maximum"), MaximumMaxResults);
    Properties->SetObjectField(TEXT("maxResults"), MaxResults);

    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    return Schema;
}
