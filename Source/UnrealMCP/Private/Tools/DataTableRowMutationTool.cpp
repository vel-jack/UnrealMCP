#include "Tools/DataTableRowMutationTool.h"

#include "DataTableEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
    FProperty* FindRowField(const UScriptStruct* RowStruct, const TCHAR* Name, FString& Error)
    {
        FProperty* Match = nullptr;
        for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
        {
            if (!It->GetName().Equals(Name) && !RowStruct->GetAuthoredNameForField(*It).Equals(Name)) continue;
            if (Match && Match != *It)
            {
                Error = FString::Printf(TEXT("Ambiguous row field name: %s"), Name);
                return nullptr;
            }
            Match = *It;
        }
        if (!Match) Error = FString::Printf(TEXT("Unknown row field: %s"), Name);
        return Match;
    }

    const TCHAR* ToolName(FDataTableRowMutationTool::EAction Action)
    {
        switch (Action)
        {
        case FDataTableRowMutationTool::EAction::Set: return TEXT("SetDataTableRowFields");
        case FDataTableRowMutationTool::EAction::Add: return TEXT("AddDataTableRow");
        default: return TEXT("RemoveDataTableRow");
        }
    }

    bool PrepareFields(const UScriptStruct* RowStruct, const TSharedPtr<FJsonObject>& Fields,
        uint8* Preview, bool bAllowEmpty, FString& Error)
    {
        if (!Fields.IsValid() || (!bAllowEmpty && Fields->Values.IsEmpty())) { Error = TEXT("fields must contain at least one property."); return false; }
        TSet<const FProperty*> Seen;
        for (const auto& Pair : Fields->Values)
        {
            FProperty* Property = FindRowField(RowStruct, *Pair.Key, Error);
            if (!Property) return false;
            if (Seen.Contains(Property)) { Error = FString::Printf(TEXT("Field supplied twice: %s"), *Pair.Key); return false; }
            Seen.Add(Property);
            if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
            {
                Error = FString::Printf(TEXT("Unknown field or non-string export text: %s"), *Pair.Key);
                return false;
            }
            const FString Value = Pair.Value->AsString();
            void* Destination = Property->ContainerPtrToValuePtr<void>(Preview);
            const TCHAR* End = Property->ImportText_Direct(*Value, Destination, nullptr, PPF_None);
            if (!End || *End != 0)
            {
                Error = FString::Printf(TEXT("Invalid export text for field %s."), *Pair.Key);
                return false;
            }
        }
        return true;
    }
}

FDataTableRowMutationTool::FDataTableRowMutationTool(EAction InAction)
    : FMCPToolBase(ToolName(InAction), TEXT("Transactionally edits one DataTable row in memory; leaves the asset dirty for an explicit save.")), Action(InAction)
{
}

UnrealMCP::FMCPResponse FDataTableRowMutationTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, RowName;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty()
        || !Request.Params->TryGetStringField(TEXT("rowName"), RowName) || RowName.IsEmpty() || RowName == TEXT("None"))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("objectPath and a nonempty rowName are required."));
    bool bDryRun = false, bConfirm = false;
    Request.Params->TryGetBoolField(TEXT("dryRun"), bDryRun);
    Request.Params->TryGetBoolField(TEXT("confirm"), bConfirm);
    if (Action == EAction::Remove && !bDryRun && !bConfirm)
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("RemoveDataTableRow requires confirm=true unless dryRun=true."));
    const TSharedPtr<FJsonObject>* Fields = nullptr;
    if (Action != EAction::Remove && (!Request.Params->TryGetObjectField(TEXT("fields"), Fields) || !Fields->IsValid()))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("fields must be an object of property names to export-text strings."));

    bool bChanged = false;
    TSharedRef<FJsonObject> Previous = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Current = MakeShared<FJsonObject>();
    FString Error;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UDataTable* Table = LoadObject<UDataTable>(nullptr, *ObjectPath);
        if (!Table || !Table->GetRowStruct()) { OutError = TEXT("DataTable or row struct not found."); return false; }
        const FName Key(*RowName);
        uint8* Existing = Table->FindRowUnchecked(Key);
        if (Action == EAction::Add && Existing) { OutError = TEXT("Row already exists."); return false; }
        if (Action != EAction::Add && !Existing) { OutError = TEXT("Row not found."); return false; }
        if (Action == EAction::Remove)
        {
            bChanged = true;
            if (!bDryRun)
            {
                if (!FDataTableEditorUtils::RemoveRow(Table, Key)) { OutError = TEXT("Row removal failed."); return false; }
            }
            return true;
        }

        FStructOnScope Preview(Table->GetRowStruct());
        if (Existing) Table->GetRowStruct()->CopyScriptStruct(Preview.GetStructMemory(), Existing);
        for (const auto& Pair : (*Fields)->Values)
        {
            FString FieldError;
            if (FProperty* Property = FindRowField(Table->GetRowStruct(), *Pair.Key, FieldError))
            {
                if (Existing)
                {
                    FString Text;
                    Property->ExportTextItem_Direct(Text, Property->ContainerPtrToValuePtr<void>(Existing), nullptr, nullptr, PPF_None);
                    Previous->SetStringField(Pair.Key, Text);
                }
            }
        }
        if (!PrepareFields(Table->GetRowStruct(), *Fields, Preview.GetStructMemory(), Action == EAction::Add, OutError)) return false;
        bChanged = !Existing;
        for (const auto& Pair : (*Fields)->Values)
        {
            FString FieldError;
            FProperty* Property = FindRowField(Table->GetRowStruct(), *Pair.Key, FieldError);
            FString Text;
            Property->ExportTextItem_Direct(Text, Property->ContainerPtrToValuePtr<void>(Preview.GetStructMemory()), nullptr, nullptr, PPF_None);
            Current->SetStringField(Pair.Key, Text);
            bChanged |= !Existing || Previous->GetStringField(Pair.Key) != Text;
        }
        if (bDryRun || !bChanged) return true;
        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "EditDataTableRow", "UnrealMCP Edit DataTable Row"));
        Table->Modify();
        FDataTableEditorUtils::BroadcastPreChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
        Table->AddRow(Key, Preview.GetStructMemory(), Table->GetRowStruct());
        FDataTableEditorUtils::BroadcastPostChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
        return true;
    }, Error);
    if (!bOk) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("rowName"), RowName);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("changed"), bChanged && !bDryRun);
    Result->SetBoolField(TEXT("wouldChange"), bChanged);
    Result->SetBoolField(TEXT("saved"), false);
    Result->SetObjectField(TEXT("previousFields"), Previous);
    Result->SetObjectField(TEXT("fields"), Current);
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FDataTableRowMutationTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    auto String = []() { TSharedRef<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("type"), TEXT("string")); return P; };
    Properties->SetObjectField(TEXT("objectPath"), String());
    Properties->SetObjectField(TEXT("rowName"), String());
    TArray<TSharedPtr<FJsonValue>> Required = {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("rowName"))};
    if (Action != EAction::Remove)
    {
        TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>(); Fields->SetStringField(TEXT("type"), TEXT("object")); Fields->SetObjectField(TEXT("additionalProperties"), String());
        Properties->SetObjectField(TEXT("fields"), Fields);
        Required.Add(MakeShared<FJsonValueString>(TEXT("fields")));
    }
    for (const TCHAR* FieldName : {TEXT("dryRun"), TEXT("confirm")})
    {
        TSharedRef<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("type"), TEXT("boolean")); Properties->SetObjectField(FieldName, P);
    }
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
