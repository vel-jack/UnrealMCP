#pragma once

#include "Async/Async.h"
#include "Components/ActorComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Set.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Selection.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"

namespace UnrealMCP::WorldToolUtils
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
            OutError = TEXT("Could not create a synchronization event for world execution.");
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

    inline bool GetEditorWorld(UWorld*& OutWorld, FString& OutError)
    {
        OutWorld = nullptr;

        if (GEditor == nullptr)
        {
            OutError = TEXT("The Unreal Editor is not available.");
            return false;
        }

        OutWorld = GEditor->GetEditorWorldContext().World();
        if (OutWorld == nullptr)
        {
            OutError = TEXT("Could not resolve the active editor world.");
            return false;
        }

        return true;
    }

    inline TSharedRef<FJsonObject> SerializeVector(const FVector& Value)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("x"), Value.X);
        Object->SetNumberField(TEXT("y"), Value.Y);
        Object->SetNumberField(TEXT("z"), Value.Z);
        return Object;
    }

    inline TSharedRef<FJsonObject> SerializeRotator(const FRotator& Value)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("pitch"), Value.Pitch);
        Object->SetNumberField(TEXT("yaw"), Value.Yaw);
        Object->SetNumberField(TEXT("roll"), Value.Roll);
        return Object;
    }

    inline FString GetActorLevelName(const AActor* Actor)
    {
        return Actor && Actor->GetLevel() ? Actor->GetLevel()->GetName() : FString();
    }

    inline FString GetFolderPathString(const AActor* Actor)
    {
#if WITH_EDITOR
        return Actor ? Actor->GetFolderPath().ToString() : FString();
#else
        return FString();
#endif
    }

    inline TArray<TSharedPtr<FJsonValue>> SerializeNameArray(const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Names.Num());
        for (const FName& Name : Names)
        {
            Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
        }
        return Values;
    }

    inline TArray<TSharedPtr<FJsonValue>> SerializeStringArray(const TArray<FString>& Items)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Items.Num());
        for (const FString& Item : Items)
        {
            Values.Add(MakeShared<FJsonValueString>(Item));
        }
        return Values;
    }

    inline TSharedRef<FJsonObject> SerializeAssetReference(const FString& Kind, const FString& ObjectPath, const FString& ClassPath = FString())
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("kind"), Kind);
        Object->SetStringField(TEXT("objectPath"), ObjectPath);
        if (!ClassPath.IsEmpty())
        {
            Object->SetStringField(TEXT("classPath"), ClassPath);
        }
        return Object;
    }

    inline void AppendComponentAssetReferences(const UActorComponent* Component, TArray<TSharedPtr<FJsonValue>>& OutReferences, TSet<FString>& SeenObjectPaths)
    {
        auto AddReference = [&OutReferences, &SeenObjectPaths](const FString& Kind, const UObject* Asset)
        {
            if (Asset == nullptr)
            {
                return;
            }

            const FString ObjectPath = Asset->GetPathName();
            if (ObjectPath.IsEmpty() || SeenObjectPaths.Contains(ObjectPath))
            {
                return;
            }

            SeenObjectPaths.Add(ObjectPath);
            OutReferences.Add(MakeShared<FJsonValueObject>(SerializeAssetReference(Kind, ObjectPath, Asset->GetClass()->GetPathName())));
        };

        if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
        {
            AddReference(TEXT("static_mesh"), StaticMeshComponent->GetStaticMesh());
        }

        if (const USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(Component))
        {
            AddReference(TEXT("skeletal_mesh"), SkeletalMeshComponent->GetSkeletalMeshAsset());
        }

        if (const UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
        {
            for (int32 MaterialIndex = 0; MaterialIndex < PrimitiveComponent->GetNumMaterials(); ++MaterialIndex)
            {
                AddReference(TEXT("material"), PrimitiveComponent->GetMaterial(MaterialIndex));
            }
        }

        if (const UChildActorComponent* ChildActorComponent = Cast<UChildActorComponent>(Component))
        {
            if (const UClass* ChildActorClass = ChildActorComponent->GetChildActorClass())
            {
                const UObject* ChildActorAsset = ChildActorClass->ClassGeneratedBy.Get();
                AddReference(TEXT("child_actor_class"), ChildActorAsset != nullptr ? ChildActorAsset : static_cast<const UObject*>(ChildActorClass));
            }
        }
    }

    inline TSharedRef<FJsonObject> SerializeComponentSummary(const UActorComponent* Component)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("name"), Component ? Component->GetName() : FString());
        Object->SetStringField(TEXT("classPath"), Component ? Component->GetClass()->GetPathName() : FString());
        Object->SetStringField(TEXT("componentPath"), Component ? Component->GetPathName() : FString());
        Object->SetBoolField(TEXT("active"), Component ? Component->IsActive() : false);

        if (const USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
        {
            Object->SetObjectField(TEXT("relativeLocation"), SerializeVector(SceneComponent->GetRelativeLocation()));
            Object->SetObjectField(TEXT("relativeRotation"), SerializeRotator(SceneComponent->GetRelativeRotation()));
            Object->SetObjectField(TEXT("relativeScale"), SerializeVector(SceneComponent->GetRelativeScale3D()));
        }

        return Object;
    }

    inline TSharedRef<FJsonObject> SerializeActorSummary(const AActor* Actor, const bool bIncludeTransform = true)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("name"), Actor ? Actor->GetName() : FString());
        Object->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : FString());
        Object->SetStringField(TEXT("objectPath"), Actor ? Actor->GetPathName() : FString());
        Object->SetStringField(TEXT("classPath"), Actor ? Actor->GetClass()->GetPathName() : FString());
        Object->SetStringField(TEXT("className"), Actor ? Actor->GetClass()->GetName() : FString());
        Object->SetStringField(TEXT("levelName"), GetActorLevelName(Actor));
        Object->SetStringField(TEXT("folderPath"), GetFolderPathString(Actor));
        Object->SetBoolField(TEXT("selected"), Actor && Actor->IsSelected());
        Object->SetBoolField(TEXT("hiddenInGame"), Actor ? Actor->IsHidden() : false);
        Object->SetBoolField(TEXT("pendingKill"), Actor ? Actor->IsActorBeingDestroyed() : false);
        Object->SetNumberField(TEXT("tagCount"), Actor ? Actor->Tags.Num() : 0);

        if (Actor)
        {
            Object->SetArrayField(TEXT("tags"), SerializeNameArray(Actor->Tags));
            if (bIncludeTransform)
            {
                Object->SetObjectField(TEXT("location"), SerializeVector(Actor->GetActorLocation()));
                Object->SetObjectField(TEXT("rotation"), SerializeRotator(Actor->GetActorRotation()));
                Object->SetObjectField(TEXT("scale"), SerializeVector(Actor->GetActorScale3D()));
            }

            if (const AActor* ParentActor = Actor->GetAttachParentActor())
            {
                Object->SetStringField(TEXT("parentActorPath"), ParentActor->GetPathName());
                Object->SetStringField(TEXT("parentActorLabel"), ParentActor->GetActorLabel());
            }

            if (const UClass* ActorClass = Actor->GetClass())
            {
                Object->SetBoolField(TEXT("isBlueprintClass"), ActorClass->ClassGeneratedBy != nullptr);
                if (const UObject* GeneratedBy = ActorClass->ClassGeneratedBy)
                {
                    Object->SetStringField(TEXT("blueprintAssetPath"), GeneratedBy->GetPathName());
                }
            }
        }

        return Object;
    }

    inline AActor* FindActorByObjectPath(UWorld* World, const FString& ObjectPath)
    {
        if (World == nullptr || ObjectPath.IsEmpty())
        {
            return nullptr;
        }

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetPathName().Equals(ObjectPath, ESearchCase::CaseSensitive))
            {
                return *It;
            }
        }

        return nullptr;
    }

    inline AActor* FindActorByNameOrLabel(UWorld* World, const FString& NameOrLabel)
    {
        if (World == nullptr || NameOrLabel.IsEmpty())
        {
            return nullptr;
        }

        AActor* PrefixMatch = nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase)
                || It->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase))
            {
                return *It;
            }

            if (PrefixMatch == nullptr
                && (It->GetName().Contains(NameOrLabel, ESearchCase::IgnoreCase)
                    || It->GetActorLabel().Contains(NameOrLabel, ESearchCase::IgnoreCase)))
            {
                PrefixMatch = *It;
            }
        }

        return PrefixMatch;
    }
}
