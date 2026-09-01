#include "Tools/ListChildBlueprintsTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Containers/Queue.h"
#include "Dom/JsonObject.h"
#include "Tools/BlueprintToolUtils.h"

FListChildBlueprintsTool::FListChildBlueprintsTool()
    : FMCPToolBase(TEXT("ListChildBlueprints"), TEXT("Lists child Blueprints whose parent class is this Blueprint's generated class."))
{
}

UnrealMCP::FMCPResponse FListChildBlueprintsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListChildBlueprints requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListChildBlueprints requires a non-empty params.objectPath."));
    }

    bool bRecursive = false;
    Request.Params->TryGetBoolField(TEXT("recursive"), bRecursive);

    FString RootGeneratedClassPath;
    TArray<TSharedPtr<FJsonValue>> Children;
    FString ExecutionError;
    UnrealMCP::EMCPErrorCode ErrorCode = UnrealMCP::EMCPErrorCode::InvalidParams;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            UBlueprint* Blueprint = nullptr;
            FAssetData AssetData;
            if (!UnrealMCP::BlueprintToolUtils::ResolveBlueprintAssetData(AssetRegistryModule.Get(), ObjectPath, Blueprint, AssetData))
            {
                OutError = TEXT("ListChildBlueprints could not load a Blueprint from params.objectPath.");
                return false;
            }

            RootGeneratedClassPath = AssetData.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath);
            if (RootGeneratedClassPath.IsEmpty())
            {
                OutError = TEXT("ListChildBlueprints could not resolve GeneratedClass asset tag for the Blueprint.");
                ErrorCode = UnrealMCP::EMCPErrorCode::InternalError;
                return false;
            }

            TArray<FAssetData> ChildAssets;
            if (bRecursive)
            {
                TSet<FString> VisitedGeneratedClasses;
                TQueue<FString> PendingGeneratedClasses;
                PendingGeneratedClasses.Enqueue(RootGeneratedClassPath);
                VisitedGeneratedClasses.Add(RootGeneratedClassPath);

                FString CurrentGeneratedClass;
                while (PendingGeneratedClasses.Dequeue(CurrentGeneratedClass))
                {
                    const TArray<FAssetData> DirectChildren = UnrealMCP::BlueprintToolUtils::FindBlueprintAssetsByParentGeneratedClass(AssetRegistryModule.Get(), CurrentGeneratedClass);
                    for (const FAssetData& ChildAsset : DirectChildren)
                    {
                        ChildAssets.Add(ChildAsset);
                        const FString ChildGeneratedClass = ChildAsset.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath);
                        if (!ChildGeneratedClass.IsEmpty() && !VisitedGeneratedClasses.Contains(ChildGeneratedClass))
                        {
                            VisitedGeneratedClasses.Add(ChildGeneratedClass);
                            PendingGeneratedClasses.Enqueue(ChildGeneratedClass);
                        }
                    }
                }
            }
            else
            {
                ChildAssets = UnrealMCP::BlueprintToolUtils::FindBlueprintAssetsByParentGeneratedClass(AssetRegistryModule.Get(), RootGeneratedClassPath);
            }

            Children.Reserve(ChildAssets.Num());
            for (const FAssetData& ChildAsset : ChildAssets)
            {
                Children.Add(MakeShared<FJsonValueObject>(UnrealMCP::BlueprintToolUtils::SerializeBlueprintAssetReference(ChildAsset)));
            }
            return true;
        },
        ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, ErrorCode, ExecutionError.IsEmpty() ? TEXT("ListChildBlueprints could not load a Blueprint from params.objectPath.") : ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetBoolField(TEXT("recursive"), bRecursive);
    Result->SetStringField(TEXT("generatedClassPath"), RootGeneratedClassPath);
    Result->SetNumberField(TEXT("count"), Children.Num());
    Result->SetArrayField(TEXT("children"), Children);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FListChildBlueprintsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path, for example /Game/BP_MyActor.BP_MyActor"));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> RecursiveProperty = MakeShared<FJsonObject>();
    RecursiveProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    RecursiveProperty->SetStringField(TEXT("description"), TEXT("Whether to include descendants beyond direct children. Defaults to false."));
    Properties->SetObjectField(TEXT("recursive"), RecursiveProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
