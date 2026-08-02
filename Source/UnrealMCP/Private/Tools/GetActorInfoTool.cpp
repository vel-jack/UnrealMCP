#include "Tools/GetActorInfoTool.h"

#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/WorldToolUtils.h"

FGetActorInfoTool::FGetActorInfoTool()
    : FMCPToolBase(TEXT("GetActorInfo"), TEXT("Returns detailed information about one actor from the active editor world, including transform, components, and attached actors."))
{
}

UnrealMCP::FMCPResponse FGetActorInfoTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString ActorObjectPath = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("actorObjectPath"));
    const FString ActorName = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("actorName"));
    const FString ActorLabel = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("actorLabel"));

    if (ActorObjectPath.IsEmpty() && ActorName.IsEmpty() && ActorLabel.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetActorInfo requires params.actorObjectPath, params.actorName, or params.actorLabel."));
    }

    TSharedPtr<FJsonObject> ActorInfo;
    FString Error;
    const bool bSucceeded = UnrealMCP::WorldToolUtils::ExecuteOnGameThreadSync(
        [&ActorInfo, ActorObjectPath, ActorName, ActorLabel](FString& OutError)
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

            TSharedRef<FJsonObject> ActorObject = UnrealMCP::WorldToolUtils::SerializeActorSummary(Actor, true);
            ActorObject->SetBoolField(TEXT("isEditable"), Actor->IsEditable());
            ActorObject->SetBoolField(TEXT("isListedInSceneOutliner"), Actor->IsListedInSceneOutliner());
            ActorObject->SetBoolField(TEXT("isTemporarilyHiddenInEditor"), Actor->IsTemporarilyHiddenInEditor());

            TArray<TSharedPtr<FJsonValue>> ComponentValues;
            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            for (const UActorComponent* Component : Components)
            {
                ComponentValues.Add(MakeShared<FJsonValueObject>(UnrealMCP::WorldToolUtils::SerializeComponentSummary(Component)));
            }
            ActorObject->SetNumberField(TEXT("componentCount"), ComponentValues.Num());
            ActorObject->SetArrayField(TEXT("components"), ComponentValues);

            TArray<AActor*> AttachedActors;
            Actor->GetAttachedActors(AttachedActors);
            TArray<TSharedPtr<FJsonValue>> AttachedActorValues;
            for (const AActor* AttachedActor : AttachedActors)
            {
                AttachedActorValues.Add(MakeShared<FJsonValueObject>(UnrealMCP::WorldToolUtils::SerializeActorSummary(AttachedActor, false)));
            }
            ActorObject->SetNumberField(TEXT("attachedActorCount"), AttachedActorValues.Num());
            ActorObject->SetArrayField(TEXT("attachedActors"), AttachedActorValues);

            ActorInfo = ActorObject;
            return true;
        },
        Error);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("GetActorInfo failed: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetObjectField(TEXT("actor"), ActorInfo.ToSharedRef());
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetActorInfoTool::BuildInputSchema() const
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
