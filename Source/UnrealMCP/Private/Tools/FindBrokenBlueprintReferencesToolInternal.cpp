#include "Tools/FindBrokenBlueprintReferencesToolInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

namespace UnrealMCP::FindBrokenBlueprintReferencesInternal
{
    const TCHAR* AssetDependencyCategory = TEXT("asset_dependency");
    const TCHAR* ComponentBlueprintCategory = TEXT("component_blueprint");
    const TCHAR* VariableTypeCategory = TEXT("variable_type");
    const TCHAR* PinTypeCategory = TEXT("pin_type");
    const TCHAR* MemberParentCategory = TEXT("node_member_parent");
    const TCHAR* EdgeSourceNodeCategory = TEXT("edge_source_node");
    const TCHAR* EdgeTargetNodeCategory = TEXT("edge_target_node");

    namespace
    {
        FString NormalizeReferencePath(FString Path)
        {
            Path.TrimStartAndEndInline();
            if (Path.IsEmpty())
            {
                return Path;
            }

            const int32 FirstQuote = Path.Find(TEXT("'"));
            const int32 LastQuote = Path.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            if (FirstQuote != INDEX_NONE && LastQuote > FirstQuote)
            {
                Path = Path.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
            }

            Path.TrimStartAndEndInline();
            Path.ReplaceInline(TEXT("\\"), TEXT("/"));
            const int32 SubobjectSeparator = Path.Find(TEXT(":"));
            if (SubobjectSeparator != INDEX_NONE)
            {
                Path.LeftInline(SubobjectSeparator);
            }
            return Path;
        }

        FString ReferenceToPackageName(const FString& ReferencePath)
        {
            FString Normalized = NormalizeReferencePath(ReferencePath);
            if (!Normalized.StartsWith(TEXT("/Game"), ESearchCase::IgnoreCase))
            {
                return FString();
            }

            const int32 ObjectSeparator = Normalized.Find(TEXT("."));
            if (ObjectSeparator != INDEX_NONE)
            {
                Normalized.LeftInline(ObjectSeparator);
            }
            return Normalized;
        }
    }

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

    bool ReadPositiveInteger(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* FieldName,
        const int32 DefaultValue,
        const int32 MaximumValue,
        int32& OutValue,
        FString& OutError)
    {
        double Number = DefaultValue;
        if (Params.IsValid() && Params->HasField(FieldName))
        {
            if (!Params->TryGetNumberField(FieldName, Number)
                || !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
                || Number < 1.0
                || Number > MaximumValue)
            {
                OutError = FString::Printf(TEXT("%s must be an integer from 1 to %d."), FieldName, MaximumValue);
                return false;
            }
        }

        OutValue = FMath::RoundToInt(Number);
        return true;
    }

    TSet<FString> GetAllCategories()
    {
        return {
            AssetDependencyCategory,
            ComponentBlueprintCategory,
            VariableTypeCategory,
            PinTypeCategory,
            MemberParentCategory,
            EdgeSourceNodeCategory,
            EdgeTargetNodeCategory
        };
    }

    bool ParseCategories(
        const TSharedPtr<FJsonObject>& Params,
        TSet<FString>& OutCategories,
        FString& OutError)
    {
        OutCategories = GetAllCategories();
        if (!Params.IsValid() || !Params->HasField(TEXT("categories")))
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params->TryGetArrayField(TEXT("categories"), Values) || Values == nullptr)
        {
            OutError = TEXT("categories must be an array of supported category strings.");
            return false;
        }

        const TSet<FString> ValidCategories = GetAllCategories();
        OutCategories.Reset();
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Category;
            if (!Value.IsValid() || !Value->TryGetString(Category) || !ValidCategories.Contains(Category))
            {
                OutError = FString::Printf(TEXT("Unsupported category '%s'."), *Category);
                return false;
            }
            OutCategories.Add(Category);
        }
        return true;
    }

    bool BindScope(
        FSQLitePreparedStatement& Statement,
        const FString& ObjectPath,
        const FString& RootPath)
    {
        return Statement.SetBindingValueByIndex(1, ObjectPath)
            && Statement.SetBindingValueByIndex(2, RootPath);
    }

    FString GetQueryError(FSQLiteDatabase& Database, const TCHAR* Fallback)
    {
        const FString DatabaseError = Database.GetLastError();
        return DatabaseError.IsEmpty() ? FString(Fallback) : DatabaseError;
    }

    bool IsMissingProjectReference(
        const FString& ReferencePath,
        const TSet<FString>& IndexedPackages)
    {
        const FString PackageName = ReferenceToPackageName(ReferencePath);
        return !PackageName.IsEmpty() && !IndexedPackages.Contains(PackageName);
    }

    TSharedRef<FJsonObject> SerializeFinding(const FBrokenReferenceFinding& Finding)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("category"), Finding.Category);
        Json->SetStringField(TEXT("severity"), Finding.Severity);
        Json->SetStringField(TEXT("blueprintObjectPath"), Finding.BlueprintObjectPath);
        if (!Finding.GraphName.IsEmpty())
        {
            Json->SetStringField(TEXT("graphName"), Finding.GraphName);
        }
        if (!Finding.NodeGuid.IsEmpty())
        {
            Json->SetStringField(TEXT("nodeGuid"), Finding.NodeGuid);
        }
        if (!Finding.PinId.IsEmpty())
        {
            Json->SetStringField(TEXT("pinId"), Finding.PinId);
        }
        if (!Finding.MemberName.IsEmpty())
        {
            Json->SetStringField(TEXT("memberName"), Finding.MemberName);
        }
        Json->SetStringField(TEXT("referencedPath"), Finding.ReferencedPath);
        Json->SetStringField(TEXT("evidence"), Finding.Evidence);
        Json->SetStringField(TEXT("recommendedAction"), Finding.RecommendedAction);
        return Json;
    }
}
