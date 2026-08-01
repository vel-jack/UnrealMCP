# Unreal MCP Plugin

This plugin embeds a minimal Model Context Protocol server directly into the Unreal Editor.

## Current Scope

- Editor plugin module
- Project settings
- Structured MCP request/response types
- Transport abstraction
- Tool registry and dispatcher
- Safe capability discovery tools
- Asset Registry search and existence checks

## Implemented Tools

- `HealthCheck`
- `GetServerInfo`
- `ListTools`
- `SearchAssets`
- `AssetExists`
- `tools/list` JSON-RPC method

## Example Request

```json
{
  "jsonrpc": "2.0",
  "id": "1",
  "method": "SearchAssets",
  "params": {
    "query": "Door"
  }
}
```

## Example Response

```json
{
  "jsonrpc": "2.0",
  "id": "1",
  "result": {
    "success": true,
    "query": "Door",
    "assets": []
  }
}
```

## Next Steps

- Add a transport layer such as stdio or named pipe
- Expand Asset Registry coverage
- Add Blueprint, PIE, and build tools
- Expose progress reporting for long-running operations
