#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UClass;
class UObject;

namespace UnrealMCP::BlueprintEditToolUtils
{
    bool ResolveBlueprint(const FString& ObjectPath, UBlueprint*& OutBlueprint, FString& OutError);
    bool ResolveClass(const FString& ClassPath, UClass*& OutClass, FString& OutError);
    bool SaveAsset(UObject* Asset, FString& OutFilename, FString& OutError);
    bool RefreshAssetIndex(const FString& ObjectPath, FString& OutError);
    bool BuildPinType(
        const FString& TypeName,
        const FString& TypeObjectPath,
        bool bIsArray,
        FEdGraphPinType& OutPinType,
        FString& OutError);
    bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, bool DefaultValue);
    TSharedRef<FJsonObject> BuildStringProperty(const FString& Description);
    TSharedRef<FJsonObject> BuildBoolProperty(const FString& Description);
}
