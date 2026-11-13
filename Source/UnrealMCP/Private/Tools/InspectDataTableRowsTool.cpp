#include "Tools/InspectDataTableRowsTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Misc/PackageName.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UnrealType.h"

namespace
{
    void InspectReferences(const FProperty* Property, const void* Value, const FString& Field,
        TArray<TSharedPtr<FJsonValue>>& Broken)
    {
        if (Broken.Num() > 200) return;
        if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(Property))
        {
            const FSoftObjectPath Path = Soft->GetPropertyValue(Value).ToSoftObjectPath();
            if (!Path.IsNull() && !Path.ResolveObject() && !Path.TryLoad(nullptr, LOAD_NoWarn))
            {
                TSharedRef<FJsonObject> Hit = MakeShared<FJsonObject>();
                Hit->SetStringField(TEXT("field"), Field);
                Hit->SetStringField(TEXT("path"), Path.ToString());
                const bool bMissingPackage = !Path.GetLongPackageName().StartsWith(TEXT("/Script/"))
                    && !FPackageName::DoesPackageExist(Path.GetLongPackageName());
                Hit->SetStringField(TEXT("reason"), bMissingPackage ? TEXT("package_missing") : TEXT("object_unresolved"));
                Broken.Add(MakeShared<FJsonValueObject>(Hit));
            }
        }
        else if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
        {
            for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
                InspectReferences(*It, It->ContainerPtrToValuePtr<void>(Value), Field + TEXT(".") + It->GetName(), Broken);
        }
        else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper Helper(Array, Value);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
                InspectReferences(Array->Inner, Helper.GetRawPtr(Index), FString::Printf(TEXT("%s[%d]"), *Field, Index), Broken);
        }
        else if (const FSetProperty* Set = CastField<FSetProperty>(Property))
        {
            FScriptSetHelper Helper(Set, Value);
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
                if (Helper.IsValidIndex(Index)) InspectReferences(Set->ElementProp, Helper.GetElementPtr(Index), FString::Printf(TEXT("%s[%d]"), *Field, Index), Broken);
        }
        else if (const FMapProperty* Map = CastField<FMapProperty>(Property))
        {
            FScriptMapHelper Helper(Map, Value);
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index)) continue;
                InspectReferences(Map->KeyProp, Helper.GetKeyPtr(Index), FString::Printf(TEXT("%s[%d].key"), *Field, Index), Broken);
                InspectReferences(Map->ValueProp, Helper.GetValuePtr(Index), FString::Printf(TEXT("%s[%d].value"), *Field, Index), Broken);
            }
        }
    }
}

FInspectDataTableRowsTool::FInspectDataTableRowsTool()
    : FMCPToolBase(TEXT("InspectDataTableRows"), TEXT("Reads paged DataTable rows as export text and reports soft references that cannot resolve, without changing the table."))
{
}

UnrealMCP::FMCPResponse FInspectDataTableRowsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("InspectDataTableRows requires objectPath."));
    double OffsetNumber = 0, LimitNumber = 20;
    Request.Params->TryGetNumberField(TEXT("offset"), OffsetNumber);
    Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
    if (!FMath::IsFinite(OffsetNumber) || !FMath::IsFinite(LimitNumber) || OffsetNumber < 0 || OffsetNumber > MAX_int32 - 100 || LimitNumber < 1 || LimitNumber > 100)
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("offset must be 0 to 2147483547 and limit must be 1 to 100."));
    const int32 Offset = FMath::RoundToInt(OffsetNumber);
    const int32 Limit = FMath::RoundToInt(LimitNumber);
    TArray<TSharedPtr<FJsonValue>> Rows;
    FString RowStructPath, Error;
    int32 TotalRows = 0;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UDataTable* Table = LoadObject<UDataTable>(nullptr, *ObjectPath);
        if (!Table || !Table->GetRowStruct()) { OutError = TEXT("DataTable or its row struct was not found."); return false; }
        RowStructPath = Table->GetRowStruct()->GetPathName();
        TArray<FName> Names = Table->GetRowNames();
        Names.Sort(FNameLexicalLess());
        TotalRows = Names.Num();
        for (int32 Index = Offset; Index < FMath::Min(Offset + Limit, TotalRows); ++Index)
        {
            const uint8* RowData = Table->FindRowUnchecked(Names[Index]);
            if (!RowData) continue;
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Names[Index].ToString());
            TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> Broken;
            TArray<TSharedPtr<FJsonValue>> TruncatedFields;
            for (TFieldIterator<FProperty> It(Table->GetRowStruct()); It; ++It)
            {
                const void* Value = It->ContainerPtrToValuePtr<void>(RowData);
                FString Exported;
                It->ExportTextItem_Direct(Exported, Value, nullptr, nullptr, PPF_None);
                if (Exported.Len() > 8192)
                {
                    Exported.LeftInline(8192);
                    TruncatedFields.Add(MakeShared<FJsonValueString>(It->GetName()));
                }
                Fields->SetStringField(It->GetName(), Exported);
                InspectReferences(*It, Value, It->GetName(), Broken);
            }
            Row->SetObjectField(TEXT("fields"), Fields);
            Row->SetArrayField(TEXT("truncatedFields"), TruncatedFields);
            Row->SetBoolField(TEXT("unresolvedReferencesTruncated"), Broken.Num() > 200);
            if (Broken.Num() > 200) Broken.SetNum(200);
            Row->SetArrayField(TEXT("unresolvedReferences"), Broken);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        return true;
    }, Error);
    if (!bOk) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("rowStructPath"), RowStructPath);
    Result->SetNumberField(TEXT("totalRows"), TotalRows);
    Result->SetNumberField(TEXT("offset"), Offset);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetStringField(TEXT("referenceCheckScope"), TEXT("soft_references_resolved_or_loaded"));
    Result->SetBoolField(TEXT("hasMore"), Offset + Rows.Num() < TotalRows);
    Result->SetArrayField(TEXT("rows"), Rows);
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FInspectDataTableRowsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("objectPath"), TEXT("offset"), TEXT("limit")})
    {
        TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), FCString::Strcmp(FieldName, TEXT("objectPath")) == 0 ? TEXT("string") : TEXT("integer"));
        Properties->SetObjectField(FieldName, Property);
    }
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath"))});
    return Schema;
}
