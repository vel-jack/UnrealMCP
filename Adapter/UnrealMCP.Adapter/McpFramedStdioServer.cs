using System.Text;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed class McpFramedStdioServer(McpRequestDispatcher dispatcher)
{
    private readonly Stream _input = Console.OpenStandardInput();
    private readonly Stream _output = Console.OpenStandardOutput();
    private readonly byte[] _headerTerminator = "\r\n\r\n"u8.ToArray();

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var message = await ReadMessageAsync(cancellationToken);
            if (message is null)
            {
                return;
            }

            JsonObject? response;
            try
            {
                response = await dispatcher.DispatchAsync(message, cancellationToken);
            }
            catch (Exception exception)
            {
                response = McpProtocol.CreateJsonRpcError(null, -32603, "Adapter internal error.", new JsonObject
                {
                    ["exceptionType"] = exception.GetType().FullName,
                    ["message"] = exception.Message
                });
            }

            if (response is not null)
            {
                await WriteMessageAsync(response, cancellationToken);
            }
        }
    }

    private async Task<string?> ReadMessageAsync(CancellationToken cancellationToken)
    {
        var headerBytes = new List<byte>();
        while (true)
        {
            var nextByte = new byte[1];
            var bytesRead = await _input.ReadAsync(nextByte, cancellationToken);
            if (bytesRead == 0)
            {
                return headerBytes.Count == 0 ? null : throw new EndOfStreamException("Unexpected end of stream while reading MCP headers.");
            }

            headerBytes.Add(nextByte[0]);
            if (headerBytes.Count >= _headerTerminator.Length &&
                headerBytes.Skip(headerBytes.Count - _headerTerminator.Length).SequenceEqual(_headerTerminator))
            {
                break;
            }
        }

        var headerText = Encoding.ASCII.GetString(headerBytes.ToArray());
        var contentLength = ParseContentLength(headerText);
        if (contentLength < 0)
        {
            var diagnosticHeader = headerText
                .Replace("\r", "\\r", StringComparison.Ordinal)
                .Replace("\n", "\\n", StringComparison.Ordinal)
                .Replace("\0", "\\0", StringComparison.Ordinal);
            throw new InvalidDataException($"Missing Content-Length header. Raw header: {diagnosticHeader}");
        }

        var payloadBytes = new byte[contentLength];
        var totalRead = 0;
        while (totalRead < contentLength)
        {
            var bytesRead = await _input.ReadAsync(payloadBytes.AsMemory(totalRead, contentLength - totalRead), cancellationToken);
            if (bytesRead == 0)
            {
                throw new EndOfStreamException("Unexpected end of stream while reading MCP payload.");
            }

            totalRead += bytesRead;
        }

        return Encoding.UTF8.GetString(payloadBytes);
    }

    private async Task WriteMessageAsync(JsonObject message, CancellationToken cancellationToken)
    {
        var payload = Encoding.UTF8.GetBytes(message.ToJsonString(McpProtocol.JsonOptions));
        var header = Encoding.ASCII.GetBytes($"Content-Length: {payload.Length}\r\n\r\n");
        await _output.WriteAsync(header, cancellationToken);
        await _output.WriteAsync(payload, cancellationToken);
        await _output.FlushAsync(cancellationToken);
    }

    private static int ParseContentLength(string headers)
    {
        foreach (var line in headers.Split(["\r\n"], StringSplitOptions.RemoveEmptyEntries))
        {
            var sanitizedLine = line.Replace("\0", string.Empty, StringComparison.Ordinal).TrimStart('\uFEFF', ' ', '\t');
            var contentLengthIndex = sanitizedLine.IndexOf("Content-Length:", StringComparison.OrdinalIgnoreCase);
            if (contentLengthIndex < 0)
            {
                continue;
            }

            var rawValue = sanitizedLine[(contentLengthIndex + "Content-Length:".Length)..].Trim();
            if (int.TryParse(rawValue, out var parsed))
            {
                return parsed;
            }
        }

        return -1;
    }
}
