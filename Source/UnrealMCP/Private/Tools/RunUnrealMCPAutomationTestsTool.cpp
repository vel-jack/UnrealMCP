#include "Tools/RunUnrealMCPAutomationTestsTool.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/RunUnrealMCPAutomationTestTool.h"

namespace
{
    bool RequiresSuiteTailIsolation(const FString& TestName)
    {
        // This negative fixture deliberately leaves a Blueprint compiler-invalid before
        // verifying rollback. Unreal's compiler/streaming cleanup can otherwise leak into
        // the immediately following synchronous test even though the test itself completed.
        return TestName == TEXT("UnrealMCP.Blueprint.Authoring.GraphPatch.CompileRollback.Live");
    }
}

FRunUnrealMCPAutomationTestsTool::FRunUnrealMCPAutomationTestsTool()
    : FMCPToolBase(
        TEXT("RunUnrealMCPAutomationTests"),
        TEXT("Runs a bounded explicit list of synchronous UnrealMCP editor automation tests and returns one suite result."))
{
}

UnrealMCP::FMCPResponse FRunUnrealMCPAutomationTestsTool::Execute(
    const UnrealMCP::FMCPRequest& Request) const
{
    const TArray<TSharedPtr<FJsonValue>>* TestValues = nullptr;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetArrayField(TEXT("testNames"), TestValues)
        || TestValues == nullptr || TestValues->IsEmpty() || TestValues->Num() > 64)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("RunUnrealMCPAutomationTests requires 1-64 exact testNames."));
    }
    if (!UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("RunUnrealMCPAutomationTests requires confirm=true because tests may create temporary editor assets."));
    }
    const bool bContinueOnFailure = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("continueOnFailure"), true);

    TArray<FString> RequestedTestNames;
    TSet<FString> UniqueNames;
    for (int32 Index = 0; Index < TestValues->Num(); ++Index)
    {
        FString TestName;
        if (!(*TestValues)[Index].IsValid()
            || !(*TestValues)[Index]->TryGetString(TestName)
            || !TestName.StartsWith(TEXT("UnrealMCP."), ESearchCase::CaseSensitive)
            || UniqueNames.Contains(TestName))
        {
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
                FString::Printf(TEXT("testNames[%d] must be a unique exact UnrealMCP.* test name."), Index));
        }
        UniqueNames.Add(TestName);
        RequestedTestNames.Add(MoveTemp(TestName));
    }

    TArray<FString> TestNames;
    TestNames.Reserve(RequestedTestNames.Num());
    for (const FString& TestName : RequestedTestNames)
    {
        if (!RequiresSuiteTailIsolation(TestName)) TestNames.Add(TestName);
    }
    for (const FString& TestName : RequestedTestNames)
    {
        if (RequiresSuiteTailIsolation(TestName)) TestNames.Add(TestName);
    }
    const bool bExecutionOrderAdjusted = TestNames != RequestedTestNames;

    FRunUnrealMCPAutomationTestTool SingleTestTool;
    TArray<TSharedPtr<FJsonValue>> TestResults;
    int32 PassedCount = 0;
    int32 FailedCount = 0;
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    double DurationSeconds = 0.0;
    bool bStoppedEarly = false;

    for (int32 Index = 0; Index < TestNames.Num(); ++Index)
    {
        UnrealMCP::FMCPRequest SingleRequest;
        SingleRequest.Id = FString::Printf(TEXT("%s:%d"), *Request.Id, Index);
        SingleRequest.Method = TEXT("RunUnrealMCPAutomationTest");
        SingleRequest.Params = MakeShared<FJsonObject>();
        SingleRequest.Params->SetStringField(TEXT("testName"), TestNames[Index]);
        SingleRequest.Params->SetBoolField(TEXT("confirm"), true);
        const UnrealMCP::FMCPResponse SingleResponse = SingleTestTool.Execute(SingleRequest);

        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("testName"), TestNames[Index]);
        bool bPassed = false;
        if (SingleResponse.Error.IsSet())
        {
            Item->SetBoolField(TEXT("success"), false);
            Item->SetBoolField(TEXT("passed"), false);
            Item->SetStringField(TEXT("message"), SingleResponse.Error.GetValue().Message);
            ++ErrorCount;
        }
        else if (SingleResponse.Result.IsValid())
        {
            Item = SingleResponse.Result.ToSharedRef();
            Item->TryGetBoolField(TEXT("passed"), bPassed);
            double TestErrors = 0.0;
            double TestWarnings = 0.0;
            double TestDuration = 0.0;
            Item->TryGetNumberField(TEXT("errorCount"), TestErrors);
            Item->TryGetNumberField(TEXT("warningCount"), TestWarnings);
            Item->TryGetNumberField(TEXT("durationSeconds"), TestDuration);
            ErrorCount += FMath::RoundToInt(TestErrors);
            WarningCount += FMath::RoundToInt(TestWarnings);
            DurationSeconds += TestDuration;
        }

        if (bPassed) ++PassedCount;
        else ++FailedCount;
        TestResults.Add(MakeShared<FJsonValueObject>(Item));
        if (!bPassed && !bContinueOnFailure)
        {
            bStoppedEarly = Index + 1 < TestNames.Num();
            break;
        }
        if (Index + 1 < TestNames.Num())
        {
            // Each exact test runs synchronously, but fixture cleanup, compiler diagnostics,
            // and streaming state can settle on the following editor frame. Keep that work
            // outside both tests so it cannot contaminate the next result.
            FPlatformProcess::SleepNoStats(0.1f);
            if (GLog != nullptr)
            {
                GLog->FlushThreadedLogs();
            }
        }
    }

    const bool bPassed = FailedCount == 0 && TestResults.Num() == TestNames.Num();
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(bPassed);
    Result->SetBoolField(TEXT("passed"), bPassed);
    Result->SetNumberField(TEXT("requestedCount"), RequestedTestNames.Num());
    Result->SetNumberField(TEXT("executedCount"), TestResults.Num());
    Result->SetNumberField(TEXT("passedCount"), PassedCount);
    Result->SetNumberField(TEXT("failedCount"), FailedCount);
    Result->SetNumberField(TEXT("errorCount"), ErrorCount);
    Result->SetNumberField(TEXT("warningCount"), WarningCount);
    Result->SetNumberField(TEXT("durationSeconds"), DurationSeconds);
    Result->SetBoolField(TEXT("continueOnFailure"), bContinueOnFailure);
    Result->SetBoolField(TEXT("stoppedEarly"), bStoppedEarly);
    Result->SetBoolField(TEXT("executionOrderAdjusted"), bExecutionOrderAdjusted);
    TArray<TSharedPtr<FJsonValue>> ExecutionOrder;
    ExecutionOrder.Reserve(TestNames.Num());
    for (const FString& TestName : TestNames)
    {
        ExecutionOrder.Add(MakeShared<FJsonValueString>(TestName));
    }
    Result->SetArrayField(TEXT("executionOrder"), ExecutionOrder);
    Result->SetArrayField(TEXT("tests"), TestResults);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FRunUnrealMCPAutomationTestsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Names = MakeShared<FJsonObject>();
    Names->SetStringField(TEXT("type"), TEXT("array"));
    Names->SetNumberField(TEXT("minItems"), 1);
    Names->SetNumberField(TEXT("maxItems"), 64);
    Names->SetBoolField(TEXT("uniqueItems"), true);
    Names->SetStringField(TEXT("description"),
        TEXT("Explicit registered UnrealMCP.* tests in preferred order; isolation-sensitive negative tests run last."));
    Names->SetObjectField(TEXT("items"), BuildStringProperty(TEXT("Exact registered test name.")));
    Properties->SetObjectField(TEXT("testNames"), Names);
    Properties->SetObjectField(TEXT("confirm"),
        BuildBoolProperty(TEXT("Required acknowledgement that tests may create temporary editor assets.")));
    Properties->SetObjectField(TEXT("continueOnFailure"),
        BuildBoolProperty(TEXT("Run the remaining requested tests after a failure. Defaults true.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {
        MakeShared<FJsonValueString>(TEXT("testNames")),
        MakeShared<FJsonValueString>(TEXT("confirm"))});
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    return Schema;
}
