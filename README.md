# Unreal MCP Plugin

This plugin embeds a minimal Model Context Protocol server directly into the Unreal Editor.

## Current Scope

- Editor plugin module
- Structured MCP request/response types
- Named pipe transport inside Unreal Editor
- External adapter MCP entry point
- Tool registry and dispatcher
- Safe capability discovery tools
- Asset Registry search and existence checks

## Implemented Unreal Tools

- `HealthCheck`
- `GetServerInfo`
- `ListTools`
- `SearchAssets`
- `AssetExists`
- `GetAssetInfo`
- `ListAssets`
- `ListFolders`
- `GetDependencies`
- `GetReferencers`
- `FindAssetsByClass`
- `FindAssetsByPath`
- `GetBlueprintInfo`
- `ListVariables`
- `ListFunctions`
- `ListComponents`
- `GetParentBlueprint`
- `GetImplementedInterfaces`
- `ListChildBlueprints`
- `tools/list` JSON-RPC method

## Adapter

The plugin includes a standalone adapter at `Adapter/UnrealMCP.Adapter`.

The adapter is the recommended MCP entry point for coding agents:

- speaks MCP over stdio
- discovers Unreal projects from a workspace
- resolves Unreal Editor installs from `.uproject` `EngineAssociation`
- supports explicit engine overrides for custom editor locations
- connects to the in-editor named pipe transport
- proxies Unreal tool calls
- keeps answering adapter status tools when Unreal is down

### Adapter Local Tools

- `unreal.adapter.GetUnrealStatus`
- `unreal.adapter.GetUnrealSessionInfo`
- `unreal.adapter.DiscoverProjects`
- `unreal.adapter.SelectProject`
- `unreal.adapter.ClearSelectedProject`
- `unreal.adapter.ListRunningUnrealSessions`
- `unreal.adapter.LaunchUnrealProject`
- `unreal.adapter.AttachToUnrealProject`
- `unreal.adapter.ReconnectUnreal`
- `unreal.adapter.RequestUnrealShutdown`

## Agent Integration

Register only the adapter in your MCP client.

Do not register the Unreal named pipe transport directly.

The adapter is the single MCP entry point and is responsible for:

- stdio MCP for the agent
- workspace project discovery
- Unreal Editor install resolution
- Unreal process detection
- UnrealMCP pipe attachment
- unavailable and disconnect errors
- lifecycle and recovery tools

### Build The Adapter

```powershell
dotnet build .\Adapter\UnrealMCP.Adapter\UnrealMCP.Adapter.csproj -c Release
```

### Recommended MCP Config

Minimal:

```json
{
  "mcpServers": {
    "unreal": {
      "command": "C:\\Users\\LTX_MSI\\Documents\\UE_544_MCP\\Plugins\\UnrealMCP\\Adapter\\UnrealMCP.Adapter\\bin\\Release\\net9.0-windows\\UnrealMCP.Adapter.exe"
    }
  }
}
```

This is the recommended Codex setup.

When `--workspace` is omitted, the adapter uses its current working directory as the discovery root. In Codex, that is intended to align with the chat's primary source folder / working directory.

With a default Unreal Editor path:

```json
{
  "mcpServers": {
    "unreal": {
      "command": "C:\\Users\\LTX_MSI\\Documents\\UE_544_MCP\\Plugins\\UnrealMCP\\Adapter\\UnrealMCP.Adapter\\bin\\Release\\net9.0-windows\\UnrealMCP.Adapter.exe",
      "args": [
        "serve",
        "--engine-exe",
        "C:\\Program Files\\Epic Games\\UE_5.4\\Engine\\Binaries\\Win64\\UnrealEditor.exe"
      ]
    }
  }
}
```

With an explicit workspace override:

```json
{
  "mcpServers": {
    "unreal": {
      "command": "C:\\Users\\LTX_MSI\\Documents\\UE_544_MCP\\Plugins\\UnrealMCP\\Adapter\\UnrealMCP.Adapter\\bin\\Release\\net9.0-windows\\UnrealMCP.Adapter.exe",
      "args": [
        "serve",
        "--workspace",
        "C:\\Projects\\MyGameRepo"
      ]
    }
  }
}
```

Use `--workspace` only when you want to override the default working directory based discovery root.

Use `--engine-exe` when:

- Unreal is installed in a custom location
- you want to force a specific editor binary
- you want to bypass `EngineAssociation` resolution

### Engine Resolution

When launching a selected project, the adapter resolves the editor in this order:

1. `LaunchUnrealProject.engineExe`
2. `SelectProject.engineExe`
3. adapter startup `--engine-exe`
4. `.uproject` `EngineAssociation`

If `EngineAssociation` resolves to multiple matching installs, the adapter returns `engine_resolution_ambiguous`.

If no matching install is found, the adapter returns `engine_not_found`.

## Standalone CLI

The adapter executable can also run directly outside MCP.

### Serve Mode

```powershell
UnrealMCP.Adapter.exe serve --engine-exe C:\Program Files\Epic Games\UE_5.4\Engine\Binaries\Win64\UnrealEditor.exe
```

### Discover Mode

```powershell
UnrealMCP.Adapter.exe discover --workspace C:\Projects\MyGameRepo
```

### Launch Mode

```powershell
UnrealMCP.Adapter.exe launch --project C:\Projects\MyGameRepo\MyGame.uproject
```

### Status Mode

```powershell
UnrealMCP.Adapter.exe status --workspace C:\Projects\MyGameRepo
```

All standalone modes emit JSON to stdout.

## Recommended Workflow

1. Build the adapter.
2. Register the adapter as the only MCP entry in your client.
3. Call `unreal.adapter.DiscoverProjects`.
4. Call `unreal.adapter.SelectProject`.
5. Call `unreal.adapter.LaunchUnrealProject` or open the project manually.
6. Call `unreal.adapter.AttachToUnrealProject`.
7. Use mirrored Unreal tools normally.

## Expected Behavior

When Unreal Editor is closed:

- adapter tools remain available
- Unreal tools return structured errors such as `unreal_not_running`

When no project is selected:

- discovery and status tools still work
- Unreal tool calls return `project_not_selected`

When Unreal Editor is open with the selected project:

- `unreal.adapter.AttachToUnrealProject` attaches to the running session
- `tools/list` includes both adapter tools and mirrored Unreal tools
- proxied tools like `HealthCheck`, `GetServerInfo`, and `SearchAssets` execute inside Unreal

## Next Steps

- Expand Asset Registry coverage
- Add Blueprint, PIE, and build tools
- Expose progress reporting for long-running operations
