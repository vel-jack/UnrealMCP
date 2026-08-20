#include "Tools/BlueprintComponentEditUtils.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "UObject/UnrealType.h"

namespace UnrealMCP::BlueprintComponentEditUtils
{
    namespace
    {
        bool JsonValueToImportText(const TSharedPtr<FJsonValue>& Value, FString& OutText)
        {
            if (!Value.IsValid())
            {
                return false;
            }

            switch (Value->Type)
            {
            case EJson::String:
                OutText = Value->AsString();
                return true;
            case EJson::Number:
                OutText = FString::SanitizeFloat(Value->AsNumber(), 17);
                return true;
            case EJson::Boolean:
                OutText = Value->AsBool() ? TEXT("true") : TEXT("false");
                return true;
            default:
                return false;
            }
        }

        bool IsSafelyEditable(const FProperty* Property)
        {
            return Property != nullptr
                && Property->HasAnyPropertyFlags(CPF_Edit)
                && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated | CPF_DisableEditOnTemplate);
        }

        FString ExportPropertyValue(const FProperty* Property, UObject* Target)
        {
            FString Value;
            const void* Address = Property->ContainerPtrToValuePtr<void>(Target);
            Property->ExportText_Direct(Value, Address, Address, Target, PPF_None);
            return Value;
        }
    }

    USCS_Node* FindComponentNode(UBlueprint* Blueprint, const FString& ComponentName)
    {
        if (Blueprint == nullptr || Blueprint->SimpleConstructionScript == nullptr) return nullptr;
        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Node != nullptr && Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase)) return Node;
        }
        return nullptr;
    }

    bool GetEditablePropertyValues(UObject* Target, const TArray<FString>& PropertyNames, const int32 MaxProperties, TArray<FPropertyValue>& OutValues, FString& OutError)
    {
        if (Target == nullptr) { OutError = TEXT("Component template is null."); return false; }
        TSet<FString> Requested;
        for (const FString& Name : PropertyNames) Requested.Add(Name.ToLower());
        for (TFieldIterator<FProperty> It(Target->GetClass(), EFieldIteratorFlags::IncludeSuper); It && OutValues.Num() < MaxProperties; ++It)
        {
            FProperty* Property = *It;
            if (!IsSafelyEditable(Property) || (!Requested.IsEmpty() && !Requested.Contains(Property->GetName().ToLower()))) continue;
            FPropertyValue Item;
            Item.Name = Property->GetName();
            Item.Type = Property->GetCPPType();
            Item.Value = ExportPropertyValue(Property, Target);
            OutValues.Add(MoveTemp(Item));
        }
        if (!Requested.IsEmpty())
        {
            for (const FString& Name : PropertyNames)
            {
                if (!OutValues.ContainsByPredicate([&](const FPropertyValue& Value){ return Value.Name.Equals(Name, ESearchCase::IgnoreCase); }))
                { OutError = FString::Printf(TEXT("Component property '%s' was not found or is not safely editable."), *Name); return false; }
            }
        }
        return true;
    }

    bool ApplyPropertyDefaults(UObject* Target, const TSharedPtr<FJsonObject>& PropertyDefaults, TArray<FPropertyValue>& OutValues, FString& OutError)
    {
        if (!PropertyDefaults.IsValid()) return true;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PropertyDefaults->Values)
        {
            FProperty* Property = FindFProperty<FProperty>(Target->GetClass(), *Pair.Key);
            if (Property == nullptr) { OutError = FString::Printf(TEXT("Component property '%s' was not found on class '%s'."), *Pair.Key, *Target->GetClass()->GetPathName()); return false; }
            if (!IsSafelyEditable(Property)) { OutError = FString::Printf(TEXT("Component property '%s' is not safely editable on a component template."), *Pair.Key); return false; }
            FString ImportText;
            if (!JsonValueToImportText(Pair.Value, ImportText)) { OutError = FString::Printf(TEXT("Component property '%s' must be a JSON string, number, or boolean."), *Pair.Key); return false; }
            FPropertyValue Item;
            Item.Name = Pair.Key;
            Item.Type = Property->GetCPPType();
            Item.PreviousValue = ExportPropertyValue(Property, Target);
            void* Address = Property->ContainerPtrToValuePtr<void>(Target);
            if (Property->ImportText_Direct(*ImportText, Address, Target, PPF_None) == nullptr)
            { OutError = FString::Printf(TEXT("Value '%s' is invalid for component property '%s'. Use Unreal export-text syntax for complex values."), *ImportText, *Pair.Key); return false; }
            Item.Value = ExportPropertyValue(Property, Target);
            Item.bChanged = Item.PreviousValue != Item.Value;
            OutValues.Add(MoveTemp(Item));
        }
        return true;
    }

    bool ValidatePropertyDefaults(UObject* Target, const TSharedPtr<FJsonObject>& PropertyDefaults, FString& OutError)
    {
        UObject* ValidationObject = Target != nullptr ? DuplicateObject<UObject>(Target, GetTransientPackage()) : nullptr;
        TArray<FPropertyValue> Ignored;
        return ValidationObject != nullptr && ApplyPropertyDefaults(ValidationObject, PropertyDefaults, Ignored, OutError);
    }

    bool ParseComponentSpec(const TSharedPtr<FJsonObject>& Object, FComponentSpec& OutSpec, FString& OutError)
    {
        if (!Object.IsValid()
            || !Object->TryGetStringField(TEXT("componentName"), OutSpec.ComponentName)
            || !Object->TryGetStringField(TEXT("componentClassPath"), OutSpec.ComponentClassPath)
            || OutSpec.ComponentName.IsEmpty()
            || OutSpec.ComponentClassPath.IsEmpty())
        {
            OutError = TEXT("Each component requires non-empty componentName and componentClassPath fields.");
            return false;
        }

        Object->TryGetStringField(TEXT("parentComponentName"), OutSpec.ParentComponentName);
        const TSharedPtr<FJsonObject>* Defaults = nullptr;
        if (Object->TryGetObjectField(TEXT("propertyDefaults"), Defaults))
        {
            OutSpec.PropertyDefaults = *Defaults;
        }
        return true;
    }

    bool ResolveAndValidateSpecs(
        UBlueprint* Blueprint,
        TArray<FComponentSpec>& Specs,
        TArray<FComponentResult>& OutResults,
        FString& OutError)
    {
        if (Blueprint == nullptr || Blueprint->SimpleConstructionScript == nullptr)
        {
            OutError = TEXT("Blueprint does not support Simple Construction Script components.");
            return false;
        }

        TMap<FString, UClass*> AvailableComponents;
        for (USCS_Node* Existing : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Existing != nullptr)
            {
                AvailableComponents.Add(Existing->GetVariableName().ToString().ToLower(), Existing->ComponentClass);
            }
        }

        TSet<FString> RequestedNames;
        for (FComponentSpec& Spec : Specs)
        {
            const FString NormalizedName = Spec.ComponentName.ToLower();
            if (RequestedNames.Contains(NormalizedName))
            {
                OutError = FString::Printf(TEXT("Component '%s' appears more than once in the request."), *Spec.ComponentName);
                return false;
            }
            RequestedNames.Add(NormalizedName);

            if (!BlueprintEditToolUtils::ResolveClass(Spec.ComponentClassPath, Spec.ComponentClass, OutError))
            {
                return false;
            }
            if (!Spec.ComponentClass->IsChildOf<UActorComponent>())
            {
                OutError = FString::Printf(TEXT("Component class '%s' must derive from UActorComponent."), *Spec.ComponentClassPath);
                return false;
            }

            FComponentResult Result;
            Result.ComponentName = Spec.ComponentName;
            Result.ComponentClassPath = Spec.ComponentClass->GetPathName();
            Result.ParentComponentName = Spec.ParentComponentName;
            if (UClass* const* ExistingClass = AvailableComponents.Find(NormalizedName))
            {
                if (*ExistingClass != Spec.ComponentClass)
                {
                    OutError = FString::Printf(TEXT("Component '%s' already exists with a different class."), *Spec.ComponentName);
                    return false;
                }
                Result.bAlreadyExists = true;
            }

            if (!Spec.ParentComponentName.IsEmpty())
            {
                UClass* const* ParentClass = AvailableComponents.Find(Spec.ParentComponentName.ToLower());
                if (ParentClass == nullptr)
                {
                    OutError = FString::Printf(
                        TEXT("Parent component '%s' was not found. Batch parents must already exist or appear earlier in the components array."),
                        *Spec.ParentComponentName);
                    return false;
                }
                if (!Spec.ComponentClass->IsChildOf<USceneComponent>() || !(*ParentClass)->IsChildOf<USceneComponent>())
                {
                    OutError = TEXT("Only scene components can participate in parent-child attachment.");
                    return false;
                }
            }

            if (!Result.bAlreadyExists && Spec.PropertyDefaults.IsValid())
            {
                UObject* ValidationObject = DuplicateObject<UObject>(Spec.ComponentClass->GetDefaultObject(), GetTransientPackage());
                TArray<FPropertyValue> IgnoredProperties;
                if (ValidationObject == nullptr || !ApplyPropertyDefaults(ValidationObject, Spec.PropertyDefaults, IgnoredProperties, OutError))
                {
                    return false;
                }
            }

            AvailableComponents.Add(NormalizedName, Spec.ComponentClass);
            OutResults.Add(MoveTemp(Result));
        }
        return true;
    }

    bool AddValidatedSpecs(
        UBlueprint* Blueprint,
        const TArray<FComponentSpec>& Specs,
        TArray<FComponentResult>& InOutResults,
        FString& OutError)
    {
        for (int32 Index = 0; Index < Specs.Num(); ++Index)
        {
            const FComponentSpec& Spec = Specs[Index];
            FComponentResult& Result = InOutResults[Index];
            if (Result.bAlreadyExists)
            {
                continue;
            }

            USCS_Node* ParentNode = Spec.ParentComponentName.IsEmpty()
                ? nullptr
                : FindComponentNode(Blueprint, Spec.ParentComponentName);
            USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(Spec.ComponentClass, *Spec.ComponentName);
            if (NewNode == nullptr || NewNode->ComponentTemplate == nullptr)
            {
                OutError = FString::Printf(TEXT("Unreal failed to create component '%s'."), *Spec.ComponentName);
                return false;
            }

            NewNode->Modify();
            NewNode->ComponentTemplate->Modify();
            TArray<FPropertyValue> AppliedValues;
            if (!ApplyPropertyDefaults(NewNode->ComponentTemplate, Spec.PropertyDefaults, AppliedValues, OutError))
            {
                return false;
            }
            for (const FPropertyValue& Value : AppliedValues)
            {
                Result.AppliedProperties.Add(Value.Name);
                Result.bChanged |= Value.bChanged;
            }

            if (ParentNode != nullptr)
            {
                ParentNode->Modify();
                ParentNode->AddChildNode(NewNode);
            }
            else
            {
                Blueprint->SimpleConstructionScript->AddNode(NewNode);
            }
            Result.bAdded = true;
            Result.bChanged = true;
        }
        return true;
    }
}
