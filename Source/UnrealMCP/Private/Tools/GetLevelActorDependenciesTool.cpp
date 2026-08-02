#include "Tools/GetLevelActorDependenciesTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/WorldToolUtils.h"

FGetLevelActorDependenciesTool::FGetLevelActorDependenciesTool()
    : FMCPToolBase(TEXT("GetLevelActorDependencies"), TEXT("Returns actor-to-asset relationships for one placed actor, including class source, attached actors, and referenced component assets."))
{
}

UnrealMCP::FMCPResponse FGetLevelActorDependenciesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString ActorObjectPath = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("actorObjectPath"));
    const FString ActorName = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("actorName"));
    const FString ActorLabel = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("actorLabel"));

    if (ActorObjectPath.IsEmpty() && ActorName.IsEmpty() && ActorLabel.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetLevelActorDependencies requires params.actorObjectPath, params.actorName, or params.actorLabel."));
    }

    TSharedPtr<FJsonObject> DependencyObject;
    FString Error;
    const bool bSucceeded = UnrealMCP::WorldToolUtils::ExecuteOnGameThreadSync(
        [&DependencyObject, ActorObjectPath, ActorName, ActorLabel](FString& OutError)
        {
            UWorld* World = nullptr;
            if (!UnrealMCP::WorldToolUtils::GetEditorWorld(World, OutError))
            {
                return false;
            }

            AActor* Actor = !ActorObjectPath.IsEmpty()
                ? UnrealMCP::WorldToolUtils::FindActorByObjectPath(World, ActorObjectPath)
                : UnrealMCP::WorldToolUtils::FindActorByNameOrLabel(World, !ActorLabel.IsEmpty() ? ActorLabel : ActorName);
            if (Actor == nullptr)
            {
                OutError = TEXT("Could not find the requested actor in the active editor world.");
                return false;
            }

            TSharedRef<FJsonObject> ResultObject = MakeShared<FJsonObject>();
            ResultObject->SetObjectField(TEXT("actor"), UnrealMCP::WorldToolUtils::SerializeActorSummary(Actor, true));

            TArray<TSharedPtr<FJsonValue>> AttachedActorValues;
            TArray<AActor*> AttachedActors;
            Actor->GetAttachedActors(AttachedActors);
            for (const AActor* AttachedActor : AttachedActors)
            {
                AttachedActorValues.Add(MakeShared<FJsonValueObject>(UnrealMCP::WorldToolUtils::SerializeActorSummary(AttachedActor, false)));
            }
            ResultObject->SetNumberField(TEXT("attachedActorCount"), AttachedActorValues.Num());
            ResultObject->SetArrayField(TEXT("attachedActors"), AttachedActorValues);

            TArray<TSharedPtr<FJsonValue>> ComponentValues;
            TArray<TSharedPtr<FJsonValue>> AssetReferenceValues;
            TSet<FString> SeenObjectPaths;
            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            for (const UActorComponent* Component : Components)
            {
                ComponentValues.Add(MakeShared<FJsonValueObject>(UnrealMCP::WorldToolUtils::SerializeComponentSummary(Component)));
                UnrealMCP::WorldToolUtils::AppendComponentAssetReferences(Component, AssetReferenceValues, SeenObjectPaths);
            }
            ResultObject->SetNumberField(TEXT("componentCount"), ComponentValues.Num());
            ResultObject->SetArrayField(TEXT("components"), ComponentValues);
            ResultObject->SetNumberField(TEXT("assetReferenceCount"), AssetReferenceValues.Num());
            ResultObject->SetArrayField(TEXT("assetReferences"), AssetReferenceValues);

            TArray<TSharedPtr<FJsonValue>> ClassDependencyValues;
            if (const UClass* ActorClass = Actor->GetClass())
            {
                if (const UObject* GeneratedBy = ActorClass->ClassGeneratedBy)
                {
                    ResultObject->SetStringField(TEXT("classSourceObjectPath"), GeneratedBy->GetPathName());

                    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
                    TArray<FName> DependencyNames;
                    AssetRegistryModule.Get().GetDependencies(FName(*GeneratedBy->GetOutermost()->GetName()), DependencyNames, UE::AssetRegistry::EDependencyCategory::Package);
                    UnrealMCP::AssetRegistryToolUtils::SortNames(DependencyNames);

                    for (const FName& DependencyName : DependencyNames)
                    {
                        ClassDependencyValues.Add(MakeShared<FJsonValueString>(DependencyName.ToString()));
                    }
                }
            }
            ResultObject->SetNumberField(TEXT("classDependencyCount"), ClassDependencyValues.Num());
            ResultObject->SetArrayField(TEXT("classDependencies"), ClassDependencyValues);

            DependencyObject = ResultObject;
            return true;
        },
        Error);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("GetLevelActorDependencies failed: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetObjectField(TEXT("dependencies"), DependencyObject.ToSharedRef());
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetLevelActorDependenciesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Optional exact actor object path from the current editor world."));
    Properties->SetObjectField(TEXT("actorObjectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> ActorNameProperty = MakeShared<FJsonObject>();
    ActorNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    ActorNameProperty->SetStringField(TEXT("description"), TEXT("Optional actor name to resolve when actorObjectPath is not provided."));
    Properties->SetObjectField(TEXT("actorName"), ActorNameProperty);

    TSharedRef<FJsonObject> ActorLabelProperty = MakeShared<FJsonObject>();
    ActorLabelProperty->SetStringField(TEXT("type"), TEXT("string"));
    ActorLabelProperty->SetStringField(TEXT("description"), TEXT("Optional actor label to resolve when actorObjectPath is not provided."));
    Properties->SetObjectField(TEXT("actorLabel"), ActorLabelProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
