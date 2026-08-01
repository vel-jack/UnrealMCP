#include "Tools/CompileAllBlueprintsTool.h"

#include "Algo/Sort.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    FString GetCompileAllBlueprintsSeverityString(EMessageSeverity::Type Severity)
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

FCompileAllBlueprintsTool::FCompileAllBlueprintsTool()
    : FMCPToolBase(TEXT("CompileAllBlueprints"), TEXT("Compiles multiple Blueprint assets under a package path and returns aggregate results."))
{
}

UnrealMCP::FMCPResponse FCompileAllBlueprintsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString PackagePath = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("packagePath"), TEXT("/Game"));
    const bool bRecursive = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("recursive"), true);
    const int32 Limit = UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 100);

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*PackagePath));
    Filter.bRecursivePaths = bRecursive;
    Filter.bIncludeOnlyOnDiskAssets = true;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.Blueprint")));
    Filter.bRecursiveClasses = true;

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets;
    AssetRegistryModule.Get().GetAssets(Filter, Assets);
    Algo::SortBy(Assets, [](const FAssetData& Asset)
    {
        return Asset.GetObjectPathString();
    });
    if (Assets.Num() > Limit)
    {
        Assets.SetNum(Limit);
    }

    int32 SuccessCount = 0;
    int32 FailureCount = 0;
    int32 TotalErrors = 0;
    int32 TotalWarnings = 0;

    TArray<TSharedPtr<FJsonValue>> Results;
    Results.Reserve(Assets.Num());

    for (const FAssetData& Asset : Assets)
    {
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Asset.GetObjectPathString());
        if (!Blueprint)
        {
            FailureCount++;

            TSharedRef<FJsonObject> ItemObject = MakeShared<FJsonObject>();
            ItemObject->SetStringField(TEXT("objectPath"), Asset.GetObjectPathString());
            ItemObject->SetBoolField(TEXT("success"), false);
            ItemObject->SetStringField(TEXT("statusAfter"), TEXT("load_failed"));
            ItemObject->SetStringField(TEXT("message"), TEXT("Could not load Blueprint asset."));
            Results.Add(MakeShared<FJsonValueObject>(ItemObject));
            continue;
        }

        const FString StatusBefore = UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status);

        FCompilerResultsLog CompilerResults;
        CompilerResults.bSilentMode = true;
        FKismetEditorUtilities::CompileBlueprint(
            Blueprint,
            EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection | EBlueprintCompileOptions::BatchCompile,
            &CompilerResults);

        const FString StatusAfter = UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status);
        const bool bCompiledSuccessfully = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
        if (bCompiledSuccessfully)
        {
            SuccessCount++;
        }
        else
        {
            FailureCount++;
        }

        TotalErrors += CompilerResults.NumErrors;
        TotalWarnings += CompilerResults.NumWarnings;

        TArray<TSharedPtr<FJsonValue>> Messages;
        for (const TSharedRef<FTokenizedMessage>& Message : CompilerResults.Messages)
        {
            TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
            MessageObject->SetStringField(TEXT("severity"), GetCompileAllBlueprintsSeverityString(Message->GetSeverity()));
            MessageObject->SetStringField(TEXT("text"), Message->ToText().ToString());
            Messages.Add(MakeShared<FJsonValueObject>(MessageObject));
        }

        TSharedRef<FJsonObject> ItemObject = MakeShared<FJsonObject>();
        ItemObject->SetStringField(TEXT("objectPath"), Asset.GetObjectPathString());
        ItemObject->SetBoolField(TEXT("success"), bCompiledSuccessfully);
        ItemObject->SetStringField(TEXT("statusBefore"), StatusBefore);
        ItemObject->SetStringField(TEXT("statusAfter"), StatusAfter);
        ItemObject->SetNumberField(TEXT("errorCount"), CompilerResults.NumErrors);
        ItemObject->SetNumberField(TEXT("warningCount"), CompilerResults.NumWarnings);
        ItemObject->SetArrayField(TEXT("messages"), Messages);
        Results.Add(MakeShared<FJsonValueObject>(ItemObject));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(FailureCount == 0);
    Result->SetStringField(TEXT("packagePath"), PackagePath);
    Result->SetBoolField(TEXT("recursive"), bRecursive);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetNumberField(TEXT("compiledCount"), Results.Num());
    Result->SetNumberField(TEXT("successCount"), SuccessCount);
    Result->SetNumberField(TEXT("failureCount"), FailureCount);
    Result->SetNumberField(TEXT("totalErrorCount"), TotalErrors);
    Result->SetNumberField(TEXT("totalWarningCount"), TotalWarnings);
    Result->SetArrayField(TEXT("results"), Results);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FCompileAllBlueprintsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> PackagePathProperty = MakeShared<FJsonObject>();
    PackagePathProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackagePathProperty->SetStringField(TEXT("description"), TEXT("Package path scope, for example /Game. Defaults to /Game."));
    Properties->SetObjectField(TEXT("packagePath"), PackagePathProperty);

    TSharedRef<FJsonObject> RecursiveProperty = MakeShared<FJsonObject>();
    RecursiveProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    RecursiveProperty->SetStringField(TEXT("description"), TEXT("Whether to recurse subfolders. Defaults to true."));
    Properties->SetObjectField(TEXT("recursive"), RecursiveProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional max number of Blueprints to compile. Defaults to 100."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
