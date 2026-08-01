#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "Transport/IMCPTransport.h"

class FMCPServer;
class FRunnableThread;

class FNamedPipeMCPTransport final : public IMCPTransport, public FRunnable
{
public:
    explicit FNamedPipeMCPTransport(FString InPipeName);
    virtual ~FNamedPipeMCPTransport() override;

    virtual FString GetTransportName() const override;
    virtual bool Start(FMCPServer& InServer) override;
    virtual void Stop() override;

    virtual uint32 Run() override;

private:
    FString BuildPipePath() const;
    bool CreatePipeInstance();
    bool WaitForConnection();
    void ClosePipeInstance();
    void UnblockPendingConnection() const;
    bool ReadNextMessage(FString& OutMessage);
    bool WriteMessage(const FString& Message) const;
    bool ProcessConnectedClient();

    FString PipeName;
    FMCPServer* Server = nullptr;
    void* PipeHandle = nullptr;
    FRunnableThread* Thread = nullptr;
    FThreadSafeBool bStopping = false;
    FThreadSafeBool bStopCompleted = false;
    TArray<uint8> PendingReadBytes;
};
