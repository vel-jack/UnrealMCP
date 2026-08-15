#include "Tools/FindUnusedBlueprintAssetsTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    constexpr int32 DefaultMaxResults = 100;
    constexpr int32 MaximumMaxResults = 1000;

    struct FUnusedBlueprintCandidate
    {
        FString ObjectPath;
        FString AssetName;
        FString ClassPath;
        FString PackageName;
        FString PackagePath;
        FString ParentClassPath;
        FString NativeParentClassPath;
        FString BlueprintType;
        int32 OutgoingDependencyCount = 0;
        bool bPossibleRoot = false;
        FString CandidateConfidence;
        TArray<FString> Reasons;
        TArray<FString> PossibleRootSignals;
    };

    FString NormalizePackagePath(FString Path)
    {
        Path.TrimStartAndEndInline();
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
        {
            Path.LeftChopInline(1);
        }
        return Path;
    }

    void AddSignalIfContains(
        const FString& Value,
        const TCHAR* Needle,
        const TCHAR* Signal,
        TArray<FString>& OutSignals)
    {
        if (Value.Contains(Needle, ESearchCase::IgnoreCase))
        {
            OutSignals.AddUnique(Signal);
        }
    }

    void ClassifyPossibleRoot(FUnusedBlueprintCandidate& Candidate)
    {
        if (Candidate.ParentClassPath.IsEmpty() && Candidate.NativeParentClassPath.IsEmpty())
        {
            Candidate.PossibleRootSignals.Add(TEXT("missing_indexed_parent_class"));
        }

        AddSignalIfContains(Candidate.ClassPath, TEXT("LevelScriptBlueprint"), TEXT("level_script_blueprint"), Candidate.PossibleRootSignals);
        AddSignalIfContains(Candidate.ClassPath, TEXT("EditorUtility"), TEXT("manually_invoked_editor_utility"), Candidate.PossibleRootSignals);
        AddSignalIfContains(Candidate.BlueprintType, TEXT("Interface"), TEXT("blueprint_interface"), Candidate.PossibleRootSignals);
        AddSignalIfContains(Candidate.BlueprintType, TEXT("MacroLibrary"), TEXT("blueprint_macro_library"), Candidate.PossibleRootSignals);
        AddSignalIfContains(Candidate.BlueprintType, TEXT("FunctionLibrary"), TEXT("blueprint_function_library"), Candidate.PossibleRootSignals);

        const FString ParentIdentity = Candidate.ParentClassPath + TEXT("|") + Candidate.NativeParentClassPath;
        AddSignalIfContains(ParentIdentity, TEXT("GameInstance"), TEXT("project_settings_game_instance_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("GameMode"), TEXT("project_or_map_game_mode_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("GameState"), TEXT("game_mode_selected_game_state_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("PlayerController"), TEXT("game_mode_selected_player_controller_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("PlayerState"), TEXT("game_mode_selected_player_state_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("GameSession"), TEXT("game_mode_selected_game_session_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("HUD"), TEXT("game_mode_selected_hud_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("Pawn"), TEXT("game_mode_selected_default_pawn_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("Character"), TEXT("game_mode_selected_character_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("WorldSettings"), TEXT("map_selected_world_settings_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("LevelScriptActor"), TEXT("map_owned_level_script_candidate"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("BlueprintFunctionLibrary"), TEXT("blueprint_function_library"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("BlueprintMacroLibrary"), TEXT("blueprint_macro_library"), Candidate.PossibleRootSignals);
        AddSignalIfContains(ParentIdentity, TEXT("EditorUtility"), TEXT("manually_invoked_editor_utility"), Candidate.PossibleRootSignals);

        Candidate.bPossibleRoot = !Candidate.PossibleRootSignals.IsEmpty();
        Candidate.CandidateConfidence = Candidate.bPossibleRoot ? TEXT("low") : TEXT("medium");
        Candidate.Reasons.Add(TEXT("zero_indexed_incoming_dependencies_from_project_packages"));
        if (Candidate.bPossibleRoot)
        {
            Candidate.Reasons.Add(TEXT("possible_root_or_indirect_entry_point"));
        }
        else
        {
            Candidate.Reasons.Add(TEXT("no_indexed_root_like_signal"));
        }
    }

    TArray<TSharedPtr<FJsonValue>> SerializeStrings(const TArray<FString>& Strings)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Strings.Num());
        for (const FString& Value : Strings)
        {
            Values.Add(MakeShared<FJsonValueString>(Value));
        }
        return Values;
    }

    TSharedRef<FJsonObject> SerializeCandidate(const FUnusedBlueprintCandidate& Candidate)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("objectPath"), Candidate.ObjectPath);
        Json->SetStringField(TEXT("assetName"), Candidate.AssetName);
        Json->SetStringField(TEXT("classPath"), Candidate.ClassPath);
        Json->SetStringField(TEXT("packageName"), Candidate.PackageName);
        Json->SetStringField(TEXT("packagePath"), Candidate.PackagePath);
        Json->SetStringField(TEXT("candidateConfidence"), Candidate.CandidateConfidence);
        Json->SetNumberField(TEXT("outgoingDependencyCount"), Candidate.OutgoingDependencyCount);
        Json->SetBoolField(TEXT("possibleRoot"), Candidate.bPossibleRoot);
        Json->SetArrayField(TEXT("reasons"), SerializeStrings(Candidate.Reasons));
        Json->SetArrayField(TEXT("possibleRootSignals"), SerializeStrings(Candidate.PossibleRootSignals));
        return Json;
    }

    bool ReadMaxResults(const TSharedPtr<FJsonObject>& Params, int32& OutMaxResults, FString& OutError)
    {
        double Value = DefaultMaxResults;
        if (Params.IsValid() && Params->HasField(TEXT("maxResults")))
        {
            if (!Params->TryGetNumberField(TEXT("maxResults"), Value)
                || !FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value))
                || Value < 1.0
                || Value > MaximumMaxResults)
            {
                OutError = FString::Printf(TEXT("maxResults must be an integer from 1 to %d."), MaximumMaxResults);
                return false;
            }
        }
        OutMaxResults = FMath::RoundToInt(Value);
        return true;
    }
}

FFindUnusedBlueprintAssetsTool::FFindUnusedBlueprintAssetsTool()
    : FMCPToolBase(
        TEXT("FindUnusedBlueprintAssets"),
        TEXT("Finds conservative project Blueprint candidates with no indexed incoming package dependencies. Results are leads for review, never proof that an asset is unused."))
{
}

UnrealMCP::FMCPResponse FFindUnusedBlueprintAssetsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString RootPath;
    FString PackagePath;
    bool bIncludePossibleRoots = false;
    if (Request.Params.IsValid())
    {
        Request.Params->TryGetStringField(TEXT("rootPath"), RootPath);
        Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath);
        if (Request.Params->HasField(TEXT("includePossibleRoots"))
            && !Request.Params->TryGetBoolField(TEXT("includePossibleRoots"), bIncludePossibleRoots))
        {
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("includePossibleRoots must be a boolean."));
        }
    }

    RootPath = NormalizePackagePath(RootPath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!RootPath.IsEmpty() && !PackagePath.IsEmpty() && !RootPath.Equals(PackagePath, ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindUnusedBlueprintAssets rootPath and packagePath must match when both are supplied."));
    }

    const FString ScopePath = !PackagePath.IsEmpty() ? PackagePath : RootPath;
    if (!ScopePath.IsEmpty() && !ScopePath.StartsWith(TEXT("/Game"), ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindUnusedBlueprintAssets rootPath/packagePath must begin with /Game."));
    }

    int32 MaxResults = DefaultMaxResults;
    FString ParameterError;
    if (!ReadMaxResults(Request.Params, MaxResults, ParameterError))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ParameterError);
    }

    TArray<FUnusedBlueprintCandidate> AllCandidates;
    FString QueryError;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutError)
        {
            FSQLitePreparedStatement Statement(
                Database,
                TEXT("SELECT a.object_path, a.asset_name, a.class_path, a.package_name, a.package_path, "
                     "COALESCE(a.parent_class_path, ''), COALESCE(a.native_parent_class_path, ''), COALESCE(a.blueprint_type, ''), "
                     "(SELECT COUNT(*) FROM asset_dependencies outgoing WHERE outgoing.source_package_name = a.package_name) "
                     "FROM assets a "
                     "WHERE a.content_scope = 'project' AND a.is_blueprint = 1 "
                     "AND (?1 = '' OR a.package_path = ?1 OR a.package_path LIKE ?1 || '/%') "
                     "AND NOT EXISTS ("
                     "  SELECT 1 FROM asset_dependencies incoming "
                     "  WHERE incoming.target_package_name = a.package_name "
                     "  AND EXISTS (SELECT 1 FROM assets source WHERE source.package_name = incoming.source_package_name AND source.content_scope = 'project')"
                     ") "
                     "ORDER BY a.object_path ASC;"),
                ESQLitePreparedStatementFlags::None);

            if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ScopePath))
            {
                OutError = TEXT("FindUnusedBlueprintAssets could not prepare the project index query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FUnusedBlueprintCandidate Candidate;
                if (!Row.GetColumnValueByIndex(0, Candidate.ObjectPath)
                    || !Row.GetColumnValueByIndex(1, Candidate.AssetName)
                    || !Row.GetColumnValueByIndex(2, Candidate.ClassPath)
                    || !Row.GetColumnValueByIndex(3, Candidate.PackageName)
                    || !Row.GetColumnValueByIndex(4, Candidate.PackagePath)
                    || !Row.GetColumnValueByIndex(5, Candidate.ParentClassPath)
                    || !Row.GetColumnValueByIndex(6, Candidate.NativeParentClassPath)
                    || !Row.GetColumnValueByIndex(7, Candidate.BlueprintType)
                    || !Row.GetColumnValueByIndex(8, Candidate.OutgoingDependencyCount))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                ClassifyPossibleRoot(Candidate);
                AllCandidates.Add(MoveTemp(Candidate));
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });

            if (QueryResult == INDEX_NONE)
            {
                OutError = Database.GetLastError().IsEmpty()
                    ? TEXT("FindUnusedBlueprintAssets project index query failed.")
                    : Database.GetLastError();
                return false;
            }
            return true;
        },
        QueryError);

    if (!bQuerySucceeded)
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InternalError,
            FString::Printf(TEXT("FindUnusedBlueprintAssets failed to query the project index: %s"), *QueryError));
    }

    AllCandidates.Sort([](const FUnusedBlueprintCandidate& A, const FUnusedBlueprintCandidate& B)
    {
        if (A.bPossibleRoot != B.bPossibleRoot)
        {
            return !A.bPossibleRoot;
        }
        return A.ObjectPath < B.ObjectPath;
    });

    int32 ExcludedPossibleRootCount = 0;
    TArray<TSharedPtr<FJsonValue>> Candidates;
    Candidates.Reserve(FMath::Min(AllCandidates.Num(), MaxResults));
    int32 EligibleCandidateCount = 0;
    for (const FUnusedBlueprintCandidate& Candidate : AllCandidates)
    {
        if (Candidate.bPossibleRoot && !bIncludePossibleRoots)
        {
            ++ExcludedPossibleRootCount;
            continue;
        }

        ++EligibleCandidateCount;
        if (Candidates.Num() < MaxResults)
        {
            Candidates.Add(MakeShared<FJsonValueObject>(SerializeCandidate(Candidate)));
        }
    }

    TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("zeroProjectIncomingBlueprintCount"), AllCandidates.Num());
    Summary->SetNumberField(TEXT("eligibleCandidateCount"), EligibleCandidateCount);
    Summary->SetNumberField(TEXT("returnedCandidateCount"), Candidates.Num());
    Summary->SetNumberField(TEXT("excludedPossibleRootCount"), ExcludedPossibleRootCount);
    Summary->SetBoolField(TEXT("truncated"), Candidates.Num() < EligibleCandidateCount);

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("analysisType"), TEXT("conservative_indexed_unused_blueprint_candidates"));
    Result->SetStringField(TEXT("rootPath"), ScopePath);
    Result->SetNumberField(TEXT("maxResults"), MaxResults);
    Result->SetBoolField(TEXT("includePossibleRoots"), bIncludePossibleRoots);
    Result->SetObjectField(TEXT("summary"), Summary);
    Result->SetArrayField(TEXT("candidates"), Candidates);
    Result->SetStringField(
        TEXT("interpretation"),
        TEXT("Each result is only a review candidate with zero indexed incoming asset dependencies from project packages; no asset is declared definitively unused."));
    Result->SetStringField(
        TEXT("limitation"),
        TEXT("Runtime dynamic loading, soft references, configuration and project settings, map or level usage not represented in asset_dependencies, C++ references, Asset Manager roots, external tooling, and other indirect entry points may be invisible to this index."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindUnusedBlueprintAssetsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> RootPath = MakeShared<FJsonObject>();
    RootPath->SetStringField(TEXT("type"), TEXT("string"));
    RootPath->SetStringField(TEXT("description"), TEXT("Optional /Game package-path prefix. Alias of packagePath."));
    Properties->SetObjectField(TEXT("rootPath"), RootPath);

    TSharedRef<FJsonObject> PackagePath = MakeShared<FJsonObject>();
    PackagePath->SetStringField(TEXT("type"), TEXT("string"));
    PackagePath->SetStringField(TEXT("description"), TEXT("Optional /Game package-path prefix. Alias of rootPath."));
    Properties->SetObjectField(TEXT("packagePath"), PackagePath);

    TSharedRef<FJsonObject> MaxResults = MakeShared<FJsonObject>();
    MaxResults->SetStringField(TEXT("type"), TEXT("integer"));
    MaxResults->SetNumberField(TEXT("minimum"), 1);
    MaxResults->SetNumberField(TEXT("maximum"), MaximumMaxResults);
    MaxResults->SetNumberField(TEXT("default"), DefaultMaxResults);
    MaxResults->SetStringField(TEXT("description"), TEXT("Maximum candidates to return."));
    Properties->SetObjectField(TEXT("maxResults"), MaxResults);

    TSharedRef<FJsonObject> IncludePossibleRoots = MakeShared<FJsonObject>();
    IncludePossibleRoots->SetStringField(TEXT("type"), TEXT("boolean"));
    IncludePossibleRoots->SetBoolField(TEXT("default"), false);
    IncludePossibleRoots->SetStringField(TEXT("description"), TEXT("Include low-confidence assets with indexed root-like or indirect-entry-point signals."));
    Properties->SetObjectField(TEXT("includePossibleRoots"), IncludePossibleRoots);

    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    return Schema;
}
