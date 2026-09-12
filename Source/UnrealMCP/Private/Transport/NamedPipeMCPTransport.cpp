#include "Transport/NamedPipeMCPTransport.h"

#include "MCP/MCPServer.h"
#include "UnrealMCPLog.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
#if PLATFORM_WINDOWS
    HANDLE ToHandle(void* Value)
    {
        return static_cast<HANDLE>(Value);
    }
#endif
}

FNamedPipeMCPTransport::FNamedPipeMCPTransport(FString InPipeName)
    : PipeName(MoveTemp(InPipeName))
{
}

FNamedPipeMCPTransport::~FNamedPipeMCPTransport()
{
    Stop();
}

FString FNamedPipeMCPTransport::GetTransportName() const
{
    return FString::Printf(TEXT("NamedPipe(%s)"), *PipeName);
}

bool FNamedPipeMCPTransport::Start(FMCPServer& InServer)
{
#if !PLATFORM_WINDOWS
    return false;
#else
    if (Thread != nullptr)
    {
        return true;
    }

    Server = &InServer;
    bStopping = false;
    bStopCompleted = false;
    Thread = FRunnableThread::Create(this, TEXT("UnrealMCPNamedPipeTransport"));
    if (Thread == nullptr)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Failed to create named pipe transport thread."));
        Server = nullptr;
        return false;
    }

    return true;
#endif
}

void FNamedPipeMCPTransport::Stop()
{
#if PLATFORM_WINDOWS
    if (bStopCompleted)
    {
        return;
    }

    bStopping = true;
    UnblockPendingConnection();

    FRunnableThread* LocalThread = Thread;
    Thread = nullptr;

    if (LocalThread != nullptr)
    {
        LocalThread->WaitForCompletion();
        delete LocalThread;
    }

    ClosePipeInstance();
    Server = nullptr;
    bStopCompleted = true;
#endif
}

uint32 FNamedPipeMCPTransport::Run()
{
#if !PLATFORM_WINDOWS
    return 0;
#else
    while (!bStopping)
    {
        if (!CreatePipeInstance())
        {
            FPlatformProcess::Sleep(1.0f);
            continue;
        }

        if (!WaitForConnection())
        {
            ClosePipeInstance();
            continue;
        }

        ProcessConnectedClient();
        ClosePipeInstance();
    }

    return 0;
#endif
}

FString FNamedPipeMCPTransport::BuildPipePath() const
{
    return FString::Printf(TEXT("\\\\.\\pipe\\%s"), *PipeName);
}

bool FNamedPipeMCPTransport::CreatePipeInstance()
{
#if !PLATFORM_WINDOWS
    return false;
#else
    ClosePipeInstance();

    const FString PipePath = BuildPipePath();
    HANDLE NewPipeHandle = CreateNamedPipeW(
        *PipePath,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        64 * 1024,
        64 * 1024,
        0,
        nullptr);

    if (NewPipeHandle == INVALID_HANDLE_VALUE)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Failed to create named pipe transport at %s. Win32=%lu"), *PipePath, GetLastError());
        PipeHandle = nullptr;
        return false;
    }

    PipeHandle = NewPipeHandle;
    return true;
#endif
}

bool FNamedPipeMCPTransport::WaitForConnection()
{
#if !PLATFORM_WINDOWS
    return false;
#else
    HANDLE Handle = ToHandle(PipeHandle);
    if (Handle == nullptr || Handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    const bool bConnected = ConnectNamedPipe(Handle, nullptr) != 0 || GetLastError() == ERROR_PIPE_CONNECTED;
    return !bStopping && bConnected;
#endif
}

void FNamedPipeMCPTransport::ClosePipeInstance()
{
#if PLATFORM_WINDOWS
    HANDLE Handle = ToHandle(PipeHandle);
    if (Handle != nullptr && Handle != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(Handle);
        DisconnectNamedPipe(Handle);
        CloseHandle(Handle);
    }

    PipeHandle = nullptr;
#endif
}

void FNamedPipeMCPTransport::UnblockPendingConnection() const
{
#if PLATFORM_WINDOWS
    const FString PipePath = BuildPipePath();
    HANDLE ClientHandle = CreateFileW(
        *PipePath,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (ClientHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(ClientHandle);
    }
#endif
}

bool FNamedPipeMCPTransport::ReadNextMessage(FString& OutMessage)
{
#if !PLATFORM_WINDOWS
    return false;
#else
    HANDLE Handle = ToHandle(PipeHandle);
    if (Handle == nullptr || Handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    uint8 Buffer[4096];

    while (!bStopping)
    {
        int32 LineFeedIndex = INDEX_NONE;
        for (int32 Index = 0; Index < PendingReadBytes.Num(); ++Index)
        {
            if (PendingReadBytes[Index] == '\n')
            {
                LineFeedIndex = Index;
                break;
            }
        }

        if (LineFeedIndex != INDEX_NONE)
        {
            TArray<uint8> LineBytes;
            LineBytes.Append(PendingReadBytes.GetData(), LineFeedIndex);
            PendingReadBytes.RemoveAt(0, LineFeedIndex + 1, EAllowShrinking::No);

            if (LineBytes.Num() > 0 && LineBytes.Last() == '\r')
            {
                LineBytes.Pop();
            }

            FUTF8ToTCHAR Converter(reinterpret_cast<const UTF8CHAR*>(LineBytes.GetData()), LineBytes.Num());
            OutMessage = FString(Converter.Length(), Converter.Get());
            return true;
        }

        DWORD BytesRead = 0;
        if (!ReadFile(Handle, Buffer, sizeof(Buffer), &BytesRead, nullptr))
        {
            const DWORD ErrorCode = GetLastError();
            if (ErrorCode == ERROR_MORE_DATA)
            {
                continue;
            }

            return false;
        }

        if (BytesRead == 0)
        {
            return false;
        }

        PendingReadBytes.Append(Buffer, static_cast<int32>(BytesRead));
    }

    return false;
#endif
}

bool FNamedPipeMCPTransport::WriteMessage(const FString& Message) const
{
#if !PLATFORM_WINDOWS
    return false;
#else
    HANDLE Handle = ToHandle(PipeHandle);
    if (Handle == nullptr || Handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    FTCHARToUTF8 Converter(*Message);
    DWORD BytesWritten = 0;
    return WriteFile(Handle, Converter.Get(), Converter.Length(), &BytesWritten, nullptr) != 0;
#endif
}

bool FNamedPipeMCPTransport::ProcessConnectedClient()
{
#if !PLATFORM_WINDOWS
    return false;
#else
    if (Server == nullptr)
    {
        return false;
    }

    // Verbose, not Log: the adapter opens one connection per request, so at Log level this pair buried
    // the request lines that actually say what the agent asked for.
    UE_LOG(LogUnrealMCP, Verbose, TEXT("Named pipe client connected on %s"), *BuildPipePath());
    PendingReadBytes.Reset();

    const double SessionStartSeconds = FPlatformTime::Seconds();
    int32 RequestCount = 0;

    while (!bStopping)
    {
        FString RequestJson;
        if (!ReadNextMessage(RequestJson))
        {
            break;
        }

        if (RequestJson.IsEmpty())
        {
            continue;
        }

        ++RequestCount;
        const FString ResponseJson = Server->HandleJsonRequest(RequestJson) + TEXT("\n");
        if (!WriteMessage(ResponseJson))
        {
            break;
        }
    }

    // A connection that carried no request never showed up in the per-request log at all, so that one
    // case stays at Log level; an ordinary serviced connection is already described by its request line.
    if (RequestCount == 0)
    {
        UE_LOG(LogUnrealMCP, Log, TEXT("Named pipe client connected and disconnected from %s without sending a request (connection probe)."),
            *BuildPipePath());
    }
    else
    {
        UE_LOG(LogUnrealMCP, Verbose, TEXT("Named pipe client disconnected from %s after %d request(s) over %.1fs"),
            *BuildPipePath(), RequestCount, FPlatformTime::Seconds() - SessionStartSeconds);
    }
    return true;
#endif
}
