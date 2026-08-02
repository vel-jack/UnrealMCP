#include "Tools/CompileBlueprintTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    FString GetCompileBlueprintSeverityString(EMessageSeverity::Type Severity)
    {
        switch (Severity)
        {
        case EMessageSeverity::Error:
            return TEXT("error");
        case EMessageSeverity::PerformanceWarning:
        case EMessageSeverity::Warning:
            return TEXT("warning");
        case EMessageSeverity::Info:
            return TEXT("info");
        default:
            return TEXT("unknown");
        }
    }
}

FCompileBlueprintTool::FCompileBlueprintTool()
    : FMCPToolBase(TEXT("CompileBlueprint"), TEXT("Compiles one Blueprint asset and returns compiler result details."))
{
}

UnrealMCP::FMCPResponse FCompileBlueprintTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("CompileBlueprint requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("CompileBlueprint requires a non-empty params.objectPath."));
    }

    FString StatusBefore;
    FString StatusAfter;
    bool bCompiledSuccessfully = false;
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    TArray<TSharedPtr<FJsonValue>> Messages;
    FString ExecutionError;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            UBlueprint* Blueprint = nullptr;
            FAssetData AssetData;
            if (!UnrealMCP::BlueprintToolUtils::ResolveBlueprintAssetData(AssetRegistryModule.Get(), ObjectPath, Blueprint, AssetData))
            {
                OutError = TEXT("CompileBlueprint could not load a Blueprint from params.objectPath.");
                return false;
            }

            StatusBefore = UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status);

            FCompilerResultsLog CompilerResults;
            CompilerResults.bSilentMode = true;
            FKismetEditorUtilities::CompileBlueprint(
                Blueprint,
                EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection,
                &CompilerResults);

            Messages.Reset();
            Messages.Reserve(CompilerResults.Messages.Num());
            for (const TSharedRef<FTokenizedMessage>& Message : CompilerResults.Messages)
            {
                TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
                MessageObject->SetStringField(TEXT("severity"), GetCompileBlueprintSeverityString(Message->GetSeverity()));
                MessageObject->SetStringField(TEXT("text"), Message->ToText().ToString());
                Messages.Add(MakeShared<FJsonValueObject>(MessageObject));
            }

            ErrorCount = CompilerResults.NumErrors;
            WarningCount = CompilerResults.NumWarnings;
            StatusAfter = UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status);
            bCompiledSuccessfully = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
            return true;
        },
        ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError.IsEmpty() ? TEXT("CompileBlueprint failed on the game thread.") : ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(bCompiledSuccessfully);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("statusBefore"), StatusBefore);
    Result->SetStringField(TEXT("statusAfter"), StatusAfter);
    Result->SetNumberField(TEXT("errorCount"), ErrorCount);
    Result->SetNumberField(TEXT("warningCount"), WarningCount);
    Result->SetArrayField(TEXT("messages"), Messages);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FCompileBlueprintTool::BuildInputSchema() const
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
