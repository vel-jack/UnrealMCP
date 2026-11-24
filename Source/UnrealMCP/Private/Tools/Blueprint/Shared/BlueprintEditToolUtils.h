#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UClass;
class UObject;
class FTokenizedMessage;
class FJsonObject;

namespace UnrealMCP::BlueprintEditToolUtils
{
    bool ResolveBlueprint(const FString& ObjectPath, UBlueprint*& OutBlueprint, FString& OutError);
    bool ResolveClass(const FString& ClassPath, UClass*& OutClass, FString& OutError);
    bool DescribeBrokenNodes(UBlueprint* Blueprint, FJsonObject& Result);
    bool SaveAsset(UObject* Asset, FString& OutFilename, FString& OutError);
    bool RefreshAssetIndex(const FString& ObjectPath, FString& OutError);
    bool BuildPinType(
        const FString& TypeName,
        const FString& TypeObjectPath,
        bool bIsArray,
        FEdGraphPinType& OutPinType,
        FString& OutError);
    bool BuildContainerPinType(
        const FString& TypeName,
        const FString& TypeObjectPath,
        const FString& ContainerTypeName,
        const FString& ValueTypeName,
        const FString& ValueTypeObjectPath,
        FEdGraphPinType& OutPinType,
        FString& OutError);
    bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, bool DefaultValue);
    TSharedRef<FJsonObject> BuildStringProperty(const FString& Description);
    TSharedRef<FJsonObject> BuildBoolProperty(const FString& Description);
    // {graph, nodeGuid, message} for one compiler message, located through its object tokens.
    TSharedRef<FJsonObject> BuildLocatedCompileMessage(const FTokenizedMessage& Message);
    // Compiles without saving; returns true when the Blueprint ends up up-to-date (warnings allowed).
    bool CompileWithLocatedMessages(UBlueprint* Blueprint, TArray<TSharedPtr<FJsonValue>>& OutErrors, TArray<TSharedPtr<FJsonValue>>& OutWarnings);
}
