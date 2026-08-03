#include "Tools/ExplainFeatureWorkflowTool.h"

#include "Dom/JsonObject.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/ExplainFeatureWorkflowToolInternal.h"
#include "Tools/IndexedQueryToolUtils.h"

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
    const int32 EntryPointLimit = FMath::Clamp(UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 3), 1, ExplainFeatureWorkflowMaxEntryPointLimit);
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
                        ExplainFeatureWorkflowPreviewLimit,
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
                        ExplainFeatureWorkflowPreviewLimit,
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
                        ExplainFeatureWorkflowPreviewLimit,
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
                        ExplainFeatureWorkflowPreviewLimit,
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
                        ExplainFeatureWorkflowPreviewLimit,
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
