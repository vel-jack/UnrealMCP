#include "Tools/GetBlueprintInterfacesTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tools/BlueprintToolUtils.h"

FGetBlueprintInterfacesTool::FGetBlueprintInterfacesTool()
    : FMCPToolBase(TEXT("GetImplementedInterfaces"), TEXT("Lists interfaces implemented by a Blueprint, with optional inherited interfaces."))
{
}

UnrealMCP::FMCPResponse FGetBlueprintInterfacesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetImplementedInterfaces requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetImplementedInterfaces requires a non-empty params.objectPath."));
    }

    bool bIncludeInherited = false;
    Request.Params->TryGetBoolField(TEXT("includeInherited"), bIncludeInherited);

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    UBlueprint* Blueprint = nullptr;
    FAssetData AssetData;
    if (!UnrealMCP::BlueprintToolUtils::ResolveBlueprintAssetData(AssetRegistryModule.Get(), ObjectPath, Blueprint, AssetData))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetImplementedInterfaces could not load a Blueprint from params.objectPath."));
    }

    TArray<TSharedPtr<FJsonValue>> Interfaces;
    if (bIncludeInherited)
    {
        TArray<UClass*> ImplementedInterfaces;
        FBlueprintEditorUtils::FindImplementedInterfaces(Blueprint, true, ImplementedInterfaces);
        ImplementedInterfaces.Sort([](const UClass& Left, const UClass& Right)
        {
            return Left.GetPathName() < Right.GetPathName();
        });

        for (const UClass* InterfaceClass : ImplementedInterfaces)
        {
            TSharedRef<FJsonObject> InterfaceObject = MakeShared<FJsonObject>();
            InterfaceObject->SetStringField(TEXT("name"), InterfaceClass ? InterfaceClass->GetName() : FString());
            InterfaceObject->SetStringField(TEXT("path"), InterfaceClass ? InterfaceClass->GetPathName() : FString());
            InterfaceObject->SetStringField(TEXT("source"), TEXT("direct_or_inherited"));
            Interfaces.Add(MakeShared<FJsonValueObject>(InterfaceObject));
        }
    }
    else
    {
        for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
        {
            TSharedRef<FJsonObject> InterfaceObject = MakeShared<FJsonObject>();
            InterfaceObject->SetStringField(TEXT("name"), InterfaceDescription.Interface ? InterfaceDescription.Interface->GetName() : FString());
            InterfaceObject->SetStringField(TEXT("path"), InterfaceDescription.Interface ? InterfaceDescription.Interface->GetPathName() : FString());
            InterfaceObject->SetStringField(TEXT("source"), TEXT("direct"));
            InterfaceObject->SetNumberField(TEXT("graphCount"), InterfaceDescription.Graphs.Num());
            Interfaces.Add(MakeShared<FJsonValueObject>(InterfaceObject));
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetBoolField(TEXT("includeInherited"), bIncludeInherited);
    Result->SetNumberField(TEXT("count"), Interfaces.Num());
    Result->SetArrayField(TEXT("interfaces"), Interfaces);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetBlueprintInterfacesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path, for example /Game/BP_MyActor.BP_MyActor"));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> InheritedProperty = MakeShared<FJsonObject>();
    InheritedProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    InheritedProperty->SetStringField(TEXT("description"), TEXT("Whether to include inherited interfaces. Defaults to false."));
    Properties->SetObjectField(TEXT("includeInherited"), InheritedProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
