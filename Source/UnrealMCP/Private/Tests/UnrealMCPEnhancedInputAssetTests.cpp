#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "MCP/MCPServer.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/AddInputMappingContextMappingTool.h"
#include "Tools/CreateInputActionTool.h"
#include "Tools/CreateInputMappingContextTool.h"
#include "Tools/GetInputMappingContextMappingsTool.h"
#include "Tools/InputAssetToolUtils.h"
#include "Tools/RemoveInputMappingContextMappingTool.h"
#include "Tools/SetInputMappingContextMappingKeyTool.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPEnhancedInputAssetsLiveTest,
    "UnrealMCP.EnhancedInput.AssetAuthoring.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPEnhancedInputAssetsLiveTest::RunTest(const FString& Parameters)
{
    using namespace UnrealMCP;
    const FString Root = TEXT("/Game/UnrealMCP_Automation/Input_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ActionPackage = Root + TEXT("/IA_Test");
    const FString ContextPackage = Root + TEXT("/IMC_Test");
    const FString ActionPath = ActionPackage + TEXT(".IA_Test");
    const FString ContextPath = ContextPackage + TEXT(".IMC_Test");
    ON_SCOPE_EXIT
    {
        // These unsaved data assets need no delete dialog or forced GC. ObjectTools deletion can
        // flush streaming while the adapter game-thread task is nested in a rendering wait.
        for (const FString& Path : {ContextPath, ActionPath})
        {
            if (UObject* Object = FindObject<UObject>(nullptr, *Path))
            {
                FAssetRegistryModule::AssetDeleted(Object);
                Object->ClearFlags(RF_Public | RF_Standalone);
                const FName TransientName = MakeUniqueObjectName(GetTransientPackage(), Object->GetClass(), Object->GetFName());
                Object->Rename(*TransientName.ToString(), GetTransientPackage(),
                    REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty | REN_ForceNoResetLoaders);
                TestNull(TEXT("Fixture removed from original asset path"), FindObject<UObject>(nullptr, *Path));
            }
        }
        for (const FString& Path : {ContextPackage, ActionPackage})
        {
            if (UPackage* Package = FindPackage(nullptr, *Path)) Package->SetDirtyFlag(false);
        }
    };

    FMCPServer Server;
    Server.GetToolRegistry().RegisterTool(MakeShared<FCreateInputActionTool>());
    Server.GetToolRegistry().RegisterTool(MakeShared<FCreateInputMappingContextTool>());
    Server.GetToolRegistry().RegisterTool(MakeShared<FAddInputMappingContextMappingTool>());
    Server.GetToolRegistry().RegisterTool(MakeShared<FRemoveInputMappingContextMappingTool>());
    Server.GetToolRegistry().RegisterTool(MakeShared<FGetInputMappingContextMappingsTool>());
    Server.GetToolRegistry().RegisterTool(MakeShared<FSetInputMappingContextMappingKeyTool>());
    auto Call = [&](const TCHAR* Method, const TSharedPtr<FJsonObject>& Args)
    {
        FMCPRequest Request;
        Request.Id = FGuid::NewGuid().ToString();
        Request.Method = Method;
        Request.Params = Args;
        return Server.HandleRequest(Request);
    };
    auto Success = [&](const TCHAR* Label, const FMCPResponse& Response)
    {
        return TestFalse(Label, Response.Error.IsSet()) && TestTrue(TEXT("Structured success"),
            Response.Result.IsValid() && Response.Result->GetBoolField(TEXT("success")));
    };
    auto MappingArgs = [&]()
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("objectPath"), ContextPath);
        Args->SetStringField(TEXT("inputActionPath"), ActionPath);
        Args->SetStringField(TEXT("key"), TEXT("W"));
        return Args;
    };
    auto ReadCount = [&]() -> int32
    {
        const FMCPResponse Response = Call(TEXT("GetInputMappingContextMappings"), MappingArgs());
        if (!Success(TEXT("Read mapping rows"), Response)) return INDEX_NONE;
        return static_cast<int32>(Response.Result->GetNumberField(TEXT("count")));
    };

    auto CreateArgs = MakeShared<FJsonObject>();
    CreateArgs->SetStringField(TEXT("packagePath"), ActionPackage);
    CreateArgs->SetStringField(TEXT("valueType"), TEXT("Axis2D"));
    CreateArgs->SetBoolField(TEXT("dryRun"), true);
    if (!Success(TEXT("Create action dry-run"), Call(TEXT("CreateInputAction"), CreateArgs))) return false;
    TestNull(TEXT("Dry-run does not create action"), FindObject<UObject>(nullptr, *ActionPath));
    CreateArgs->SetStringField(TEXT("valueType"), TEXT("EInputActionValueType_MAX"));
    TestTrue(TEXT("Reject enum sentinel"), Call(TEXT("CreateInputAction"), CreateArgs).Error.IsSet());
    CreateArgs->SetStringField(TEXT("valueType"), TEXT("UnknownValueType"));
    TestTrue(TEXT("Reject unknown value type"), Call(TEXT("CreateInputAction"), CreateArgs).Error.IsSet());
    CreateArgs->SetStringField(TEXT("valueType"), TEXT("Axis2D"));
    CreateArgs->SetBoolField(TEXT("dryRun"), false);
    if (!Success(TEXT("Create action"), Call(TEXT("CreateInputAction"), CreateArgs))) return false;
    TestTrue(TEXT("Reject action overwrite"), Call(TEXT("CreateInputAction"), CreateArgs).Error.IsSet());
    UObject* Action = FindObject<UObject>(nullptr, *ActionPath);
    if (!TestNotNull(TEXT("Created action exists"), Action)) return false;
    FEnumProperty* ValueType = FindFProperty<FEnumProperty>(Action->GetClass(), TEXT("ValueType"));
    if (!TestNotNull(TEXT("Value type reflected"), ValueType)) return false;
    TestEqual(TEXT("Exact action value type"), ValueType->GetEnum()->GetNameStringByValue(
        ValueType->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValueType->ContainerPtrToValuePtr<void>(Action))), FString(TEXT("Axis2D")));

    auto ContextArgs = MakeShared<FJsonObject>();
    ContextArgs->SetStringField(TEXT("packagePath"), ContextPackage);
    ContextArgs->SetBoolField(TEXT("dryRun"), true);
    if (!Success(TEXT("Create context dry-run"), Call(TEXT("CreateInputMappingContext"), ContextArgs))) return false;
    TestNull(TEXT("Dry-run does not create context"), FindObject<UObject>(nullptr, *ContextPath));
    ContextArgs->SetBoolField(TEXT("dryRun"), false);
    if (!Success(TEXT("Create context"), Call(TEXT("CreateInputMappingContext"), ContextArgs))) return false;
    TestTrue(TEXT("Reject context overwrite"), Call(TEXT("CreateInputMappingContext"), ContextArgs).Error.IsSet());
    UObject* Context = FindObject<UObject>(nullptr, *ContextPath);
    if (!TestNotNull(TEXT("Created context exists"), Context)) return false;
    Context->GetPackage()->SetDirtyFlag(false);

    auto Args = MappingArgs();
    Args->SetBoolField(TEXT("dryRun"), true);
    if (!Success(TEXT("Add mapping dry-run"), Call(TEXT("AddInputMappingContextMapping"), Args))) return false;
    TestEqual(TEXT("Dry-run leaves rows empty"), ReadCount(), 0);
    TestFalse(TEXT("Dry-run preserves clean package"), Context->GetPackage()->IsDirty());
    Args->SetStringField(TEXT("key"), TEXT("UnrealMCP_NotARealKey"));
    TestTrue(TEXT("Reject invalid key"), Call(TEXT("AddInputMappingContextMapping"), Args).Error.IsSet());
    Args = MappingArgs();
    Args->SetStringField(TEXT("inputActionPath"), ContextPath);
    TestTrue(TEXT("Reject wrong asset class"), Call(TEXT("AddInputMappingContextMapping"), Args).Error.IsSet());
    Args = MappingArgs();
    if (!Success(TEXT("Add W mapping"), Call(TEXT("AddInputMappingContextMapping"), Args))) return false;
    const FMCPResponse Duplicate = Call(TEXT("AddInputMappingContextMapping"), Args);
    if (!Success(TEXT("Idempotent add"), Duplicate)) return false;
    TestTrue(TEXT("Duplicate reports alreadyExists"), Duplicate.Result->GetBoolField(TEXT("alreadyExists")));
    TestEqual(TEXT("Idempotent add does not duplicate row"), ReadCount(), 1);
    Args->SetStringField(TEXT("key"), TEXT("S"));
    Args->SetBoolField(TEXT("dryRun"), true);
    if (!Success(TEXT("Second key dry-run"), Call(TEXT("AddInputMappingContextMapping"), Args))) return false;
    Args->SetBoolField(TEXT("dryRun"), false);
    if (!Success(TEXT("Second key for same action"), Call(TEXT("AddInputMappingContextMapping"), Args))) return false;
    TestEqual(TEXT("Context groups two keys for one action"), ReadCount(), 2);
    const FMCPResponse Read = Call(TEXT("GetInputMappingContextMappings"), MappingArgs());
    if (!Success(TEXT("Exact readback"), Read)) return false;
    const auto& Rows = Read.Result->GetArrayField(TEXT("mappings"));
    if (!TestEqual(TEXT("Readback row count"), Rows.Num(), 2)) return false;
    TestEqual(TEXT("First action identity"), Rows[0]->AsObject()->GetStringField(TEXT("actionPath")), ActionPath);
    TestEqual(TEXT("First key"), Rows[0]->AsObject()->GetStringField(TEXT("key")), FString(TEXT("W")));
    TestEqual(TEXT("Second key"), Rows[1]->AsObject()->GetStringField(TEXT("key")), FString(TEXT("S")));

    FString ReflectionError;
    FArrayProperty* MappingProperty = InputAssetToolUtils::FindMappingsProperty(Context->GetClass(), ReflectionError);
    if (!TestNotNull(TEXT("Mapping property available"), MappingProperty)) return false;
    UScriptStruct* MappingStruct = InputAssetToolUtils::GetMappingElementStruct(MappingProperty);
    auto CurrentRow = [&]() -> void*
    {
        FScriptArrayHelper Helper(MappingProperty, MappingProperty->ContainerPtrToValuePtr<void>(Context));
        return Helper.GetRawPtr(0);
    };
    // Populate inline objects and modern metadata directly as test fixtures, not as new public authoring APIs.
    auto AddInlineFixture = [&](const TCHAR* PropertyName, const TCHAR* ClassPath) -> UObject*
    {
        UClass* Class = LoadClass<UObject>(nullptr, ClassPath);
        FArrayProperty* Property = FindFProperty<FArrayProperty>(MappingStruct, PropertyName);
        if (!Class || !Property) return nullptr;
        auto* ObjectProperty = CastField<FObjectProperty>(Property->Inner);
        if (!ObjectProperty) return nullptr;
        UObject* Object = NewObject<UObject>(Context, Class, NAME_None, RF_Transactional);
        FScriptArrayHelper Values(Property, Property->ContainerPtrToValuePtr<void>(CurrentRow()));
        ObjectProperty->SetObjectPropertyValue(Values.GetRawPtr(Values.AddValue()), Object);
        return Object;
    };
    if (!TestNotNull(TEXT("Inline Negate fixture"), AddInlineFixture(TEXT("Modifiers"), TEXT("/Script/EnhancedInput.InputModifierNegate")))) return false;
    if (!TestNotNull(TEXT("Inline Pressed fixture"), AddInlineFixture(TEXT("Triggers"), TEXT("/Script/EnhancedInput.InputTriggerPressed")))) return false;
    FObjectProperty* SettingsProperty = FindFProperty<FObjectProperty>(MappingStruct, TEXT("PlayerMappableKeySettings"));
    UClass* SettingsClass = LoadClass<UObject>(nullptr, TEXT("/Script/EnhancedInput.PlayerMappableKeySettings"));
    if (!TestNotNull(TEXT("Modern settings property"), SettingsProperty) || !TestNotNull(TEXT("Modern settings class"), SettingsClass)) return false;
    UObject* Settings = NewObject<UObject>(Context, SettingsClass, NAME_None, RF_Transactional);
    SettingsProperty->SetObjectPropertyValue(SettingsProperty->ContainerPtrToValuePtr<void>(CurrentRow()), Settings);
    FNameProperty* SettingsName = FindFProperty<FNameProperty>(SettingsClass, TEXT("Name"));
    if (!TestNotNull(TEXT("Mapping settings name"), SettingsName)) return false;
    SettingsName->SetPropertyValue_InContainer(Settings, TEXT("MovementForward"));
    FStructOnScope OriginalRow(MappingStruct);
    MappingStruct->CopyScriptStruct(OriginalRow.GetStructMemory(), CurrentRow());
    FStructProperty* KeyProperty = FindFProperty<FStructProperty>(MappingStruct, TEXT("Key"));
    if (!TestNotNull(TEXT("Key property"), KeyProperty)) return false;
    auto KeyArgs = MappingArgs();
    KeyArgs->SetStringField(TEXT("newKey"), TEXT("A"));
    KeyArgs->SetBoolField(TEXT("dryRun"), true);
    Context->GetPackage()->SetDirtyFlag(false);
    if (!Success(TEXT("Replace key dry-run"), Call(TEXT("SetInputMappingContextMappingKey"), KeyArgs))) return false;
    TestTrue(TEXT("Dry-run preserves complete mapping including inline identities"), MappingStruct->CompareScriptStruct(CurrentRow(), OriginalRow.GetStructMemory(), 0));
    TestFalse(TEXT("Key dry-run preserves clean flag"), Context->GetPackage()->IsDirty());
    KeyArgs->SetBoolField(TEXT("dryRun"), false);
    auto RejectKeyEdit = [&](const TCHAR* Label)
    {
        const FMCPResponse Response = Call(TEXT("SetInputMappingContextMappingKey"), KeyArgs);
        TestTrue(Label, Response.Error.IsSet() || (Response.Result.IsValid() && !Response.Result->GetBoolField(TEXT("success"))));
        TestTrue(TEXT("Rejected edit preserves exact row"), MappingStruct->CompareScriptStruct(CurrentRow(), OriginalRow.GetStructMemory(), 0));
        TestFalse(TEXT("Rejected edit preserves clean state"), Context->GetPackage()->IsDirty());
    };
    RejectKeyEdit(TEXT("Replacing a key requires confirmation"));
    KeyArgs->SetBoolField(TEXT("confirm"), true);
    KeyArgs->SetStringField(TEXT("newKey"), TEXT("S"));
    RejectKeyEdit(TEXT("Reject duplicate replacement key"));
    KeyArgs->SetStringField(TEXT("newKey"), TEXT("NotARealKey"));
    RejectKeyEdit(TEXT("Reject invalid replacement key"));
    Context->GetPackage()->SetDirtyFlag(true);
    const FMCPResponse DirtyRejection = Call(TEXT("SetInputMappingContextMappingKey"), KeyArgs);
    TestFalse(TEXT("Invalid key rejected on pre-dirty asset"), DirtyRejection.Result->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Rejection preserves pre-existing dirty work"), Context->GetPackage()->IsDirty());
    Context->GetPackage()->SetDirtyFlag(false);
    KeyArgs->SetStringField(TEXT("newKey"), TEXT("W"));
    KeyArgs->SetBoolField(TEXT("saveAfterEdit"), true);
    const FMCPResponse NoOp = Call(TEXT("SetInputMappingContextMappingKey"), KeyArgs);
    if (!Success(TEXT("Same key no-op"), NoOp)) return false;
    TestFalse(TEXT("No-op does not save unrelated dirty work"), NoOp.Result->GetBoolField(TEXT("saved")));
    TestFalse(TEXT("No-op does not dirty context"), Context->GetPackage()->IsDirty());
    KeyArgs->RemoveField(TEXT("saveAfterEdit"));
    KeyArgs->SetStringField(TEXT("newKey"), TEXT("A"));
    KeyArgs->SetStringField(TEXT("operationId"), FGuid::NewGuid().ToString());
    if (!Success(TEXT("Replace W with A"), Call(TEXT("SetInputMappingContextMappingKey"), KeyArgs))) return false;
    *KeyProperty->ContainerPtrToValuePtr<FKey>(OriginalRow.GetStructMemory()) = FKey(TEXT("A"));
    TestTrue(TEXT("Only key changed; modifiers, triggers, metadata identities and row order retained"), MappingStruct->CompareScriptStruct(CurrentRow(), OriginalRow.GetStructMemory(), 0));
    TestEqual(TEXT("Key replacement preserves row count"), ReadCount(), 2);
    if (!Success(TEXT("Replay key replacement"), Call(TEXT("SetInputMappingContextMappingKey"), KeyArgs))) return false;
    TestTrue(TEXT("One transaction undoes key replacement"), GEditor->UndoTransaction());
    *KeyProperty->ContainerPtrToValuePtr<FKey>(OriginalRow.GetStructMemory()) = FKey(TEXT("W"));
    TestTrue(TEXT("Undo restores complete original row"), MappingStruct->CompareScriptStruct(CurrentRow(), OriginalRow.GetStructMemory(), 0));

    Args = MappingArgs();
    Args->SetBoolField(TEXT("dryRun"), true);
    if (!Success(TEXT("Remove dry-run"), Call(TEXT("RemoveInputMappingContextMapping"), Args))) return false;
    TestEqual(TEXT("Remove dry-run preserves both rows"), ReadCount(), 2);
    Args->SetBoolField(TEXT("dryRun"), false);
    TestTrue(TEXT("Removal requires confirmation"), Call(TEXT("RemoveInputMappingContextMapping"), Args).Error.IsSet());
    TestEqual(TEXT("Missing confirmation preserves rows"), ReadCount(), 2);

    FString Error;
    FArrayProperty* Mappings = InputAssetToolUtils::FindMappingsProperty(Context->GetClass(), Error);
    if (!TestNotNull(TEXT("Mapping array reflected"), Mappings)) return false;
    {
        FScriptArrayHelper Helper(Mappings, Mappings->ContainerPtrToValuePtr<void>(Context));
        const int32 DuplicateIndex = Helper.AddValue();
        Mappings->Inner->CopySingleValue(Helper.GetRawPtr(DuplicateIndex), Helper.GetRawPtr(0));
        Args->SetBoolField(TEXT("confirm"), true);
        TestTrue(TEXT("Reject ambiguous removal"), Call(TEXT("RemoveInputMappingContextMapping"), Args).Error.IsSet());
        TestTrue(TEXT("Reject ambiguous add"), Call(TEXT("AddInputMappingContextMapping"), Args).Error.IsSet());
        KeyArgs->RemoveField(TEXT("operationId"));
        const FMCPResponse AmbiguousKey = Call(TEXT("SetInputMappingContextMappingKey"), KeyArgs);
        TestFalse(TEXT("Reject ambiguous key replacement"), AmbiguousKey.Result->GetBoolField(TEXT("success")));
        TestEqual(TEXT("Ambiguity does not remove rows"), Helper.Num(), 3);
        Helper.RemoveValues(DuplicateIndex);
    }
    Args->SetStringField(TEXT("operationId"), FGuid::NewGuid().ToString());
    if (!Success(TEXT("Confirmed removal"), Call(TEXT("RemoveInputMappingContextMapping"), Args))) return false;
    if (!Success(TEXT("Identical removal replay"), Call(TEXT("RemoveInputMappingContextMapping"), Args))) return false;
    TestEqual(TEXT("Replay preserves unrelated S row"), ReadCount(), 1);
    Args->RemoveField(TEXT("operationId"));
    TestTrue(TEXT("Fresh second removal rejects zero matches"), Call(TEXT("RemoveInputMappingContextMapping"), Args).Error.IsSet());
    Args->SetStringField(TEXT("key"), TEXT("S"));
    Args->SetBoolField(TEXT("dryRun"), true);
    if (!Success(TEXT("Final row removal preview"), Call(TEXT("RemoveInputMappingContextMapping"), Args))) return false;
    Args->SetBoolField(TEXT("dryRun"), false);
    if (!Success(TEXT("Remove final row"), Call(TEXT("RemoveInputMappingContextMapping"), Args))) return false;
    TestEqual(TEXT("Context empty after final removal"), ReadCount(), 0);
    TestTrue(TEXT("Mutations leave package dirty by default"), Context->GetPackage()->IsDirty());
    TestFalse(TEXT("Action never implicitly saved"), FPackageName::DoesPackageExist(ActionPackage));
    TestFalse(TEXT("Context never implicitly saved"), FPackageName::DoesPackageExist(ContextPackage));
    return !HasAnyErrors();
}

#endif
