#include "Tools/ListBlueprintComponentsTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SimpleConstructionScript.h"
#include "Tools/BlueprintToolUtils.h"

FListBlueprintComponentsTool::FListBlueprintComponentsTool()
    : FMCPToolBase(TEXT("ListComponents"), TEXT("Lists Blueprint simple-construction-script components and hierarchy data."))
{
}

UnrealMCP::FMCPResponse FListBlueprintComponentsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListComponents requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListComponents requires a non-empty params.objectPath."));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    UBlueprint* Blueprint = nullptr;
    FAssetData AssetData;
    if (!UnrealMCP::BlueprintToolUtils::ResolveBlueprintAssetData(AssetRegistryModule.Get(), ObjectPath, Blueprint, AssetData))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListComponents could not load a Blueprint from params.objectPath."));
    }

    TArray<TSharedPtr<FJsonValue>> Components;
    if (Blueprint->SimpleConstructionScript)
    {
        TArray<const USCS_Node*> SortedNodes;
        for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            SortedNodes.Add(Node);
        }

        SortedNodes.Sort([](const USCS_Node& Left, const USCS_Node& Right)
        {
            return Left.GetVariableName().LexicalLess(Right.GetVariableName());
        });

        for (const USCS_Node* Node : SortedNodes)
        {
            Components.Add(MakeShared<FJsonValueObject>(UnrealMCP::BlueprintToolUtils::SerializeComponentNode(Node, Blueprint->SimpleConstructionScript)));
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetBoolField(TEXT("hasSimpleConstructionScript"), Blueprint->SimpleConstructionScript != nullptr);
    Result->SetNumberField(TEXT("count"), Components.Num());
    Result->SetArrayField(TEXT("components"), Components);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FListBlueprintComponentsTool::BuildInputSchema() const
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
