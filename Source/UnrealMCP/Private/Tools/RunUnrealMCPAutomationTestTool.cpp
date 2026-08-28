#include "Tools/RunUnrealMCPAutomationTestTool.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FRunUnrealMCPAutomationTestTool::FRunUnrealMCPAutomationTestTool()
    : FMCPToolBase(
        TEXT("RunUnrealMCPAutomationTest"),
        TEXT("Runs one exact synchronous UnrealMCP.* editor automation test and returns structured execution evidence."))
{
}

UnrealMCP::FMCPResponse FRunUnrealMCPAutomationTestTool::Execute(
    const UnrealMCP::FMCPRequest& Request) const
{
    FString TestName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("testName"), TestName)
        || !TestName.StartsWith(TEXT("UnrealMCP."), ESearchCase::CaseSensitive))
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("RunUnrealMCPAutomationTest requires an exact testName beginning with 'UnrealMCP.'."));
    }
    if (!UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false))
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("RunUnrealMCPAutomationTest requires confirm=true because tests may create temporary editor assets."));
    }

    bool bPassed = false;
    bool bLatentCommandsEmpty = false;
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    double DurationSeconds = 0.0;
    TArray<TSharedPtr<FJsonValue>> Entries;
    FString ExecutionError;
    const bool bExecuted = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& Error)
        {
            FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
            Framework.LoadTestModules();
            Framework.SetRequestedTestFilter(
                EAutomationTestFlags::EngineFilter | EAutomationTestFlags::ProductFilter);
            TArray<FAutomationTestInfo> ValidTests;
            Framework.GetValidTestNames(ValidTests);
            const FAutomationTestInfo* ResolvedTest = ValidTests.FindByPredicate(
                [&](const FAutomationTestInfo& Candidate)
                {
                    return Candidate.GetFullTestPath().Equals(TestName, ESearchCase::CaseSensitive)
                        || Candidate.GetDisplayName().Equals(TestName, ESearchCase::CaseSensitive)
                        || Candidate.GetTestName().Equals(TestName, ESearchCase::CaseSensitive);
                });
            if (ResolvedTest == nullptr)
            {
                Error = FString::Printf(TEXT("Automation test '%s' is not registered in this editor build."), *TestName);
                return false;
            }
            if (Framework.GetCurrentTest() != nullptr)
            {
                Error = TEXT("Another editor automation test is already running.");
                return false;
            }

            Framework.StartTestByName(ResolvedTest->GetTestName(), 0, ResolvedTest->GetFullTestPath());
            bLatentCommandsEmpty = Framework.ExecuteLatentCommands();
            if (!bLatentCommandsEmpty)
            {
                Framework.DequeueAllCommands();
            }

            FAutomationTestExecutionInfo Info;
            bPassed = Framework.StopTest(Info) && bLatentCommandsEmpty;
            ErrorCount = Info.GetErrorTotal();
            WarningCount = Info.GetWarningTotal();
            DurationSeconds = Info.Duration;
            for (const FAutomationExecutionEntry& Entry : Info.GetEntries())
            {
                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                const TCHAR* EventType = Entry.Event.Type == EAutomationEventType::Error
                    ? TEXT("error")
                    : (Entry.Event.Type == EAutomationEventType::Warning ? TEXT("warning") : TEXT("info"));
                Item->SetStringField(TEXT("type"), EventType);
                Item->SetStringField(TEXT("message"), Entry.Event.Message);
                Item->SetStringField(TEXT("context"), Entry.Event.Context);
                Item->SetStringField(TEXT("filename"), Entry.Filename);
                Item->SetNumberField(TEXT("lineNumber"), Entry.LineNumber);
                Entries.Add(MakeShared<FJsonValueObject>(Item));
            }
            return true;
        },
        ExecutionError);

    if (!bExecuted)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(bPassed);
    Result->SetStringField(TEXT("testName"), TestName);
    Result->SetBoolField(TEXT("passed"), bPassed);
    Result->SetBoolField(TEXT("synchronous"), bLatentCommandsEmpty);
    Result->SetNumberField(TEXT("errorCount"), ErrorCount);
    Result->SetNumberField(TEXT("warningCount"), WarningCount);
    Result->SetNumberField(TEXT("durationSeconds"), DurationSeconds);
    Result->SetArrayField(TEXT("entries"), Entries);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FRunUnrealMCPAutomationTestTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(
        TEXT("testName"), BuildStringProperty(TEXT("Exact registered synchronous UnrealMCP.* automation test name.")));
    Properties->SetObjectField(
        TEXT("confirm"), BuildBoolProperty(TEXT("Required acknowledgement that the test may create temporary editor assets.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {
        MakeShared<FJsonValueString>(TEXT("testName")),
        MakeShared<FJsonValueString>(TEXT("confirm"))});
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    return Schema;
}
