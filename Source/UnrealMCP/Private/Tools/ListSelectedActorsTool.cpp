#include "Tools/ListSelectedActorsTool.h"

#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Tools/WorldToolUtils.h"

FListSelectedActorsTool::FListSelectedActorsTool()
    : FMCPToolBase(TEXT("ListSelectedActors"), TEXT("Lists the currently selected actors from the active editor world."))
{
}

UnrealMCP::FMCPResponse FListSelectedActorsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    TArray<TSharedPtr<FJsonValue>> ActorValues;
    FString Error;

    const bool bSucceeded = UnrealMCP::WorldToolUtils::ExecuteOnGameThreadSync(
        [&ActorValues](FString& OutError)
        {
            UWorld* World = nullptr;
            if (!UnrealMCP::WorldToolUtils::GetEditorWorld(World, OutError))
            {
                return false;
            }

            for (TActorIterator<AActor> It(World); It; ++It)
            {
                if (AActor* Actor = *It; Actor != nullptr && Actor->IsSelected())
                {
                    ActorValues.Add(MakeShared<FJsonValueObject>(UnrealMCP::WorldToolUtils::SerializeActorSummary(Actor, true)));
                }
            }

            return true;
        },
        Error);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("ListSelectedActors failed: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetNumberField(TEXT("count"), ActorValues.Num());
    Result->SetArrayField(TEXT("actors"), ActorValues);
    Response.Result = Result;
    return Response;
}
