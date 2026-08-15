#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UBlueprint;
class UClass;
class UObject;
class USCS_Node;

namespace UnrealMCP::BlueprintComponentEditUtils
{
    struct FComponentSpec
    {
        FString ComponentName;
        FString ComponentClassPath;
        FString ParentComponentName;
        TSharedPtr<FJsonObject> PropertyDefaults;
        UClass* ComponentClass = nullptr;
    };

    struct FComponentResult
    {
        FString ComponentName;
        FString ComponentClassPath;
        FString ParentComponentName;
        bool bAlreadyExists = false;
        bool bAdded = false;
        TArray<FString> AppliedProperties;
    };

    bool ParseComponentSpec(
        const TSharedPtr<FJsonObject>& Object,
        FComponentSpec& OutSpec,
        FString& OutError);

    bool ResolveAndValidateSpecs(
        UBlueprint* Blueprint,
        TArray<FComponentSpec>& Specs,
        TArray<FComponentResult>& OutResults,
        FString& OutError);

    bool AddValidatedSpecs(
        UBlueprint* Blueprint,
        const TArray<FComponentSpec>& Specs,
        TArray<FComponentResult>& InOutResults,
        FString& OutError);
}
