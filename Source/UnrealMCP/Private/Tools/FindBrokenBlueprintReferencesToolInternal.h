#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FSQLiteDatabase;
class FSQLitePreparedStatement;

namespace UnrealMCP::FindBrokenBlueprintReferencesInternal
{
    constexpr int32 DefaultMaxResults = 100;
    constexpr int32 MaximumMaxResults = 1000;

    extern const TCHAR* AssetDependencyCategory;
    extern const TCHAR* ComponentBlueprintCategory;
    extern const TCHAR* VariableTypeCategory;
    extern const TCHAR* PinTypeCategory;
    extern const TCHAR* MemberParentCategory;
    extern const TCHAR* EdgeSourceNodeCategory;
    extern const TCHAR* EdgeTargetNodeCategory;

    struct FBrokenReferenceFinding
    {
        FString Category;
        FString Severity;
        FString BlueprintObjectPath;
        FString GraphName;
        FString NodeGuid;
        FString PinId;
        FString MemberName;
        FString ReferencedPath;
        FString Evidence;
        FString RecommendedAction;
    };

    FString NormalizePackagePath(FString Path);

    bool ReadPositiveInteger(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* FieldName,
        int32 DefaultValue,
        int32 MaximumValue,
        int32& OutValue,
        FString& OutError);

    TSet<FString> GetAllCategories();

    bool ParseCategories(
        const TSharedPtr<FJsonObject>& Params,
        TSet<FString>& OutCategories,
        FString& OutError);

    bool BindScope(
        FSQLitePreparedStatement& Statement,
        const FString& ObjectPath,
        const FString& RootPath);

    FString GetQueryError(FSQLiteDatabase& Database, const TCHAR* Fallback);

    bool IsMissingProjectReference(
        const FString& ReferencePath,
        const TSet<FString>& IndexedPackages);

    TSharedRef<FJsonObject> SerializeFinding(const FBrokenReferenceFinding& Finding);
}
