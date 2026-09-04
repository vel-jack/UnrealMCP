#include "Tools/BlueprintEditToolUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Index/UnrealMCPProjectIndex.h"
#include "Misc/PackageName.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/SavePackage.h"
#include "UnrealMCPModule.h"

namespace UnrealMCP::BlueprintEditToolUtils
{
    bool ResolveBlueprint(const FString& ObjectPath, UBlueprint*& OutBlueprint, FString& OutError)
    {
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FAssetData AssetData;
        if (!BlueprintToolUtils::ResolveBlueprintAssetData(AssetRegistryModule.Get(), ObjectPath, OutBlueprint, AssetData))
        {
            OutError = FString::Printf(TEXT("Could not load Blueprint '%s'."), *ObjectPath);
            return false;
        }
        return true;
    }

    bool ResolveClass(const FString& ClassPath, UClass*& OutClass, FString& OutError)
    {
        OutClass = LoadObject<UClass>(nullptr, *ClassPath);
        if (OutClass == nullptr && !ClassPath.EndsWith(TEXT("_C")))
        {
            FString GeneratedPath = ClassPath;
            FString PackagePath;
            FString ObjectName;
            if (GeneratedPath.Split(TEXT("."), &PackagePath, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
            {
                GeneratedPath = FString::Printf(TEXT("%s.%s_C"), *PackagePath, *ObjectName);
                OutClass = LoadObject<UClass>(nullptr, *GeneratedPath);
            }
        }
        if (OutClass == nullptr)
        {
            OutError = FString::Printf(TEXT("Could not resolve class '%s'. Use a /Script/... class path or generated Blueprint class path."), *ClassPath);
            return false;
        }
        return true;
    }

    bool SaveAsset(UObject* Asset, FString& OutFilename, FString& OutError)
    {
        if (Asset == nullptr || Asset->GetPackage() == nullptr)
        {
            OutError = TEXT("Cannot save a null asset or package.");
            return false;
        }

        UPackage* Package = Asset->GetPackage();
        OutFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, Asset, *OutFilename, SaveArgs))
        {
            OutError = FString::Printf(TEXT("Failed to save package '%s'."), *Package->GetName());
            return false;
        }
        return true;
    }

    bool RefreshAssetIndex(const FString& ObjectPath, FString& OutError)
    {
        FUnrealMCPProjectIndex::FRefreshResult RefreshResult;
        return FUnrealMCPModule::Get().GetProjectIndex().RefreshProjectIndex({ObjectPath}, RefreshResult, OutError);
    }

    bool BuildPinType(const FString& TypeName, const FString& TypeObjectPath, bool bIsArray, FEdGraphPinType& OutPinType, FString& OutError)
    {
        const FString Normalized = TypeName.ToLower();
        if (Normalized == TEXT("bool")) OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        else if (Normalized == TEXT("float"))
        {
            // PC_Real pins require a PinSubCategory naming the concrete precision
            // (PC_Float or PC_Double) -- an empty subcategory is invalid and hard-
            // asserts the Kismet compiler on the next compile (KismetCompilerMisc.cpp,
            // "Erroneous pin subcategory for PC_Real: None"). TypeObjectPath carries
            // that precision hint here ("float" or "double"); UE5 defaults Blueprint
            // reals to double precision, so default to PC_Double when TypeObjectPath
            // is empty or anything other than exactly "float", matching every
            // existing double-typed variable/parameter a caller is likely to add
            // alongside.
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            const FString NormalizedTypeObjectPath = TypeObjectPath.ToLower();
            OutPinType.PinSubCategory = (NormalizedTypeObjectPath == TEXT("float"))
                ? UEdGraphSchema_K2::PC_Float
                : UEdGraphSchema_K2::PC_Double;
        }
        else if (Normalized == TEXT("int") || Normalized == TEXT("integer")) OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
        else if (Normalized == TEXT("string")) OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
        else if (Normalized == TEXT("name")) OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
        else if (Normalized == TEXT("text")) OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
        else if (Normalized == TEXT("object") || Normalized == TEXT("objectreference"))
        {
            UClass* ObjectClass = nullptr;
            if (!ResolveClass(TypeObjectPath, ObjectClass, OutError)) return false;
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            OutPinType.PinSubCategoryObject = ObjectClass;
        }
        else if (Normalized == TEXT("class") || Normalized == TEXT("classreference"))
        {
            UClass* ObjectClass = nullptr;
            if (!ResolveClass(TypeObjectPath, ObjectClass, OutError)) return false;
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
            OutPinType.PinSubCategoryObject = ObjectClass;
        }
        else
        {
            OutError = FString::Printf(TEXT("Unsupported variable type '%s'."), *TypeName);
            return false;
        }
        OutPinType.ContainerType = bIsArray ? EPinContainerType::Array : EPinContainerType::None;
        return true;
    }

    bool BuildContainerPinType(
        const FString& TypeName,
        const FString& TypeObjectPath,
        const FString& ContainerTypeName,
        const FString& ValueTypeName,
        const FString& ValueTypeObjectPath,
        FEdGraphPinType& OutPinType,
        FString& OutError)
    {
        if (!BuildPinType(TypeName, TypeObjectPath, false, OutPinType, OutError))
        {
            return false;
        }

        const FString NormalizedContainer = ContainerTypeName.IsEmpty()
            ? TEXT("none")
            : ContainerTypeName.ToLower();
        if (NormalizedContainer == TEXT("none") || NormalizedContainer == TEXT("scalar"))
        {
            OutPinType.ContainerType = EPinContainerType::None;
        }
        else if (NormalizedContainer == TEXT("array"))
        {
            OutPinType.ContainerType = EPinContainerType::Array;
        }
        else if (NormalizedContainer == TEXT("set"))
        {
            OutPinType.ContainerType = EPinContainerType::Set;
        }
        else if (NormalizedContainer == TEXT("map"))
        {
            if (ValueTypeName.IsEmpty())
            {
                OutError = TEXT("Map container types require valueType.");
                return false;
            }
            FEdGraphPinType ValuePinType;
            if (!BuildPinType(ValueTypeName, ValueTypeObjectPath, false, ValuePinType, OutError))
            {
                return false;
            }
            OutPinType.ContainerType = EPinContainerType::Map;
            OutPinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;
            OutPinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;
            OutPinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;
        }
        else
        {
            OutError = FString::Printf(
                TEXT("Unsupported containerType '%s'. Use none, array, set, or map."),
                *ContainerTypeName);
            return false;
        }
        return true;
    }

    bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, bool DefaultValue)
    {
        bool Value = DefaultValue;
        if (Params.IsValid()) Params->TryGetBoolField(Name, Value);
        return Value;
    }

    TSharedRef<FJsonObject> BuildStringProperty(const FString& Description)
    {
        TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), TEXT("string"));
        Property->SetStringField(TEXT("description"), Description);
        return Property;
    }

    TSharedRef<FJsonObject> BuildBoolProperty(const FString& Description)
    {
        TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), TEXT("boolean"));
        Property->SetStringField(TEXT("description"), Description);
        return Property;
    }
}
