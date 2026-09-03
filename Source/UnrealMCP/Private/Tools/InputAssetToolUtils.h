#pragma once

// Reflection-based helpers for authoring/reading Enhanced Input data assets
// (UInputAction, UInputMappingContext) without adding EnhancedInput as a hard
// build dependency of this module, matching the existing convention used by
// AddEnhancedInputActionNodeTool.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "InputCoreTypes.h"
#include "UObject/Class.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UnrealMCP::InputAssetToolUtils
{
    inline bool ResolveInputActionClass(UClass*& OutClass, FString& OutError)
    {
        OutClass = LoadClass<UObject>(nullptr, TEXT("/Script/EnhancedInput.InputAction"));
        if (OutClass == nullptr)
        {
            OutError = TEXT("Enhanced Input plugin classes are unavailable. Enable the Enhanced Input plugin for this project.");
            return false;
        }
        return true;
    }

    inline bool ResolveInputMappingContextClass(UClass*& OutClass, FString& OutError)
    {
        OutClass = LoadClass<UObject>(nullptr, TEXT("/Script/EnhancedInput.InputMappingContext"));
        if (OutClass == nullptr)
        {
            OutError = TEXT("Enhanced Input plugin classes are unavailable. Enable the Enhanced Input plugin for this project.");
            return false;
        }
        return true;
    }

    // Loads an existing asset and validates it is an instance of ExpectedClass.
    inline bool ResolveExistingObjectOfClass(
        const FString& ObjectPath,
        UClass* ExpectedClass,
        UObject*& OutObject,
        FString& OutError)
    {
        OutObject = LoadObject<UObject>(nullptr, *ObjectPath);
        if (OutObject == nullptr)
        {
            OutError = FString::Printf(TEXT("Could not load asset '%s'."), *ObjectPath);
            return false;
        }
        if (ExpectedClass != nullptr && !OutObject->IsA(ExpectedClass))
        {
            OutError = FString::Printf(
                TEXT("Asset '%s' is a '%s', not a '%s'."),
                *ObjectPath,
                *OutObject->GetClass()->GetName(),
                *ExpectedClass->GetName());
            return false;
        }
        return true;
    }

    // Parses a value-type name (enumerator short name such as "Boolean"/"Axis1D",
    // or the exact metadata DisplayName such as "Digital (bool)") against the
    // real EInputActionValueType enum reflected from UInputAction::ValueType.
    inline bool ParseInputActionValueType(
        UClass* InputActionClass,
        const FString& ValueTypeName,
        FEnumProperty*& OutEnumProperty,
        int64& OutValue,
        FString& OutError)
    {
        FProperty* Prop = InputActionClass->FindPropertyByName(TEXT("ValueType"));
        OutEnumProperty = CastField<FEnumProperty>(Prop);
        if (OutEnumProperty == nullptr)
        {
            OutError = TEXT("UInputAction no longer exposes a reflected 'ValueType' enum property.");
            return false;
        }

        UEnum* Enum = OutEnumProperty->GetEnum();
        OutValue = Enum->GetValueByNameString(ValueTypeName);
        if (OutValue == INDEX_NONE)
        {
            OutValue = Enum->GetValueByNameString(Enum->GenerateFullEnumName(*ValueTypeName));
        }
        if (OutValue == INDEX_NONE)
        {
            for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
            {
                if (Enum->GetDisplayNameTextByIndex(Index).ToString() == ValueTypeName)
                {
                    OutValue = Enum->GetValueByIndex(Index);
                    break;
                }
            }
        }
        const FString ResolvedName = Enum->GetNameStringByValue(OutValue);
        if (OutValue == INDEX_NONE ||
            (ResolvedName != TEXT("Boolean") && ResolvedName != TEXT("Axis1D") &&
             ResolvedName != TEXT("Axis2D") && ResolvedName != TEXT("Axis3D")))
        {
            OutError = FString::Printf(
                TEXT("Unknown InputAction value type '%s'. Expected one of: Boolean, Axis1D, Axis2D, Axis3D."),
                *ValueTypeName);
            return false;
        }
        return true;
    }

    inline FArrayProperty* FindMappingsProperty(UClass* ContextClass, FString& OutError)
    {
        FArrayProperty* Prop = CastField<FArrayProperty>(ContextClass->FindPropertyByName(TEXT("Mappings")));
        if (Prop == nullptr)
        {
            OutError = TEXT("UInputMappingContext no longer exposes a reflected 'Mappings' array property.");
        }
        return Prop;
    }

    inline UScriptStruct* GetMappingElementStruct(FArrayProperty* MappingsProperty)
    {
        const FStructProperty* InnerStruct = CastField<FStructProperty>(MappingsProperty->Inner);
        return InnerStruct != nullptr ? InnerStruct->Struct : nullptr;
    }

    inline TSharedRef<FJsonObject> SerializeMappingRow(void* ElementPtr, UScriptStruct* MappingStruct, int32 Index)
    {
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("index"), Index);

        if (FObjectProperty* ActionProp = FindFProperty<FObjectProperty>(MappingStruct, TEXT("Action")))
        {
            UObject* ActionObject = ActionProp->GetObjectPropertyValue(
                ActionProp->ContainerPtrToValuePtr<void>(ElementPtr));
            Row->SetStringField(TEXT("actionPath"), ActionObject != nullptr ? ActionObject->GetPathName() : FString());
        }

        if (FStructProperty* KeyProp = FindFProperty<FStructProperty>(MappingStruct, TEXT("Key")))
        {
            const FKey* KeyPtr = KeyProp->ContainerPtrToValuePtr<FKey>(ElementPtr);
            Row->SetStringField(TEXT("key"), KeyPtr != nullptr ? KeyPtr->ToString() : FString());
        }

        auto SerializeObjectArray = [&](const TCHAR* FieldName, const TCHAR* JsonName)
        {
            TArray<TSharedPtr<FJsonValue>> Names;
            if (FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(MappingStruct, FieldName))
            {
                FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(ElementPtr));
                const FObjectProperty* InnerObjProp = CastField<FObjectProperty>(ArrayProp->Inner);
                for (int32 Idx = 0; Idx < Helper.Num(); ++Idx)
                {
                    UObject* Element = InnerObjProp != nullptr
                        ? InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(Idx))
                        : nullptr;
                    Names.Add(MakeShared<FJsonValueString>(Element != nullptr ? Element->GetClass()->GetName() : FString()));
                }
            }
            Row->SetArrayField(JsonName, Names);
        };
        SerializeObjectArray(TEXT("Triggers"), TEXT("triggerClasses"));
        SerializeObjectArray(TEXT("Modifiers"), TEXT("modifierClasses"));

        return Row;
    }

    // Finds every mapping row whose Action and Key exactly match. Returns the count;
    // appends matching indices to OutIndices when provided.
    inline int32 CountMatchingMappings(
        UObject* ContextObject,
        FArrayProperty* MappingsProperty,
        UObject* ActionObject,
        const FKey& Key,
        TArray<int32>* OutIndices = nullptr)
    {
        UScriptStruct* MappingStruct = GetMappingElementStruct(MappingsProperty);
        FObjectProperty* ActionProp = MappingStruct != nullptr
            ? FindFProperty<FObjectProperty>(MappingStruct, TEXT("Action")) : nullptr;
        FStructProperty* KeyProp = MappingStruct != nullptr
            ? FindFProperty<FStructProperty>(MappingStruct, TEXT("Key")) : nullptr;
        if (ActionProp == nullptr || KeyProp == nullptr)
        {
            return 0;
        }

        FScriptArrayHelper Helper(MappingsProperty, MappingsProperty->ContainerPtrToValuePtr<void>(ContextObject));
        int32 Count = 0;
        for (int32 Idx = 0; Idx < Helper.Num(); ++Idx)
        {
            void* ElementPtr = Helper.GetRawPtr(Idx);
            UObject* RowAction = ActionProp->GetObjectPropertyValue(ActionProp->ContainerPtrToValuePtr<void>(ElementPtr));
            const FKey* RowKey = KeyProp->ContainerPtrToValuePtr<FKey>(ElementPtr);
            if (RowAction == ActionObject && RowKey != nullptr && *RowKey == Key)
            {
                ++Count;
                if (OutIndices != nullptr)
                {
                    OutIndices->Add(Idx);
                }
            }
        }
        return Count;
    }

    // Invokes a UInputMappingContext member function (MapKey/UnmapKey/UnmapAllKeysFromAction)
    // purely through reflection, matching parameters by property type/order rather than by
    // name so this stays correct even if a future engine version renames a parameter.
    inline bool CallMappingContextFunction(
        UObject* ContextObject,
        const TCHAR* FunctionName,
        UObject* ActionObject,
        const FKey* KeyValue,
        FString& OutError)
    {
        UFunction* Function = ContextObject->FindFunction(FName(FunctionName));
        if (Function == nullptr)
        {
            OutError = FString::Printf(TEXT("UInputMappingContext no longer exposes '%s'."), FunctionName);
            return false;
        }

        FStructOnScope Params(Function);
        bool bSetAction = false;
        bool bSetKey = false;
        for (TFieldIterator<FProperty> It(Function); It; ++It)
        {
            FProperty* Prop = *It;
            if (!bSetAction)
            {
                if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
                {
                    ObjProp->SetObjectPropertyValue_InContainer(Params.GetStructMemory(), ActionObject);
                    bSetAction = true;
                    continue;
                }
            }
            if (KeyValue != nullptr && !bSetKey)
            {
                if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
                {
                    if (StructProp->Struct == FKey::StaticStruct())
                    {
                        *StructProp->ContainerPtrToValuePtr<FKey>(Params.GetStructMemory()) = *KeyValue;
                        bSetKey = true;
                        continue;
                    }
                }
            }
        }

        if (!bSetAction)
        {
            OutError = FString::Printf(TEXT("Could not bind the Action parameter for '%s'."), FunctionName);
            return false;
        }
        if (KeyValue != nullptr && !bSetKey)
        {
            OutError = FString::Printf(TEXT("Could not bind the Key parameter for '%s'."), FunctionName);
            return false;
        }

        ContextObject->ProcessEvent(Function, Params.GetStructMemory());
        return true;
    }
}
