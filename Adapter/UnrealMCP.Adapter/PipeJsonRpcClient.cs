using System.IO.Pipes;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using System.Text;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed class PipeJsonRpcClient(string pipeName, TimeSpan connectTimeout, TimeSpan requestTimeout, IReadOnlySet<int>? expectedProcessIds = null)
{
    public int? ServerProcessId { get; private set; }
    private readonly string _pipeName = pipeName;
    private readonly TimeSpan _connectTimeout = connectTimeout;
    private readonly TimeSpan _requestTimeout = requestTimeout;
    private static readonly Encoding Utf8NoBom = new UTF8Encoding(false);

    public async Task<JsonObject> SendRequestAsync(string method, JsonNode? parameters, CancellationToken cancellationToken)
    {
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutCts.CancelAfter(_requestTimeout);
        try
        {
            using var pipe = new NamedPipeClientStream(".", _pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            using var connectCts = CancellationTokenSource.CreateLinkedTokenSource(timeoutCts.Token);
            connectCts.CancelAfter(_connectTimeout);
            await pipe.ConnectAsync(connectCts.Token);
            if (expectedProcessIds is not null)
            {
                if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle, out var processId)
                    || !expectedProcessIds.Contains(checked((int)processId)))
                    throw new PipeIdentityException("The UnrealMCP pipe belongs to a different or unverifiable editor process. No request was sent.");
                ServerProcessId = (int)processId;
            }

            using var writer = new StreamWriter(pipe, Utf8NoBom, leaveOpen: true) { AutoFlush = true };
            using var reader = new StreamReader(pipe, Utf8NoBom, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
            var request = new JsonObject
            {
                ["jsonrpc"] = "2.0",
                ["id"] = Guid.NewGuid().ToString("N"),
                ["method"] = method,
                ["params"] = parameters?.DeepClone() ?? new JsonObject()
            };
            await writer.WriteLineAsync(request.ToJsonString(McpProtocol.JsonOptions));
            var responseLine = await ReadLineWithTimeoutAsync(reader, timeoutCts.Token);
            if (string.IsNullOrWhiteSpace(responseLine))
                throw new IOException("The UnrealMCP pipe returned an empty response.");
            return JsonNode.Parse(responseLine)?.AsObject()
                ?? throw new InvalidDataException("The UnrealMCP pipe returned invalid JSON.");
        }
        catch (OperationCanceledException exception) when (!cancellationToken.IsCancellationRequested)
        {
            throw new TimeoutException($"Timed out while waiting for UnrealMCP method '{method}'.", exception);
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint processId);

    private static async Task<string?> ReadLineWithTimeoutAsync(StreamReader reader, CancellationToken cancellationToken)
    {
        var readTask = reader.ReadLineAsync();
        var completedTask = await Task.WhenAny(readTask, Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken));
        cancellationToken.ThrowIfCancellationRequested();
        return await readTask;
    }
}

internal sealed class PipeIdentityException(string message) : IOException(message);
