#include "Tools/ListBlueprintFunctionsTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Tools/BlueprintToolUtils.h"

FListBlueprintFunctionsTool::FListBlueprintFunctionsTool()
    : FMCPToolBase(TEXT("ListFunctions"), TEXT("Lists Blueprint-declared function graphs and interface graphs."))
{
}

UnrealMCP::FMCPResponse FListBlueprintFunctionsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListFunctions requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListFunctions requires a non-empty params.objectPath."));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    UBlueprint* Blueprint = nullptr;
    FAssetData AssetData;
    if (!UnrealMCP::BlueprintToolUtils::ResolveBlueprintAssetData(AssetRegistryModule.Get(), ObjectPath, Blueprint, AssetData))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListFunctions could not load a Blueprint from params.objectPath."));
    }

    TArray<TSharedPtr<FJsonValue>> Functions;

    TArray<UEdGraph*> SortedFunctionGraphs = Blueprint->FunctionGraphs;
    SortedFunctionGraphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
    {
        return Left.GetName() < Right.GetName();
    });

    for (const UEdGraph* Graph : SortedFunctionGraphs)
    {
        TSharedRef<FJsonObject> FunctionObject = MakeShared<FJsonObject>();
        FunctionObject->SetStringField(TEXT("name"), Graph ? Graph->GetName() : FString());
        FunctionObject->SetStringField(TEXT("displayName"), Graph ? Graph->GetFName().ToString() : FString());
        FunctionObject->SetStringField(TEXT("source"), TEXT("functionGraph"));
        Functions.Add(MakeShared<FJsonValueObject>(FunctionObject));
    }

    for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
    {
        for (const UEdGraph* Graph : InterfaceDescription.Graphs)
        {
            TSharedRef<FJsonObject> FunctionObject = MakeShared<FJsonObject>();
            FunctionObject->SetStringField(TEXT("name"), Graph ? Graph->GetName() : FString());
            FunctionObject->SetStringField(TEXT("displayName"), Graph ? Graph->GetFName().ToString() : FString());
            FunctionObject->SetStringField(TEXT("source"), TEXT("interfaceGraph"));
            FunctionObject->SetStringField(TEXT("interfacePath"), InterfaceDescription.Interface ? InterfaceDescription.Interface->GetPathName() : FString());
            Functions.Add(MakeShared<FJsonValueObject>(FunctionObject));
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetNumberField(TEXT("count"), Functions.Num());
    Result->SetArrayField(TEXT("functions"), Functions);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FListBlueprintFunctionsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path, for example /Game/BP_MyActor.BP_MyActor"));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
