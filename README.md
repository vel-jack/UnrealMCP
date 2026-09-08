# Unreal MCP Plugin

This plugin embeds a minimal Model Context Protocol server directly into the Unreal Editor.

## Development Context

Coding agents and contributors should read [AGENTS.md](AGENTS.md) before changing the plugin. Current milestone status, priority order, and acceptance criteria are tracked in [ROADMAP.md](ROADMAP.md).

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
- `GetBlueprintComponentDefaults`
- `SetBlueprintComponentDefaults`
- `AddBlueprintVariable`
- `ListBlueprintGraphs`
- `InspectLiveBlueprint`
- `TraceLiveBlueprintFlow`
- `ValidateBlueprint`
- `SaveBlueprint`
- `SaveAllDirtyPackages`
- `ListUnrealMCPAutomationTests`
- `RunUnrealMCPAutomationTest`
- `RunUnrealMCPAutomationTests`
- `GetMutationRequestStatus`
- `RefreshBlueprintCallSites`
- `SaveValidatedBlueprints`
- `WireSelectionWorkflow`
- `AddBlueprintBranchNode`
- `AddBlueprintMacroNode`
- `AddBlueprintArrayOperationNode`
- `AddBlueprintSetOperationNode`
- `AddBlueprintMapOperationNode`
- `AddBlueprintTypedOperatorNode`
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
- `InspectEnhancedInputActionWiring`
- `CreateInputAction`
- `CreateInputMappingContext`
- `AddInputMappingContextMapping`
- `RemoveInputMappingContextMapping`
- `SetInputMappingContextMappingKey`
- `GetInputMappingContextMappings`
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
- `WireEnhancedInputActionToComponent`
- `ApplyBlueprintInteractionPlan`
- `ApplyBlueprintGraphPatch`
- `SpliceBlueprintExecFlow`
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

## Live Level Blueprint Inspection

`InspectLiveBlueprint` and `TraceLiveBlueprintFlow` read resident editor objects, including the `LevelScriptBlueprint` embedded in a `.umap`. They build on the current editor-world context used by level/selected-actor inspection. They do not require an Asset Registry entry or a project index, and include unsaved in-memory graph changes.

Target either the current level (omit selectors), `level: "persistent"`, an exact loaded `mapPath` (package/world/level path), or a resident Blueprint/graph `objectPath`. Use only one target selector. `graphPath` optionally narrows the resolved Blueprint to one exact graph, avoiding duplicate graph-name ambiguity. A missing map/object returns a structured diagnostic; these tools never load an asset, create a Level Blueprint, or switch the active map.

Example calls (tool name followed by JSON arguments):

```text
InspectLiveBlueprint {"level":"current"}
InspectLiveBlueprint {"mapPath":"/Game/Maps/MyMap","mode":"nodes","query":"SetWorldLocation","limit":20}
InspectLiveBlueprint {"objectPath":"<returned Blueprint objectPath>","graphPath":"<returned graphPath>","nodeGuid":"<returned nodeGuid>"}
TraceLiveBlueprintFlow {"startEvent":"BeginPlay","maxNodes":200,"maxDepth":128}
TraceLiveBlueprintFlow {"objectPath":"<returned Blueprint objectPath>","graphPath":"<returned graphPath>","startNodeGuid":"<returned nodeGuid>"}
```

Inspection defaults to paginated graph inventory. `mode: "nodes"`, `query`, or `nodeGuid` returns nodes with GUIDs, exact linked endpoints, pin types, split-pin relationships, literal/object/text defaults, function/variable owner information, and direct level-actor references where present. Defaults are authored values, not evaluated runtime values. `offset` and `limit` (default 50, maximum 200) page the selected graph/node inventory; use `nextOffset` while `hasMore` is true. Graph inventory includes revisions for before/after comparison.

Tracing defaults to `BeginPlay` (`ReceiveBeginPlay`), or accepts an exact event member name or `startNodeGuid`. It follows outgoing execution/data links and incoming data dependencies, including the connected continuation after delays and function calls. Data producers do not imply their execution successors ran. Resident Blueprint function, custom-event, macro, and collapsed-graph bodies are expanded with caller/body pin bindings; native implementations, unavailable bodies, and dynamic interface dispatch are explicit boundaries. Declared external instance bodies are static candidates, not proof of runtime dispatch. Caller continuation is retained even when a body is unavailable or expansion is bounded.

Trace defaults are `maxNodes: 100`, `maxDepth: 128`, and `maxCallDepth: 4` (maximums 1000, 512, and 16). `includePins: false` reduces output; inspect individual nodes for full pins. Cycles/repeated visits are deduplicated. `frontier`, `hasMore`, `truncated`, and `coverageComplete` expose traversal limits and unresolved boundaries. Increase bounds or inspect/trace the returned frontier nodes; partial traces are never proof of complete coverage. This is a static connected subgraph, not an execution timeline or runtime-value simulation.

Live results explicitly report `source: "live_editor"`, `readOnly: true`, `indexUsed: false`, and package dirty state. Existing `ListBlueprintGraphs`, `InspectBlueprintNode`, and `TraceBlueprintFlow` keep their indexed behavior and report `source: "cached_index"`; the live tools never silently fall back to them.

Inspection does not start PIE, compile, reconstruct nodes, edit assets, save packages, or refresh/rebuild the index. Even the Level Blueprint resolver reads the existing pointer directly because UE 5.4's `GetLevelScriptBlueprint(true)` assigns its friendly name. All object access runs on the game thread. Runtime camera behavior still requires separate testing.

The focused static regression is `UnrealMCP.Blueprint.LiveInspection.EmbeddedLevelAndTrace`. It constructs an unregistered transient object tree without compiling or saving a Blueprint and exercises embedded paths, pin evidence, delay/function continuation, downstream nodes, cycle and budget handling, data dependency isolation, and unchanged graph/dirty/compile state.

New tool registration requires loading the rebuilt plugin in a fresh editor session, then refreshing the adapter's live tool catalog. Preserve unsaved work before restarting; the adapter's generic shutdown command saves dirty packages and is unsuitable for a strict read-only inspection handoff.

## Safe Blueprint Authoring

Phase 1 authoring supports transactional Blueprint creation, component addition, component-default inspection/update, variable addition, validation, and explicit saving. `AddBlueprintComponent` accepts safe editable component-template `propertyDefaults`; setting `updateExistingDefaults=true` explicitly applies those defaults when the same-name/same-class component already exists. `GetBlueprintComponentDefaults` reads bounded editable template values, while `SetBlueprintComponentDefaults` preflights changes on a transient duplicate before one transaction/compile/save. `AddBlueprintComponents` prevalidates an ordered component hierarchy and applies it in one transaction, with one optional compile/save/index refresh. Mutation tools support `dryRun`, `compileAfterEdit`, and `saveAfterEdit`; creation never overwrites an existing asset. Successful saved edits refresh only the touched asset in the project index.

Component defaults accept JSON strings, numbers, and booleans. Complex Unreal values can be supplied as export-text strings. Existing same-name/same-class components remain unchanged unless `updateExistingDefaults=true` is supplied, so retrying a normal add request cannot silently replace their configured defaults.

The first Phase 2 graph-editing slice supports stable GUID targeting, collision-aware Branch placement, node movement, validated pin connections/defaults, and confirmed node deletion. Automatic layout uses 320-pixel horizontal spacing and 180-pixel vertical collision steps by default.

The second slice adds Sequence, Custom Event, exact reflected function-call, and member-GUID Variable Get/Set nodes, plus guarded single-link or all-link disconnection. Saved authored flows are immediately available to indexed tracing after partial refresh.

The next slice adds live pin/linked-endpoint readback, typed casts, wildcard reroutes, bounded comments, and uniquely named function graphs. All were live-tested with save, validation, and indexed graph readback.

Pin inspection preserves Unreal's separate literal, object/class, and text defaults. `defaultValue` is the effective AI-facing value, while `literalDefaultValue`, `defaultObjectPath`, `defaultTextValue`, and `defaultValueSource` explain exactly where Unreal stored it.

`SetBlueprintPinDefaultObject` safely assigns exact UObject or UClass defaults to unconnected object/class input pins. This covers nodes such as `Get Component by Class`, whose selected class is stored in Unreal's `DefaultObject` rather than its literal `DefaultValue`.

Indexed traces classify latent, async, timeline, and timer boundaries from existing graph metadata. Trace results include exact, inferred, or unresolved confidence, explicit continuation models, and unresolved transition records. Runtime-dependent callback selection still requires PIE validation.

Project-oriented authoring now includes function input/output parameters and return-node creation, function metadata, parent-class override events, reflected Enhanced Input Action events, generic struct pin splitting/recombining, and dispatcher bind/unbind nodes with exact signature-matched Custom Events. These primitives were live-tested together with validation, save, and partial index refresh.

`AddBlueprintInterfaceEventNode` adds or idempotently resolves event-compatible interface implementations in Ubergraphs. Interface functions with output/return parameters are rejected because Unreal requires those implementations to use function graphs.

`AddBlueprintInterfaceFunctionGraph` creates or resolves signature-correct implementation graphs for non-event interface functions, including output and return signatures. `AddBlueprintDelegateBroadcastNode` creates validated Blueprint-callable multicast delegate broadcasts with exact signature pins.

Output-bearing interface graph creation is regression-tested by `UnrealMCP.Blueprint.Authoring.AddInterfaceFunctionGraph.Live`, including signature propagation, idempotent retry, target compilation, and temporary fixture cleanup.

`LayoutBlueprintNodes` arranges an explicit node set by local execution-flow depth with configurable spacing and collision avoidance. It returns every proposed or applied position and supports dry-run before mutation.

`WireBlueprintEventToFunction` is the first Phase 4 high-level workflow. It idempotently creates or reuses a Custom Event, adds one exact impure Blueprint-callable function call, optionally wires a Blueprint component/member variable into the call target, lays out the nodes, and performs at most one compile/save/index refresh. It refuses to replace an event's existing execution route or a function call's existing target connection. Use `dryRun=true` before applying changes to an unfamiliar Blueprint.

`AddBlueprintMacroNode` adds macros from a Blueprint macro library and defaults to Unreal's `StandardMacros`. `AddBlueprintArrayOperationNode` adds typed native array `Contains`, `Add`, `AddUnique`, `RemoveItem`, `Clear`, `Length`, and indexed `Get` nodes. Supply `elementType`; object/class arrays also require `typeObjectPath`. Unlike a generic reflected function call, this tool resolves Unreal's wildcard array and item pins before connection and returns their container/subtype metadata for verification.

`AddBlueprintSetOperationNode` adds persistent typed Set `Contains`, `Add`, `Remove`, and `Clear` nodes. `AddBlueprintMapOperationNode` adds persistent typed Map `Add`, `Find`, `Contains`, `Remove`, `Clear`, `Keys`, and `Values` nodes with independent key/value types. Their custom node class reapplies declared types after graph reconstruction, so unconnected authored nodes do not revert to wildcards after reopening the Blueprint.

`AddBlueprintTypedOperatorNode` provides exact object equality/inequality, Boolean `AND`/`OR`/`NOT`, and Vector add/subtract/nearly-equal operations. Vector nearly-equal accepts an optional non-negative `tolerance`.

Function-call, typed-operator, Branch, Variable Get/Set, and Reroute dry-runs now return complete predicted pin arrays from detached preview nodes without changing the graph. Each serialized pin includes its direction, full scalar/container type, default source/value, exec/data classification, self-pin classification, hidden state, wildcard state through its category, and `requiresExplicitTarget` for external instance calls. These tools report `pinsPredicted`, while function calls also report top-level `requiresExplicitTarget`, allowing declarative graph-patch preflight to validate complete planned wiring before mutation.

`AddBlueprintVariable` and `AddBlueprintFunctionParameter` accept `containerType` values `none`, `array`, `set`, or `map`. For Maps, `type` describes the key and `valueType` describes the value; object/class terminals use their corresponding object-path fields. Existing `isArray=true` requests remain supported. `ConnectBlueprintPins` now returns pin types before and after connection, complete resolved node pin lists, and `wildcardResolved` when Unreal specializes a wildcard from the first typed connection.

`InspectEnhancedInputActionWiring` reports every matching Enhanced Input Action node and the exact execution targets of its five phase pins. `WireEnhancedInputActionToComponent` inserts one exact component function call into one phase, preserves a single existing continuation after the new call, and is idempotent on retries. It rejects ambiguous action nodes, multiple phase routes, missing components, and non-callable functions. Use inspection and `dryRun=true` first; required data inputs are reported as `unconnectedInputPins` for explicit follow-up wiring.

`CreateInputAction` and `CreateInputMappingContext` create new `UInputAction`/`UInputMappingContext` data assets, following the same reject-overwrite/dry-run/optional-save pattern as `CreateBlueprintAsset`; `CreateInputAction` also sets `ValueType` (`Boolean`, `Axis1D`, `Axis2D`, `Axis3D`). `AddInputMappingContextMapping` and `RemoveInputMappingContextMapping` add or remove one exact key-mapping row in an existing `InputMappingContext` by calling the real `UInputMappingContext::MapKey`/`UnmapKey` functions through reflection, so Epic's own mutation logic runs unchanged; adding an already-present row is a no-op (`alreadyExists=true`), and removing a row that does not exist fails clearly instead of guessing. `GetInputMappingContextMappings` reads back every row (action, key, trigger/modifier classes) without loading the asset for editing. None of the five tools add Enhanced Input as a hard module dependency — `UInputAction`/`UInputMappingContext` are resolved at runtime the same way `AddEnhancedInputActionNode` resolves its node class.

Mapping removal now requires `confirm=true` for a real edit; inspect its `dryRun=true` response first. Add/remove reject ambiguous duplicate Action+Key rows. The adapter treats removal as a mutation: it assigns an `operationId` and directs an uncertain timeout to `GetMutationRequestStatus`, not a blind retry. Action value types reject enum sentinels as well as unknown names.

`SetInputMappingContextMappingKey` takes `objectPath`, `inputActionPath`, `key` (expected current key), and `newKey`. Use `dryRun=true` first, then `confirm=true` for replacement. It changes only the key of one exact row, retaining row order, action, inline modifiers/triggers and player-mappable metadata; it rejects stale/ambiguous selectors and duplicate targets. Data validation uses a detached context preview, then a real edit uses one undoable transaction. `saveAfterEdit` defaults to false; an unchanged key is a no-op and does not save. A disk-save failure is reported separately (`changed=true`, `saved=false`, `errorCode=asset_save_failed`), so the in-memory edit is not mistaken for a rollback. Reuse the original operation ID for identical retries; replay is session-scoped. Custom IMC subclasses and runtime mapping rebuilds are outside this bounded tool's scope.

Phase 5A is partial: action-property editing, key/class discovery, detailed settings readback, modifier/trigger authoring, player-mappable metadata editing, and batch edits are still planned. Deprecated input-config APIs are excluded. Runtime user settings and gameplay validation remain deferred.

`UnrealMCP.EnhancedInput.AssetAuthoring.Live` covers the original five tools plus key replacement, dry-run/confirmation guards, invalid values/classes/keys, duplicate and ambiguous rows, multiple keys per action, inline-object/metadata preservation, one-step undo, dirty-state preservation, and operation replay. Fixtures are unique unsaved data assets, retired to the transient package after registry removal without forced GC. Explicit save/reload and injected post-mutation/save-failure regression cases remain pending.

The workflow is regression-tested by `UnrealMCP.Blueprint.Authoring.WireEventToFunction.Live`, covering dry-run, exact execution and component-target pin links, compilation, idempotent retry, duplicate prevention, and temporary fixture cleanup.

`ApplyBlueprintInteractionPlan` applies a complete declarative graph fragment in one preflighted operation. Its schema supports `existingNode` GUID anchors, `customEvent`, `functionCall`, `variableGet`, `branch`, and `sequence` nodes plus exact named-pin connections. Stable `workflowId` and node `id` values make retries idempotent. Existing target-pin or execution-route conflicts are rejected instead of replaced, and compile/save/index refresh run at most once after the plan.

A `sequence` node can safely extend one occupied execution output by specifying `spliceAfterNodeId` and `spliceAfterPinName`. The tool inserts the Sequence, reconnects the original route through `Then_0`, and leaves `Then_1` available for the new workflow. The splice must have exactly one existing route, is fully preflighted, and is validated rather than repeated on retry. This is the preferred way to add a new interaction call beside legacy pointer/interface handling without removing that behavior.

Plan nodes also accept `inputDefaults` entries with either `defaultValue` or `defaultObjectPath`. Defaults are applied before connection validation, enabling typed nodes such as `Get Component by Class` to connect safely in the same transaction. `executionInsertions` atomically insert one impure node into an exact existing exec link and validate the completed route on retry.

The declarative workflow is regression-tested by `UnrealMCP.Blueprint.Authoring.InteractionPlan.Live`, covering complete dry-run preflight, exact existing-node anchors, four-node/three-link application, component-target data wiring, legacy-route-preserving Sequence insertion, compilation, idempotent retry, duplicate prevention, and temporary fixture cleanup.

`ApplyBlueprintGraphPatch` is the atomic Phase 4B patch primitive. It supports `existingNode`, `functionCall`, `variableGet`, `variableSet`, `typedOperator`, `branch`, and `reroute` nodes; literal/object input defaults; exact named-pin connections and confirmed disconnections; node positions and comments; and explicitly confirmed replacement of one exact occupied data link. `patchId` plus each plan-local node `id` produce deterministic node GUIDs, so a retry cannot create duplicate nodes. Optional `expectedGraphRevision` rejects stale plans before mutation.

Patch dry-runs return the current graph revision, deterministic future node GUIDs, and complete predicted pins. Preflight duplicates the graph transiently and applies planned disconnections and connections in mutation order, allowing Unreal's K2 schema to predict wildcard specialization across multi-node chains without changing the live asset. Connection results report wildcard state before and after simulation plus `wildcardResolved`; the result also reports `wildcardResolvedConnectionCount`. Real patches execute in one editor transaction, compile before any save, immediately undo on an operation or compile failure, and default to compile-without-save. A requested save occurs only after successful compilation and is followed by a partial index refresh. Results include created/reused identities, complete final pins, changed/replaced/disconnected links, compile evidence, dirty-preserving save state, and final graph revision.

`UnrealMCP.Blueprint.Authoring.GraphPatch.CompileRollback.Live` provides the focused negative-path fixture for compile-before-save rollback. It starts from an intentionally compiler-invalid temporary Blueprint, applies a valid deterministic patch with saving requested, and verifies the compile error reports immediate rollback, restores the exact node count and graph revision, and never creates the package on disk.

`RunUnrealMCPAutomationTest` runs one exact registered synchronous `UnrealMCP.*` editor automation test through the adapter, covering both product- and engine-filtered plugin tests. It requires `confirm=true`, rejects non-UnrealMCP test names and concurrent or latent test execution, and returns structured pass state, duration, errors, warnings, and execution entries. A failed test returns `success=false`, so MCP clients surface the call as failed while preserving its complete diagnostics. This provides a UI-free static verification path while keeping editor launch, attachment, and shutdown under adapter control.

`ListUnrealMCPAutomationTests` discovers the registered product- and engine-filtered plugin tests with an optional name substring and bounded result limit. `RunUnrealMCPAutomationTests` accepts an explicit preferred-order list of 1-64 discovered test names, requires `confirm=true`, and returns per-test evidence plus aggregate pass/fail, diagnostic, warning, duration, and early-stop counts. Isolation-sensitive negative compiler fixtures are stable-partitioned to the suite tail; `executionOrderAdjusted` and `executionOrder` make that behavior explicit. It never starts PIE and defaults to continuing after failures so one run produces complete bounded regression evidence; set `continueOnFailure=false` for fail-fast execution.

The adapter assigns an `operationId` before every recognized mutation and exposes that optional field in mirrored mutation schemas. Unreal keeps a bounded in-editor request record with canonical request fingerprinting. Reusing the ID for the identical request replays its terminal response without executing again; reusing it for different arguments is rejected. `GetMutationRequestStatus(operationId)` returns the tool name, lifecycle state, timestamps, terminal flag, diagnostics, and stored terminal result/error. States are `queued`, `preflighting`, `mutating`, `compiling`, `completed`, `failed`, `rolled_back`, and `cancelled`; graph patches and exec splices publish their detailed phases while other mutations receive queued and terminal tracking.

A mutation transport timeout no longer invalidates an otherwise healthy editor attachment. The adapter returns `mutation_request_timeout`, the preassigned `operationId`, `mutationMayStillBeRunning=true`, and `canRetry=false`. Query `GetMutationRequestStatus` with that ID before deciding whether any retry is necessary. Read-only request timeouts are retryable and likewise do not clear the session; reconnect only if a later health check shows the pipe is unavailable.

`SpliceBlueprintExecFlow` atomically replaces one exact existing execution link with an ordered chain of existing nodes. It requires exact source, target, and inserted-node exec pin identities; preflights the whole route; rejects occupied inserted pins or a stale graph revision; and treats an already-complete route as an idempotent retry. Dry-run never changes the graph. Apply uses one transaction, restores the original route on mutation or compile failure, compiles before an optional save, and defaults to compile-without-save.

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

- uses the official MCP C# SDK and newline-delimited JSON-RPC over stdio
- discovers Unreal projects from a workspace
- resolves Unreal Editor installs from `.uproject` `EngineAssociation`
- supports explicit engine overrides for custom editor locations
- connects to the in-editor named pipe transport
- proxies Unreal tool calls
- keeps answering adapter status tools when Unreal is down
- caches the authoritative plugin tool catalog so client tasks retain Unreal tool schemas while the editor is closed

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
- `unreal.adapter.RefreshToolManifest`
- `unreal.adapter.RequestUnrealShutdown`

`unreal.adapter.RequestUnrealShutdown` first calls `SaveAllDirtyPackages` on a best-effort basis before closing the editor window, so a routine shutdown does not block on the editor's native "Save Content" confirmation dialog. `SaveAllDirtyPackages` saves every dirty package directly to its existing on-disk path (no picker, no prompt); pass `dryRun=true` to list dirty packages without saving.

The first `tools/list` automatically selects and attaches to the sole discoverable project, without launching the Editor. Ambiguous workspaces still require explicit selection. Once an attachment finds a changed native catalog, the adapter persists it and sends `notifications/tools/list_changed`; `unreal.adapter.RefreshToolManifest` also explicitly reattaches/refetches without launching. It accepts an optional `projectPath`. Clients must re-list tools after this notification; a client that ignores it still needs its own manifest refresh or reconnection. Running processes using an older adapter binary must load the new build once. Cache write failures do not hide live schemas, and unavailable Editors retain the last catalog.

Adapter regression checks use isolated temporary catalogs and a simulated native pipe (no Editor required):

```powershell
dotnet run --project .\Adapter\UnrealMCP.Adapter.RegressionTests -c Release
```

If the configured adapter executable is locked by active MCP clients, build to a separate output directory with `dotnet build ... -o <verification-directory>` and run `UnrealMCP.Adapter.RegressionTests.exe` there. This does not replace/restart connected clients. The runner's optional `--live <adapter-exe> <project.uproject> [exact-test-name]` checks a running Editor's freshly discovered catalog and can invoke one explicitly selected static test through the adapter. It never launches PIE or starts the Editor itself.

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

### Universal Setup

Run the built executable without arguments in a terminal for interactive setup, or configure clients explicitly:

```powershell
UnrealMCP.Adapter.exe
UnrealMCP.Adapter.exe configure --all --dry-run
UnrealMCP.Adapter.exe configure --client codex,cursor --yes
UnrealMCP.Adapter.exe doctor --json
```

By default, setup registers `UnrealMCP.Adapter.exe` from its current directory and does not copy any files. Use `--install-dir <path>` only when the user explicitly chooses a separate installation location. Setup uses official client CLIs for Codex and Claude Code, and surgically merges only `mcpServers.unreal-mcp-adapter` for Cursor, Cline, and Antigravity. Existing unrelated configuration is preserved; conflicting UnrealMCP entries are refused rather than overwritten.

Supported client targets:

- Codex desktop/CLI
- Claude Code
- Cursor
- Cline CLI and VS Code/Cursor extension storage
- Antigravity

The generated registration is project-agnostic:

```json
{
  "mcpServers": {
    "unreal-mcp-adapter": {
      "command": "C:\\Path\\To\\Current\\UnrealMCP.Adapter.exe",
      "args": ["serve"]
    }
  }
}
```

Do not put a `.uproject` path in MCP client configuration. The adapter discovers projects from the client-provided working directory, then agents select or launch a project with adapter-local tools.

Optional user-selected copy:

```powershell
UnrealMCP.Adapter.exe configure --client codex,cursor --install-dir D:\Tools\UnrealMCP --yes
```

When `--workspace` is omitted, the adapter uses its current working directory as the discovery root. In Codex, that is intended to align with the chat's primary source folder / working directory.

With a default Unreal Editor path:

```json
{
  "mcpServers": {
    "unreal": {
      "command": "C:\\Path\\To\\Current\\UnrealMCP.Adapter.exe",
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
      "command": "C:\\Path\\To\\Current\\UnrealMCP.Adapter.exe",
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

### Doctor And Removal

```powershell
UnrealMCP.Adapter.exe doctor --json
UnrealMCP.Adapter.exe uninstall --client codex,cursor --dry-run
UnrealMCP.Adapter.exe uninstall --client codex,cursor --yes
```

`doctor` starts a fresh adapter process and verifies MCP initialization framing. JSON configuration writes use a same-directory temporary file and preserve a `.unrealmcp.bak` backup before mutation. Removal only deletes an entry that still matches UnrealMCP's installed command.

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
