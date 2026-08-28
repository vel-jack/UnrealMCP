#include "Tools/ListUnrealMCPAutomationTestsTool.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FListUnrealMCPAutomationTestsTool::FListUnrealMCPAutomationTestsTool()
    : FMCPToolBase(
        TEXT("ListUnrealMCPAutomationTests"),
        TEXT("Lists registered UnrealMCP editor automation tests available to the bounded static regression runner."))
{
}

UnrealMCP::FMCPResponse FListUnrealMCPAutomationTestsTool::Execute(
    const UnrealMCP::FMCPRequest& Request) const
{
    FString NameContains;
    if (Request.Params.IsValid())
    {
        Request.Params->TryGetStringField(TEXT("nameContains"), NameContains);
    }
    double RequestedLimit = 100.0;
    if (Request.Params.IsValid())
    {
        Request.Params->TryGetNumberField(TEXT("limit"), RequestedLimit);
    }
    const int32 Limit = FMath::Clamp(FMath::RoundToInt(RequestedLimit), 1, 256);

    TArray<TSharedPtr<FJsonValue>> Tests;
    int32 MatchedCount = 0;
    FString ExecutionError;
    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& Error)
        {
            FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
            Framework.LoadTestModules();
            Framework.SetRequestedTestFilter(
                EAutomationTestFlags::EngineFilter | EAutomationTestFlags::ProductFilter);
            TArray<FAutomationTestInfo> ValidTests;
            Framework.GetValidTestNames(ValidTests);
            ValidTests.Sort([](const FAutomationTestInfo& Left, const FAutomationTestInfo& Right)
            {
                return Left.GetFullTestPath() < Right.GetFullTestPath();
            });

            for (const FAutomationTestInfo& Test : ValidTests)
            {
                const FString FullPath = Test.GetFullTestPath();
                if (!FullPath.StartsWith(TEXT("UnrealMCP."), ESearchCase::CaseSensitive)
                    || (!NameContains.IsEmpty() && !FullPath.Contains(NameContains, ESearchCase::IgnoreCase)))
                {
                    continue;
                }
                ++MatchedCount;
                if (Tests.Num() >= Limit)
                {
                    continue;
                }

                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("testName"), FullPath);
                Item->SetStringField(TEXT("displayName"), Test.GetDisplayName());
                Item->SetStringField(TEXT("command"), Test.GetTestName());
                Item->SetStringField(TEXT("filter"),
                    (Test.GetTestFlags() & EAutomationTestFlags::ProductFilter) != 0
                        ? TEXT("product") : TEXT("engine"));
                Item->SetStringField(TEXT("sourceFile"), Test.GetSourceFile());
                Item->SetNumberField(TEXT("sourceLine"), Test.GetSourceFileLine());
                Tests.Add(MakeShared<FJsonValueObject>(Item));
            }
            return true;
        },
        ExecutionError);
    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetNumberField(TEXT("matchedCount"), MatchedCount);
    Result->SetNumberField(TEXT("returnedCount"), Tests.Num());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetBoolField(TEXT("truncated"), MatchedCount > Tests.Num());
    Result->SetArrayField(TEXT("tests"), Tests);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FListUnrealMCPAutomationTestsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("nameContains"),
        BuildStringProperty(TEXT("Optional case-insensitive substring applied only within registered UnrealMCP.* tests.")));
    TSharedRef<FJsonObject> Limit = MakeShared<FJsonObject>();
    Limit->SetStringField(TEXT("type"), TEXT("integer"));
    Limit->SetNumberField(TEXT("minimum"), 1);
    Limit->SetNumberField(TEXT("maximum"), 256);
    Limit->SetStringField(TEXT("description"), TEXT("Maximum returned tests. Defaults to 100."));
    Properties->SetObjectField(TEXT("limit"), Limit);
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    return Schema;
}
