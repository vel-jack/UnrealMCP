#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

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

constexpr int32 ExplainFeatureWorkflowMaxEntryPointLimit = 5;
constexpr int32 ExplainFeatureWorkflowPreviewLimit = 6;

void AddUniqueString(TArray<FString>& Items, const FString& Item);
void AddRoleHintsFromClassPath(const FString& ClassPath, TArray<FString>& RoleHints);
void AddRoleHintsFromComponentClass(const FString& ComponentClassPath, TArray<FString>& RoleHints);
void AddReason(TArray<FString>& Reasons, const FString& Reason);
int32 ScoreCandidate(const FCandidateRow& Candidate, const FString& Query, const FString& ContextPackagePath, TArray<FString>& OutReasons);
TSharedRef<FJsonObject> MakeLinkedAssetObject(
    const FString& PackageName,
    const FString& ObjectPath,
    const FString& AssetName,
    const FString& ClassPath,
    const FString& ContentScope,
    bool bIsBlueprint);
bool QueryEntryCandidates(
    FSQLiteDatabase& Database,
    const FString& Query,
    bool bBlueprintsOnly,
    int32 Limit,
    const FString& PackagePathPrefix,
    const FString& ContextPackageName,
    const FString& ContextPackagePath,
    TArray<FCandidateRow>& OutCandidates,
    FString& OutError);
bool QueryBlueprintProfile(FSQLiteDatabase& Database, const FString& ObjectPath, TSharedPtr<FJsonObject>& OutProfile, FString& OutError);
bool QueryRelatedAssets(
    FSQLiteDatabase& Database,
    const TCHAR* Sql,
    const FString& PackageName,
    int32 Limit,
    TArray<TSharedPtr<FJsonValue>>& OutRows,
    int32& OutCount,
    FString& OutError);
bool QueryStringPreview(
    FSQLiteDatabase& Database,
    const TCHAR* Sql,
    const FString& ObjectPath,
    int32 Limit,
    TArray<TSharedPtr<FJsonValue>>& OutRows,
    FString& OutError,
    TFunctionRef<void(const FSQLitePreparedStatement&, TSharedRef<FJsonObject>)> Populate);
bool QueryFlowSummary(
    FSQLiteDatabase& Database,
    const FString& StartPackageName,
    int32 MaxDepth,
    int32 MaxNodes,
    TArray<TSharedPtr<FJsonValue>>& OutNodes,
    TArray<TSharedPtr<FJsonValue>>& OutEdges,
    FString& OutError);
