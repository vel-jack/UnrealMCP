#include "MCP/MutationRequestTracker.h"

#include "Misc/ScopeLock.h"

namespace UnrealMCP
{
    FMutationRequestTracker& FMutationRequestTracker::Get()
    {
        static FMutationRequestTracker Instance;
        return Instance;
    }

    EMutationBeginResult FMutationRequestTracker::Begin(
        const FString& OperationId,
        const FString& ToolName,
        const FString& RequestFingerprint,
        FMCPResponse& OutTerminalResponse,
        FString& OutState)
    {
        FScopeLock Lock(&Mutex);
        if (FMutationRequestRecord* Existing = Records.Find(OperationId))
        {
            OutState = StateToString(Existing->State);
            if (Existing->ToolName != ToolName || Existing->RequestFingerprint != RequestFingerprint)
                return EMutationBeginResult::Conflict;
            if (Existing->TerminalResponse.IsSet())
            {
                OutTerminalResponse = Existing->TerminalResponse.GetValue();
                return EMutationBeginResult::ReplayTerminal;
            }
            return EMutationBeginResult::AlreadyRunning;
        }

        if (Records.Num() >= 1024)
        {
            FString OldestTerminalId;
            FDateTime OldestTerminalTime = FDateTime::MaxValue();
            for (const TPair<FString, FMutationRequestRecord>& Pair : Records)
            {
                if (Pair.Value.TerminalResponse.IsSet() && Pair.Value.UpdatedAtUtc < OldestTerminalTime)
                {
                    OldestTerminalId = Pair.Key;
                    OldestTerminalTime = Pair.Value.UpdatedAtUtc;
                }
            }
            if (!OldestTerminalId.IsEmpty())
            {
                Records.Remove(OldestTerminalId);
            }
        }

        FMutationRequestRecord& Record = Records.Add(OperationId);
        Record.OperationId = OperationId;
        Record.ToolName = ToolName;
        Record.RequestFingerprint = RequestFingerprint;
        Record.State = EMutationRequestState::Queued;
        Record.CreatedAtUtc = FDateTime::UtcNow();
        Record.UpdatedAtUtc = Record.CreatedAtUtc;
        OutState = StateToString(Record.State);
        return EMutationBeginResult::Begun;
    }

    void FMutationRequestTracker::Update(
        const FString& OperationId,
        EMutationRequestState State,
        const FString& Message)
    {
        if (OperationId.IsEmpty()) return;
        FScopeLock Lock(&Mutex);
        if (FMutationRequestRecord* Record = Records.Find(OperationId))
        {
            if (Record->TerminalResponse.IsSet()) return;
            Record->State = State;
            Record->Message = Message;
            Record->UpdatedAtUtc = FDateTime::UtcNow();
        }
    }

    void FMutationRequestTracker::Complete(const FString& OperationId, const FMCPResponse& Response)
    {
        if (OperationId.IsEmpty()) return;
        FScopeLock Lock(&Mutex);
        if (FMutationRequestRecord* Record = Records.Find(OperationId))
        {
            if (!IsTerminal(Record->State))
                Record->State = Response.Error.IsSet() ? EMutationRequestState::Failed : EMutationRequestState::Completed;
            Record->Message = Response.Error.IsSet()
                ? Response.Error.GetValue().Message
                : TEXT("Mutation request completed.");
            Record->UpdatedAtUtc = FDateTime::UtcNow();
            Record->TerminalResponse = Response;
        }
    }

    bool FMutationRequestTracker::GetRecord(const FString& OperationId, FMutationRequestRecord& OutRecord) const
    {
        FScopeLock Lock(&Mutex);
        const FMutationRequestRecord* Record = Records.Find(OperationId);
        if (Record == nullptr) return false;
        OutRecord = *Record;
        return true;
    }

    FString FMutationRequestTracker::StateToString(EMutationRequestState State)
    {
        switch (State)
        {
        case EMutationRequestState::Queued: return TEXT("queued");
        case EMutationRequestState::Preflighting: return TEXT("preflighting");
        case EMutationRequestState::Mutating: return TEXT("mutating");
        case EMutationRequestState::Compiling: return TEXT("compiling");
        case EMutationRequestState::Completed: return TEXT("completed");
        case EMutationRequestState::Failed: return TEXT("failed");
        case EMutationRequestState::RolledBack: return TEXT("rolled_back");
        case EMutationRequestState::Cancelled: return TEXT("cancelled");
        default: return TEXT("unknown");
        }
    }

    bool FMutationRequestTracker::IsTerminal(EMutationRequestState State)
    {
        return State == EMutationRequestState::Completed
            || State == EMutationRequestState::Failed
            || State == EMutationRequestState::RolledBack
            || State == EMutationRequestState::Cancelled;
    }
}
