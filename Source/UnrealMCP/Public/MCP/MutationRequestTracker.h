#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPProtocol.h"

namespace UnrealMCP
{
    enum class EMutationRequestState : uint8
    {
        Queued,
        Preflighting,
        Mutating,
        Compiling,
        Completed,
        Failed,
        RolledBack,
        Cancelled
    };

    struct FMutationRequestRecord
    {
        FString OperationId;
        FString ToolName;
        FString RequestFingerprint;
        EMutationRequestState State = EMutationRequestState::Queued;
        FString Message;
        FDateTime CreatedAtUtc;
        FDateTime UpdatedAtUtc;
        TOptional<FMCPResponse> TerminalResponse;
    };

    enum class EMutationBeginResult : uint8
    {
        Begun,
        ReplayTerminal,
        AlreadyRunning,
        Conflict
    };

    class UNREALMCP_API FMutationRequestTracker
    {
    public:
        static FMutationRequestTracker& Get();

        EMutationBeginResult Begin(
            const FString& OperationId,
            const FString& ToolName,
            const FString& RequestFingerprint,
            FMCPResponse& OutTerminalResponse,
            FString& OutState);
        void Update(const FString& OperationId, EMutationRequestState State, const FString& Message = FString());
        void Complete(const FString& OperationId, const FMCPResponse& Response);
        bool GetRecord(const FString& OperationId, FMutationRequestRecord& OutRecord) const;

        static FString StateToString(EMutationRequestState State);
        static bool IsTerminal(EMutationRequestState State);

    private:
        mutable FCriticalSection Mutex;
        TMap<FString, FMutationRequestRecord> Records;
    };
}
