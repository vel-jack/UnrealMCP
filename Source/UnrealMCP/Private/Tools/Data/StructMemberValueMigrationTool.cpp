#include "Tools/Data/StructMemberValueMigrationTool.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace StructMemberMigration
{
    namespace Edit = UnrealMCP::BlueprintEditToolUtils;
    constexpr int32 MaxRoots = 10000, MaxVisits = 100000, MaxBackupBytes = 16 * 1024 * 1024;

    FString FullExport(FProperty* P, const void* Data, bool bDelimited, const UScriptStruct* SkipStruct = nullptr, FName SkipField = NAME_None);
    FString ExportStruct(UScriptStruct* S, const void* Data, const UScriptStruct* SkipStruct = nullptr, FName SkipField = NAME_None)
    {
        // Unreal's map exporter omits zero fields in struct keys even in external-editor mode.
        // Emit complete tagged struct fields so restoring never depends on today's defaults.
        if (S->StructFlags & STRUCT_ExportTextItemNative)
        {
            FString Text; S->ExportText(Text, Data, Data, nullptr, PPF_ExternalEditor, nullptr); return Text;
        }
        TArray<FString> Fields;
        for (TFieldIterator<FProperty> It(S); It; ++It) if (It->ShouldPort(PPF_ExternalEditor) && !(S == SkipStruct && It->GetFName() == SkipField))
            for (int32 I = 0; I < It->ArrayDim; ++I)
                Fields.Add(It->GetAuthoredName() + (It->ArrayDim > 1 ? FString::Printf(TEXT("[%d]"), I) : FString()) + TEXT("=")
                    + FullExport(*It, It->ContainerPtrToValuePtr<void>(Data, I), true, SkipStruct, SkipField));
        return TEXT("(") + FString::Join(Fields, TEXT(",")) + TEXT(")");
    }
    FString FullExport(FProperty* P, const void* Data, bool bDelimited, const UScriptStruct* SkipStruct, FName SkipField)
    {
        if (auto* S = CastField<FStructProperty>(P)) return ExportStruct(S->Struct, Data, SkipStruct, SkipField);
        TArray<FString> Items;
        if (auto* A = CastField<FArrayProperty>(P))
        {
            FScriptArrayHelper H(A, Data);
            for (int32 I = 0; I < H.Num(); ++I) Items.Add(FullExport(A->Inner, H.GetRawPtr(I), true, SkipStruct, SkipField));
        }
        else if (auto* S = CastField<FSetProperty>(P))
        {
            FScriptSetHelper H(S, Data);
            for (int32 I = 0; I < H.GetMaxIndex(); ++I) if (H.IsValidIndex(I)) Items.Add(FullExport(S->ElementProp, H.GetElementPtr(I), true, SkipStruct, SkipField));
            Items.Sort();
        }
        else if (auto* M = CastField<FMapProperty>(P))
        {
            FScriptMapHelper H(M, Data);
            for (int32 I = 0; I < H.GetMaxIndex(); ++I) if (H.IsValidIndex(I))
                Items.Add(TEXT("(") + FullExport(M->KeyProp, H.GetKeyPtr(I), true, SkipStruct, SkipField) + TEXT(",") + FullExport(M->ValueProp, H.GetValuePtr(I), true, SkipStruct, SkipField) + TEXT(")"));
            Items.Sort();
        }
        else
        {
            if (auto* D = CastField<FDoubleProperty>(P)) return FString::Printf(TEXT("%.17g"), D->GetPropertyValue(Data));
            if (auto* F = CastField<FFloatProperty>(P)) return FString::Printf(TEXT("%.9g"), static_cast<double>(F->GetPropertyValue(Data)));
            FString Text; P->ExportTextItem_Direct(Text, Data, Data, nullptr, PPF_ExternalEditor | (bDelimited ? PPF_Delimited : 0)); return Text;
        }
        return TEXT("(") + FString::Join(Items, TEXT(",")) + TEXT(")");
    }
    FString Export(FProperty* P, const void* Data) { return FullExport(P, Data, false); }
    FString Layout(UUserDefinedStruct* S)
    {
        FString Text;
        for (const auto& V : FStructureEditorUtils::GetVarDesc(S))
        {
            const FProperty* P = FStructureEditorUtils::GetPropertyByGuid(S, V.VarGuid);
            Text += V.VarGuid.ToString() + TEXT(":") + V.VarName.ToString() + TEXT(":") + (P ? P->GetCPPType() : TEXT("invalid")) + TEXT(";");
        }
        return Text;
    }
    FGuid Member(UUserDefinedStruct* S, const FString& Selector)
    {
        FGuid Id; if (FGuid::Parse(Selector, Id)) return Id;
        for (const auto& V : FStructureEditorUtils::GetVarDesc(S))
            if (V.FriendlyName == Selector || V.VarName == FName(*Selector)) return V.VarGuid;
        return FGuid();
    }
    FString Directory() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealMCP/FieldMigrations")); }
    bool SaveJson(const FString& File, const TSharedRef<FJsonObject>& Json, FString& Error)
    {
        FString Text;
        if (!FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text)) || FTCHARToUTF8(*Text).Length() > MaxBackupBytes)
        { Error = TEXT("Backup exceeds 16 MiB; split the migration scope."); return false; }
        if (!IFileManager::Get().MakeDirectory(*Directory(), true)
            || !FFileHelper::SaveStringToFile(Text, *(File + TEXT(".tmp")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            || !IFileManager::Get().Move(*File, *(File + TEXT(".tmp")), true, false, false, true))
        { Error = TEXT("Cannot persist the backup; no migration allowed."); return false; }
        FString Check;
        if (!FFileHelper::LoadFileToString(Check, *File) || Check != Text) { Error = TEXT("Backup read-back failed."); return false; }
        return true;
    }
    bool ReadPaths(const FJsonObject& J, TArray<FString>& Paths)
    {
        const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
        if (!J.TryGetArrayField(TEXT("assetPaths"), A) || A->Num() > 256) return false;
        for (const auto& V : *A)
        {
            FString P;
            if (!V || !V->TryGetString(P) || !P.StartsWith(TEXT("/Game/")) || Paths.Contains(P)) return false;
            Paths.Add(P);
        }
        return true;
    }
    TArray<FName> Dirty()
    {
        TArray<UPackage*> Content, Worlds; FEditorFileUtils::GetDirtyContentPackages(Content); FEditorFileUtils::GetDirtyWorldPackages(Worlds); Content.Append(Worlds);
        TArray<FName> Names; for (const UPackage* P : Content) if (P) Names.AddUnique(P->GetFName()); return Names;
    }
    TArray<TSharedPtr<FJsonValue>> JsonNames(const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> A; for (FName N : Names) A.Add(MakeShared<FJsonValueString>(N.ToString())); return A;
    }
    struct FBuffer
    {
        FProperty* P; void* Data;
        FBuffer(FProperty* InP, const void* Original) : P(InP)
        { Data = FMemory::Malloc(P->GetSize(), P->GetMinAlignment()); P->InitializeValue(Data); P->CopyCompleteValue(Data, Original); }
        ~FBuffer() { P->DestroyValue(Data); FMemory::Free(Data); }
    };
    bool RoundTrips(FProperty* P, UScriptStruct* S, const void* Expected, const FString& Text, UObject* Owner)
    {
        FOutputDeviceNull Null;
        if (P)
        {
            FBuffer Buffer(P, Expected);
            const TCHAR* End = P->ImportText_Direct(*Text, Buffer.Data, Owner, PPF_ExternalEditor, &Null);
            return End && !*End && P->Identical(Expected, Buffer.Data);
        }
        FStructOnScope Buffer(S); S->CopyScriptStruct(Buffer.GetStructMemory(), Expected);
        const TCHAR* End = S->ImportText(*Text, Buffer.GetStructMemory(), Owner, PPF_ExternalEditor, &Null, S->GetName());
        return End && !*End && S->CompareScriptStruct(Expected, Buffer.GetStructMemory(), PPF_None);
    }
    // Preview changes on detached data, including rehash and collision validation.
    struct FWalker
    {
        UUserDefinedStruct* Target; FProperty* Source; FProperty* Destination;
        FString DefaultDestination, Error;
        bool bOverwrite = false;
        int32 Visits = 0;
        TArray<TSharedPtr<FJsonValue>> Values;
        TArray<FName> OpaqueTypes;
        bool Structure(UScriptStruct* S, void* Data, const FString& Path, int32 Depth)
        {
            if (++Visits > MaxVisits || Depth > 32) { Error = TEXT("Traversal limit reached; split the scope."); return false; }
            if (S->PropertyLink == nullptr && (S->StructFlags & STRUCT_SerializeNative))
            {
                OpaqueTypes.AddUnique(FName(*S->GetPathName()));
                return true;
            }
            const int32 ValuesBefore = Values.Num();
            if (S == Target)
            {
                FString Value = Export(Source, Source->ContainerPtrToValuePtr<void>(Data));
                FString Before = Export(Destination, Destination->ContainerPtrToValuePtr<void>(Data));
                if (!bOverwrite && Before != Value && Before != DefaultDestination)
                { Error = TEXT("Conflicting destination at ") + Path + TEXT("; overwriteDestination=true is required to replace non-default values."); return false; }
                FOutputDeviceNull Null;
                const TCHAR* End = Destination->ImportText_Direct(*Value, Destination->ContainerPtrToValuePtr<void>(Data), nullptr, PPF_ExternalEditor, &Null);
                if (!End || *End || Export(Destination, Destination->ContainerPtrToValuePtr<void>(Data)) != Value
                    || !Source->Identical(Source->ContainerPtrToValuePtr<void>(Data), Destination->ContainerPtrToValuePtr<void>(Data)))
                { Error = TEXT("Field value failed round-trip at ") + Path; return false; }
                auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("location"), Path); J->SetStringField(TEXT("sourceValue"), Value); J->SetStringField(TEXT("destinationBefore"), Before);
                Values.Add(MakeShared<FJsonValueObject>(J));
            }
            for (TFieldIterator<FProperty> It(S); It; ++It)
                if (!Property(*It, It->ContainerPtrToValuePtr<void>(Data), Path + TEXT(".") + It->GetName(), Depth + 1)) return false;
            if ((S->StructFlags & STRUCT_ExportTextItemNative) && Values.Num() > ValuesBefore)
            { Error = TEXT("Target occurrences inside a custom native text-export struct are unsupported: ") + Path; return false; }
            return true;
        }
        bool Property(FProperty* P, void* Data, const FString& Path, int32 Depth)
        {
            if (P->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) return true;
            if (++Visits > MaxVisits || Depth > 32) { Error = TEXT("Traversal limit reached; split the scope."); return false; }
            for (int32 I = 0; I < P->ArrayDim; ++I)
            {
                void* Item = static_cast<uint8*>(Data) + I * (P->GetSize() / P->ArrayDim);
                if (auto* S = CastField<FStructProperty>(P)) { if (!Structure(S->Struct, Item, Path, Depth + 1)) return false; }
                if (auto* A = CastField<FArrayProperty>(P))
                {
                    FScriptArrayHelper H(A, Item);
                    for (int32 J = 0; J < H.Num(); ++J)
                        if (!Property(A->Inner, H.GetRawPtr(J), Path + FString::Printf(TEXT("[%d]"), J), Depth + 1)) return false;
                }
                if (auto* S = CastField<FSetProperty>(P))
                {
                    FScriptSetHelper H(S, Item);
                    if (H.GetMaxIndex() > 2048) { Error = TEXT("Set exceeds collision-check limit; split the scope."); return false; }
                    for (int32 J = 0; J < H.GetMaxIndex(); ++J) if (H.IsValidIndex(J))
                        if (!Property(S->ElementProp, H.GetElementPtr(J), Path + FString::Printf(TEXT("{set:%d}"), J), Depth + 1)) return false;
                    for (int32 J = 0; J < H.GetMaxIndex(); ++J) if (H.IsValidIndex(J))
                        for (int32 K = J + 1; K < H.GetMaxIndex(); ++K) if (H.IsValidIndex(K) && S->ElementProp->Identical(H.GetElementPtr(J), H.GetElementPtr(K)))
                        { Error = TEXT("Migration would collapse set entries at ") + Path; return false; }
                    H.Rehash();
                }
                if (auto* M = CastField<FMapProperty>(P))
                {
                    FScriptMapHelper H(M, Item);
                    if (H.GetMaxIndex() > 2048) { Error = TEXT("Map exceeds collision-check limit; split the scope."); return false; }
                    for (int32 J = 0; J < H.GetMaxIndex(); ++J) if (H.IsValidIndex(J))
                        if (!Property(M->KeyProp, H.GetKeyPtr(J), Path + FString::Printf(TEXT("{key:%d}"), J), Depth + 1)
                            || !Property(M->ValueProp, H.GetValuePtr(J), Path + FString::Printf(TEXT("{value:%d}"), J), Depth + 1)) return false;
                    for (int32 J = 0; J < H.GetMaxIndex(); ++J) if (H.IsValidIndex(J))
                        for (int32 K = J + 1; K < H.GetMaxIndex(); ++K) if (H.IsValidIndex(K) && M->KeyProp->Identical(H.GetKeyPtr(J), H.GetKeyPtr(K)))
                        { Error = TEXT("Migration would collapse map keys at ") + Path; return false; }
                    H.Rehash();
                }
            }
            return true;
        }
    };
    struct FRoot
    {
        FString AssetPath, Row, PropertyName, Type, Before, After, AfterRemoval;
        TArray<TSharedPtr<FJsonValue>> Values;
        bool Resolve(UObject*& Owner, FProperty*& P, UScriptStruct*& S, void*& Data, FString& Error) const
        {
            Owner = LoadObject<UObject>(nullptr, *AssetPath);
            if (auto* BP = Cast<UBlueprint>(Owner))
            {
                P = BP->GeneratedClass ? FindFProperty<FProperty>(BP->GeneratedClass, *PropertyName) : nullptr;
                if (!P || P->GetCPPType() != Type) { Error = FString::Printf(TEXT("Blueprint property type/name changed: %s.%s expected %s, found %s"), *AssetPath, *PropertyName, *Type, P ? *P->GetCPPType() : TEXT("missing")); return false; }
                Data = P->ContainerPtrToValuePtr<void>(BP->GeneratedClass->GetDefaultObject());
            }
            else if (auto* Table = Cast<UDataTable>(Owner))
            {
                S = const_cast<UScriptStruct*>(Table->GetRowStruct());
                uint8* const* Value = Table->GetRowMap().Find(FName(*Row));
                if (Table->GetClass() != UDataTable::StaticClass() || !S || S->GetPathName() != Type || !Value)
                { Error = TEXT("DataTable row/type changed or unsupported derived table."); return false; }
                Data = *Value;
            }
            else { Error = TEXT("Only Blueprint class defaults and ordinary DataTables are supported."); return false; }
            return true;
        }
        bool Read(FString& Text, FString& Error) const
        {
            UObject* O = nullptr; FProperty* P = nullptr; UScriptStruct* S = nullptr; void* Data = nullptr;
            if (!Resolve(O, P, S, Data, Error)) return false;
            Text = P ? Export(P, Data) : ExportStruct(S, Data); return true;
        }
        bool Import(const FString& Text, bool bPreview, FString& Error) const
        {
            UObject* O = nullptr; FProperty* P = nullptr; UScriptStruct* S = nullptr; void* Data = nullptr;
            if (!Resolve(O, P, S, Data, Error)) return false;
            FOutputDeviceNull Null;
            if (P)
            {
                FBuffer Buffer(P, Data); void* Target = bPreview ? Buffer.Data : Data;
                const TCHAR* End = P->ImportText_Direct(*Text, Target, O, PPF_ExternalEditor, &Null);
                if (End && !*End && Export(P, Target) == Text) return true;
            }
            else
            {
                FStructOnScope Buffer(S); void* Target = bPreview ? Buffer.GetStructMemory() : Data;
                const TCHAR* End = S->ImportText(*Text, Target, O, PPF_ExternalEditor, &Null, S->GetName());
                if (End && !*End && ExportStruct(S, Target) == Text) return true;
            }
            Error = TEXT("Root value failed complete import/round-trip: ") + AssetPath + TEXT(" property=") + PropertyName + TEXT(" row=") + Row; return false;
        }
        TSharedRef<FJsonObject> Json() const
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("assetPath"), AssetPath); J->SetStringField(TEXT("row"), Row); J->SetStringField(TEXT("property"), PropertyName);
            J->SetStringField(TEXT("afterSourceRemoval"), AfterRemoval); J->SetStringField(TEXT("type"), Type); J->SetStringField(TEXT("before"), Before); J->SetStringField(TEXT("after"), After); J->SetArrayField(TEXT("values"), Values); return J;
        }
    };
    bool Prepare(const TArray<FString>& Paths, FWalker& W, TArray<FRoot>& Roots, FString& Error)
    {
        for (const FString& Path : Paths)
        {
            UObject* O = LoadObject<UObject>(nullptr, *Path);
            if (auto* T = Cast<UDataTable>(O))
            {
                if (T->GetClass() != UDataTable::StaticClass() || !T->GetRowStruct()) { Error = TEXT("Unsupported DataTable type."); return false; }
                auto* S = const_cast<UScriptStruct*>(T->GetRowStruct());
                TArray<FName> Rows = T->GetRowNames(); Rows.Sort(FNameLexicalLess());
                for (FName Row : Rows)
                {
                    FRoot R; R.AssetPath = Path; R.Row = Row.ToString(); R.Type = S->GetPathName();
                    FStructOnScope Preview(S); S->CopyScriptStruct(Preview.GetStructMemory(), T->GetRowMap().FindChecked(Row));
                    R.Before = ExportStruct(S, Preview.GetStructMemory()); W.Values.Reset();
                    if (!W.Structure(S, Preview.GetStructMemory(), R.Row, 0)) { Error = W.Error; return false; }
                    if (!W.Values.IsEmpty() && (!RoundTrips(nullptr, S, T->GetRowMap().FindChecked(Row), R.Before, T)
                        || !RoundTrips(nullptr, S, Preview.GetStructMemory(), ExportStruct(S, Preview.GetStructMemory()), T)))
                    { Error = TEXT("Row values cannot be backed up losslessly: ") + Path + TEXT(":") + Row.ToString(); return false; }
                    if (!W.Values.IsEmpty()) { R.After = ExportStruct(S, Preview.GetStructMemory()); R.AfterRemoval = ExportStruct(S, Preview.GetStructMemory(), W.Target, W.Source->GetFName()); R.Values = MoveTemp(W.Values); Roots.Add(MoveTemp(R)); }
                    if (Roots.Num() > MaxRoots) { Error = TEXT("Too many roots; split the scope."); return false; }
                }
            }
            else if (auto* BP = Cast<UBlueprint>(O))
            {
                if (!BP->GeneratedClass || (BP->Status != BS_UpToDate && BP->Status != BS_UpToDateWithWarnings))
                { Error = TEXT("Compile the Blueprint before migration: ") + Path; return false; }
                for (TFieldIterator<FProperty> It(BP->GeneratedClass); It; ++It)
                {
                    FProperty* P = *It; if (P->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
                    FRoot R; R.AssetPath = Path; R.PropertyName = P->GetName(); R.Type = P->GetCPPType();
                    FBuffer Preview(P, P->ContainerPtrToValuePtr<void>(BP->GeneratedClass->GetDefaultObject())); R.Before = Export(P, Preview.Data); W.Values.Reset();
                    if (!W.Property(P, Preview.Data, R.PropertyName, 0)) { Error = W.Error; return false; }
                    if (!W.Values.IsEmpty() && P->ArrayDim != 1) { Error = TEXT("Target occurrences in native fixed-size root arrays are unsupported."); return false; }
                    if (!W.Values.IsEmpty() && (!RoundTrips(P, nullptr, P->ContainerPtrToValuePtr<void>(BP->GeneratedClass->GetDefaultObject()), R.Before, BP->GeneratedClass->GetDefaultObject())
                        || !RoundTrips(P, nullptr, Preview.Data, Export(P, Preview.Data), BP->GeneratedClass->GetDefaultObject())))
                    { Error = TEXT("Property values cannot be backed up losslessly: ") + Path + TEXT(":") + P->GetName(); return false; }
                    if (!W.Values.IsEmpty()) { R.After = Export(P, Preview.Data); R.AfterRemoval = FullExport(P, Preview.Data, false, W.Target, W.Source->GetFName()); R.Values = MoveTemp(W.Values); Roots.Add(MoveTemp(R)); }
                    if (Roots.Num() > MaxRoots) { Error = TEXT("Too many roots; split the scope."); return false; }
                }
            }
            else { Error = TEXT("Unsupported migration asset: ") + Path; return false; }
        }
        return true;
    }
}

FStructMemberValueMigrationTool::FStructMemberValueMigrationTool(bool bInRestore)
    : FMCPToolBase(bInRestore ? TEXT("RestoreStructMemberValueBackup") : TEXT("MigrateUserDefinedStructMemberValues"),
        bInRestore ? TEXT("Restores live value snapshots from a durable migration backup in the same project and unchanged struct layout. Refuses stale roots. Requires confirm outside dryRun; save=true compiles, saves and reload-verifies explicit assets. Never deletes a field.")
        : TEXT("Copies a user-defined struct member to an explicitly selected existing member of exactly the same type. Covers struct defaults plus explicit /Game DataTable rows and Blueprint class defaults, recursively through structs, arrays, sets and map keys/values. Rejects destination conflicts/key collisions on detached previews. Writes a durable backup before editing. save=true compiles, saves and reload-verifies. Source removal is opt-in and requires save/reload verification plus complete indexed scope and no struct graph pins. Graph literals and placed instances are excluded.")), bRestore(bInRestore) {}

UnrealMCP::FMCPResponse FStructMemberValueMigrationTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    using namespace StructMemberMigration;
    if (!Request.Params) return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Parameters required."));
    const bool bDry = Edit::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = Edit::GetOptionalBool(Request.Params, TEXT("save"), false);
    const bool bRemove = !bRestore && Edit::GetOptionalBool(Request.Params, TEXT("removeSource"), false);
    if (bRemove && !bSave) return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("removeSource requires save=true for verified migration before deletion."));
    if (!bDry && !Edit::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Apply requires confirm=true; inspect a dry run first."));
    auto Result = BuildBooleanResult(true);
    Result->SetBoolField(TEXT("dryRun"), bDry); Result->SetBoolField(TEXT("sourceRemoved"), false);
    Result->SetBoolField(TEXT("sourceRemovalRequiresReview"), true);
    Result->SetStringField(TEXT("coverage"), TEXT("Struct default and explicit asset DataTable rows / Blueprint class defaults. Excludes graph literals, local-variable defaults, placed instances and unlisted assets."));
    FString Error;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        FString Path, SourceSelector, DestinationSelector, BackupId, OldDefault, NewDefault, ExpectedLayout;
        TArray<FString> Paths;
        TArray<FRoot> Roots;
        TSharedPtr<FJsonObject> Manifest;
        FGuid SourceId, DestinationId;
        if (bRestore)
        {
            FGuid Id;
            if (!Request.Params->TryGetStringField(TEXT("backupId"), BackupId) || !FGuid::Parse(BackupId, Id))
            { OutError = TEXT("backupId must be a migration backup GUID."); return false; }
            BackupId = Id.ToString(EGuidFormats::DigitsWithHyphensLower);
            FString Text;
            const FString File = Directory() / (BackupId + TEXT(".json"));
            const int64 Size = IFileManager::Get().FileSize(*File);
            if (Size < 0 || Size > MaxBackupBytes || !FFileHelper::LoadFileToString(Text, *File)
                || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Manifest) || !Manifest)
            { OutError = TEXT("Missing, oversized or invalid backup."); return false; }
            FString Project, SourceGuid, DestinationGuid;
            int32 Version = 0;
            if (!Manifest->TryGetNumberField(TEXT("formatVersion"), Version) || Version != 1
                || !Manifest->TryGetStringField(TEXT("projectFile"), Project) || Project != FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())
                || !Manifest->TryGetStringField(TEXT("objectPath"), Path)
                || !Manifest->TryGetStringField(TEXT("layout"), ExpectedLayout)
                || !Manifest->TryGetStringField(TEXT("sourceGuid"), SourceGuid) || !FGuid::Parse(SourceGuid, SourceId)
                || !Manifest->TryGetStringField(TEXT("destinationGuid"), DestinationGuid) || !FGuid::Parse(DestinationGuid, DestinationId)
                || !Manifest->TryGetStringField(TEXT("defaultBefore"), OldDefault)
                || !Manifest->TryGetStringField(TEXT("defaultAfter"), NewDefault) || !ReadPaths(*Manifest, Paths))
            { OutError = TEXT("Invalid backup or different project."); return false; }
            const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
            if (!Manifest->TryGetArrayField(TEXT("roots"), Entries) || Entries->Num() > MaxRoots)
            { OutError = TEXT("Invalid backup roots."); return false; }
            TSet<FString> Identities;
            for (const auto& E : *Entries)
            {
                const TSharedPtr<FJsonObject>* J = nullptr;
                FRoot R;
                if (!E || !E->TryGetObject(J) || !J->IsValid()
                    || !(*J)->TryGetStringField(TEXT("assetPath"), R.AssetPath) || !Paths.Contains(R.AssetPath)
                    || !(*J)->TryGetStringField(TEXT("row"), R.Row) || !(*J)->TryGetStringField(TEXT("property"), R.PropertyName)
                    || (R.Row.IsEmpty() == R.PropertyName.IsEmpty())
                    || !(*J)->TryGetStringField(TEXT("type"), R.Type) || !(*J)->TryGetStringField(TEXT("before"), R.Before)
                    || !(*J)->TryGetStringField(TEXT("after"), R.After))
                { OutError = TEXT("Invalid root in backup."); return false; }
                const FString Key = R.AssetPath + TEXT(":") + R.Row + TEXT(":") + R.PropertyName;
                if (Identities.Contains(Key)) { OutError = TEXT("Duplicate backup root."); return false; }
                Identities.Add(Key); Roots.Add(MoveTemp(R));
            }
        }
        else if (!Request.Params->TryGetStringField(TEXT("objectPath"), Path)
            || !Request.Params->TryGetStringField(TEXT("sourceMember"), SourceSelector)
            || !Request.Params->TryGetStringField(TEXT("destinationMember"), DestinationSelector)
            || !ReadPaths(*Request.Params, Paths))
        { OutError = TEXT("Requires objectPath, sourceMember, destinationMember and assetPaths (empty means struct defaults only)."); return false; }
        if (!Path.StartsWith(TEXT("/Game/"))) { OutError = TEXT("Struct must be a /Game asset in the selected project."); return false; }
        UUserDefinedStruct* Struct = LoadObject<UUserDefinedStruct>(nullptr, *Path);
        if (!Struct || Struct->Status != UDSS_UpToDate) { OutError = TEXT("Target must be a compiled user-defined struct."); return false; }
        if (bRestore && Layout(Struct) != ExpectedLayout)
        { OutError = TEXT("Struct layout changed. Restore the original schema first; this tool does not guess mappings into changed layouts."); return false; }
        if (!bRestore) { SourceId = Member(Struct, SourceSelector); DestinationId = Member(Struct, DestinationSelector); }
        FProperty* Source = FStructureEditorUtils::GetPropertyByGuid(Struct, SourceId);
        FProperty* Destination = FStructureEditorUtils::GetPropertyByGuid(Struct, DestinationId);
        if (!Source || !Destination || SourceId == DestinationId || !Source->SameType(Destination))
        { OutError = TEXT("Choose distinct existing source/destination members with exactly matching types. No implicit conversion."); return false; }
        FWalker Walker;
        Walker.Target = Struct; Walker.Source = Source; Walker.Destination = Destination;
        Walker.DefaultDestination = Export(Destination, Destination->ContainerPtrToValuePtr<void>(Struct->GetDefaultInstance()));
        Walker.bOverwrite = Edit::GetOptionalBool(Request.Params, TEXT("overwriteDestination"), false);
        if (!bRestore)
        {
            OldDefault = Walker.DefaultDestination;
            NewDefault = Export(Source, Source->ContainerPtrToValuePtr<void>(Struct->GetDefaultInstance()));
            if (!Prepare(Paths, Walker, Roots, OutError)) return false;
        }
        else
        {
            if (Walker.DefaultDestination != OldDefault && Walker.DefaultDestination != NewDefault)
            { OutError = TEXT("Struct destination default changed since migration; refusing stale restore."); return false; }
            for (const FRoot& R : Roots)
            {
                FString Current;
                if (!R.Read(Current, OutError)) return false;
                if (Current != R.Before && Current != R.After)
                { OutError = TEXT("Root changed since migration; refusing to overwrite newer edits: ") + R.AssetPath; return false; }
                if (!R.Import(R.Before, true, OutError)) return false;
            }
        }
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        if (Registry.IsLoadingAssets()) { OutError = TEXT("Wait for asset discovery."); return false; }
        TArray<UObject*> Assets = {Struct};
        for (const FString& P : Paths)
        {
            UObject* A = LoadObject<UObject>(nullptr, *P);
            if (!A || (!A->IsA<UBlueprint>() && A->GetClass() != UDataTable::StaticClass()))
            { OutError = TEXT("Unsupported migration asset: ") + P; return false; }
            Assets.AddUnique(A);
        }
        TArray<FName> Referencers, Unlisted;
        Registry.GetReferencers(Struct->GetPackage()->GetFName(), Referencers);
        for (FName Ref : Referencers)
            if (!Assets.ContainsByPredicate([&](const UObject* A) { return A->GetPackage()->GetFName() == Ref; })) Unlisted.Add(Ref);
        Result->SetArrayField(TEXT("unlistedReferencerPackages"), JsonNames(Unlisted));
        Result->SetNumberField(TEXT("opaqueTypeCount"), Walker.OpaqueTypes.Num());
        if (Walker.OpaqueTypes.Num() > 32) Walker.OpaqueTypes.SetNum(32);
        Result->SetArrayField(TEXT("opaqueStructTypes"), JsonNames(Walker.OpaqueTypes));
        if (bRemove)
        {
            if (!Walker.OpaqueTypes.IsEmpty()) { OutError = TEXT("Automatic removal cannot prove coverage of opaque native struct storage; copy reflected values first and review unsupported storage separately."); return false; }
            for (const UObject* A : Assets) if (A != Struct && !Roots.ContainsByPredicate([&](const FRoot& R) { return R.AssetPath == A->GetPathName(); }))
            {
                const auto* Table = Cast<UDataTable>(A);
                if (!Table || !Table->GetRowMap().IsEmpty()) { OutError = TEXT("Automatic removal requires supported value roots for each non-empty explicit asset: ") + A->GetPathName(); return false; }
            }
            // Follow explicit assets' referencers too: placed instances or child assets outside scope must block deletion.
            TArray<FName> Pending = {Struct->GetPackage()->GetFName()};
            TSet<FName> Seen;
            for (int32 I = 0; I < Pending.Num(); ++I)
            {
                if (Pending.Num() > 2048) { OutError = TEXT("Referencer traversal limit reached; automatic removal refused."); return false; }
                const FName Package = Pending[I]; if (Seen.Contains(Package)) continue; Seen.Add(Package);
                TArray<FName> Refs; Registry.GetReferencers(Package, Refs);
                for (FName Ref : Refs) if (!Seen.Contains(Ref))
                {
                    if (Assets.ContainsByPredicate([&](const UObject* A) { return A->GetPackage()->GetFName() == Ref; })) Pending.AddUnique(Ref);
                    else Unlisted.AddUnique(Ref);
                }
            }
            Result->SetArrayField(TEXT("unlistedReferencerPackages"), JsonNames(Unlisted));
            if (!Unlisted.IsEmpty()) { OutError = TEXT("Source removal is refused while indexed referencer packages are outside the explicit migration scope."); return false; }
            for (UObject* A : Assets) if (const auto* BP = Cast<UBlueprint>(A))
            {
                TArray<UEdGraph*> Graphs; BP->GetAllGraphs(Graphs);
                for (const UEdGraph* Graph : Graphs) if (Graph) for (const UEdGraphNode* Node : Graph->Nodes) if (Node)
                    for (const UEdGraphPin* Pin : Node->Pins) if (Pin && (Pin->PinType.PinSubCategoryObject.Get() == Struct || Pin->PinType.PinValueType.TerminalSubCategoryObject.Get() == Struct))
                    { OutError = TEXT("Source removal requires graph review: a Blueprint graph pin uses this struct. Copy values first and update graph references before removal."); return false; }
            }
        }
        Result->SetNumberField(TEXT("rootCount"), Roots.Num());
        int32 Instances = 1; for (const auto& R : Roots) Instances += R.Values.Num();
        if (!bRestore) Result->SetNumberField(TEXT("instanceCount"), Instances);
        Result->SetStringField(TEXT("objectPath"), Path);
        Result->SetBoolField(TEXT("saved"), false); Result->SetBoolField(TEXT("persistedVerified"), false);
        if (bDry) return true;
        if (!GEditor || GEditor->IsTransactionActive()) { OutError = TEXT("An idle editor transaction system is required."); return false; }
        TMap<FString, bool> DirtyBefore;
        for (const UObject* A : Assets)
        {
            DirtyBefore.Add(A->GetPathName(), A->GetPackage()->IsDirty());
            if (bSave && A->GetPackage()->IsDirty())
            { OutError = TEXT("Save or revert existing dirty assets before save/reload migration: ") + A->GetPathName(); return false; }
        }
        const auto AllDirtyBefore = Dirty();
        ON_SCOPE_EXIT
        {
            TArray<FName> NewlyDirty; for (FName P : Dirty()) if (!AllDirtyBefore.Contains(P)) NewlyDirty.Add(P);
            Result->SetArrayField(TEXT("newlyDirtiedPackages"), JsonNames(NewlyDirty));
        };
        if (!bRestore)
        {
            BackupId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
            Manifest = MakeShared<FJsonObject>();
            Manifest->SetNumberField(TEXT("formatVersion"), 1);
            Manifest->SetStringField(TEXT("projectFile"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
            Manifest->SetStringField(TEXT("objectPath"), Path); Manifest->SetStringField(TEXT("layout"), Layout(Struct));
            Manifest->SetStringField(TEXT("sourceGuid"), SourceId.ToString()); Manifest->SetStringField(TEXT("destinationGuid"), DestinationId.ToString());
            Manifest->SetStringField(TEXT("defaultBefore"), OldDefault); Manifest->SetStringField(TEXT("defaultAfter"), NewDefault);
            TArray<TSharedPtr<FJsonValue>> JsonPaths, JsonRoots, Files;
            for (const FString& P : Paths) JsonPaths.Add(MakeShared<FJsonValueString>(P));
            for (const auto& R : Roots) JsonRoots.Add(MakeShared<FJsonValueObject>(R.Json()));
            if (!IFileManager::Get().MakeDirectory(*Directory(), true)) { OutError = TEXT("Cannot create backup directory."); return false; }
            for (int32 I = 0; I < Assets.Num(); ++I)
            {
                const FString Original = FPackageName::LongPackageNameToFilename(Assets[I]->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
                if (!IFileManager::Get().FileExists(*Original)) continue;
                const FString Copy = Directory() / (BackupId + FString::Printf(TEXT("_%d.uasset"), I));
                if (IFileManager::Get().Copy(*Copy, *Original, false) != COPY_OK)
                { OutError = TEXT("Original package backup failed; no edits applied."); return false; }
                auto File = MakeShared<FJsonObject>(); File->SetStringField(TEXT("original"), Original); File->SetStringField(TEXT("backup"), Copy); Files.Add(MakeShared<FJsonValueObject>(File));
            }
            Manifest->SetArrayField(TEXT("assetPaths"), JsonPaths); Manifest->SetArrayField(TEXT("roots"), JsonRoots); Manifest->SetArrayField(TEXT("packageCopies"), Files);
            if (!SaveJson(Directory() / (BackupId + TEXT(".json")), Manifest.ToSharedRef(), OutError)) return false;
        }
        Result->SetStringField(TEXT("backupId"), BackupId); Result->SetStringField(TEXT("backupFile"), Directory() / (BackupId + TEXT(".json")));
        const FString DefaultBeforeEdit = Walker.DefaultDestination;
        const FString DesiredDefault = bRestore ? OldDefault : NewDefault;
        TArray<FString> BeforeEdit;
        for (const auto& R : Roots) { FString Current; if (!R.Read(Current, OutError)) return false; BeforeEdit.Add(Current); }
        auto Verify = [&](bool bBefore, FString& VerifyError)
        {
            for (int32 I = 0; I < Roots.Num(); ++I)
            {
                FString Current;
                if (!Roots[I].Read(Current, VerifyError)) return false;
                if (Current != (bBefore ? BeforeEdit[I] : (bRestore ? Roots[I].Before : Roots[I].After)))
                { VerifyError = TEXT("Root value verification failed: ") + Roots[I].AssetPath; return false; }
            }
            UUserDefinedStruct* CurrentStruct = LoadObject<UUserDefinedStruct>(nullptr, *Path);
            FProperty* CurrentField = CurrentStruct ? FStructureEditorUtils::GetPropertyByGuid(CurrentStruct, DestinationId) : nullptr;
            if (!CurrentField || Export(CurrentField, CurrentField->ContainerPtrToValuePtr<void>(CurrentStruct->GetDefaultInstance())) != (bBefore ? DefaultBeforeEdit : DesiredDefault))
            { VerifyError = TEXT("Struct default verification failed."); return false; }
            return true;
        };
        auto Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UnrealMCP", "MigrateStructValues", "UnrealMCP Migrate Struct Member Values"));
        for (UObject* A : Assets)
        {
            if (A == Struct) continue; // StructureEditorUtils records editor data, not compiled fields.
            A->Modify(); if (auto* BP = Cast<UBlueprint>(A)) BP->GeneratedClass->GetDefaultObject()->Modify();
        }
        auto CompileBlueprints = [&](FString& CompileError)
        {
            bool bCompiled = true;
            for (UObject* A : Assets) if (auto* BP = Cast<UBlueprint>(A))
            {
                FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
                TArray<TSharedPtr<FJsonValue>> E, W;
                if (!Edit::CompileWithLocatedMessages(BP, E, W))
                { Result->SetArrayField(TEXT("compileErrors"), E); CompileError = TEXT("Blueprint compile failed."); bCompiled = false; }
            }
            return bCompiled;
        };
        bool bApplied = DesiredDefault == DefaultBeforeEdit || FStructureEditorUtils::ChangeVariableDefaultValue(Struct, DestinationId, DesiredDefault);
        if (!bApplied) OutError = TEXT("Could not change destination struct default.");
        if (bApplied) bApplied = CompileBlueprints(OutError);
        for (int32 I = 0; bApplied && I < Roots.Num(); ++I) bApplied = Roots[I].Import(bRestore ? Roots[I].Before : Roots[I].After, false, OutError);
        if (bApplied) bApplied = CompileBlueprints(OutError) && Verify(false, OutError);
        if (!bApplied)
        {
            bool bRestored = DefaultBeforeEdit == DesiredDefault || FStructureEditorUtils::ChangeVariableDefaultValue(Struct, DestinationId, DefaultBeforeEdit);
            FString RestoreError;
            CompileBlueprints(RestoreError);
            for (int32 I = 0; I < Roots.Num(); ++I) bRestored = Roots[I].Import(BeforeEdit[I], false, RestoreError) && bRestored;
            CompileBlueprints(RestoreError); bRestored = Verify(true, RestoreError) && bRestored;
            Transaction->Cancel(); Transaction.Reset();
            if (bRestored) for (UObject* A : Assets) A->GetPackage()->SetDirtyFlag(DirtyBefore.FindChecked(A->GetPathName()));
            Result->SetBoolField(TEXT("rolledBack"), bRestored); Result->SetBoolField(TEXT("valuesVerified"), false);
            return false;
        }
        for (UObject* A : Assets) if (auto* T = Cast<UDataTable>(A)) { T->MarkPackageDirty(); T->HandleDataTableChanged(); }
        Transaction.Reset();
        Result->SetBoolField(TEXT("valuesVerified"), true);
        if (!bSave) return true;
        TArray<TSharedPtr<FJsonValue>> Saved;
        TArray<UPackage*> Packages;
        for (UObject* A : Assets)
        {
            FString Filename;
            if (!Edit::SaveAsset(A, Filename, OutError))
            { Result->SetArrayField(TEXT("savedPackages"), Saved); Result->SetBoolField(TEXT("partialSave"), !Saved.IsEmpty()); return false; }
            Saved.Add(MakeShared<FJsonValueString>(A->GetPackage()->GetName())); Packages.AddUnique(A->GetPackage());
        }
        Result->SetArrayField(TEXT("savedPackages"), Saved); Result->SetBoolField(TEXT("saved"), true);
        FText ReloadError;
        if (!UPackageTools::ReloadPackages(Packages, ReloadError, EReloadPackagesInteractionMode::AssumeNegative))
        { OutError = TEXT("Saved, but reload verification failed: ") + ReloadError.ToString(); return false; }
        if (!Verify(false, OutError)) return false;
        Result->SetBoolField(TEXT("persistedVerified"), true);
        Result->SetBoolField(TEXT("copyPersistedVerified"), true);
        if (bRemove)
        {
            // The first save/reload proves the copy before the destructive schema edit.
            Result->SetBoolField(TEXT("persistedVerified"), false);
            Struct = LoadObject<UUserDefinedStruct>(nullptr, *Path);
            Assets.Reset(); Assets.Add(Struct);
            for (const FString& P : Paths) Assets.Add(LoadObject<UObject>(nullptr, *P));
            const TArray<FStructVariableDescription> OriginalFields = FStructureEditorUtils::GetVarDesc(Struct);
            auto RemovalTransaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UnrealMCP", "RemoveMigratedStructField", "UnrealMCP Remove Migrated Struct Field"));
            for (UObject* A : Assets) if (A != Struct)
            { A->Modify(); if (auto* BP = Cast<UBlueprint>(A)) BP->GeneratedClass->GetDefaultObject()->Modify(); }
            bool bRemoved = FStructureEditorUtils::RemoveVariable(Struct, SourceId);
            if (bRemoved) bRemoved = CompileBlueprints(OutError);
            for (int32 I = 0; bRemoved && I < Roots.Num(); ++I) bRemoved = Roots[I].Import(Roots[I].AfterRemoval, false, OutError);
            if (bRemoved) bRemoved = CompileBlueprints(OutError);
            auto VerifyRemoval = [&](FString& VerifyError)
            {
                for (const auto& R : Roots)
                {
                    FString Current;
                    if (!R.Read(Current, VerifyError)) return false;
                    if (Current != R.AfterRemoval) { VerifyError = TEXT("Surviving values failed verification after source removal: ") + R.AssetPath; return false; }
                }
                UUserDefinedStruct* Current = LoadObject<UUserDefinedStruct>(nullptr, *Path);
                return Current && !FStructureEditorUtils::GetPropertyByGuid(Current, SourceId);
            };
            if (bRemoved) bRemoved = VerifyRemoval(OutError);
            if (!bRemoved)
            {
                // Restore the original GUIDs/layout, then the already verified migrated roots.
                FStructureEditorUtils::GetVarDesc(Struct) = OriginalFields;
                FStructureEditorUtils::CompileStructure(Struct);
                FString RestoreError; CompileBlueprints(RestoreError);
                bool bRestored = true;
                for (const auto& R : Roots) bRestored = R.Import(R.After, false, RestoreError) && bRestored;
                CompileBlueprints(RestoreError); bRestored = Verify(false, RestoreError) && bRestored;
                RemovalTransaction->Cancel(); RemovalTransaction.Reset();
                Result->SetBoolField(TEXT("removalRolledBack"), bRestored);
                if (OutError.IsEmpty()) OutError = TEXT("Source removal failed; restored migrated values where indicated. The verified on-disk copy is retained.");
                return false;
            }
            Result->SetBoolField(TEXT("sourceRemoved"), true);
            RemovalTransaction.Reset();
            Packages.Reset(); Saved.Reset();
            for (UObject* A : Assets)
            {
                FString Filename;
                if (!Edit::SaveAsset(A, Filename, OutError))
                { Result->SetArrayField(TEXT("savedRemovalPackages"), Saved); Result->SetBoolField(TEXT("partialSave"), !Saved.IsEmpty()); return false; }
                Saved.Add(MakeShared<FJsonValueString>(A->GetPackage()->GetName())); Packages.AddUnique(A->GetPackage());
            }
            Result->SetArrayField(TEXT("savedRemovalPackages"), Saved);
            if (!UPackageTools::ReloadPackages(Packages, ReloadError, EReloadPackagesInteractionMode::AssumeNegative))
            { OutError = TEXT("Source removed and saved, but final reload failed: ") + ReloadError.ToString(); return false; }
            if (!VerifyRemoval(OutError)) return false;
            Result->SetBoolField(TEXT("persistedVerified"), true);
        }
        FString IndexError; Result->SetBoolField(TEXT("indexRefreshed"), Edit::RefreshAssetIndex(Path, IndexError));
        return true;
    }, Error);
    if (!bOk) { Result->SetBoolField(TEXT("success"), false); return BuildError(Request, EMCPErrorCode::InvalidParams, Error, Result); }
    FMCPResponse Response; Response.Id = Request.Id; Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FStructMemberValueMigrationTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>(), P = MakeShared<FJsonObject>(); S->SetStringField(TEXT("type"), TEXT("object"));
    TArray<TSharedPtr<FJsonValue>> Required;
    if (bRestore)
    {
        P->SetObjectField(TEXT("backupId"), BuildStringProperty(TEXT("GUID returned by migration; only the same project and unchanged struct layout are accepted.")));
        Required.Add(MakeShared<FJsonValueString>(TEXT("backupId")));
    }
    else
    {
        P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("/Game user-defined struct asset path.")));
        P->SetObjectField(TEXT("sourceMember"), BuildStringProperty(TEXT("Existing source field GUID (preferred), authored name or internal name.")));
        P->SetObjectField(TEXT("destinationMember"), BuildStringProperty(TEXT("Distinct existing destination field GUID/name with exactly the same type.")));
        auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("type"), TEXT("array")); A->SetObjectField(TEXT("items"), BuildStringProperty(TEXT("Explicit /Game Blueprint or ordinary DataTable asset."))); A->SetNumberField(TEXT("maxItems"), 256);
        P->SetObjectField(TEXT("assetPaths"), A);
        P->SetObjectField(TEXT("removeSource"), BuildBoolProperty(TEXT("Optional verified removal after copy/save/reload. Requires save=true, complete indexed referencer scope and no graph pins using the struct. Reapplies surviving values after Unreal rebuilds the schema. Default false.")));
        P->SetObjectField(TEXT("overwriteDestination"), BuildBoolProperty(TEXT("Explicitly overwrite conflicting non-default destination values. Default false.")));
        for (const TCHAR* Key : {TEXT("objectPath"), TEXT("sourceMember"), TEXT("destinationMember"), TEXT("assetPaths")}) Required.Add(MakeShared<FJsonValueString>(Key));
    }
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preflight only; no backup or asset writes.")));
    P->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required to apply.")));
    P->SetObjectField(TEXT("save"), BuildBoolProperty(TEXT("Compile, save and reload-verify explicit packages. Refuses already dirty packages. Default false.")));
    S->SetObjectField(TEXT("properties"), P); S->SetArrayField(TEXT("required"), Required); return S;
}
