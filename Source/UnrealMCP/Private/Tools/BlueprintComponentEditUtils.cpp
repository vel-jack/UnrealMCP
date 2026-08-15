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

        bool ApplyPropertyDefaults(
            UObject* Target,
            const TSharedPtr<FJsonObject>& PropertyDefaults,
            TArray<FString>& OutAppliedProperties,
            FString& OutError)
        {
            if (!PropertyDefaults.IsValid())
            {
                return true;
            }

            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PropertyDefaults->Values)
            {
                FProperty* Property = FindFProperty<FProperty>(Target->GetClass(), *Pair.Key);
                if (Property == nullptr)
                {
                    OutError = FString::Printf(TEXT("Component property '%s' was not found on class '%s'."), *Pair.Key, *Target->GetClass()->GetPathName());
                    return false;
                }
                if (!Property->HasAnyPropertyFlags(CPF_Edit)
                    || Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated | CPF_DisableEditOnTemplate))
                {
                    OutError = FString::Printf(TEXT("Component property '%s' is not safely editable on a component template."), *Pair.Key);
                    return false;
                }

                FString ImportText;
                if (!JsonValueToImportText(Pair.Value, ImportText))
                {
                    OutError = FString::Printf(TEXT("Component property '%s' must be a JSON string, number, or boolean."), *Pair.Key);
                    return false;
                }

                void* ValueAddress = Property->ContainerPtrToValuePtr<void>(Target);
                if (Property->ImportText_Direct(*ImportText, ValueAddress, Target, PPF_None) == nullptr)
                {
                    OutError = FString::Printf(
                        TEXT("Value '%s' is invalid for component property '%s'. Use Unreal export-text syntax for complex values."),
                        *ImportText,
                        *Pair.Key);
                    return false;
                }
                OutAppliedProperties.Add(Pair.Key);
            }
            return true;
        }

        USCS_Node* FindNodeByName(UBlueprint* Blueprint, const FString& ComponentName)
        {
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node != nullptr && Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
                {
                    return Node;
                }
            }
            return nullptr;
        }
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
                TArray<FString> IgnoredProperties;
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
                : FindNodeByName(Blueprint, Spec.ParentComponentName);
            USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(Spec.ComponentClass, *Spec.ComponentName);
            if (NewNode == nullptr || NewNode->ComponentTemplate == nullptr)
            {
                OutError = FString::Printf(TEXT("Unreal failed to create component '%s'."), *Spec.ComponentName);
                return false;
            }

            NewNode->Modify();
            NewNode->ComponentTemplate->Modify();
            if (!ApplyPropertyDefaults(NewNode->ComponentTemplate, Spec.PropertyDefaults, Result.AppliedProperties, OutError))
            {
                return false;
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
        }
        return true;
    }
}
