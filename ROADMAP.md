# Unreal MCP Plugin Roadmap

## Status Snapshot

As of August 25, 2026:

- Milestone 1 through Milestone 7 are implemented.
- Milestone 8 is usable; latent, async, timeline, and timer boundaries now have indexed semantic and confidence reporting, while runtime callback confirmation remains.
- Milestone 9 is the active milestone. Safe Blueprint authoring Phases 1 through 3 and Phase 4A collection-aware primitives are implemented and live-tested. Phase 4B transactional graph patching, created-node pin prediction, cross-Blueprint call-site reconstruction, and deterministic mutation timeout handling are the immediate blockers for the AXIS camera/selection correction.
- Milestone 10 indexed project architecture analysis is complete for the current static-analysis scope.
- Token-efficient query profiles, explicit search coverage, structured diagnostics, and bounded test evidence have been incorporated into the upcoming work.
- A non-milestone cleanup pass (September 1, 2026) removed dead pre-SDK adapter code, split the adapter's oversized `UnrealSessionManager.cs` into responsibility-scoped partial-class files, de-minified 18 hand-packed native tool files back to normal formatting, and de-duplicated 5 of the `AddBlueprint*NodeTool` family (Reroute, Branch, FunctionCall, VariableGet, VariableSet) onto a new shared `BlueprintGraphEditToolUtils::AddSimpleGraphNode` helper. All ~104 tool schemas remain byte-identical to pre-cleanup; the automation regression suite passed throughout. A new `SaveAllDirtyPackages` tool was added to unblock automated editor shutdown during the cleanup and is now part of the adapter's shutdown flow. The `AGENTS.md` file-size guideline was reworded to drop its numeric threshold in favor of "split when responsibilities can be separated cleanly." Splitting `ApplyBlueprintGraphPatchTool.cpp` remains deferred until Phase 4B below lands, to avoid colliding with in-flight work on that file.

## Priority Timeline

1. Complete Milestone 9 Phase 4B transactional graph patching with full dry-run pin prediction, typed/instance function wiring, rollback, and compile-before-save semantics.
2. Add cross-Blueprint function-signature propagation and deterministic cold-load mutation request handling, then validate the ordered three-Blueprint AXIS editing workflow.
3. Implement Milestone 9 Phase 4C high-level idempotent Blueprint interaction workflows.
4. Complete the Milestone 8 token-efficient query contract and coverage reporting without changing the indexed graph model.
5. Extend Milestone 10 with focused project-health diagnostics while preserving its completed architecture-analysis core.
6. Add Milestone 11 structured logging/diagnostics and Milestone 12 C++ build feedback required for safe refactoring.
7. Add only the restricted Milestone 13 runtime-evidence/session controls needed by explicit deterministic tests, followed by Milestone 15 reusable test evidence and history.
8. Implement Milestone 14 Blueprint-to-C++ migration planning, rewiring, and equivalence validation.
9. Continue with advanced editor systems, source control, performance, and documentation.

## Goal

Build a native Unreal Engine Editor plugin that exposes the Model Context Protocol (MCP) directly from Unreal Engine, enabling AI coding agents such as Codex CLI and Claude Code to interact with the editor without an external bridge server.

## Current Target

### Engine Version

- Unreal Engine 5.4.4

### Primary Language

- C++20

### Optional

- Unreal Python as fallback only

## Vision

The Unreal Editor itself becomes an MCP server.

```text
AI Coding Agent
        |
        | MCP
        v
Unreal MCP Plugin
        |
        v
Unreal Editor APIs
```

- No standalone Python server
- No Node.js bridge
- No external daemon

## Runtime Interaction Policy

UnrealMCP does not autonomously perform exploratory gameplay or user-interface interaction.

- The user owns manual PIE playtesting, mouse and touch interaction, dragging, camera behavior, gameplay feel, visual quality, and final acceptance decisions.
- UnrealMCP owns static project analysis, indexed Blueprint tracing, safe editor mutations, compilation, bounded log collection, and structured diagnostics.
- Runtime automation is limited to explicit, deterministic, project-defined tests and evidence collection requested or approved by the user.
- Generic clicking, dragging, unrestricted console execution, arbitrary runtime function calls, and autonomous gameplay are outside the default MCP surface.
- Starting PIE, standalone sessions, screenshots, or runtime tests must target an explicit project/session and must not be inferred from an ordinary analysis or editing request.
- Runtime evidence can support a diagnosis, but UnrealMCP must not declare interaction quality or gameplay behavior correct without user validation.

## Milestone 1 - Plugin Foundation

### Goal

Create a loadable editor plugin.

### Tasks

- Create editor plugin
- Module startup and shutdown
- Settings object
- Logging category
- Version information
- Configuration system
- Enable and disable plugin
- Health check

### Deliverable

Plugin loads successfully and reports status.

### Status

- Completed

## Milestone 2 - MCP Runtime

### Goal

Implement an embedded MCP server.

### Tasks

- MCP server core
- Tool registry
- JSON serialization
- Request dispatcher
- Response serializer
- Error handling
- Capability discovery

### Deliverable

Agent can connect and list available tools.

### Status

- Completed

## Milestone 3 - Project Discovery

### Goal

Read and navigate a large Unreal project safely before making changes.

### Tools

- `SearchAssets`
- `GetAssetInfo`
- `ListAssets`
- `GetDependencies`
- `GetReferencers`
- `ListFolders`
- `AssetExists`
- `FindAssetsByClass`
- `FindAssetsByPath`

### Deliverable

Agent can discover project structure and inspect assets without unnecessary loading.

### Status

- Completed

## Milestone 4 - Blueprint Reading

### Goal

Inspect Blueprint structure deeply without editing it.

### Tools

- `GetBlueprintInfo`
- `ListVariables`
- `ListFunctions`
- `ListComponents`
- `GetParentBlueprint`
- `GetImplementedInterfaces`
- `ListChildBlueprints`

### Deliverable

Agent can understand Blueprint inheritance, variables, functions, and component layouts.

### Status

- Completed

## Milestone 5 - Project Index Foundation

### Goal

Build a local Unreal-owned knowledge index so agents can understand large projects without repeated full scans.

### Tools

- `GetIndexStatus`
- `BuildProjectIndex`
- `RefreshProjectIndex`
- `GetIndexedAssetSummary`
- `GetIndexedBlueprintSummary`

### Deliverable

Agent has a persistent local database of project structure that can be refreshed and validated.

### Status

- Completed

### Notes

- Partial refresh is preferred for ordinary changed, added, and deleted assets.
- Full manual rebuild is reserved for missing, invalid, or intentionally reset indexes.
- Project-only indexing is the current default behavior.

## Milestone 6 - Feature Discovery and Relationship Queries

### Goal

Answer project-understanding questions such as "what uses this Blueprint?" or "how does this feature connect?".

### Tools

- `GetAssetReferences`
- `GetAssetReferencers`
- `GetBlueprintDependencies`
- `GetBlueprintComponentHierarchy`
- `TraceFeatureFlow`
- `SummarizeBlueprintCluster`

### Deliverable

Agent can navigate feature relationships and dependency clusters safely.

### Status

- Completed

### Notes

- Structured feature-understanding tools are now good enough for real project exploration.
- Outputs were improved to better support entry-point discovery, cluster summaries, and flow summaries.

## Milestone 7 - Level and World Reading

### Goal

Read current level and world context to connect assets with runtime/editor placement.

### Tools

- `ListActors`
- `GetActorInfo`
- `FindActorsUsingBlueprint`
- `GetLevelActorDependencies`
- `ListSelectedActors`

### Deliverable

AI can understand level composition and actor-to-asset relationships.

### Status

- Implemented
- Built successfully
- Live-tested successfully

### Notes

- Current tools:
  - `ListActors`
  - `GetActorInfo`
  - `FindActorsUsingBlueprint`
  - `GetLevelActorDependencies`
  - `ListSelectedActors`
- This milestone is the current stopping point for real-world validation before moving further.

## Milestone 8 - Indexed Blueprint Graph and Trace Engine

### Goal

Build a tracing-first indexed Blueprint graph model so agents can explain real Blueprint logic without forcing the AI to reconstruct execution from raw metadata.

### Core design rules

- Store normalized Blueprint graph facts in the project index.
- Do not store precomputed traces in the database.
- Perform trace traversal locally inside UnrealMCP on demand.
- Return already-traced structured results to the coding agent.

### Phase 1 required

- Stable graph identity model
- Stable node identity model
- Stable pin identity model
- Graph extraction during `BuildProjectIndex`
- Indexed storage for:
  - graphs
  - nodes
  - pins
  - exec edges
  - data edges
  - variable read and write usage
  - function call sites
  - interface call sites
  - cross-Blueprint interaction edges
- Local trace engine for:
  - forward trace
  - backward trace
  - bounded trace by depth and node count
  - cycle detection
  - cross-Blueprint continuation

### Phase 2 important

- Event dispatcher bind and broadcast-to-handler tracing completed and live-tested across Blueprints
- Interface implementation resolution completed
- Macro expansion completed and live-tested
- Recursive collapsed/composite graph indexing and expansion completed and live-tested
- Nested collapsed graphs use hierarchical stable identities, so duplicate display names do not collide in large projects
- Variable ownership classification
- Pure-function vs exec-flow classification
- Object reference resolution for calls on connected Blueprint instance variables completed and live-tested
- Runtime bridge between Blueprint graph traces and level actor context
- Exact vs inferred vs unresolved trace confidence markers

### Phase 3 advanced

- Latent and async flow markers completed for indexed metadata classification
- Timeline and timer continuation semantics completed for static/indexed boundaries and resolvable timer callbacks; runtime callback confirmation remains
- Partial graph refresh for dirty Blueprints completed
- Trace summaries optimized for token efficiency
- Future graph-aware editing support built on stable node identities

### Token-efficient query contract

- Add progressive response profiles instead of returning full graph data by default:
  - `overview` for graph inventory, entry points, counts, and important relationships
  - `trace` for bounded execution and data-flow evidence
  - `full` for complete selected-node and pin details
  - `diagram` for a compact execution-flow representation
- Support consistent `maxDepth`, `maxNodes`, `offset`, and `limit` bounds where applicable.
- Return `hasMore`, `truncated`, `coverageComplete`, `scannedCount`, and `matchedCount` so a zero-result never implies full coverage incorrectly.
- Preserve owning Blueprint/class information for variable reads, writes, function calls, interface calls, and dispatcher interactions.
- Prefer indexed server-side filtering and local traversal over returning raw nodes for the coding agent to reconstruct.
- Allow optional local result artifacts for exceptionally large diagnostics, but keep compact structured MCP results as the default.
- Track response bytes, elapsed time, result counts, and estimated serialized tokens; do not claim model-token savings without client-side token measurements.

### Implemented tools

- `FindBlueprintTraceStartPoints`
- `TraceBlueprintFlow`
- `FindBlueprintVariableUsage`
- `FindCrossBlueprintCalls`
- `InspectBlueprintNode`
- `FindBlueprintNodeReferences`
- `ExplainBlueprintTrace`

### Deliverable

Agents can ask how Blueprint logic actually flows across events, functions, variables, and cross-Blueprint calls, and UnrealMCP returns a locally computed traced subgraph from indexed graph facts.

### Status

- In progress and usable
- Phase 1 indexed graph extraction and bounded local tracing are implemented
- Function, custom-event, cross-Blueprint, macro, interface implementation, and dispatcher binding transitions are live-tested
- Forward traces include incoming data dependencies so value-producing macros and pure nodes are visible
- Remaining work includes runtime confirmation of dynamic async/delegate/timer callbacks, timeline track data, and broader UFunction latent-metadata indexing
- Remaining work also includes the progressive response profiles and shared coverage/pagination metadata defined above.
- Runtime level actor context is intentionally deferred
- Deterministic editor automation tests cover node-semantic classification and exact/inferred/unresolved trace confidence.

## Milestone 9 - Blueprint Editing and Compile

### Goal

Add safe, transactional Blueprint authoring on top of the indexed understanding model.

### Tools

- Phase 1: `CreateBlueprintAsset`, `AddBlueprintComponent`, `AddBlueprintComponents`, `AddBlueprintVariable`, `SaveBlueprint`, `ValidateBlueprint`
- Existing: `CompileBlueprint`, `CompileAllBlueprints`, component/variable/graph read tools
- Phase 2: dedicated graph node and pin editing tools (in progress)
- Phase 3: function, interface, input, and dispatcher authoring
- Phase 4: idempotent high-level interaction wiring workflows

### Phase 4A - Collection-aware graph authoring

Add the generic primitives required to author real selection, inventory, grouping, and relationship logic without manual Blueprint wiring.

- `AddBlueprintVariable` and `AddBlueprintFunctionParameter` support explicit scalar, array, set, and map container types, including independently typed map key/value terminals and backward-compatible `isArray` requests.
- Add typed collection operation nodes:
  - array `Contains`, `Add`, `AddUnique`, `RemoveItem`, `Clear`, `Length`, and indexed `Get` are implemented through `AddBlueprintArrayOperationNode`
  - set `Contains`, `Add`, `Remove`, and `Clear` are implemented through `AddBlueprintSetOperationNode`
  - map `Add`, `Find`, `Contains`, `Remove`, `Clear`, `Keys`, and `Values` are implemented through `AddBlueprintMapOperationNode`
- Generic StandardMacros authoring is implemented through `AddBlueprintMacroNode`; `ForEachLoop` is live-tested with stable loop-body, array-element, index, and completed pins.
- `ForEachLoopWithBreak` authoring, typed array connection, stable Break pin, reconstruction, and Blueprint compilation are live-tested.
- `AddBlueprintTypedOperatorNode` implements object equality/inequality, Boolean `AND`/`OR`/`NOT`, and vector add/subtract/equality-with-tolerance.
- `ConnectBlueprintPins` resolves wildcard pins transactionally after the first typed connection and returns before/after types plus both resolved node pin models.
- Support dry-run, compile/save batching, idempotent retries, and deterministic placement for every new primitive.

### Phase 4B - Transactional graph patching

Allow an agent to insert bounded logic into an existing graph without manually disconnecting and reconstructing fragile execution paths.

- `SpliceBlueprintExecFlow` inserts one or more nodes between an exact source and target exec pin.
- `ApplyBlueprintGraphPatch` validates and applies a declarative batch of node creation, pin defaults, connections, disconnections, moves, and comments in one editor transaction.
- Required declarative node kinds are `existingNode`, `functionCall`, `variableGet`, `variableSet`, `typedOperator`, `branch`, and `reroute`.
- Created nodes are addressable by plan-local IDs; successful results map every plan-local ID to its final graph name, node GUID, and complete pin IDs.
- Function-call nodes support static functions and instance functions. Preflight validates explicit target/self connections, including object/class compatibility such as a `CesiumGeoreference` variable connected to a Cesium instance-function target.
- Typed operators inside a patch use the same exact type rules as `AddBlueprintTypedOperatorNode`; initial required operations are `VectorAdd`, `VectorSubtract`, `VectorNearlyEqual`, and `BooleanNot`.
- Preflight predicts the complete pin model for every created node before mutation: name, direction, category, subcategory/object type, container type, default source/value, exec/data classification, target/self requirement, and planned-connection compatibility.
- `AddBlueprintFunctionCallNode(dryRun=true)` and shared node factories must return predicted pins rather than an empty `pins` array, so patch preflight and standalone dry-runs use one authoritative model.
- Preflight verifies graph identity, node/pin identity, type compatibility, wildcard resolution, instance targets, existing links, placement, and expected graph revision before mutation.
- One exact existing data link may be disconnected and replaced only when the plan identifies both endpoints and explicitly confirms replacement; unrelated links remain untouched.
- On any failed operation, roll back the complete patch and return structured diagnostics.
- Compile the edited Blueprint before saving. Compilation failure rolls back the transaction and leaves the asset unsaved.
- Save and partially refresh the index only after successful compilation and only when requested.
- Return created/reused node identities, final pin identities, changed links, compile results, save results, and partial-index refresh evidence.
- Require explicit confirmation before replacing an existing non-exec data connection or deleting linked nodes.

#### Phase 4B.1 - Function signature propagation

Changing a Blueprint function signature must update callers safely, including callers in other Blueprint assets.

- Extend `AddBlueprintFunctionParameter` or add `RefreshBlueprintCallSites` with `ownerBlueprint`, `functionName`, `compileCallers`, and `refreshIndex` inputs.
- Update the function entry/return nodes, find all indexed and live call sites, reconstruct caller nodes, and expose their refreshed pins.
- Return affected Blueprint paths, caller graph/node GUIDs, old/new pin IDs, reconstruction results, dirty state, compile results, and index-refresh evidence.
- Same-Blueprint-only reconstruction is insufficient; external callers such as `AC_ContextMenu` calling `AC_DragShapes.SelectEntityFromWorld` must be covered.
- Caller compilation can occur without saving. Saving remains an explicit later step so a multi-asset workflow can validate every affected asset first.

#### Phase 4B.2 - Ordered multi-asset editing

Per-asset transactions are sufficient initially; a global Unreal transaction across assets is optional.

- Support the safe sequence: patch owner Blueprint, compile, refresh/reconstruct call sites, patch callers, compile all touched assets, then explicitly save all validated assets.
- Preserve unsaved validated edits while later assets are patched; report every asset's transaction, compile, dirty, and save state.
- If a later asset fails, return a recovery plan identifying which earlier assets remain dirty and unsaved rather than silently saving a partial feature.
- An optional future `ApplyMultiBlueprintGraphPatch` may coordinate this workflow, but it must not delay the per-asset compile-without-save path.

#### Phase 4B.3 - Deterministic mutation request lifecycle

Cold Blueprint loads and long preflight work must not create an unknown mutation state.

- Add configurable per-request timeouts and a bounded `PreloadBlueprintForEditing` operation for large assets.
- Assign a request/operation ID before mutation and expose `GetMutationRequestStatus` for `queued`, `preflighting`, `mutating`, `compiling`, `completed`, `failed`, `rolled_back`, and `cancelled` states.
- A client transport timeout must not automatically mark the Unreal session failed when the editor and pipe remain healthy.
- After timeout, the adapter returns `operationId`, `mutationMayStillBeRunning`, and the required status/reconnect action; agents must not retry mutations while state is unknown.
- Idempotency keys prevent duplicate graph nodes or repeated link replacement after reconnect/retry.

### Phase 4C - Reusable selection and group-transform workflows

Build project-agnostic workflows from the collection and graph-patch primitives rather than hardcoding DSM asset names.

- Scaffold collection-backed helper functions such as select-one, toggle-selection, clear-selection, and contains-selection.
- Wire pointer-result branches for single selection, modifier-assisted toggle, and empty-hit clearing while preserving existing project hit testing.
- Wire group transforms from one primary target and a bounded per-update transform delta while preserving relative offsets.
- Support optional project-provided highlight and property-update functions/events through exact reflected signatures.
- Preserve legacy single-object movement, replication, context-menu, and property flows unless the request explicitly replaces them.
- Produce a dry-run interaction plan showing insertion points, preserved links, unresolved project-specific hooks, and manual steps.
- Do not perform autonomous mouse, touch, camera, or gameplay validation; the user remains responsible under the Runtime Interaction Policy.

### Phase 4D - Authoring verification

- Typed arrays, sets, maps, common typed operators, loop macros, and connection-driven wildcard resolution have editor automation coverage.
- Add editor automation coverage for vector-delta graphs and idempotent graph patches.
- Verify graph patches compile, save, partially reindex, and remain traceable through Milestone 8.
- Add rollback tests for stale graph identities, incompatible pins, occupied data inputs, and partial batch failures.
- Add dry-run pin-model tests for reflected static functions, instance functions, typed operators, branches, variable nodes, and reroutes.
- Add external call-site reconstruction tests for added function inputs/outputs and caller compilation across Blueprint assets.
- Add cold-load timeout tests proving a timed-out transport response cannot cause duplicate mutation on retry.
- Add ordered multi-asset tests that compile without saving, report dirty assets, and save only after every patch validates.
- Add fixture coverage for single selection, Ctrl-toggle selection, empty-hit clearing, and delta-based group movement graph structure.

### Safety rules

- Mutations run on the game thread and use editor transactions.
- Mutation tools support dry-run and explicit save/compile control.
- Return structured change summaries and stable identities.
- Never compile all or build the project implicitly.
- Destructive rename/delete/replace operations require reference analysis and explicit confirmation.
- Refresh only touched assets in the project index after saved edits.

### Deliverable

Reliable Blueprint compilation and limited safe editing.

### Status

- Phase 1 foundation is implemented and live-tested.
- Component authoring supports safe editable template defaults plus ordered, prevalidated batch creation in one transaction. The batch compiles, saves, and partially reindexes only once and remains idempotent on retry.
- Phase 2 graph primitives implemented and live-tested: collision-aware Branch, Sequence, Custom Event, Function Call, Variable Get, and Variable Set creation; node movement; pin connection/defaults/disconnection; and guarded node deletion.
- Authored graph changes are saved, partially reindexed, compiled, and trace-readable through the existing indexed trace engine.
- Additional Phase 2 tools implemented and live-tested: live pin listing with linked endpoint IDs, typed Dynamic Cast, wildcard Reroute, bounded Comment, and user function graph creation.
- AXIS-blocking primitives implemented and live-tested: engine override events, reflected Enhanced Input Action events, function input/output parameters with return-node creation, function metadata, generic struct pin splitting/recombining, dispatcher bind/unbind nodes, and exact signature-matched dispatcher events.
- Sequence output-pin management, node comment updates, and deterministic execution-flow batch layout are implemented. Phase 2 primitives are complete for the current scope.
- Phase 3 interface authoring supports exact interface calls, live-tested event-compatible implementations, and signature-correct output-bearing interface function graphs.
- Output-bearing interface implementation creation, signature pins, idempotency, target compilation, and temporary-asset cleanup are covered by the passing `UnrealMCP.Blueprint.Authoring.AddInterfaceFunctionGraph.Live` editor automation test.
- Dispatcher bind, unbind, exact signature event, and Blueprint-callable broadcast authoring are implemented and live-tested, including indexed cross-Blueprint broadcast-to-handler tracing.
- Generic object/class pin defaults are implemented and live-tested through `SetBlueprintPinDefaultObject`, including class selection for nodes such as `Get Component by Class`.
- Phase 4 has started with `WireBlueprintEventToFunction`, a generic idempotent workflow for Custom Event creation/reuse, exact function-call creation, optional component/member target wiring, deterministic layout, and one optional compile/save/index refresh. It preserves existing conflicting event and target wiring instead of replacing it.
- The first Phase 4 workflow is covered by the passing `UnrealMCP.Blueprint.Authoring.WireEventToFunction.Live` editor automation test, including dry-run, exact pin wiring, compilation, retry idempotency, duplicate prevention, and cleanup.
- `AddBlueprintMacroNode` is implemented and live-tested with the engine `StandardMacros` library. It created and wired the `ForEachLoop` used by `AC_DragShapes.ClearEntitySelection` to clear every selected entity highlight.
- `AddBlueprintArrayOperationNode` is implemented for typed array `Contains`, `Add`, `AddUnique`, `RemoveItem`, `Clear`, `Length`, and native indexed `Get`. It specializes wildcard array/item pins before connection, returns the resolved pin model, and is covered by the passing `UnrealMCP.Blueprint.Authoring.ArrayOperations.Live` automation test, including target Blueprint compilation.
- Typed Set/Map operations persist their declared types across node reconstruction and are covered by `UnrealMCP.Blueprint.Authoring.TypedContainerOperations.Live`.
- Set/Map variable and function-parameter declarations are covered by `UnrealMCP.Blueprint.Authoring.ContainerDeclarations.Live`; all common typed operators are covered by `UnrealMCP.Blueprint.Authoring.TypedOperators.Live`.
- `ForEachLoopWithBreak` and connection-driven wildcard readback are covered by `UnrealMCP.Blueprint.Authoring.ForEachLoopWithBreak.Live`.
- Phase 4A is complete for the current scope. Remaining Phase 4 work is transactional graph patching, reusable selection/group-transform workflows, vector-delta collection graphs, and broader authoring verification.
- Phase 4B is now explicitly scoped to atomic declarative patches, complete created-node pin prediction, instance-function targets, typed operators, cross-Blueprint signature propagation, ordered compile-without-save workflows, and deterministic timeout/retry semantics.
- The created-node preview prerequisite is implemented for reflected function calls, typed operators, Branch, Variable Get/Set, and Reroute nodes. Their standalone dry-runs allocate detached preview nodes and return complete predicted pin models without adding graph nodes. Shared pin serialization classifies exec/data/self pins, hidden pins, defaults, reflected types, wildcard state, and whether an external instance call requires an explicit target connection. Native compilation and live adapter dry-runs passed for all required kinds; focused editor automation coverage is present in `UnrealMCP.Blueprint.Authoring.FunctionCallPinPrediction.Live`.
- The first `ApplyBlueprintGraphPatch` slice is implemented with every required node kind, deterministic patch-local node GUIDs, complete dry-run/final pins, static and explicit-target instance functions, shared typed-operator resolution, input defaults, exact direct connections and confirmed disconnections, positions/comments, expected graph revision, and confirmed replacement of one exact occupied data link. Real changes use one transaction, compile before save, undo on operation/compile failure, default to compile-without-save, and partially refresh only after a requested successful save. Native compilation and an adapter-live three-node/two-connection dry-run passed; `UnrealMCP.Blueprint.Authoring.GraphPatch.Live` covers dry-run, apply, compile-without-save, deterministic retry, exact replacement rejection/application, and stale-revision rejection once run through editor automation.
- `SpliceBlueprintExecFlow` is implemented for inserting an ordered chain of existing nodes into one exact execution link. It provides full-route preflight, graph-revision guards, dry-run, idempotent retry, one transaction, original-route restoration on operation/compile/save failure, compile-before-save semantics, complete inserted-node pin readback, and exact removed/added link identities. Native compilation passes and `UnrealMCP.Blueprint.Authoring.ExecSplice.Live` covers dry-run, apply, retry, and stale-revision rejection once run through editor automation.
- `ApplyBlueprintGraphPatch` preflight now simulates planned disconnections and connections, in mutation order, on a transient duplicate of the graph. This invokes Unreal's K2 connection schema without touching the live asset, predicts connection-driven wildcard propagation across multi-reroute chains, returns fully specialized dry-run pins, and reports per-connection before/after wildcard state. Native compilation and an adapter-live positive Boolean/two-reroute dry-run passed with an unchanged graph revision; `UnrealMCP.Blueprint.Authoring.GraphPatch.WildcardPreflight.Live` covers dry-run non-mutation and compile-without-save apply once run through editor automation.
- Focused compile-failure rollback coverage is implemented and passing in `UnrealMCP.Blueprint.Authoring.GraphPatch.CompileRollback.Live`. Its reachable unresolved Array Add fixture requests a real patch with compile and save enabled, then verifies the compile error reports successful immediate rollback, the exact node count and graph revision are restored, and no package reaches disk. Adapter-controlled execution passed synchronously with zero unexpected errors or warnings.
- `RunUnrealMCPAutomationTest` is implemented as the adapter-accessible static regression runner for exact synchronous product- or engine-filtered `UnrealMCP.*` tests. It requires explicit confirmation, rejects arbitrary/non-UnrealMCP and latent tests, returns structured duration, pass state, diagnostics, errors, and warnings, and exposes a failed test as `success=false` at the MCP boundary.
- Phase 4B.3 request safety is implemented for the current reliability iteration. The adapter preassigns and exposes mutation operation IDs, preserves healthy attachment state on a single request timeout, and directs uncertain mutations to `GetMutationRequestStatus` instead of automatic retry. The native server tracks a bounded 1,024-operation session history with canonical request fingerprints, terminal response replay for identical requests, and conflict rejection for ID reuse with different arguments. `ApplyBlueprintGraphPatch` and `SpliceBlueprintExecFlow` additionally report preflight, mutation, compile, and rollback phases. `UnrealMCP.Reliability.MutationRequestReplay` covers canonical replay, duplicate prevention, conflict rejection, and terminal status lookup.
- Live timeout recovery is verified with an intentionally short adapter request timeout: the adapter returned `mutation_request_timeout`, retained `sessionState=ready` and `attached=true`, and a later `GetMutationRequestStatus` reported the original mutation `completed` and passed. Bounded cold-asset preloading and dedicated cold-load fixtures remain deferred, not removed, along with Phase 4B.1 cross-Blueprint signature propagation and Phase 4B.2 ordered multi-asset editing.
- Static regression discovery and bounded batch execution are implemented through `ListUnrealMCPAutomationTests` and `RunUnrealMCPAutomationTests`. The batch accepts only 1-64 explicit `UnrealMCP.*` names, supports complete-run or fail-fast behavior, never starts PIE, and returns aggregate plus per-test evidence. Suite-order execution exposed rollback compiler/streaming state leaking into the following test; isolation-sensitive negative compiler fixtures are now stable-partitioned to the suite tail with the actual order reported in the result.
- The immediate implementation focus for this iteration is reliability regression execution and correction of any failures. Phase 4B.1 and 4B.2 remain the next deferred implementation phases after this focused iteration.
- The immediate acceptance scenario spans `AC_DragShapes`, `AC_ContextMenu`, and `BP_GIS_AdvancedPawn`; UnrealMCP performs static editing/compilation while the user performs PIE, multiplayer, camera-quality, and visual validation.
- Remaining Phase 4 order is Phase 4B transactional graph patching and request safety, Phase 4C reusable selection/group-transform workflows, and Phase 4D authoring verification.

## Milestone 10 - Project Architecture Analysis

### Goal

Analyze unhealthy Unreal project structures before editing or migrating implementation code.

### Analysis

- Circular hard-reference and Blueprint inheritance dependencies
- Cross-Blueprint function, event, interface, and dispatcher coupling
- Missing assets, unresolved classes/functions, broken graph links, and stale redirectors
- God Blueprints, excessive fan-in/fan-out, dead assets, and unreachable logic
- Feature, folder, module, runtime, editor, UI, and level ownership boundaries
- Asset clusters that force-load unexpectedly large dependency groups

### Tools

- `AnalyzeProjectArchitecture`
- `FindCircularDependencies`
- `FindBrokenBlueprintReferences`
- `FindUnusedBlueprintAssets`
- `AnalyzeBlueprintCoupling`
- `AnalyzeFeatureBoundary`
- `PlanProjectRefactor`

### Deliverable

The agent receives an evidence-backed, ordered refactoring plan that breaks dependency cycles and establishes feature boundaries before migration.

### Status

- Complete for the current indexed static-analysis scope
- `FindCircularDependencies` implemented with deterministic strongly connected component analysis over indexed project Blueprint dependencies, bounded output, scoped package filtering, and exact internal cycle edges.
- `AnalyzeBlueprintCoupling` implemented with deterministic indexed package fan-in/fan-out ranking, internal/external counts, explicit risk thresholds, and clear static-analysis limitations.
- `FindBrokenBlueprintReferences` implemented with conservative indexed evidence for unresolved project dependencies, component/type/member paths, and orphaned graph edges, plus category and scope filters.
- `FindUnusedBlueprintAssets` implemented as conservative review-candidate analysis with explicit invisible-reference limitations and default exclusion of root-like assets.
- `AnalyzeFeatureBoundary` implemented with internal/crossing package-edge metrics, ranked boundary Blueprints, outside-scope grouping, bounded evidence, and explicit risk thresholds.
- `AnalyzeProjectArchitecture` implemented as a fault-tolerant, token-bounded composition of all focused analyzers with compact section summaries, risk overview, and prioritized hotspots.
- `PlanProjectRefactor` implemented as a deterministic, non-mutating action plan ordered by broken references, cycles, coupling, feature boundaries, and manual unused-candidate review. Each action includes verification tooling and safety metadata.
- Runtime architecture, dynamic-loading proof, C++ migration advice, and destructive cleanup remain outside this milestone.

### Focused extension backlog

These additions build on the existing index and do not invalidate the completed architecture-analysis scope:

- `CompareBlueprints` for bounded structural, dependency, component, variable, function, and graph differences
- `FindBlueprintsByFunction` for implementation and call-site discovery
- `SearchIndexedBlueprintNodes` with exact coverage and truncation reporting
- `FindBlueprintVariableAccess` with read/write operation and owning Blueprint/class classification
- `AnalyzeNamingConventions` for configurable project naming rules
- `AnalyzeContentPlacement` for misplaced assets and feature-boundary violations
- Asset-health summaries for widget complexity, material complexity, texture configuration, mesh LOD/Nanite configuration, and unusually expensive dependency clusters
- Index freshness evidence on every analysis response: build timestamp, dirty state/count, indexed scope, and whether live confirmation is recommended

## Milestone 11 - Logging and Diagnostics

### Goal

Improve debugging and provide structured evidence for architecture cleanup, builds, and runtime validation.

### Tools

- `TailLog`
- `WaitForLogPattern`
- `GetLogDiagnostics`
- `GetWarnings`
- `GetErrors`
- `CaptureScreenshot`

### Requirements

- Cursor-based or timestamp-based log reads so repeated calls return only new evidence.
- Filters for category, severity, time range, text pattern, PIE session, and maximum lines.
- Group repeated messages and return counts instead of duplicating identical lines.
- Normalize Blueprint runtime errors, assertions, ensures, crashes, compiler diagnostics, and plugin transport failures into structured records.
- Include source file, line, asset/object path, session state, and recommended next action when available.
- Long-running operations return progress and a bounded diagnostic summary rather than dumping the complete Output Log.
- Diagnostic tools are read-only; clearing or deleting logs is not part of the default agent surface.

### Deliverable

Agent can investigate failures with bounded, structured evidence without requesting complete logs.

### Status

- Not started

## Milestone 12 - Build System

### Goal

Compile C++ projects and return structured compiler diagnostics.

### Status

- Not started

### Tools

- `BuildProject`
- `BuildModule`
- `HotReload`
- `GenerateProjectFiles`
- `GetCompilerErrors`
- `GetBuildStatus`
- `CancelBuild`

### Requirements

- Build operations run as explicit tracked jobs with job ID, target, platform, configuration, start time, progress, and final state.
- Parse compiler and linker output into grouped diagnostics with file, line, module, code, message, and repeated-count fields.
- Return log and artifact paths while keeping the inline response bounded.
- Never start a full project build implicitly from a Blueprint edit or validation tool.
- Preserve custom engine locations and project-specific target selection.

### Deliverable

End-to-end C++ iteration.

## Milestone 13 - Restricted Runtime Evidence and Test Sessions

### Goal

Provide bounded runtime sessions and deterministic evidence without autonomously operating or playtesting the application.

### Tools

- `PlayPIE`
- `StopPIE`
- `PausePIE`
- `ResumePIE`
- `Simulate`
- `GetPIEState`
- `LaunchStandaloneGame`
- `StopStandaloneGame`
- `GetRuntimeSessionInfo`
- `WaitForRuntimeEvidence`

### Validation modes

- Mode A: in-editor PIE for fast interactive checks
- Mode B: standalone game launched from the editor for process-boundary checks
- Mode C: packaged build for release-like validation

### Requirements

- Every operation targets an explicit Unreal/project session.
- PIE, standalone, screenshot, and runtime-test operations require an explicit user request or approval.
- Start and stop operations are idempotent and verify the resulting state.
- Runtime evidence is limited to bounded logs, reflected properties, screenshots, and project-defined test assertions.
- Manual mouse/touch interaction, dragging, camera evaluation, gameplay feel, visual acceptance, and exploratory playtesting remain user responsibilities.
- UnrealMCP does not use generic desktop automation or autonomous input playback as a substitute for project-defined tests.
- Known crash-prone console commands are rejected in code, not merely documented as warnings.
- Generic process launch, arbitrary remote function calls, and arbitrary console execution are not exposed as unrestricted tools.

### Deliverable

AI can run explicitly requested deterministic tests and collect bounded runtime evidence; the user remains responsible for interactive behavior validation.

### Status

- Restricted and deprioritized
- Session status, deterministic project tests, logs, reflected assertions, and screenshots remain valid future scope.
- Autonomous gameplay, generic clicking/dragging, and interaction-quality judgments are explicitly out of scope.

## Milestone 14 - Blueprint-to-C++ Migration

### Goal

Analyze Blueprint migration candidates, validate reflected C++ replacements, and safely rewire remaining Blueprint logic.

### Responsibilities

- UnrealMCP analyzes Blueprint graphs and project dependencies.
- The coding agent creates and edits `.h` and `.cpp` files using normal source tools.
- UnrealMCP inspects the compiled reflected API, rewires Blueprint call sites, and validates the resulting graph and runtime behavior.

### Tools

- `AnalyzeBlueprintForCppMigration`
- `PlanBlueprintCppMigration`
- `PlanCppMigrationOrder`
- `InspectCppBlueprintAPI`
- `MapBlueprintNodesToCpp`
- `WireBlueprintToCpp`
- `ValidateCppMigration`

### Planning evidence

- Rank candidates using graph size, tick/latent usage, dependency fan-in/fan-out, cycle participation, inheritance, component ownership, and engine/editor API requirements.
- Identify subsystem, actor-component, function-library, UObject, and actor migration candidates separately.
- Produce dependency-aware migration order so cycle-breaking foundations are migrated before dependent Blueprints.
- Map Blueprint nodes and pins to reflected C++ functions/properties only after a successful build and reflection refresh.
- Distinguish exact replacements, wrappers still required in Blueprint, unsupported nodes, and behavior that requires runtime validation.
- Compare pre/post graph structure, compile diagnostics, references, PIE behavior, and test evidence before declaring equivalence.

### Deliverable

An incremental migration workflow that does not reproduce existing Blueprint dependency problems in C++.

### Status

- Not started

## Milestone 15 - Testing

### Automation

- `RunAutomationTests`
- `RunSingleTest`
- `ListTests`

### Gameplay

- Only for explicitly requested deterministic project tests; exploratory interaction remains manual.
- Launch PIE
- Execute test
- Collect results
- Stop PIE

### Test evidence and history

- Record test mode, project, engine version, plugin version, git revision when available, start/end time, result, and bounded diagnostics.
- Keep machine-readable latest-run and historical reports.
- Support wait-for-log assertions, reflected property assertions, screenshot evidence, and automation-test results.
- Keep screenshots and large raw logs as referenced artifacts rather than embedding them in every MCP response.

## Milestone 16 - Python Fallback

### Goal

Support editor automation not yet implemented as native tools.

### Status

- Deferred

### Tool

- `ExecutePython`

### Rules

- Disabled by default
- Enabled through project settings
- Sandboxed where practical
- Used only when no dedicated tool exists

### Deliverable

Maximum flexibility during development.

## Milestone 17 - Advanced Editor Systems

### Material Editor

- Create Material
- Create Material Instance
- Assign Material

### Niagara

- Create System
- Open System
- Compile System

### Animation

- Animation Blueprint
- Skeleton inspection

### Sequencer

- Create Sequence
- Add Track
- Add Camera

### Landscape

- Landscape inspection
- Landscape layers

## Milestone 18 - Source Control

### Tools

- `CheckOut`
- `Submit`
- `Revert`
- `Diff`
- `History`
- `CurrentBranch`
- Conflict detection

## Milestone 19 - Performance

### Goals

- Cache Asset Registry lookups
- Avoid unnecessary asset loading
- Stream large responses
- Return structured data
- Keep UI responsive during long-running tasks
- Use progressive response profiles and server-side filtering for Blueprint graph queries
- Include explicit pagination, truncation, and coverage metadata
- Measure response size and duration per tool; treat estimated token counts separately from actual model usage
- Distinguish a slow cold asset load from a lost Unreal session; do not invalidate attachment solely because one request exceeded the client timeout
- Support bounded preload/warm-up for explicitly selected Blueprints before expensive graph preflight or mutation
- Track long mutation state by operation ID so timeout recovery never depends on blind retries

## Milestone 20 - Documentation

### Create

- API reference
- Tool reference
- Examples
- Agent integration guide
- Plugin configuration guide
- One-time MCP server instructions describing tool families, safety rules, index freshness, and preferred workflows
- Tool job map separating indexed analysis, live editor reads, mutations, diagnostics, builds, and runtime validation
- Prompt recipes for overview-first Blueprint analysis, bounded traces, project-health review, test execution, and migration planning

### Universal MCP Distribution

- [x] Replace custom Content-Length framing with the official MCP C# SDK stdio transport
- [x] Keep stdout protocol-only and route adapter diagnostics to stderr
- [x] Provide one project-agnostic `unrealmcp.exe serve` entry for Codex, Claude Code, Cursor, Cline, and Antigravity
- [x] Add client detection, interactive setup, dry-run configuration, guarded removal, and `doctor --json`
- [x] Register the current adapter directory by default, with optional user-selected `--install-dir` copying
- [x] Preserve unrelated client configuration through scoped JSON merges and backups
- [x] Persist the authoritative live Unreal tool catalog so tools remain discoverable while Unreal is closed
- [ ] Include a freshly captured plugin tool catalog in each public release artifact
- [ ] Validate packaged builds against MCP Inspector and one clean installation of each supported client

## Coding Standards

- Follow Unreal Engine coding conventions
- Use modern C++
- Avoid global state
- Return structured results
- Prefer Asset Registry over loading assets
- Never block the editor unnecessarily
- Separate transport from tool implementations

## Success Criteria

The project is considered feature complete when an AI coding agent can:

- Explore a large Unreal project safely
- Search and inspect assets
- Read Blueprint inheritance, variables, functions, and components
- Edit project content safely
- Compile Blueprints
- Build C++
- Launch and stop PIE
- Read logs
- Diagnose build errors
- Create common gameplay assets
- Save project changes

...without requiring manual editor interaction for routine development tasks.

## Future Ideas

- Multi-user collaboration support
- Live event subscriptions
- AI-assisted Blueprint generation
- Graph editing APIs
- Gameplay Ability System tools
- Control Rig tools
- MetaSound tools
- Remote editor sessions
- Distributed build integration
- Plugin marketplace support
