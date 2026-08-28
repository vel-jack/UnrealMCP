#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MCP/MCPServer.h"
#include "Tools/GetMutationRequestStatusTool.h"

namespace
{
    class FCountingMutationTool final : public IMCPTool
    {
    public:
        virtual FMCPToolDefinition GetDefinition() const override
        {
            FMCPToolDefinition Definition;
            Definition.Name = TEXT("ReliabilityCountingMutation");
            Definition.Description = TEXT("Test-only counting mutation.");
            Definition.InputSchema = MakeShared<FJsonObject>();
            return Definition;
        }

        virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override
        {
            ++ExecuteCount;
            UnrealMCP::FMCPResponse Response;
            Response.Id = Request.Id;
            Response.Result = MakeShared<FJsonObject>();
            Response.Result->SetBoolField(TEXT("success"), true);
            Response.Result->SetNumberField(TEXT("executeCount"), ExecuteCount);
            return Response;
        }

        mutable int32 ExecuteCount = 0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPMutationRequestReplayTest,
    "UnrealMCP.Reliability.MutationRequestReplay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPMutationRequestReplayTest::RunTest(const FString& Parameters)
{
    FMCPServer Server;
    TSharedRef<FCountingMutationTool> CountingTool = MakeShared<FCountingMutationTool>();
    Server.GetToolRegistry().RegisterTool(CountingTool);
    Server.GetToolRegistry().RegisterTool(MakeShared<FGetMutationRequestStatusTool>());

    const FString OperationId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UnrealMCP::FMCPRequest First;
    First.Id = TEXT("first");
    First.Method = TEXT("ReliabilityCountingMutation");
    First.Params = MakeShared<FJsonObject>();
    First.Params->SetStringField(TEXT("value"), TEXT("same"));
    First.Params->SetBoolField(TEXT("confirm"), true);
    First.Params->SetStringField(TEXT("operationId"), OperationId);
    const UnrealMCP::FMCPResponse FirstResponse = Server.HandleRequest(First);
    TestFalse(TEXT("First mutation has no error"), FirstResponse.Error.IsSet());
    TestEqual(TEXT("First mutation executes once"), CountingTool->ExecuteCount, 1);

    UnrealMCP::FMCPRequest Retry;
    Retry.Id = TEXT("retry");
    Retry.Method = First.Method;
    Retry.Params = MakeShared<FJsonObject>();
    Retry.Params->SetStringField(TEXT("operationId"), OperationId);
    Retry.Params->SetBoolField(TEXT("confirm"), true);
    Retry.Params->SetStringField(TEXT("value"), TEXT("same"));
    const UnrealMCP::FMCPResponse RetryResponse = Server.HandleRequest(Retry);
    TestFalse(TEXT("Identical retry replays without error"), RetryResponse.Error.IsSet());
    TestEqual(TEXT("Identical retry does not execute twice"), CountingTool->ExecuteCount, 1);
    TestEqual(TEXT("Replay uses the current request ID"), RetryResponse.Id, Retry.Id);

    UnrealMCP::FMCPRequest Conflict = Retry;
    Conflict.Id = TEXT("conflict");
    Conflict.Params = MakeShared<FJsonObject>();
    Conflict.Params->SetStringField(TEXT("operationId"), OperationId);
    Conflict.Params->SetStringField(TEXT("value"), TEXT("different"));
    Conflict.Params->SetBoolField(TEXT("confirm"), true);
    const UnrealMCP::FMCPResponse ConflictResponse = Server.HandleRequest(Conflict);
    TestTrue(TEXT("Different retry is rejected"), ConflictResponse.Error.IsSet());
    TestEqual(TEXT("Conflicting retry does not execute"), CountingTool->ExecuteCount, 1);

    UnrealMCP::FMCPRequest Status;
    Status.Id = TEXT("status");
    Status.Method = TEXT("GetMutationRequestStatus");
    Status.Params = MakeShared<FJsonObject>();
    Status.Params->SetStringField(TEXT("operationId"), OperationId);
    const UnrealMCP::FMCPResponse StatusResponse = Server.HandleRequest(Status);
    TestFalse(TEXT("Terminal status can be queried"), StatusResponse.Error.IsSet());
    FString State;
    if (StatusResponse.Result.IsValid())
    {
        StatusResponse.Result->TryGetStringField(TEXT("state"), State);
    }
    TestEqual(TEXT("Tracked request is terminal completed"), State, FString(TEXT("completed")));
    return true;
}

#endif
