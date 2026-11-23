#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Data/StructMemberValueMigrationTool.h"
#include "Tools/Data/UserDefinedStructMemberTools.h"
#include "UObject/StructOnScope.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace
{
    struct FMigrationFixture
    {
        FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
        FString StructPath, TablePath, BlueprintPath;
        FGuid SourceId, DestinationId, MapSourceId, MapDestinationId;
        TArray<FString> Backups;
        UUserDefinedStruct* Struct() const { return LoadObject<UUserDefinedStruct>(nullptr, *StructPath); }
        UDataTable* Table() const { return LoadObject<UDataTable>(nullptr, *TablePath); }
        UBlueprint* Blueprint() const { return LoadObject<UBlueprint>(nullptr, *BlueprintPath); }
        FString Package(const TCHAR* Prefix) const { return TEXT("/Game/UnrealMCP_Automation/") + FString(Prefix) + TEXT("_") + Suffix; }
        FGuid Add(const TCHAR* Field, const FEdGraphPinType& Type)
        {
            FStructureEditorUtils::AddVariable(Struct(), Type);
            FGuid Id = FStructureEditorUtils::GetVarDesc(Struct()).Last().VarGuid;
            FStructureEditorUtils::RenameVariable(Struct(), Id, Field); return Id;
        }
        double Get(void* Data, FGuid Id) const
        { return CastFieldChecked<FDoubleProperty>(FStructureEditorUtils::GetPropertyByGuid(Struct(), Id))->GetPropertyValue_InContainer(Data); }
        void Set(void* Data, FGuid Id, double Value) const
        { CastFieldChecked<FDoubleProperty>(FStructureEditorUtils::GetPropertyByGuid(Struct(), Id))->SetPropertyValue_InContainer(Data, Value); }
        bool Create()
        {
            const FString SName = FPackageName::GetLongPackageAssetName(Package(TEXT("S_Migration")));
            UUserDefinedStruct* S = FStructureEditorUtils::CreateUserDefinedStruct(CreatePackage(*Package(TEXT("S_Migration"))), FName(*SName), RF_Public | RF_Standalone | RF_Transactional);
            StructPath = S->GetPathName(); FAssetRegistryModule::AssetCreated(S);
            SourceId = FStructureEditorUtils::GetVarDesc(S)[0].VarGuid;
            FStructureEditorUtils::RenameVariable(S, SourceId, TEXT("OldValue"));
            FEdGraphPinType Double; Double.PinCategory = UEdGraphSchema_K2::PC_Real; Double.PinSubCategory = UEdGraphSchema_K2::PC_Double;
            FStructureEditorUtils::ChangeVariableType(S, SourceId, Double);
            FStructureEditorUtils::ChangeVariableDefaultValue(S, SourceId, TEXT("1"));
            DestinationId = Add(TEXT("NewValue"), Double);
            FEdGraphPinType String; String.PinCategory = UEdGraphSchema_K2::PC_String;
            const FGuid Label = Add(TEXT("Label"), String);
            FStructureEditorUtils::ChangeVariableDefaultValue(S, Label, TEXT("KeepMe"));
            FEdGraphPinType ClassType;
            ClassType.PinCategory = UEdGraphSchema_K2::PC_Class; ClassType.PinSubCategoryObject = AActor::StaticClass();
            MapSourceId = Add(TEXT("OldClass"), ClassType); MapDestinationId = Add(TEXT("NewClass"), ClassType);
            FStructureEditorUtils::ChangeVariableDefaultValue(S, MapSourceId, TEXT("/Script/Engine.Actor"));

            const FString TName = FPackageName::GetLongPackageAssetName(Package(TEXT("DT_Migration")));
            UDataTable* T = NewObject<UDataTable>(CreatePackage(*Package(TEXT("DT_Migration"))), FName(*TName), RF_Public | RF_Standalone | RF_Transactional);
            T->RowStruct = S; TablePath = T->GetPathName(); FAssetRegistryModule::AssetCreated(T);
            FStructOnScope Row(S);
            Set(Row.GetStructMemory(), SourceId, 2.123456789012345);
            TMap<FName, const uint8*> Rows; Rows.Add(TEXT("Alpha"), Row.GetStructMemory());
            if (!T->CreateTableFromRawData(Rows, S).IsEmpty()) return false;

            const FString BName = FPackageName::GetLongPackageAssetName(Package(TEXT("BP_Migration")));
            UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*Package(TEXT("BP_Migration"))), FName(*BName),
                BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
            BlueprintPath = BP->GetPathName(); FAssetRegistryModule::AssetCreated(BP);
            FEdGraphPinType Payload; Payload.PinCategory = UEdGraphSchema_K2::PC_Struct; Payload.PinSubCategoryObject = S;
            FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Payload"), Payload);
            Payload.ContainerType = EPinContainerType::Array; FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Batch"), Payload);
            Payload.ContainerType = EPinContainerType::Set; FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Unique"), Payload);
            Payload.ContainerType = EPinContainerType::Map; Payload.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
            FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("ByStruct"), Payload);
            Payload.PinCategory = UEdGraphSchema_K2::PC_Byte; Payload.PinSubCategoryObject = StaticEnum<ECollisionChannel>();
            Payload.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Struct; Payload.PinValueType.TerminalSubCategoryObject = S;
            FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("ByName"), Payload);
            FKismetEditorUtilities::CompileBlueprint(BP);
            UObject* CDO = BP->GeneratedClass->GetDefaultObject();
            Set(FindFProperty<FStructProperty>(BP->GeneratedClass, TEXT("Payload"))->ContainerPtrToValuePtr<void>(CDO), SourceId, 11);
            auto* Array = FindFProperty<FArrayProperty>(BP->GeneratedClass, TEXT("Batch"));
            FScriptArrayHelper A(Array, Array->ContainerPtrToValuePtr<void>(CDO)); A.AddValues(2); Set(A.GetRawPtr(0), SourceId, 22); Set(A.GetRawPtr(1), SourceId, 33);
            auto* SetProp = FindFProperty<FSetProperty>(BP->GeneratedClass, TEXT("Unique"));
            FScriptSetHelper U(SetProp, SetProp->ContainerPtrToValuePtr<void>(CDO)); const int32 SI = U.AddDefaultValue_Invalid_NeedsRehash(); Set(U.GetElementPtr(SI), SourceId, 66); U.Rehash();
            auto* Keys = FindFProperty<FMapProperty>(BP->GeneratedClass, TEXT("ByStruct"));
            FScriptMapHelper K(Keys, Keys->ContainerPtrToValuePtr<void>(CDO)); const int32 KI = K.AddDefaultValue_Invalid_NeedsRehash();
            Set(K.GetKeyPtr(KI), SourceId, 55); CastFieldChecked<FIntProperty>(Keys->ValueProp)->SetPropertyValue(K.GetValuePtr(KI), 777); K.Rehash();
            auto* Values = FindFProperty<FMapProperty>(BP->GeneratedClass, TEXT("ByName"));
            FScriptMapHelper V(Values, Values->ContainerPtrToValuePtr<void>(CDO)); const int32 VI = V.AddDefaultValue_Invalid_NeedsRehash();
            CastFieldChecked<FByteProperty>(Values->KeyProp)->SetPropertyValue(V.GetKeyPtr(VI), ECC_WorldStatic); Set(V.GetValuePtr(VI), SourceId, 44); V.Rehash();
            FString File, Error;
            for (UObject* Asset : TArray<UObject*>{S, T, BP})
                if (!UnrealMCP::BlueprintEditToolUtils::SaveAsset(Asset, File, Error)) return false;
            return true;
        }
        UnrealMCP::FMCPResponse Run(bool bRestore, TFunctionRef<void(FJsonObject&)> Fill)
        {
            UnrealMCP::FMCPRequest R; R.Id = TEXT("migration-test"); R.Params = MakeShared<FJsonObject>();
            R.Params->SetStringField(TEXT("objectPath"), StructPath);
            R.Params->SetStringField(TEXT("sourceMember"), SourceId.ToString()); R.Params->SetStringField(TEXT("destinationMember"), DestinationId.ToString());
            R.Params->SetArrayField(TEXT("assetPaths"), {MakeShared<FJsonValueString>(TablePath), MakeShared<FJsonValueString>(BlueprintPath)});
            Fill(*R.Params);
            const auto Response = FStructMemberValueMigrationTool(bRestore).Execute(R);
            if (Response.Result && Response.Result->HasField(TEXT("backupId"))) Backups.AddUnique(Response.Result->GetStringField(TEXT("backupId")));
            return Response;
        }
        bool Verify(FAutomationTestBase& Test, bool bMigrated) const
        {
            UObject* CDO = Blueprint()->GeneratedClass->GetDefaultObject();
            auto Expected = [&](double V) { return bMigrated ? V : 0.0; };
            bool bOk = Test.TestEqual(TEXT("Row destination preserved"), Get(Table()->GetRowMap().FindChecked(TEXT("Alpha")), DestinationId), Expected(2.123456789012345));
            bOk &= Test.TestTrue(TEXT("Exact double value preserved"), Get(Table()->GetRowMap().FindChecked(TEXT("Alpha")), DestinationId) == Expected(2.123456789012345));
            bOk &= Test.TestEqual(TEXT("CDO destination preserved"), Get(FindFProperty<FStructProperty>(Blueprint()->GeneratedClass, TEXT("Payload"))->ContainerPtrToValuePtr<void>(CDO), DestinationId), Expected(11));
            auto* Array = FindFProperty<FArrayProperty>(Blueprint()->GeneratedClass, TEXT("Batch"));
            FScriptArrayHelper A(Array, Array->ContainerPtrToValuePtr<void>(CDO));
            bOk &= Test.TestEqual(TEXT("Array shape preserved"), A.Num(), 2);
            bOk &= Test.TestEqual(TEXT("Array first value preserved"), Get(A.GetRawPtr(0), DestinationId), Expected(22));
            bOk &= Test.TestEqual(TEXT("Array second value preserved"), Get(A.GetRawPtr(1), DestinationId), Expected(33));
            auto* SetProp = FindFProperty<FSetProperty>(Blueprint()->GeneratedClass, TEXT("Unique"));
            FScriptSetHelper U(SetProp, SetProp->ContainerPtrToValuePtr<void>(CDO));
            for (int32 I = 0; I < U.GetMaxIndex(); ++I) if (U.IsValidIndex(I)) bOk &= Test.TestEqual(TEXT("Set value preserved"), Get(U.GetElementPtr(I), DestinationId), Expected(66));
            auto* Keys = FindFProperty<FMapProperty>(Blueprint()->GeneratedClass, TEXT("ByStruct"));
            FScriptMapHelper K(Keys, Keys->ContainerPtrToValuePtr<void>(CDO));
            for (int32 I = 0; I < K.GetMaxIndex(); ++I) if (K.IsValidIndex(I))
            {
                bOk &= Test.TestEqual(TEXT("Map key preserved"), Get(K.GetKeyPtr(I), DestinationId), Expected(55));
                bOk &= Test.TestEqual(TEXT("Unrelated map value preserved"), CastFieldChecked<FIntProperty>(Keys->ValueProp)->GetPropertyValue(K.GetValuePtr(I)), 777);
            }
            auto* Values = FindFProperty<FMapProperty>(Blueprint()->GeneratedClass, TEXT("ByName"));
            FScriptMapHelper V(Values, Values->ContainerPtrToValuePtr<void>(CDO));
            for (int32 I = 0; I < V.GetMaxIndex(); ++I) if (V.IsValidIndex(I)) bOk &= Test.TestEqual(TEXT("Map value preserved"), Get(V.GetValuePtr(I), DestinationId), Expected(44));
            return bOk;
        }
        ~FMigrationFixture()
        {
            for (const FString& Path : {BlueprintPath, TablePath, StructPath})
            {
                if (Path.IsEmpty()) continue;
                if (UObject* O = FindObject<UObject>(nullptr, *Path))
                { UPackage* P = O->GetPackage(); FAssetRegistryModule::AssetDeleted(O); ObjectTools::DeleteObjectsUnchecked({O}); P->SetDirtyFlag(false); }
                const FString File = FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Path), FPackageName::GetAssetPackageExtension());
                IFileManager::Get().Delete(*File, false, true);
            }
            const FString Dir = FPaths::ProjectSavedDir() / TEXT("UnrealMCP/FieldMigrations");
            for (const FString& Id : Backups)
            {
                IFileManager::Get().Delete(*(Dir / (Id + TEXT(".json"))), false, true);
                TArray<FString> Files; IFileManager::Get().FindFiles(Files, *(Dir / (Id + TEXT("_*.uasset"))), true, false);
                for (const FString& File : Files) IFileManager::Get().Delete(*(Dir / File), false, true);
            }
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPStructMigrationTest, "UnrealMCP.Struct.ValueMigration.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPStructMigrationTest::RunTest(const FString& Parameters)
{
    FMigrationFixture F;
    if (!TestTrue(TEXT("Saved fixtures created"), F.Create())) return false;
    const auto Dry = F.Run(false, [](FJsonObject& P) { P.SetBoolField(TEXT("dryRun"), true); });
    if (Dry.Error.IsSet()) { AddError(Dry.Error->Message); return false; }
    TestFalse(TEXT("Dry run creates no backup"), Dry.Result->HasField(TEXT("backupId"))); F.Verify(*this, false);
    TestTrue(TEXT("Confirmation required"), F.Run(false, [](FJsonObject&) {}).Error.IsSet());
    TestTrue(TEXT("Type conversion refused"), F.Run(false, [](FJsonObject& P) { P.SetStringField(TEXT("destinationMember"), TEXT("Label")); P.SetBoolField(TEXT("dryRun"), true); }).Error.IsSet());
    TestTrue(TEXT("Removal requires persisted verification"), F.Run(false, [](FJsonObject& P) { P.SetBoolField(TEXT("removeSource"), true); P.SetBoolField(TEXT("confirm"), true); }).Error.IsSet());
    F.Set(F.Table()->GetRowMap().FindChecked(TEXT("Alpha")), F.DestinationId, 99);
    TestTrue(TEXT("Conflicting destination refused"), F.Run(false, [](FJsonObject& P) { P.SetBoolField(TEXT("confirm"), true); }).Error.IsSet());
    TestEqual(TEXT("Conflict leaves live value untouched"), F.Get(F.Table()->GetRowMap().FindChecked(TEXT("Alpha")), F.DestinationId), 99.0);
    F.Set(F.Table()->GetRowMap().FindChecked(TEXT("Alpha")), F.DestinationId, 0);
    F.Table()->MarkPackageDirty();
    TestTrue(TEXT("Existing dirty package save refused"), F.Run(false, [](FJsonObject& P) { P.SetBoolField(TEXT("confirm"), true); P.SetBoolField(TEXT("save"), true); }).Error.IsSet());
    F.Table()->GetPackage()->SetDirtyFlag(false);
    const auto Migrated = F.Run(false, [](FJsonObject& P) { P.SetBoolField(TEXT("confirm"), true); P.SetBoolField(TEXT("save"), true); });
    if (Migrated.Error.IsSet()) { AddError(Migrated.Error->Message); return false; }
    TestTrue(TEXT("Saved and reload-verified"), Migrated.Result->GetBoolField(TEXT("persistedVerified")));
    TestTrue(TEXT("Durable backup exists"), IFileManager::Get().FileExists(*Migrated.Result->GetStringField(TEXT("backupFile"))));
    F.Verify(*this, true);
    const FString Backup = Migrated.Result->GetStringField(TEXT("backupId"));
    void* Payload = FindFProperty<FStructProperty>(F.Blueprint()->GeneratedClass, TEXT("Payload"))->ContainerPtrToValuePtr<void>(F.Blueprint()->GeneratedClass->GetDefaultObject());
    F.Set(Payload, F.SourceId, 99);
    TestTrue(TEXT("Stale restore refused"), F.Run(true, [&](FJsonObject& P) { P.SetStringField(TEXT("backupId"), Backup); P.SetBoolField(TEXT("dryRun"), true); }).Error.IsSet());
    F.Set(Payload, F.SourceId, 11);
    const auto Restored = F.Run(true, [&](FJsonObject& P) { P.SetStringField(TEXT("backupId"), Backup); P.SetBoolField(TEXT("confirm"), true); P.SetBoolField(TEXT("save"), true); });
    if (Restored.Error.IsSet()) { AddError(Restored.Error->Message); return false; }
    TestTrue(TEXT("Restored snapshots reload-verified"), Restored.Result->GetBoolField(TEXT("persistedVerified"))); F.Verify(*this, false);

    // Two distinct struct keys would become identical if a conflicting destination were overwritten.
    UObject* KeyCDO = F.Blueprint()->GeneratedClass->GetDefaultObject();
    auto* KeyProperty = FindFProperty<FMapProperty>(F.Blueprint()->GeneratedClass, TEXT("ByStruct"));
    FScriptMapHelper Keys(KeyProperty, KeyProperty->ContainerPtrToValuePtr<void>(KeyCDO));
    const int32 Extra = Keys.AddDefaultValue_Invalid_NeedsRehash();
    KeyProperty->KeyProp->CopyCompleteValue(Keys.GetKeyPtr(Extra), Keys.GetKeyPtr(0));
    F.Set(Keys.GetKeyPtr(Extra), F.DestinationId, 9); Keys.Rehash();
    TestTrue(TEXT("Duplicate map keys refused"), F.Run(false, [](FJsonObject& P) { P.SetBoolField(TEXT("confirm"), true); P.SetBoolField(TEXT("overwriteDestination"), true); }).Error.IsSet());
    TestEqual(TEXT("Collision preflight preserves map entries"), Keys.Num(), 2);
    Keys.RemoveAt(Extra); Keys.Rehash();
    const auto MapMigrated = F.Run(false, [&](FJsonObject& P)
    { P.SetStringField(TEXT("sourceMember"), F.MapSourceId.ToString()); P.SetStringField(TEXT("destinationMember"), F.MapDestinationId.ToString()); P.SetBoolField(TEXT("confirm"), true); P.SetBoolField(TEXT("save"), true); });
    if (MapMigrated.Error.IsSet()) { AddError(MapMigrated.Error->Message); return false; }
    auto* ClassField = CastFieldChecked<FObjectPropertyBase>(FStructureEditorUtils::GetPropertyByGuid(F.Struct(), F.MapDestinationId));
    TestTrue(TEXT("Class reference copied"), ClassField->GetObjectPropertyValue_InContainer(F.Table()->GetRowMap().FindChecked(TEXT("Alpha"))) == AActor::StaticClass());

    const auto Again = F.Run(false, [](FJsonObject& P) { P.SetBoolField(TEXT("confirm"), true); P.SetBoolField(TEXT("save"), true); P.SetBoolField(TEXT("removeSource"), true); });
    if (Again.Error.IsSet()) { AddError(Again.Error->Message); return false; }
    TestTrue(TEXT("Source removed only after verified copy"), Again.Result->GetBoolField(TEXT("sourceRemoved")));
    TestTrue(TEXT("Surviving values reload-verified"), Again.Result->GetBoolField(TEXT("persistedVerified")));
    F.Verify(*this, true);
    TestTrue(TEXT("Restore refuses changed layout"), F.Run(true, [&](FJsonObject& P) { P.SetStringField(TEXT("backupId"), Backup); P.SetBoolField(TEXT("dryRun"), true); }).Error.IsSet());
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPStructContainerMigrationTest, "UnrealMCP.Struct.ValueMigrationContainers.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPStructContainerMigrationTest::RunTest(const FString& Parameters)
{
    FMigrationFixture F;
    const FString Package = F.Package(TEXT("S_ContainerMigration"));
    UUserDefinedStruct* S = FStructureEditorUtils::CreateUserDefinedStruct(CreatePackage(*Package),
        FName(*FPackageName::GetLongPackageAssetName(Package)), RF_Public | RF_Standalone | RF_Transactional);
    F.StructPath = S->GetPathName(); FAssetRegistryModule::AssetCreated(S);
    FEdGraphPinType MapType;
    MapType.PinCategory = UEdGraphSchema_K2::PC_Byte; MapType.PinSubCategoryObject = StaticEnum<ECollisionChannel>(); MapType.ContainerType = EPinContainerType::Map;
    MapType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Class; MapType.PinValueType.TerminalSubCategoryObject = AActor::StaticClass();
    const FGuid Source = FStructureEditorUtils::GetVarDesc(S)[0].VarGuid;
    FStructureEditorUtils::RenameVariable(S, Source, TEXT("OldMap")); FStructureEditorUtils::ChangeVariableType(S, Source, MapType);
    const FGuid Destination = F.Add(TEXT("NewMap"), MapType);
    FEdGraphPinType ArrayType; ArrayType.PinCategory = UEdGraphSchema_K2::PC_Int; ArrayType.ContainerType = EPinContainerType::Array;
    const FGuid ArraySource = F.Add(TEXT("OldArray"), ArrayType), ArrayDestination = F.Add(TEXT("NewArray"), ArrayType);
    const FString TablePackage = F.Package(TEXT("DT_ContainerMigration"));
    UDataTable* T = NewObject<UDataTable>(CreatePackage(*TablePackage), FName(*FPackageName::GetLongPackageAssetName(TablePackage)), RF_Public | RF_Standalone | RF_Transactional);
    T->RowStruct = S; F.TablePath = T->GetPathName(); FAssetRegistryModule::AssetCreated(T);
    FStructOnScope Row(S);
    auto* Map = CastFieldChecked<FMapProperty>(FStructureEditorUtils::GetPropertyByGuid(S, Source));
    FScriptMapHelper H(Map, Map->ContainerPtrToValuePtr<void>(Row.GetStructMemory()));
    const int32 Slot = H.AddDefaultValue_Invalid_NeedsRehash();
    CastFieldChecked<FByteProperty>(Map->KeyProp)->SetPropertyValue(H.GetKeyPtr(Slot), ECC_WorldStatic);
    CastFieldChecked<FObjectPropertyBase>(Map->ValueProp)->SetObjectPropertyValue(H.GetValuePtr(Slot), AActor::StaticClass()); H.Rehash();
    auto* Array = CastFieldChecked<FArrayProperty>(FStructureEditorUtils::GetPropertyByGuid(S, ArraySource));
    FScriptArrayHelper A(Array, Array->ContainerPtrToValuePtr<void>(Row.GetStructMemory())); A.AddValues(2);
    CastFieldChecked<FIntProperty>(Array->Inner)->SetPropertyValue(A.GetRawPtr(0), 7);
    CastFieldChecked<FIntProperty>(Array->Inner)->SetPropertyValue(A.GetRawPtr(1), 9);
    TMap<FName, const uint8*> Rows; Rows.Add(TEXT("Alpha"), Row.GetStructMemory());
    if (!TestTrue(TEXT("Container row created"), T->CreateTableFromRawData(Rows, S).IsEmpty())) return false;
    FString File, Error;
    for (UObject* Asset : TArray<UObject*>{S, T}) if (!UnrealMCP::BlueprintEditToolUtils::SaveAsset(Asset, File, Error)) { AddError(Error); return false; }
    auto Run = [&](FGuid From, FGuid To, const FString& Backup = FString())
    {
        UnrealMCP::FMCPRequest R; R.Id = TEXT("container-migration"); R.Params = MakeShared<FJsonObject>();
        R.Params->SetStringField(TEXT("objectPath"), F.StructPath); R.Params->SetStringField(TEXT("sourceMember"), From.ToString()); R.Params->SetStringField(TEXT("destinationMember"), To.ToString());
        R.Params->SetArrayField(TEXT("assetPaths"), {MakeShared<FJsonValueString>(F.TablePath)});
        R.Params->SetBoolField(TEXT("confirm"), true); R.Params->SetBoolField(TEXT("save"), true);
        if (!Backup.IsEmpty()) R.Params->SetStringField(TEXT("backupId"), Backup);
        const auto Response = FStructMemberValueMigrationTool(!Backup.IsEmpty()).Execute(R);
        if (Response.Result && Response.Result->HasField(TEXT("backupId"))) F.Backups.AddUnique(Response.Result->GetStringField(TEXT("backupId")));
        return Response;
    };
    const auto Migrated = Run(Source, Destination);
    if (Migrated.Error.IsSet()) { AddError(Migrated.Error->Message); return false; }
    TestTrue(TEXT("Map field migration reload-verified"), Migrated.Result->GetBoolField(TEXT("persistedVerified")));
    auto* DestinationMap = CastFieldChecked<FMapProperty>(FStructureEditorUtils::GetPropertyByGuid(F.Struct(), Destination));
    FScriptMapHelper Copied(DestinationMap, DestinationMap->ContainerPtrToValuePtr<void>(F.Table()->GetRowMap().FindChecked(TEXT("Alpha"))));
    TestEqual(TEXT("Enum-key map size preserved"), Copied.Num(), 1);
    for (int32 I = 0; I < Copied.GetMaxIndex(); ++I) if (Copied.IsValidIndex(I))
    {
        TestEqual(TEXT("Enum key preserved"), CastFieldChecked<FByteProperty>(DestinationMap->KeyProp)->GetPropertyValue(Copied.GetKeyPtr(I)), static_cast<uint8>(ECC_WorldStatic));
        TestTrue(TEXT("Map class reference preserved"), CastFieldChecked<FObjectPropertyBase>(DestinationMap->ValueProp)->GetObjectPropertyValue(Copied.GetValuePtr(I)) == AActor::StaticClass());
    }
    const auto ArrayMigrated = Run(ArraySource, ArrayDestination);
    if (ArrayMigrated.Error.IsSet()) { AddError(ArrayMigrated.Error->Message); return false; }
    auto* DestinationArray = CastFieldChecked<FArrayProperty>(FStructureEditorUtils::GetPropertyByGuid(F.Struct(), ArrayDestination));
    FScriptArrayHelper CopiedArray(DestinationArray, DestinationArray->ContainerPtrToValuePtr<void>(F.Table()->GetRowMap().FindChecked(TEXT("Alpha"))));
    TestEqual(TEXT("Array field size preserved"), CopiedArray.Num(), 2);
    TestEqual(TEXT("Array field element preserved"), CastFieldChecked<FIntProperty>(DestinationArray->Inner)->GetPropertyValue(CopiedArray.GetRawPtr(1)), 9);
    const auto ArrayRestored = Run(ArraySource, ArrayDestination, ArrayMigrated.Result->GetStringField(TEXT("backupId")));
    if (ArrayRestored.Error.IsSet()) { AddError(ArrayRestored.Error->Message); return false; }
    const auto Restored = Run(Source, Destination, Migrated.Result->GetStringField(TEXT("backupId")));
    if (Restored.Error.IsSet()) { AddError(Restored.Error->Message); return false; }
    TestTrue(TEXT("Container backup restoration reload-verified"), Restored.Result->GetBoolField(TEXT("persistedVerified")));
    DestinationMap = CastFieldChecked<FMapProperty>(FStructureEditorUtils::GetPropertyByGuid(F.Struct(), Destination));
    FScriptMapHelper Original(DestinationMap, DestinationMap->ContainerPtrToValuePtr<void>(F.Table()->GetRowMap().FindChecked(TEXT("Alpha"))));
    TestEqual(TEXT("Original empty destination restored"), Original.Num(), 0);
    return !HasAnyErrors();
}
#endif
