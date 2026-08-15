# Unreal MCP Plugin

This plugin embeds a minimal Model Context Protocol server directly into the Unreal Editor.

## Current Scope

- Editor plugin module
- Structured MCP request/response types
- Named pipe transport inside Unreal Editor
- External adapter MCP entry point
- Local SQLite project index inside Unreal Editor
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
- `CompileBlueprint`
- `CompileAllBlueprints`
- `CreateBlueprintAsset`
- `AddBlueprintComponent`
- `AddBlueprintComponents`
- `AddBlueprintVariable`
- `ListBlueprintGraphs`
- `ValidateBlueprint`
- `SaveBlueprint`
- `AddBlueprintBranchNode`
- `MoveBlueprintNode`
- `ConnectBlueprintPins`
- `SetBlueprintPinDefaultObject`
- `SetBlueprintPinDefaultValue`
- `DeleteBlueprintNode`
- `AddBlueprintSequenceNode`
- `AddBlueprintCustomEventNode`
- `AddBlueprintFunctionCallNode`
- `AddBlueprintFunctionParameter`
- `AddBlueprintInterfaceEventNode`
- `AddBlueprintInterfaceFunctionGraph`
- `SetBlueprintFunctionMetadata`
- `AddBlueprintOverrideEventNode`
- `AddEnhancedInputActionNode`
- `SetBlueprintPinSplit`
- `SetBlueprintSequenceOutputs`
- `SetBlueprintNodeComment`
- `LayoutBlueprintNodes`
- `AddBlueprintDelegateNode`
- `AddBlueprintDelegateEventNode`
- `AddBlueprintDelegateBroadcastNode`
- `AddBlueprintVariableGetNode`
- `AddBlueprintVariableSetNode`
- `DisconnectBlueprintPins`
- `ListBlueprintNodePins`
- `AddBlueprintCastNode`
- `AddBlueprintRerouteNode`
- `AddBlueprintCommentNode`
- `CreateBlueprintFunctionGraph`
- `WireBlueprintEventToFunction`
- `ApplyBlueprintInteractionPlan`
- `GetIndexStatus`
- `BuildProjectIndex`
- `FindCircularDependencies`
- `AnalyzeBlueprintCoupling`
- `FindBrokenBlueprintReferences`
- `FindUnusedBlueprintAssets`
- `AnalyzeFeatureBoundary`
- `AnalyzeProjectArchitecture`
- `PlanProjectRefactor`
- `tools/list` JSON-RPC method

## Project Index

The plugin now includes a local SQLite index stored under `Saved/UnrealMCP/ProjectIndex.sqlite3`.

Current indexed data:

- asset identity and package location
- Blueprint summary fields
- Blueprint variable names and pin-type summaries
- Blueprint function names
- Blueprint SCS component hierarchy summaries
- package dependency edges
- stable Blueprint graph identities and component usage metadata

## Safe Blueprint Authoring

Phase 1 authoring supports transactional Blueprint creation, component addition, variable addition, validation, and explicit saving. `AddBlueprintComponent` accepts safe editable component-template `propertyDefaults`; `AddBlueprintComponents` prevalidates an ordered component hierarchy and applies it in one transaction, with one optional compile/save/index refresh. Mutation tools support `dryRun`, `compileAfterEdit`, and `saveAfterEdit`; creation never overwrites an existing asset. Successful saved edits refresh only the touched asset in the project index.

Component defaults accept JSON strings, numbers, and booleans. Complex Unreal values can be supplied as export-text strings. Existing same-name/same-class components are reported as `alreadyExists` and are not modified, so retrying a request cannot silently replace their configured defaults.

The first Phase 2 graph-editing slice supports stable GUID targeting, collision-aware Branch placement, node movement, validated pin connections/defaults, and confirmed node deletion. Automatic layout uses 320-pixel horizontal spacing and 180-pixel vertical collision steps by default.

The second slice adds Sequence, Custom Event, exact reflected function-call, and member-GUID Variable Get/Set nodes, plus guarded single-link or all-link disconnection. Saved authored flows are immediately available to indexed tracing after partial refresh.

The next slice adds live pin/linked-endpoint readback, typed casts, wildcard reroutes, bounded comments, and uniquely named function graphs. All were live-tested with save, validation, and indexed graph readback.

Pin inspection preserves Unreal's separate literal, object/class, and text defaults. `defaultValue` is the effective AI-facing value, while `literalDefaultValue`, `defaultObjectPath`, `defaultTextValue`, and `defaultValueSource` explain exactly where Unreal stored it.

`SetBlueprintPinDefaultObject` safely assigns exact UObject or UClass defaults to unconnected object/class input pins. This covers nodes such as `Get Component by Class`, whose selected class is stored in Unreal's `DefaultObject` rather than its literal `DefaultValue`.

Indexed traces classify latent, async, timeline, and timer boundaries from existing graph metadata. Trace results include exact, inferred, or unresolved confidence, explicit continuation models, and unresolved transition records. Runtime-dependent callback selection still requires PIE validation.

AXIS-oriented authoring now includes function input/output parameters and return-node creation, function metadata, parent-class override events, reflected Enhanced Input Action events, generic struct pin splitting/recombining, and dispatcher bind/unbind nodes with exact signature-matched Custom Events. These primitives were live-tested together with validation, save, and partial index refresh.

`AddBlueprintInterfaceEventNode` adds or idempotently resolves event-compatible interface implementations in Ubergraphs. Interface functions with output/return parameters are rejected because Unreal requires those implementations to use function graphs.

`AddBlueprintInterfaceFunctionGraph` creates or resolves signature-correct implementation graphs for non-event interface functions, including output and return signatures. `AddBlueprintDelegateBroadcastNode` creates validated Blueprint-callable multicast delegate broadcasts with exact signature pins.

Output-bearing interface graph creation is regression-tested by `UnrealMCP.Blueprint.Authoring.AddInterfaceFunctionGraph.Live`, including signature propagation, idempotent retry, target compilation, and temporary fixture cleanup.

`LayoutBlueprintNodes` arranges an explicit node set by local execution-flow depth with configurable spacing and collision avoidance. It returns every proposed or applied position and supports dry-run before mutation.

`WireBlueprintEventToFunction` is the first Phase 4 high-level workflow. It idempotently creates or reuses a Custom Event, adds one exact impure Blueprint-callable function call, optionally wires a Blueprint component/member variable into the call target, lays out the nodes, and performs at most one compile/save/index refresh. It refuses to replace an event's existing execution route or a function call's existing target connection. Use `dryRun=true` before applying changes to an unfamiliar Blueprint.

The workflow is regression-tested by `UnrealMCP.Blueprint.Authoring.WireEventToFunction.Live`, covering dry-run, exact execution and component-target pin links, compilation, idempotent retry, duplicate prevention, and temporary fixture cleanup.

`ApplyBlueprintInteractionPlan` applies a complete declarative graph fragment in one preflighted operation. Its schema supports `existingNode` GUID anchors, `customEvent`, `functionCall`, `variableGet`, `branch`, and `sequence` nodes plus exact named-pin connections. Stable `workflowId` and node `id` values make retries idempotent. Existing target-pin or execution-route conflicts are rejected instead of replaced, and compile/save/index refresh run at most once after the plan.

A `sequence` node can safely extend one occupied execution output by specifying `spliceAfterNodeId` and `spliceAfterPinName`. The tool inserts the Sequence, reconnects the original route through `Then_0`, and leaves `Then_1` available for the new workflow. The splice must have exactly one existing route, is fully preflighted, and is validated rather than repeated on retry. This is the preferred way to add DSM interaction calls beside legacy pointer/interface handling without removing that behavior.

Plan nodes also accept `inputDefaults` entries with either `defaultValue` or `defaultObjectPath`. Defaults are applied before connection validation, enabling typed nodes such as `Get Component by Class` to connect safely in the same transaction. `executionInsertions` atomically insert one impure node into an exact existing exec link and validate the completed route on retry.

The declarative workflow is regression-tested by `UnrealMCP.Blueprint.Authoring.InteractionPlan.Live`, covering complete dry-run preflight, exact existing-node anchors, four-node/three-link application, component-target data wiring, legacy-route-preserving Sequence insertion, compilation, idempotent retry, duplicate prevention, and temporary fixture cleanup.

## Project Architecture Analysis

`FindCircularDependencies` finds strongly connected groups among indexed project Blueprint package dependencies and returns the exact internal edges that keep each cycle connected. `AnalyzeBlueprintCoupling` ranks project Blueprints by indexed package fan-in and fan-out, with explicit thresholds and separate internal/external counts. These are static package-reference signals, not proof of runtime execution or runtime coupling.

`FindBrokenBlueprintReferences` reports conservative index evidence for missing project dependency targets, unresolved Blueprint component/type/member paths, and orphaned indexed graph edges. It does not replace live Blueprint validation or compilation.

`FindUnusedBlueprintAssets` returns review candidates with no indexed incoming project-package dependencies. It never declares an asset safe to delete and excludes likely configuration, map, framework, library, or manually invoked roots by default. `AnalyzeFeatureBoundary` evaluates a required `/Game/...` folder boundary using internal and crossing package edges, ranked boundary Blueprints, outside-scope groups, and explicit risk thresholds.

All architecture tools support bounded results and optional project scope or exact Blueprint filters where appropriate. They query the existing index and do not require a schema change or full rebuild when the current index is healthy.

`AnalyzeProjectArchitecture` composes the focused analyzers into a compact risk overview and prioritized hotspot list; detailed bounded evidence is opt-in. `PlanProjectRefactor` converts the same evidence into ordered, non-destructive actions with verification tools and parameters. It never edits assets, never issues deletion instructions, and does not include C++ migration planning.

Current index lifecycle:

- `BuildProjectIndex` performs a full rebuild
- `GetIndexStatus` reports readiness, counts, dirtiness, and DB path
- asset add, update, remove, and rename events attempt incremental sync
- Blueprint compile marks the index dirty so agents know live state changed

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

- Add dedicated graph node and pin editing tools
- Add function, interface, input, and dispatcher authoring
- Extend the first idempotent event-to-function workflow with branch/input and multi-stage selection/drag plans
