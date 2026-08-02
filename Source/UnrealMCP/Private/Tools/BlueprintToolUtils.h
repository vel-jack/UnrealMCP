#pragma once

#include "Async/Async.h"
#include "Algo/Sort.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"

namespace UnrealMCP::BlueprintToolUtils
{
    template <typename TCallable>
    bool ExecuteOnGameThreadSync(TCallable&& Callable, FString& OutError)
    {
        bool bSucceeded = false;
        auto Run = [&]()
        {
            bSucceeded = Callable(OutError);
        };

        if (IsInGameThread())
        {
            Run();
            return bSucceeded;
        }

        FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(false);
        if (CompletionEvent == nullptr)
        {
            OutError = TEXT("Could not create a synchronization event for Blueprint execution.");
            return false;
        }

        ON_SCOPE_EXIT
        {
            FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
        };

        AsyncTask(ENamedThreads::GameThread, [&Run, CompletionEvent]()
        {
            Run();
            CompletionEvent->Trigger();
        });

        CompletionEvent->Wait();
        return bSucceeded;
    }

    inline FString GetBlueprintStatusString(EBlueprintStatus Status)
    {
        switch (Status)
        {
        case BS_Unknown:
            return TEXT("unknown");
        case BS_Dirty:
            return TEXT("dirty");
        case BS_Error:
            return TEXT("error");
        case BS_UpToDate:
            return TEXT("up_to_date");
        case BS_BeingCreated:
            return TEXT("being_created");
        case BS_UpToDateWithWarnings:
            return TEXT("up_to_date_with_warnings");
        default:
            return TEXT("unknown");
        }
    }

    inline UBlueprint* LoadBlueprintFromObjectPath(const FString& ObjectPath)
    {
        if (ObjectPath.IsEmpty())
        {
            return nullptr;
        }

        return LoadObject<UBlueprint>(nullptr, *ObjectPath);
    }

    inline TSharedRef<FJsonObject> SerializePinType(const FEdGraphPinType& PinType)
    {
        TSharedRef<FJsonObject> PinTypeObject = MakeShared<FJsonObject>();
        PinTypeObject->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
        PinTypeObject->SetStringField(TEXT("subCategory"), PinType.PinSubCategory.ToString());
        PinTypeObject->SetStringField(TEXT("subCategoryObjectPath"), PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetPathName() : FString());
        PinTypeObject->SetStringField(TEXT("containerType"), StaticEnum<EPinContainerType>()->GetNameStringByValue(static_cast<int64>(PinType.ContainerType)));
        PinTypeObject->SetBoolField(TEXT("isReference"), PinType.bIsReference);
        PinTypeObject->SetBoolField(TEXT("isConst"), PinType.bIsConst);
        PinTypeObject->SetBoolField(TEXT("isArray"), PinType.IsArray());
        PinTypeObject->SetBoolField(TEXT("isSet"), PinType.IsSet());
        PinTypeObject->SetBoolField(TEXT("isMap"), PinType.IsMap());
        PinTypeObject->SetStringField(TEXT("valueCategory"), PinType.PinValueType.TerminalCategory.ToString());
        PinTypeObject->SetStringField(TEXT("valueSubCategory"), PinType.PinValueType.TerminalSubCategory.ToString());
        PinTypeObject->SetStringField(TEXT("valueSubCategoryObjectPath"), PinType.PinValueType.TerminalSubCategoryObject.IsValid() ? PinType.PinValueType.TerminalSubCategoryObject->GetPathName() : FString());
        return PinTypeObject;
    }

    inline TSharedRef<FJsonObject> SerializeVariable(const FBPVariableDescription& Variable)
    {
        TSharedRef<FJsonObject> VariableObject = MakeShared<FJsonObject>();
        VariableObject->SetStringField(TEXT("name"), Variable.VarName.ToString());
        VariableObject->SetStringField(TEXT("guid"), Variable.VarGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        VariableObject->SetStringField(TEXT("friendlyName"), Variable.FriendlyName);
        VariableObject->SetStringField(TEXT("category"), Variable.Category.ToString());
        VariableObject->SetStringField(TEXT("repNotifyFunc"), Variable.RepNotifyFunc.ToString());
        VariableObject->SetNumberField(TEXT("propertyFlags"), static_cast<double>(Variable.PropertyFlags));
        VariableObject->SetObjectField(TEXT("type"), SerializePinType(Variable.VarType));
        return VariableObject;
    }

    inline TSharedRef<FJsonObject> SerializeComponentNode(const USCS_Node* Node, const USimpleConstructionScript* SCS)
    {
        TSharedRef<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
        ComponentObject->SetStringField(TEXT("variableName"), Node ? Node->GetVariableName().ToString() : FString());
        ComponentObject->SetStringField(TEXT("componentClassPath"), Node && Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString());
        ComponentObject->SetStringField(TEXT("componentClassName"), Node && Node->ComponentClass ? Node->ComponentClass->GetName() : FString());
        ComponentObject->SetStringField(TEXT("componentTemplateName"), Node && Node->ComponentTemplate ? Node->ComponentTemplate->GetName() : FString());
        ComponentObject->SetStringField(TEXT("componentTemplatePath"), Node && Node->ComponentTemplate ? Node->ComponentTemplate->GetPathName() : FString());
        ComponentObject->SetBoolField(TEXT("isDefaultSceneRoot"), SCS && Node == SCS->GetDefaultSceneRootNode());

        const USCS_Node* ParentNode = SCS && Node ? SCS->FindParentNode(const_cast<USCS_Node*>(Node)) : nullptr;
        ComponentObject->SetStringField(TEXT("parentVariableName"), ParentNode ? ParentNode->GetVariableName().ToString() : FString());
        ComponentObject->SetNumberField(TEXT("childCount"), Node ? Node->GetChildNodes().Num() : 0);
        return ComponentObject;
    }

    inline FString GetAssetTagString(const FAssetData& AssetData, FName TagName)
    {
        return AssetData.GetTagValueRef<FString>(TagName);
    }

    inline TSharedRef<FJsonObject> SerializeBlueprintAssetReference(const FAssetData& AssetData)
    {
        TSharedRef<FJsonObject> BlueprintObject = MakeShared<FJsonObject>();
        BlueprintObject->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
        BlueprintObject->SetStringField(TEXT("classPath"), AssetData.AssetClassPath.ToString());
        BlueprintObject->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
        BlueprintObject->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
        BlueprintObject->SetStringField(TEXT("packagePath"), AssetData.PackagePath.ToString());
        BlueprintObject->SetStringField(TEXT("generatedClassPath"), GetAssetTagString(AssetData, FBlueprintTags::GeneratedClassPath));
        BlueprintObject->SetStringField(TEXT("parentClassPath"), GetAssetTagString(AssetData, FBlueprintTags::ParentClassPath));
        BlueprintObject->SetStringField(TEXT("nativeParentClassPath"), GetAssetTagString(AssetData, FBlueprintTags::NativeParentClassPath));
        return BlueprintObject;
    }

    inline bool ResolveBlueprintAssetData(IAssetRegistry& AssetRegistry, const FString& ObjectPath, UBlueprint*& OutBlueprint, FAssetData& OutAssetData)
    {
        OutBlueprint = LoadBlueprintFromObjectPath(ObjectPath);
        if (!OutBlueprint)
        {
            return false;
        }

        OutAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
        return OutAssetData.IsValid();
    }

    inline TArray<FAssetData> FindBlueprintAssetsByParentGeneratedClass(IAssetRegistry& AssetRegistry, const FString& ParentGeneratedClassPath)
    {
        TArray<FAssetData> AllAssets;
        AssetRegistry.GetAllAssets(AllAssets, true);

        TArray<FAssetData> Matches;
        for (const FAssetData& Asset : AllAssets)
        {
            const FString ParentClassPath = GetAssetTagString(Asset, FBlueprintTags::ParentClassPath);
            if (!ParentClassPath.IsEmpty() && ParentClassPath == ParentGeneratedClassPath)
            {
                Matches.Add(Asset);
            }
        }

        Algo::SortBy(Matches, [](const FAssetData& Asset)
        {
            return Asset.GetObjectPathString();
        });
        return Matches;
    }
}
