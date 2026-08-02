#include "Tools/FindActorsUsingBlueprintTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/WorldToolUtils.h"

FFindActorsUsingBlueprintTool::FFindActorsUsingBlueprintTool()
    : FMCPToolBase(TEXT("FindActorsUsingBlueprint"), TEXT("Finds actors in the active editor world whose class was generated from a specific Blueprint asset."))
{
}

UnrealMCP::FMCPResponse FFindActorsUsingBlueprintTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    FString BlueprintObjectPath;
    FName BlueprintPackageName;
    if (!UnrealMCP::AssetRegistryToolUtils::ResolvePackageName(Request.Params, AssetRegistryModule.Get(), BlueprintObjectPath, BlueprintPackageName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindActorsUsingBlueprint requires params.objectPath or params.packageName for a Blueprint asset."));
    }

    const int32 Limit = FMath::Clamp(UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 200), 1, 2000);

    TArray<TSharedPtr<FJsonValue>> ActorValues;
    FString Error;
    const bool bSucceeded = UnrealMCP::WorldToolUtils::ExecuteOnGameThreadSync(
        [&ActorValues, BlueprintObjectPath, Limit](FString& OutError)
        {
            UWorld* World = nullptr;
            if (!UnrealMCP::WorldToolUtils::GetEditorWorld(World, OutError))
            {
                return false;
            }

            UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintObjectPath);
            if (Blueprint == nullptr)
            {
                OutError = TEXT("Could not load the requested Blueprint asset.");
                return false;
            }

            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* Actor = *It;
                if (Actor == nullptr || Actor->GetClass() == nullptr)
                {
                    continue;
                }

                if (Actor->GetClass()->ClassGeneratedBy == Blueprint)
                {
                    if (ActorValues.Num() < Limit)
                    {
                        ActorValues.Add(MakeShared<FJsonValueObject>(UnrealMCP::WorldToolUtils::SerializeActorSummary(Actor, true)));
                    }
                }
            }

            return true;
        },
        Error);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("FindActorsUsingBlueprint failed: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("blueprintObjectPath"), BlueprintObjectPath);
    Result->SetStringField(TEXT("packageName"), BlueprintPackageName.ToString());
    Result->SetNumberField(TEXT("count"), ActorValues.Num());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetArrayField(TEXT("actors"), ActorValues);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindActorsUsingBlueprintTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path, for example /Game/BP_MyActor.BP_MyActor."));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> PackageNameProperty = MakeShared<FJsonObject>();
    PackageNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackageNameProperty->SetStringField(TEXT("description"), TEXT("Optional Blueprint package name when objectPath is not provided."));
    Properties->SetObjectField(TEXT("packageName"), PackageNameProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional max number of matching actors to return. Defaults to 200."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
