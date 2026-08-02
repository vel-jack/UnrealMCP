#include "Tools/ListActorsTool.h"

#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/WorldToolUtils.h"

FListActorsTool::FListActorsTool()
    : FMCPToolBase(TEXT("ListActors"), TEXT("Lists actors from the active editor world so agents can inspect current level composition without modifying it."))
{
}

UnrealMCP::FMCPResponse FListActorsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const int32 Limit = FMath::Clamp(UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 250), 1, 2000);
    const bool bSelectedOnly = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("selectedOnly"), false);
    const FString Search = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("search"));
    const FString LevelNameFilter = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("levelName"));
    const FString ClassNameFilter = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("className"));

    TArray<TSharedPtr<FJsonValue>> ActorValues;
    int32 TotalMatches = 0;
    FString Error;

    const bool bSucceeded = UnrealMCP::WorldToolUtils::ExecuteOnGameThreadSync(
        [&ActorValues, &TotalMatches, &Error, Limit, bSelectedOnly, Search, LevelNameFilter, ClassNameFilter](FString& OutError)
        {
            UWorld* World = nullptr;
            if (!UnrealMCP::WorldToolUtils::GetEditorWorld(World, OutError))
            {
                return false;
            }

            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* Actor = *It;
                if (Actor == nullptr)
                {
                    continue;
                }

                if (bSelectedOnly && !Actor->IsSelected())
                {
                    continue;
                }

                if (!Search.IsEmpty()
                    && !Actor->GetName().Contains(Search, ESearchCase::IgnoreCase)
                    && !Actor->GetActorLabel().Contains(Search, ESearchCase::IgnoreCase))
                {
                    continue;
                }

                if (!LevelNameFilter.IsEmpty() && !UnrealMCP::WorldToolUtils::GetActorLevelName(Actor).Equals(LevelNameFilter, ESearchCase::IgnoreCase))
                {
                    continue;
                }

                if (!ClassNameFilter.IsEmpty()
                    && !Actor->GetClass()->GetName().Contains(ClassNameFilter, ESearchCase::IgnoreCase)
                    && !Actor->GetClass()->GetPathName().Contains(ClassNameFilter, ESearchCase::IgnoreCase))
                {
                    continue;
                }

                ++TotalMatches;
                if (ActorValues.Num() < Limit)
                {
                    ActorValues.Add(MakeShared<FJsonValueObject>(UnrealMCP::WorldToolUtils::SerializeActorSummary(Actor, true)));
                }
            }

            return true;
        },
        Error);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("ListActors failed: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetNumberField(TEXT("count"), ActorValues.Num());
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetBoolField(TEXT("selectedOnly"), bSelectedOnly);
    Result->SetStringField(TEXT("search"), Search);
    Result->SetStringField(TEXT("levelName"), LevelNameFilter);
    Result->SetStringField(TEXT("className"), ClassNameFilter);
    Result->SetArrayField(TEXT("actors"), ActorValues);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FListActorsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional max number of actors to return. Defaults to 250."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    TSharedRef<FJsonObject> SelectedOnlyProperty = MakeShared<FJsonObject>();
    SelectedOnlyProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    SelectedOnlyProperty->SetStringField(TEXT("description"), TEXT("Whether to only list selected actors. Defaults to false."));
    Properties->SetObjectField(TEXT("selectedOnly"), SelectedOnlyProperty);

    TSharedRef<FJsonObject> SearchProperty = MakeShared<FJsonObject>();
    SearchProperty->SetStringField(TEXT("type"), TEXT("string"));
    SearchProperty->SetStringField(TEXT("description"), TEXT("Optional case-insensitive actor name or label filter."));
    Properties->SetObjectField(TEXT("search"), SearchProperty);

    TSharedRef<FJsonObject> LevelNameProperty = MakeShared<FJsonObject>();
    LevelNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    LevelNameProperty->SetStringField(TEXT("description"), TEXT("Optional exact level name filter."));
    Properties->SetObjectField(TEXT("levelName"), LevelNameProperty);

    TSharedRef<FJsonObject> ClassNameProperty = MakeShared<FJsonObject>();
    ClassNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    ClassNameProperty->SetStringField(TEXT("description"), TEXT("Optional actor class name or class path filter."));
    Properties->SetObjectField(TEXT("className"), ClassNameProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
