# Unreal MCP Plugin Roadmap

## Status Snapshot

As of September 4, 2026:

- Current user-selected priority: Milestone 8 Phase 4, Live Inspection of Embedded Level Blueprints (Annotation 1). Both tools are implemented, native-built, and verified with focused static and live-editor tests; large transitive traces explicitly report truncation. Host-specific acceptance evidence is retained only in ignored local development notes. Remaining Milestone 9 Phase 5A work is deferred for this iteration, not cancelled.

- Milestone 1 through Milestone 7 are implemented.
- Milestone 8 is usable; latent, async, timeline, and timer boundaries now have indexed semantic and confidence reporting, while runtime callback confirmation remains.
- Milestone 9 is the active milestone. Safe Blueprint authoring Phases 1 through 3 and Phase 4A collection-aware primitives are implemented and live-tested. Phase 4B transactional graph patching, created-node pin prediction, and deterministic mutation timeout handling are implemented; Phase 4B.1 cross-Blueprint call-site reconstruction is implemented via `RefreshBlueprintCallSites`; Phase 4B.2's ordered multi-asset save step is implemented via `SaveValidatedBlueprints`. Phase 4C's core (reusable selection helper-function scaffolding) is implemented via `WireSelectionWorkflow`; pointer-branch wiring and group-transform remain deferred project-agnostic workflow work.
- A separately discovered crash bug was fixed: `GetBlueprintInfo`, `GetImplementedInterfaces`, `GetParentBlueprint`, `ListComponents`, `ListFunctions`, `ListVariables`, and `ListChildBlueprints` resolved their target Blueprint directly on the pipe-handling thread instead of the game thread, so calling any of them on a Blueprint asset that was not already resident in memory hit Unreal's `IsInGameThread()` load assertion and crashed the entire editor process. All seven now wrap their Blueprint resolution in `ExecuteOnGameThreadSync`, matching every other tool's convention; verified live against genuinely cold (never-loaded-this-session) Blueprint assets after a full editor restart, with zero schema or behavior change otherwise.
- Milestone 10 indexed project architecture analysis is complete for the current static-analysis scope.
- Token-efficient query profiles, explicit search coverage, structured diagnostics, and bounded test evidence have been incorporated into the upcoming work.
- A non-milestone cleanup pass (September 1, 2026) removed dead pre-SDK adapter code, split the adapter's oversized `UnrealSessionManager.cs` into responsibility-scoped partial-class files, de-minified 18 hand-packed native tool files back to normal formatting, and de-duplicated 5 of the `AddBlueprint*NodeTool` family (Reroute, Branch, FunctionCall, VariableGet, VariableSet) onto a new shared `BlueprintGraphEditToolUtils::AddSimpleGraphNode` helper. All ~104 tool schemas remain byte-identical to pre-cleanup; the automation regression suite passed throughout. A new `SaveAllDirtyPackages` tool was added to unblock automated editor shutdown during the cleanup and is now part of the adapter's shutdown flow. The `AGENTS.md` file-size guideline was reworded to drop its numeric threshold in favor of "split when responsibilities can be separated cleanly." Splitting `ApplyBlueprintGraphPatchTool.cpp` remains deferred until Phase 4B below lands, to avoid colliding with in-flight work on that file.
- Milestone 9 Phase 5, Enhanced Input Asset Authoring, is complete and live-verified as of September 3, 2026. It was scoped after a real integration exposed that no existing tool could create a `UInputAction`/`UInputMappingContext` asset or edit an `InputMappingContext`'s key-mapping rows. All five tools (`CreateInputAction`, `CreateInputMappingContext`, `AddInputMappingContextMapping`, `RemoveInputMappingContextMapping`, `GetInputMappingContextMappings`) were implemented via reflection against the Enhanced Input plugin's exposed `UFUNCTION`s/`UPROPERTY`s (no hard EnhancedInput module dependency added), compiled clean, and live-verified end to end through the adapter against isolated `/Game/UnrealMCP_Test/` fixtures. See Milestone 9 Status for the full list.
- Phase 5's adapter catalog-staleness bug is fixed in source: initial tools/list discovers and attaches to one unambiguous project without launching the Editor; changed catalogs after attachment emit notifications/tools/list_changed and are cached immediately. RefreshToolManifest provides an explicit in-session refresh. SDK stdio regressions cover warm/cold startup, late attachment, ambiguity, simulated new-tool registration/callability without restart, and removal timeout identities. Clients must support re-listing after notifications; already-running old adapter processes need replacement/restart once.

- Milestone 9 Phase 5A is partially implemented (September 3, 2026); remaining work is deferred behind Milestone 8 Phase 4 in this integration project. Reliability prerequisites and exact mapping-key replacement are implemented. Action-property editing, bounded key/class discovery, detailed settings readback, inline modifier/trigger authoring, player-mappable metadata editing, and single-asset batches remain. Deprecated types stay excluded; deferred work is not cancelled.

## Priority Timeline

Current override (September 4, 2026): implement and verify Milestone 8 Phase 4 below first. The existing backlog order remains recorded here; Phase 5A resumes after this live Level Blueprint inspection capability.

1. Completed prerequisite: fix adapter catalog refresh and add Enhanced Input baseline automation. See Phase 5A and the verification handoff below.
2. Finish Milestone 9 Phase 4C's two remaining bullets: wiring the pointer-valid/modifier-held branch into an existing project hit-test event, and the group-transform workflow (one primary target plus a bounded per-update delta).
3. Add Phase 4D automation coverage for vector-delta/group-transform graphs once Phase 4C lands. Phase 5 now has formal UnrealMCP.EnhancedInput.AssetAuthoring.Live coverage.
4. Resume Phase 5A after the implemented reliability/key-replacement slice with action properties and bounded key/class/settings inspection, then inline modifiers/triggers, player-mappable metadata, and single-asset batches. Phase 4C/4D remain deferred for that iteration, not cancelled.
5. Complete the Milestone 8 token-efficient query contract and coverage reporting without changing the indexed graph model.
6. Add Milestone 12 C++ build/hot-reload feedback (`BuildModule`, `GetCompilerErrors`) and Milestone 14's `InspectCppBlueprintAPI` — both were manually substituted for this session (shelling out to a build tool directly, hand-reading engine headers) while building Phase 5, confirming real value ahead of their prior position in this list.
7. Extend Milestone 10 with focused project-health diagnostics while preserving its completed architecture-analysis core.
8. Add Milestone 11 structured logging/diagnostics.
9. Add only the restricted Milestone 13 runtime-evidence/session controls needed by explicit deterministic tests, followed by Milestone 15 reusable test evidence and history.
10. Implement the rest of Milestone 14 Blueprint-to-C++ migration planning, rewiring, and equivalence validation.
11. Continue with advanced editor systems, source control, performance, and documentation.

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

### Phase 4 - Live Inspection of Embedded Level Blueprints (Annotation 1)

Status (September 4, 2026): implemented, native-built, and verified in a controlled integration host. Expanded regression and live-editor inspection passed. Exhaustive transitive coverage beyond the embedded Level Blueprint remains bounded/incomplete, so do not mark the entire phase complete yet. Host-specific verification evidence is kept in ignored local development notes.

Build on Milestone 7's existing current-level and selected-actor inspection context. Ordinary Blueprint and Enhanced Input assets are already accessible; this extension reaches the `LevelScriptBlueprint` embedded in a map's `.umap` package through the live editor world/level and exposes its graph to coding agents.

#### Required scope

- [x] Resolve the embedded Level Blueprint from the currently open editor level or an explicit loaded map path. Report the resolved world, level, map package, and Blueprint object paths; distinguish persistent/current/sublevel targets and reject ambiguity. Never create a missing Level Blueprint or silently switch the active map. If a requested map is unavailable for read-only resolution, return a structured diagnostic.
- [x] Support direct live object-path inspection of Blueprints and graphs, including embedded `LevelScriptBlueprint` objects, without requiring an Asset Registry entry or indexed Blueprint row.
- [x] Enumerate graphs and nodes with stable graph identities, node GUIDs, pin identities/types, authored default values/object references, and exact execution/data connections. Authored pin values are static evidence, not evaluated runtime values.
- [x] Trace forward execution and relevant data dependencies from events such as `BeginPlay`. Follow connected outputs after delays and function calls; expand resolvable Blueprint function/macro graphs and report native, dynamic, or unavailable bodies as explicit boundaries while retaining the caller's downstream continuation.
- [x] Expose connected target pin types, function/variable owners, data-producing nodes, and exact literal actor references. External declared instance bodies are labelled inferred; dynamic/unavailable bodies are explicit boundaries. Runtime camera/pawn instance identity is not inferred from an unresolved reference.
- [x] Label graph/trace evidence with `live_editor` or `cached_index` provenance. Live inspection is independent of index availability and never silently falls back to cached facts. No mixed-source traversal is used.
- [x] Bound traversal with depth/node limits, repeated-visit/cycle suppression, coverage/truncation reporting, and frontier evidence for retrieving remaining nodes. A bounded partial trace is never reported as complete.
- [x] Keep inspection strictly read-only: no PIE, Blueprint compilation, graph reconstruction, asset modification, saving, or index rebuild/refresh. Execute UObject access on the game thread and preserve the active level, selection, and existing dirty state.

#### Implementation and verification order

1. Inspect the existing live world/selected-actor resolution, Blueprint object resolution, graph readers, and trace utilities. Reuse shared mechanisms and preserve existing tool contracts; select exact tool names/schema extensions after this review.
2. Implement generic native live Level Blueprint resolution and graph extraction, then bounded execution/data traversal with explicit continuation boundaries and provenance. Keep project-specific map names out of tool implementations.
3. Expose the capability through the adapter's mirrored tool catalog; update user-facing documentation. Build native code and any changed adapter code, then refresh the live catalog. Newly registered tools require an editor restart; coordinate deployment so unsaved project content is preserved and no save-on-shutdown workflow is invoked for this read-only acceptance run.
4. Run focused static tests for embedded object resolution, exact pin links, delay/function continuation, cycles/truncation, missing-index behavior, and unchanged editor/package state. Use a controlled test host for plugin regression, then perform read-only live MCP acceptance in an explicitly selected integration project.
5. Record exact graph/node/pin evidence and remaining unresolved boundaries from the acceptance map. No index schema change or rebuild is planned.

#### Acceptance criteria

- [x] With a representative map open, resolve its embedded Level Blueprint directly from editor context; repeat targeting by map path and returned live object path.
- [x] Trace the connected `BeginPlay` to delayed saved-position restoration chain, including its JSON vector data source and `GetPlayerPawn(0).RootComponent` target. Separate camera-component/rotation restoration is not inferred from this branch.
- [ ] Inspect every connected downstream node after `SetWorldLocation`, following additional bounded requests as needed; report any unresolved boundary or incomplete coverage explicitly.
- [x] Results identify live-editor provenance and remain usable without index coverage of the embedded Blueprint.
- [x] No PIE, compilation, explicit asset edits, or saves were invoked by inspection. Controlled regression verified dirty-state preservation; live graph revisions and selection stayed unchanged. Concurrent user editing was kept separate from the controlled clean-state acceptance evidence.

This acceptance verifies Blueprint wiring only. Camera behavior, timing, possession, and runtime restoration correctness require separate user testing.

#### Implementation handoff (September 4, 2026)

- Added `InspectLiveBlueprint` (paged graph/node inventory and exact pins) and `TraceLiveBlueprintFlow` (execution/data traversal, resident function/custom-event/macro/composite bodies, caller/body pin bindings, boundaries and frontier). Added source provenance to the existing three indexed graph/node/trace readers without changing their selection behavior.
- Resident-only resolution uses the existing editor-world helper and reads `ULevel::LevelScriptBlueprint` directly: UE 5.4's `GetLevelScriptBlueprint(true)` still assigns `FriendlyName`, which is avoided here. No dependency, index schema, or adapter C# change was needed.
- A normal controlled-host Editor Development build passed after the user closed the editor. Later refinements and expanded tests passed a suffix build while the reopened editor retained the earlier binary. Final live testing must use the latest binary after restart.
- Through the adapter, the initial `UnrealMCP.Blueprint.LiveInspection.EmbeddedLevelAndTrace` regression passed with zero errors. Its first fixture produced 14 missing-self-scope warnings; the corrected fixture supplies a class scope and adds macro binding and dirty-package checks. Its rerun passed with zero errors and zero warnings. The fixture uses transient objects and never compiles or saves a Blueprint.
- Fresh adapter `tools/list` exposed both tools, and a live call inspected an explicitly selected loaded map with one graph, `source=live_editor`, `indexUsed=false`, and `packageDirty=false`. No runtime camera acceptance is claimed from this static evidence.
- Subsequent verification loaded the latest binary after restart; expanded regression passed with zero errors and zero warnings. Representative live inspection covered every graph and EventGraph node in the selected Level Blueprint, including nested command-line and saved-state flows. Graph revisions and actor selection stayed unchanged. Broad transitive traces hit the configured node limit and reported incomplete coverage. No downstream project's plugin was synchronized. Exact host evidence remains local rather than part of the public roadmap.
- No index rebuild, Blueprint compilation, PIE, asset edits/saves, commit, or push was performed by the inspection workflow. Existing unrelated project and plugin edits are preserved.

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
- Function-call nodes support static functions and instance functions. Preflight validates explicit target/self connections, including object/class compatibility for variables connected to external plugin instance-function targets.
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

Build project-agnostic workflows from the collection and graph-patch primitives rather than hardcoding host-project asset names.

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

### Phase 5 - Enhanced Input Asset Authoring

Add the missing non-Blueprint data-asset authoring required to wire new gameplay input without hand-editing assets outside MCP. Implemented and live-verified after a real navigation feature needed new `InputAction` assets and an `InputMappingContext` key remap that no existing tool could perform.

- `CreateInputAction` creates a new `UInputAction` asset at an explicit package path with an explicit value type (`Digital` (bool), `Axis1D`, `Axis2D`, `Axis3D`). Rejects overwrite, like `CreateBlueprintAsset`.
- `CreateInputMappingContext` creates a new `UInputMappingContext` asset at an explicit package path. Rejects overwrite.
- `AddInputMappingContextMapping` adds one key-mapping row to an existing `InputMappingContext`: target `InputAction` asset and an `FKey`. Idempotent if the identical Action+Key row already exists. Authoring optional triggers/modifiers is planned under Phase 5A, not part of the shipped tool.
- `RemoveInputMappingContextMapping` removes one exact existing mapping row (key plus action), returning the removed row for confirmation. Follows the same destructive-operation confirmation rule as other mutation tools; fails clearly rather than guessing when more than one row matches.
- `GetInputMappingContextMappings` reads back every mapping row (key, action, triggers, modifiers) in an `InputMappingContext` without loading it for editing, so an agent can inspect current bindings before deciding what to add, remove, or leave untouched. This closes the same read gap that made `GetAssetInfo` insufficient for `InputMappingContext`/`InputAction` assets.
- These tools must not require Enhanced Input as a hard plugin dependency beyond what `AddEnhancedInputActionNode` already assumes.
- Follow the same dry-run, idempotency, structured-result, and explicit save/compile-control rules as other Phase 4/5 authoring tools.
- Do not hardcode host-project asset names into these tools; they must stay project-agnostic.

### Phase 5A - Enhanced Input Mapping Authoring Extension (in progress)

Provide a practical editor-side mapping-authoring workflow on top of the shipped Phase 5 tools, not every Enhanced Input subsystem. The first reliability/key-replacement slice is implemented; unchecked items remain pending.

Implementation order: reliability prerequisites → action/key editing → inline modifiers/triggers → player-mappable metadata and organization → bounded batch edits. Add focused automation coverage with each slice.

#### Reliability prerequisites

- [x] Fix adapter tool-catalog refresh for both an already-running Editor at initial `tools/list` and an Editor attached later in the same agent session; new native tools must become discoverable without manual cache editing.
- [x] Classify `RemoveInputMappingContextMapping` and future removal tools as mutations, supplying operation IDs, queryable status, terminal replay, and non-retryable uncertain-timeout responses.
- [x] Add formal `UnrealMCP.*` automation coverage for the existing five Phase 5 tools before extending their behavior.

#### Action and key editing

- [ ] Inspect and edit supported Input Action properties, including value type, with validated property types and complete readback.
- [x] Replace an exact mapping's key while preserving its action, modifiers, triggers, and player-mappable metadata. Reject ambiguous selectors and unintended duplicate rows.
- [ ] Discover valid keys and supported concrete modifier/trigger classes with bounded, filtered responses.
- [ ] Extend readback to include relevant settings, class/object identities, and ordering rather than class names alone.

#### Inline modifiers and triggers

- [ ] Add, configure, remove, and reorder modifier instances on either an Input Action or one exact mapping row. Initial coverage includes Negate, Swizzle, Scalar, and Dead Zone.
- [ ] Add, configure, and remove trigger instances at both attachment levels. Initial coverage includes Pressed, Released, Hold, Tap, and Chord Action.
- [ ] Validate property types and values, supported concrete classes, ownership, and referenced Input Actions before mutation; preserve unrelated inline objects and settings.
- [ ] Treat modifier/trigger instances as owned inline UObjects, not unnecessary standalone Content Browser assets. Use existing concrete classes; authoring new algorithms is outside this iteration.

#### Mapping organization and player-mappable metadata

- [x] Support multiple distinct keys for one action within an IMC, with precise row selection and explicit ambiguity rejection.
- [ ] Inspect and edit `UPlayerMappableKeySettings` names, display names, and display categories; distinguish action-level settings from per-mapping overrides/inheritance.
- [ ] Keep grouping in this iteration to action/key rows within an IMC and display categories. Bundling multiple IMCs into a configuration requires a separate, explicitly agreed design and is deferred.

#### Safe bounded batch edits

- [ ] Add a bounded operation for multiple edits within one explicitly selected Input Action or IMC. Cross-asset atomic batches require separate design and are not implied.
- [ ] Dry-run the complete plan and validate all targets, selectors, properties, references, and conflicts before any mutation.
- [ ] Apply one editor transaction per batch and restore the complete in-memory change on mutation or asset-validation failure; preserve pre-existing dirty work.
- [ ] Validate input data assets before an explicitly requested save; these assets do not require Blueprint compilation. Default to no save, refresh only successfully saved touched assets, and report save failures separately from edit/rollback outcomes.
- [ ] Return exact changes, final mappings/settings, validation evidence, dirty/save/index state, and mutation operation identity. Retries must not duplicate inline objects or rows.

#### Acceptance and deferred scope

- September 3 implementation/verification handoff: reliability prerequisites and `SetInputMappingContextMappingKey` are implemented. The key edit uses a detached data-validation preview, exact Action+currentKey selector, explicit confirmation, one transaction, default-no-save, and separate edit/save outcomes. Supported IMC base assets only; no runtime mapping rebuild. Same-key requests are no-ops; original operation IDs replay edits without duplicate mutation.
- Native `UE_544_MCPEditor` Win64 Development and .NET Release builds passed. The adapter regression runner passed 17 checks, including simulated new-tool registration/callability without restarting a client, cold/late attachment, ambiguity, cache persistence/fallback, and removal timeout identity. A fresh production-adapter stdio connection to the live test Editor discovered and called the new key tool at initial tools/list with no prior attach.
- `UnrealMCP.EnhancedInput.AssetAuthoring.Live` passed four consecutive adapter-controlled runs after fixture cleanup was corrected, with zero errors/warnings. It covers all original five tools plus key replacement, multiple keys/action, inline modifier/trigger and metadata preservation, dirty-state guards, confirmation, enum/key/class rejection, duplicates/ambiguity, replay, and one-step undo. `UnrealMCP.Reliability.MutationRequestReplay` also passed. These were focused tests, not a rerun of the entire native suite.
- An initial repeat run hit UE's asset-streaming ensure in `ObjectTools` fixture deletion. Cleanup now unregisters only the test's unique unsaved data assets and moves them to the transient package, leaving GC to Unreal; no forced deletion/GC or Blueprint fixture cleanup change. Final repeated runs passed. Explicit disk save/reload and injected rollback/save-failure coverage remain pending; do not interpret this slice as full Phase 5A acceptance.
- The tested adapter build is isolated under `%LocalAppData%/Temp/UnrealMCPAdapterRegression` because configured adapter executables were locked by running MCP clients. No client configuration was changed and no processes serving other sessions were stopped. Complete a normal adapter Release rebuild/restart after those clients release the configured executable to deploy the fixes there.
- Handoff: test Editor closed through the adapter; no index rebuild/schema reset, Computer Use, PIE, or runtime validation. Source/docs remain uncommitted. Immediate next implementation: supported Input Action property inspection/editing, with bounded key/class/settings inspection before modifier/trigger authoring.
- [ ] Static editor tests cover create/read/edit, key replacement with settings preserved, modifier ordering, trigger properties and chord references, metadata inheritance/override, duplicate/ambiguous-row rejection, invalid inputs, dry-run non-mutation, rollback, explicit save/reload, and retry/timeout safety.
- [ ] Exercise the tools through the UnrealMCP adapter in the controlled test host. Build affected native/adapter code and refresh the authoritative tool catalog after schema changes.
- Exclude deprecated `UPlayerMappableInputConfig` and deprecated mapping-options APIs from new work.
- Defer runtime `UEnhancedInputUserSettings` management, user-profile persistence/remapping, custom input-data asset classes, and custom modifier/trigger algorithm authoring. Do not assume a standard `UInputDataAsset` base exists.
- PIE, actual key/controller behavior, camera/input feel, multiplayer, and visual acceptance remain user-owned. No Computer Use or implicit runtime activation of contexts.
- Reuse the existing index model; no schema change or full index rebuild is planned for this extension.

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
- Integration-blocking primitives implemented and live-tested: engine override events, reflected Enhanced Input Action events, function input/output parameters with return-node creation, function metadata, generic struct pin splitting/recombining, dispatcher bind/unbind nodes, and exact signature-matched dispatcher events.
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
- Live timeout recovery is verified with an intentionally short adapter request timeout: the adapter returned `mutation_request_timeout`, retained `sessionState=ready` and `attached=true`, and a later `GetMutationRequestStatus` reported the original mutation `completed` and passed. Bounded cold-asset preloading and dedicated cold-load fixtures remain deferred, not removed, along with Phase 4B.2 ordered multi-asset editing.
- Phase 4B.1 cross-Blueprint function-signature propagation is implemented through the new `RefreshBlueprintCallSites` tool (`ownerBlueprint`, `functionName`, `compileCallers`, `refreshIndex`, `dryRun`, `operationId`). It queries the project index's `blueprint_nodes` table for every `call_function` node whose `member_parent_path`/`member_name` match the owning function, covering same-Blueprint and cross-Blueprint callers in one pass, then reconstructs each caller's `UK2Node_CallFunction` node via Unreal's own `ReconstructNode()`, which rewires old links onto matching-named new pins and reports orphaned pins that need manual attention rather than silently dropping them. Each affected Blueprint asset is mutated in its own transaction, compiled without saving, and rolled back independently on compile failure so a multi-asset run reports exactly which assets are dirty versus rolled back; saving remains an explicit later step per Phase 4B.2. Because the project index is not auto-rebuilt on edits, the tool surfaces a clear error rather than silently returning zero call sites when the index looks unbuilt, and does not implicitly trigger a rebuild. Verified via native compilation and manual live-adapter dry-run/real-run smoke tests against a real project index; consistent with `FindCrossBlueprintCalls`/`FindBlueprintNodeReferences`, this index-backed tool has no automated `Live` editor test, since building a populated project index inside the automation-test sandbox is not part of this codebase's established test infrastructure.
- Static regression discovery and bounded batch execution are implemented through `ListUnrealMCPAutomationTests` and `RunUnrealMCPAutomationTests`. The batch accepts only 1-64 explicit `UnrealMCP.*` names, supports complete-run or fail-fast behavior, never starts PIE, and returns aggregate plus per-test evidence. Suite-order execution exposed rollback compiler/streaming state leaking into the following test; isolation-sensitive negative compiler fixtures are now stable-partitioned to the suite tail with the actual order reported in the result.
- Phase 4B.1, the core of Phase 4B.2, and Phase 4C's helper-function scaffolding are implemented (see above); the pointer-branch wiring and group-transform bullets of Phase 4C are the next deferred implementation work.
- Phase 5 Enhanced Input Asset Authoring is implemented and live-verified. The five tools are implemented under `Source/UnrealMCP/{Public,Private}/Tools/`, registered in `UnrealMCPModule.cpp`, and use reflection without a hard Enhanced Input module dependency. Creation rejects overwrite and supports dry-run/optional save; mapping edits call the engine's reflected mapping functions. A controlled Editor Development build passed, and isolated `/Game/UnrealMCP_Test/` fixtures verified creation, idempotent add, exact readback, removal, zero-match rejection, and invalid-key rejection through the adapter. Formal coverage exists in `UnrealMCP.EnhancedInput.AssetAuthoring.Live`, including the original five tools plus Phase 5A key replacement. The historical manual catalog workaround is superseded by the adapter catalog-refresh fix.
- The historical Phase 5 catalog-staleness issue is now fixed by initial unambiguous discovery plus tools/list_changed notification and RefreshToolManifest after late attachment; see Milestone 20. No manual cache overwrite is part of the current workflow.
- Phase 4C's helper-function scaffolding is implemented through the new `WireSelectionWorkflow` tool (`objectPath`, `elementTypeObjectPath`, `selectionVariableName`, `collectionType`, per-function name overrides, `dryRun`, `compileAfterEdit`, `saveAfterEdit`). It composes already-shipped primitives — `AddBlueprintVariable`, `CreateBlueprintFunctionGraph`, `AddBlueprintFunctionParameter`, `AddBlueprintVariableGetNode`, `AddBlueprintArrayOperationNode`/`AddBlueprintSetOperationNode`, `AddBlueprintBranchNode`, `ConnectBlueprintPins`, `ListBlueprintNodePins` — by calling their `Execute()` directly in-process (the same composition technique `PlanProjectRefactorTool` already uses), rather than reimplementing K2Node construction. It idempotently scaffolds four collection-backed helper functions (`SelectOne`, `ToggleSelection`, `ClearSelection`, `ContainsSelection`) over a caller-named array/set-of-object selection variable: `ClearSelection` and `SelectOne` are straight-line exec chains (`Clear`, `Clear`→`AddUnique`/`Add`); `ToggleSelection` branches on a pure `Contains` check into `Remove`/`Add` paths that both converge back to the function's end; `ContainsSelection` is pure data flow with a bool return. A function already present (`CreateBlueprintFunctionGraph` reporting `alreadyExists=true`) is left untouched rather than re-scaffolded — the tool does not validate that an existing function's body matches. `dryRun` reports a structural plan (which functions/variables would be created) rather than a byte-exact pin preview, since chaining several already-mutating primitive tools together cannot be safely previewed at pin level. Compile and save happen once at the end, mirroring `WireBlueprintEventToFunctionTool`'s pattern, not per sub-tool call.
  - Live-verified end to end against `/Game/UnrealMCP_Test/BP_MCPAuthoringTest`: dry-run, a real run that created and saved all four functions, and repeated idempotent re-runs (`alreadyScaffolded=true`, `changed=false`) across a full editor restart. `ValidateBlueprint` reported 0 errors/warnings; `InspectBlueprintNode` confirmed `ToggleSelection`'s exact intended shape (`function_entry → Branch`, fed by `Array_Contains`, branching into `Array_AddUnique`/`Array_RemoveItem`).
  - Bullets not yet built: wiring the pointer-valid/modifier-held branch into an existing project hit-test event (achievable today by composing `WireBlueprintEventToFunction`/`ApplyBlueprintInteractionPlan` with the four scaffolded functions, so no new tool is required unless a future request wants it pre-packaged); the group-transform workflow (a distinct problem shape deserving its own design pass); and optional highlight/property-update hooks (a natural follow-on extension once the base tool sees more use).
- Phase 4B.2's ordered multi-asset save step is implemented through the new `SaveValidatedBlueprints` tool (`objectPaths`, `dryRun`, `continueOnFailure`, `requireUpToDateCompile`, `refreshIndex`, `operationId`). It saves an ordered list of already-mutated Blueprints one at a time — the intended sequence being `ApplyBlueprintGraphPatch`/node-authoring tools on the owner, `RefreshBlueprintCallSites` on its callers, both compiled-without-save, then this tool as the single explicit save step. Per asset it requires the package to be dirty and, unless `requireUpToDateCompile=false`, that the Blueprint's compile status is up to date before saving; a non-dirty asset is skipped as a no-op rather than an error, preserving idempotent retries. On the first unsaveable asset it stops (unless `continueOnFailure=true`) and returns a `recoveryPlan` naming which object paths are already saved to disk, which one failed, and which were never attempted — so a caller always knows exactly which assets remain dirty and unsaved. An `ApplyMultiBlueprintGraphPatch` orchestrator remains optional future work per the original phase scoping; this save step does not require it. Verified live against a real project asset (dry-run, a genuine multi-object failure/recovery-plan case, a real save, and an idempotent re-save of an already-saved asset), all matching expected behavior.
- **Operational note surfaced during this work**: Unreal's Hot Reload/Live Coding only patches existing function bodies — it does not re-run `StartupModule()`, so a brand-new tool added and hot-reloaded into an already-running editor is compiled but never actually registered in the live tool registry (calls to it fail fast with no native-side log activity at all, which is how this was diagnosed). A genuine editor restart is required after adding a new tool before it is callable; editing an existing tool's logic is fine under hot reload. This is now the established expectation for future new-tool additions, not just this one.
- The immediate acceptance scenario spans three interacting host Blueprints; UnrealMCP performs static editing/compilation while the user performs PIE, multiplayer, camera-quality, and visual validation.
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

### Notes

- Once a project adds a new host-module C++ component or library, this milestone's `BuildModule`/`HotReload`/`GetCompilerErrors` are needed so an agent can trigger and confirm the build itself; today the user must manually build/hot-reload the host project before its new `UFUNCTION`s become wireable in Blueprint through MCP.

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
- `InspectCppBlueprintAPI` will also be useful outside migration: after a project adds a new native class, it would confirm the class is actually reflected and Blueprint-callable, and report its exact signature, before an agent wires Blueprint nodes to it.

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

### Public installation documentation (deferred)

- [ ] Add a concise quick start for cloning the plugin into a project's `Plugins/UnrealMCP` directory, enabling it, and building the Editor target.
- [ ] Document supported Unreal Engine, Windows, and .NET versions and distinguish SDK/build prerequisites from runtime prerequisites.
- [ ] Document adapter Release build, setup, client registration, initial project discovery/selection, connection health checks, and first safe read-only call.
- [ ] Add upgrade, adapter restart, cache refresh, uninstall, and clean-removal instructions.
- [ ] Explain source-only versus prebuilt adapter packages, what Unreal Engine components are not included, and the separate Unreal Engine license requirement.
- [ ] Publish a supported-version matrix, beta limitations, mutation/save safety notes, troubleshooting steps, and a minimal verification checklist.
- [ ] Keep all examples project-agnostic and free of machine paths, private asset names, and private verification evidence.

### Universal MCP Distribution

- [x] Replace custom Content-Length framing with the official MCP C# SDK stdio transport
- [x] Keep stdout protocol-only and route adapter diagnostics to stderr
- [x] Provide one project-agnostic `unrealmcp.exe serve` entry for Codex, Claude Code, Cursor, Cline, and Antigravity
- [x] Add client detection, interactive setup, dry-run configuration, guarded removal, and `doctor --json`
- [x] Register the current adapter directory by default, with optional user-selected `--install-dir` copying
- [x] Preserve unrelated client configuration through scoped JSON merges and backups
- [x] Persist the authoritative live Unreal tool catalog so tools remain discoverable while Unreal is closed
- [x] Fix first-handshake and late-attach tool-catalog staleness. The adapter auto-selects only a single unambiguous discoverable project before initial tools/list, never launches implicitly, caches live schemas, advertises tools.listChanged, and notifies after a changed catalog is attached. unreal.adapter.RefreshToolManifest explicitly refreshes a selected/provided project. SDK stdio regressions simulate registering a new native tool, attach and re-list it, then call it in the same client session; cold/late startup and ambiguity are covered. A client that ignores list_changed still needs its own manifest refresh/reconnection. Previously running adapter processes must load the new binary once.
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
