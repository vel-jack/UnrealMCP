# E1 verification — September 12, 2026

Scope: progressive discovery, compact default tool surface, native Blueprint overview and removal of implicit saving from shutdown. Source baseline: `c782c69` plus the uncommitted E1 changes. Engine: Unreal 5.4.4, Windows.

## Results

- Adapter Release builds passed with zero warnings/errors, both isolated regression output and the checkout's standard `bin/Release/net9.0-windows` output.
- 41 simulated-pipe MCP regression checks passed, including the previous full-surface cases, compact discovery/schema/gateway, pagination/revision mismatch, offline failure, read and mutation timeout identity, recursion rejection, no implicit save on shutdown, and six lifecycle scenarios.
- Canonical native Editor Development build passed. The initial compile exposed an incorrect graph-enumeration API call; corrected to `UBlueprint::GetAllGraphs`. Final native changes compiled and linked successfully. The host project emits existing include-order/dependency/deprecation warnings; they were not changed by E1.
- `UnrealMCP.Blueprint.Overview.ReadOnly` passed through production stdio -> compact gateway -> native plugin with zero errors/warnings. Fixtures are transient, unregistered and unsaved. Tests cover clean/dirty and compile/graph-state preservation, filtering, pagination, zero-result coverage, invalid bounds/load policy and LevelScriptBlueprint exclusion.
- Same live tool catalog, UTF-8 serialized `tools/list` result: full **117,096 bytes**, compact **4,305 bytes**, **96.3% reduction**. Compact mode advertises 14 tools. The measurement excludes initialize/server instructions and subsequent calls; it is not a whole-workflow token measurement.
- Legacy full-surface discovery and calls remain available with `--tool-surface full`; native tool names and dispatch were retained.

## Reproduce

From the plugin root:

```powershell
dotnet build .\Adapter\UnrealMCP.Adapter\UnrealMCP.Adapter.csproj -c Release
dotnet run --project .\Adapter\UnrealMCP.Adapter.RegressionTests -c Release
```

With the explicitly selected host already running the rebuilt plugin:

```powershell
dotnet run --project .\Adapter\UnrealMCP.Adapter.RegressionTests -c Release -- --live <adapter-exe> <absolute-project.uproject>
dotnet run --project .\Adapter\UnrealMCP.Adapter.RegressionTests -c Release -- --overview <adapter-exe> <absolute-project.uproject> <exact-blueprint-object-path>
```

`--live` performs discovery, the byte comparison and the exact native overview test through the compact gateway. `--overview` makes a bounded graph inventory request against the supplied asset. `--shutdown <adapter-exe> <project.uproject>` explicitly invokes the adapter's graceful shutdown command; it never saves first and can require the user to resolve an Unreal prompt.

For a locked adapter binary, build the regression project with `-o <isolated-directory>` and run its executable there. This tests new code without terminating an existing client's process. No client configuration is changed by these checks.

## Deployment and limits

The production adapter for this checkout is `Adapter/UnrealMCP.Adapter/bin/Release/net9.0-windows/UnrealMCP.Adapter.exe`. Configured clients that launch an adapter from another checkout do not automatically receive this upgrade; their configured executable must be switched deliberately and the MCP connection restarted. No other plugin checkout was synchronized.

The Editor was gracefully closed, canonically rebuilt, reopened and reattached using the rebuild skill and MCP adapter. Startup briefly returned a timeout while the process continued launching; reconnect succeeded without launching another process. Editor remains open. No PIE, Computer Use, user asset saving or full index rebuild/schema change was needed.

The startup follow-up replaced that ambiguous result. The deployed adapter was tested from a closed Editor: launch returned `success: true`, `ready: false`, `launchAccepted: true`, `editor_starting`, PID and timing evidence after about five seconds; a later status returned `ready`. Offline status, process exit, delayed pipe, expired deadline, and established request timeout are separate regression cases. The normal Release executable was initially locked by this task's old adapter process; that exact process was stopped, the configured path rebuilt with zero warnings/errors, and production stdio/live native tests passed from the replacement binary.

Coverage limits: no full native suite, cold-load/save-reload stress benchmark, MCP Inspector or clean installations of all supported clients were run. Actual model-token and end-to-end workflow savings remain E8 work. E2's existing compound rollback/recovery findings are queued, not claimed fixed by the new gateway. Native overview pages are live reads, not revision-bound snapshots.
