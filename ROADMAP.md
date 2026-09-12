# UnrealMCP: Capability and Context Efficiency Roadmap

Updated September 12, 2026. This is the authoritative development order. The [legacy milestone record](docs/LEGACY_ROADMAP.md) preserves previous tasks and acceptance evidence; historical numbering is a cross-reference, not a dependency order.

## Mission and boundary

Make the native UnrealMCP plugin and its MCP adapter an effective development system for any coding agent: discover a capability with little context, obtain exactly the evidence needed, execute a complete bounded workflow, and verify the result without reconstructing Unreal behavior in the model.

The plugin owns Unreal semantics, reflection, asset/index access, preflight, transactions, validation and evidence. The adapter owns MCP transport, progressive capability discovery, sessions, recovery and protocol compatibility. The adapter must never approximate an Unreal edit with its own implementation.

Map/level editing, actor placement, landscapes, level composition and camera/scene authoring are deferred. Existing level inspection remains available. PIE, mouse/touch interaction, gameplay feel and visual acceptance remain user-controlled. Computer Use is prohibited for this iteration; an unavoidable UI step is handed to the user.

Target: Unreal Engine 5.4.4 on Windows, native C++20, .NET adapter using the official MCP SDK. This implementation uses `work_dsm` as the explicit build/test host. No other plugin checkout is automatically synchronized.

## What already exists

- Legacy M1-M7: plugin/runtime, project and asset discovery, Blueprint reading, SQLite index, feature relationships and read-only world context.
- Legacy M8: indexed graph extraction, bounded cross-graph traces with exact/inferred/unresolved boundaries; live embedded Blueprint inspection implemented. Runtime callback proof and exhaustive downstream acceptance are incomplete.
- Legacy M9: Blueprint/component/function/interface/delegate/collection authoring, declarative graph patches, exec splicing, mutation IDs/status/replay, call-site reconstruction and ordered saves. Several compound failure paths still require hardening.
- Legacy M9 Phase 5: Input Action/IMC creation, mapping add/remove/read and exact key replacement. Phase 5A remains partial.
- Legacy M10: static indexed architecture analysis and non-destructive refactor plans.
- Adapter: discovery, selection, attach/reconnect/launch/shutdown, cached native catalog, catalog-change notifications, client setup and protocol doctor.
- Tests: focused native automation plus simulated-pipe MCP integration tests. Historical passes are not proof of current full-suite acceptance.

## Execution order

| Priority | Milestone | Result | Status |
|---|---|---|---|
| 1 | E1: Progressive discovery and compact evidence | Small initial tool surface; one-call Blueprint overview | Initial slice implemented and verified; extensions queued |
| 2 | E2: Reliable compound operations | Trustworthy rollback, save, retry and partial-failure evidence | Shutdown saving fixed in E1; remaining work pending |
| 3 | E3: Query engine and reusable evidence | Server-side semantic search, bounded traces, snapshot/delta reads | Existing trace foundation; extensions pending |
| 4 | E4: Declarative asset workflows | Complete useful edits with few calls and one validation cycle | Existing graph patches; expanded workflows pending |
| 5 | E5: Diagnose/build/verify loop | Structured jobs and focused verification through MCP | Native test hooks exist; build/diagnostic jobs pending |
| 6 | E6: Reflected API and migration intelligence | Exact C++/Blueprint contracts and dependency-aware rewiring | Pending |
| 7 | E7: Broader asset systems | Data, UI and gameplay asset capabilities with the same contracts | Pending; level work excluded |
| Continuous | E8: Benchmarks, compatibility and distribution | Evidence of efficiency and correctness on realistic tasks | Initial catalog measurement in E1 |

E2 is a gate before adding compound mutation workflows. E8 is part of every milestone, not a final performance cleanup. Within a milestone, implement one end-to-end capability and its acceptance cases before expanding the tool count.

## E1: Progressive discovery and compact evidence

### Initial implementation

- [x] Default `serve` to a compact surface: lifecycle tools plus `SearchTools`, `GetToolSchema`, `CallTool` in the `unreal.adapter` namespace.
- [x] Keep `--tool-surface full` for clients/workflows that need every native schema advertised. Native tool names and direct dispatch remain supported.
- [x] Search names/descriptions on the server with bounded pages, exact counts, deterministic ordering and catalog-revision guards. Do not send full schemas in search results.
- [x] Return one exact native schema on demand, including adapter mutation operation IDs.
- [x] Gateway calls use the existing native dispatch and mutation tracker. Reject nested adapter calls and unknown tool names; preserve original error/timeout/replay fields.
- [x] Preserve offline catalog discovery with explicit catalog-only coverage and no claim that a cached tool is currently executable.
- [x] Add `GetBlueprintOverview`: explicit Blueprint asset selector, parent and compile/dirty state, paginated graph/variable/component/interface inventory, graph/member identities and revisions. No pin dumps, implicit compilation, saving or index refresh. Level scripts excluded.
- [x] Introduce concise server instructions describing discovery, overview-first reads, declarative edits, provenance and timeout recovery.
- [x] Stop automatic dirty-package saving during adapter shutdown. Saving is a separate explicit operation; Unreal may request user interaction.
- [x] Distinguish lifecycle startup from native request timeouts. Closed status returns `unreal_not_running` without probing the pipe; accepted launches return non-error `editor_starting` with process/timing evidence; readiness, process exit and expired startup transition to distinct terminal states. Reserve `unreal_request_timeout` for a native read sent after attachment.

### Acceptance

- [x] Run the legacy adapter checks and new compact-surface tests: warm/cold/late catalog, ambiguity, pagination/revision changes, exact schemas, unknown/recursive calls, missing arguments, offline execution, uncertain mutations.
- [x] Canonical native Development build and native `UnrealMCP.Blueprint.Overview.ReadOnly` regression through the compact adapter gateway. Check read-only state, ordering/filtering, truncation, invalid bounds and level exclusion.
- [x] Measure serialized UTF-8 `tools/list` bytes against the full surface on the same live catalog: 117,096 full, 4,305 compact, 96.3% smaller. Exceeds the initial 75% target. These are bytes, not model-token measurements.
- [x] Verify discovery -> schema -> call -> native evidence through the production stdio adapter with no desktop automation.
- [x] Refresh the live authoritative catalog and record the tested adapter binary and editor state in AGENTS.md and the E1 verification record.
- [x] Cover closed, starting, ready-after-start, exited-during-start, startup-deadline and established-request-timeout states in adapter regression tests; verify `editor_starting -> ready` against the real `work_dsm` Editor.

### Next efficiency extensions

- [ ] Define authoritative tool metadata for read-only/destructive/confirmation/cost/availability instead of relying on verb-prefix heuristics. Carry MCP annotations and constraints through both surfaces.
- [ ] Rank by capability family and workflow intent, with deterministic aliases and task recipes. Do not require model-generated tool routing or introduce a second LLM.
- [ ] Measure full workflow context, including discovery/schema overhead, failures and repeated calls. Consider selected-schema activation for clients that demand typed tools before execution.
- [ ] Per-tool overview/detail profiles and consistent coverage semantics. Do not truncate safety fields or substitute an empty result for an omitted section.

## E2: Reliable compound operations

Resolve the review findings before expanding authoring power:

- [x] End `RefreshBlueprintCallSites` transactions before rollback; make aggregate success reflect caller failures and prove restored graph/dirty state. The reconstruction transaction now closes in its own scope before any `UndoTransaction()` (matching `ApplyBlueprintGraphPatchTool`), unresolvable indexed call sites fail the asset instead of being skipped silently, aggregate `success` is derived from per-asset outcomes, and the rollback path re-resolves each node to compare pin shape and restores/measures the package dirty flag rather than inferring it. The compile-failure rollback branch itself still needs injected-failure coverage.
- [x] Make `WireSelectionWorkflow` atomic or explicitly resumable with structural verification. Existing incomplete functions must not count as successful scaffolding. It is explicitly resumable rather than atomic, because it composes several independently mutating sub-tools and cannot roll them back as one unit: a mid-sequence failure now returns the full per-function record in the error payload (`created`, `attempted`, per-function error, `changed`, `atomic:false` and reconciliation guidance) instead of a bare error string, and an existing function graph is accepted as scaffolded only after its node count is checked against the minimum this workflow actually builds.
- [x] Build `SaveValidatedBlueprints` recovery metadata from actual per-asset outcomes, including multiple failures with `continueOnFailure=true`. The plan is now derived from a recorded per-asset outcome instead of list position: every failure appears in `failedObjectPaths` (not only the last), `notAttemptedObjectPaths` holds only assets the run never reached, and assets that were clean are reported separately in `skippedCleanObjectPaths` rather than being described as dirty and unsaved.
- [x] Make `SaveAllDirtyPackages` report partial failure accurately; retain explicit user-scoped saving as the normal workflow. `success` is no longer hardcoded true: it now reflects failed packages and packages that reported a successful save but are still dirty (`stillDirtyPackages`), each failure carries a filename and an actionable reason such as a read-only target, and a partial result returns `partial_save_failure` so an agent does not close the editor believing everything reached disk. `SaveValidatedBlueprints` remains the normal explicit user-scoped workflow.
- [~] Review every exposed mutation for confirmation, native validation, idempotency, compile-before-save and game-thread loading. Prefix classification alone is insufficient. Four of the five dimensions are audited across all 119 tools. Game-thread loading is clean: every load sits inside `ExecuteOnGameThreadSync`, including the three helpers whose definitions precede their guarded call sites. `SpliceBlueprintExecFlow` gained the `confirm` gate its siblings already had, because it breaks an existing execution link despite a non-destructive-sounding name. `SaveBlueprint` now refuses by default to write a Blueprint whose compile is not up to date, returns structured `blueprint_compile_not_up_to_date` recovery evidence, and measures `isDirty` instead of asserting it. `UnrealMCP.Blueprint.Authoring.SaveBlueprint.Live` covers the guarded refusal, explicit override, actual temporary save, measured dirty state, schema and cleanup. The adapter no longer classifies mutations by prefix alone, so `BuildProjectIndex` (rewrites the whole on-disk index) and `ValidateBlueprint` (compiles the Blueprint it inspects) are tracked and receive an operationId; simulated-pipe regressions cover classification, schema injection, forwarded IDs and uncertain timeouts for both. Idempotency has not been systematically audited and is the remaining part of this item.
- [ ] Test disconnection during mutation, terminal replay, changed-argument rejection, session restart/expired IDs and rollback failure. Expose uncertain outcomes without an automatic retry. Partially done: `UnrealMCP.Blueprint.Authoring.CallSiteRollback` covers a genuine post-reconstruction compile failure and proves the rollback restored both graph shape and dirty state; the remaining disconnection/replay/expired-ID cases are still open.
- [ ] Guard lifecycle targeting by exact project identity, including two projects sharing a name; avoid window-title-only selection.

Acceptance: injected failures leave no unexplained partial state; no failed save is presented as rollback; no already-saved asset is labelled unattempted. No implicit global compile/save or unrelated asset changes.

## E3: Query engine and reusable evidence

Move reasoning over graphs and relationships to deterministic native queries while preserving uncertainty:

- [ ] Unified indexed node/member search by owner, function, variable read/write, interface, dispatcher, class and dependency scope (legacy M8/M10 extensions).
- [ ] `CompareBlueprints` and impact analysis returning changed members, callers, dependencies and affected tests without full graph dumps.
- [ ] Consistent `overview`, `trace`, `detail` and compact graph representation; stable identifiers, scoped counts, omitted sections, frontier and pagination.
- [ ] Snapshot/revision-bound pagination for live graphs. Reject stale continuation rather than mixing revisions.
- [ ] Index freshness on every indexed answer: build/refresh time, dirty scope and live confirmation guidance. Keep live and indexed provenance distinct.
- [ ] Session-scoped evidence handles with TTL/size bounds, explicit project/revision identity and stale-handle errors. Reuse prior results and request deltas without resending unchanged graphs.
- [ ] Read-only bounded multi-query composition. Return per-query outcomes and execution limits; never call mutations from a read batch.
- [ ] Better latent/timer/timeline/delegate semantic boundaries and richer function metadata. Runtime callbacks remain unresolved until appropriate user-authorized evidence exists.
- [ ] Avoid loading unrelated assets; reuse the existing index, support partial refresh, and profile expensive graph-revision computation.

Acceptance: large-project queries return bounded evidence, explain zero results and incomplete coverage, and materially reduce bytes and round trips against the equivalent primitive workflow. Index schema changes require their own explicit rebuild plan.

## E4: Declarative asset workflows

Build on `ApplyBlueprintGraphPatch`, `ApplyBlueprintInteractionPlan`, exec splicing and existing component batches:

- [ ] Expand graph patch node/type coverage using existing native primitives; retain deterministic identities and exact-link conflict checks.
- [ ] Add reusable plans for function/interface/delegate/component authoring and cross-Blueprint call-site updates with one validation phase per touched asset.
- [ ] Preflight dependency-ordered multi-asset changes. Explicitly distinguish in-memory transactions from non-atomic disk saves; never promise cross-asset disk rollback.
- [ ] Complete Enhanced Input Phase 5A in order: Input Action property inspection/editing; bounded key/class/settings discovery; modifier/trigger instances; player-mappable metadata; single-asset batches.
- [ ] Prove inline object ownership, settings preservation, modifier order, trigger references, dry-run and save/reload/failure behavior before claiming Phase 5A complete.
- [ ] Resume generic selection branch/group-transform graph workflows only when needed by an asset-authoring acceptance case; this does not authorize level actor editing.
- [ ] Publish machine-readable preflight plans with exact required inputs, conflicts, affected assets, expected revisions and verification steps.

Acceptance: compare a real workflow against the primitive-call baseline, with fewer calls, one compile per touched Blueprint and exact post-edit evidence. Dry-run failure changes nothing. Existing user logic is preserved unless exact replacement is requested.

## E5: Diagnose, build and verify through MCP

- [ ] Cursor-based log reads, severity/category/time filters, repeated-message grouping and structured Unreal/compiler diagnostics (legacy M11).
- [ ] Explicit build jobs with project/engine/target/configuration, status IDs, bounded incremental output, artifacts and safe cancellation (legacy M12). Never implicitly build from an edit tool.
- [ ] Canonical close/build/reopen/attach workflow with dirty-work prompts and exact project matching. New tool registration requires module reload, not an assumption that hot reload reruns registration.
- [ ] Focused automation discovery/run/batch using existing hooks, machine-readable reports and diagnostic grouping (legacy M15).
- [ ] Verification recipes: inspect -> plan -> edit -> compile/validate -> explicit save -> partial refresh -> confirm changed evidence.
- [ ] Bounded cold-asset preloading for exact selected targets and measured request deadlines.

Acceptance: an agent can diagnose a failed build/edit, apply an authorized correction and retrieve only new relevant evidence. Tests never invoke PIE implicitly.

## E6: Reflected API and migration intelligence

- [ ] `InspectCppBlueprintAPI` for exact compiled UClass/UFunction/UProperty signatures, flags and Blueprint-callability; prioritize this before automated migration.
- [ ] Migration candidate/ownership/dependency analysis using the existing architecture engine.
- [ ] Exact Blueprint-node-to-reflected-API mappings with unsupported cases and wrapper requirements.
- [ ] Dependency-ordered rewiring and caller validation using E2/E4 guarantees.
- [ ] Structural equivalence reports plus explicit runtime acceptance requirements; static evidence alone is not behavior equivalence.

The coding agent edits C++ source through normal source tools; native UnrealMCP verifies compiled reflection and Blueprint integration. Legacy M14 supplies historical detail.

## E7: Broader asset systems

Prioritize reusable asset capabilities by measured user value, using the same discovery/evidence/transaction contracts:

- [ ] Data Assets, Data Tables, structs/enums and gameplay tags.
- [ ] Widget Blueprint structure/bindings and UI asset inspection/authoring.
- [ ] Materials/material instances, texture and mesh configuration with bounded asset-health summaries.
- [ ] Animation assets, Niagara, Gameplay Ability System, Control Rig and MetaSound as separately scoped extensions.
- [ ] Source-control inspection/diff/check-out integration where ordinary source tools cannot inspect Unreal assets; submission/revert remain explicit actions.

Sequencer scene/camera authoring, landscape and map/level edits are deferred. Python fallback, remote/multi-user sessions and distributed builds remain future design work rather than prerequisites.

## E8: Continuous efficiency and release gates

For each capability, retain a reproducible baseline and upgraded run on the same fixtures:

- Initial schema bytes, schema retrieval bytes, result bytes, call count, elapsed time and failure/recovery calls.
- Actual client/model tokenizer measurements when available; otherwise label byte counts or token estimates accurately. Smaller catalogs alone do not prove an equal percentage saving on full tasks.
- Correctness: exact identities, provenance, freshness, coverage, stale guards, dry-run/rollback/save and retry safety.
- Stress cases: large graphs, cold/unindexed assets, ambiguous targets, engine/plugin content, late Editor availability and repeated queries.
- Client compatibility: compact and full surfaces, MCP Inspector, clean client installation and notifications. No unmeasured claim of universal client validation.
- Ship a freshly captured native catalog, tested binaries, versioned contracts and installation/troubleshooting instructions. Preserve deferred public-release/documentation tasks from legacy M20 and the development task.

Proposed targets after E1: at least 50% fewer tool round trips for chosen compound workflows and at least 50% fewer serialized response bytes for overview-led inspection, with equal required evidence. Establish fixtures and baseline before marking either target achieved.

### Public distribution documentation

- [ ] Add a concise quick start for cloning into a project's `Plugins/UnrealMCP` directory, enabling the plugin and building the Editor target.
- [ ] Document supported Unreal Engine, Windows and .NET versions, separating build prerequisites from runtime prerequisites.
- [ ] Document the adapter Release build, client setup, project selection, connection checks and first safe read-only call.
- [ ] Add upgrade, adapter restart, catalog refresh, uninstall and clean-removal instructions.
- [ ] Explain source-only versus prebuilt adapter packages, excluded Unreal Engine components and the separate Unreal Engine license requirement.
- [ ] Publish a supported-version matrix, beta limitations, mutation/save safety notes, troubleshooting and a minimal verification checklist.
- [ ] Keep public examples project-agnostic and free of machine paths, private asset names and host-specific verification evidence.

## Current handoff

E1's initial slice and roadmap reorganization are implemented and verified. See [E1 verification](docs/E1_VERIFICATION.md) for measurements, reproduction and limits. No index schema change is part of E1.

E2 is in progress. All four of its per-tool reliability fixes are implemented and live-verified against a real attached Editor through the compact adapter gateway, not from a C++ build alone. Live coverage so far: stale-index call sites fail the asset with an actionable error instead of returning success with an empty result; `RefreshBlueprintCallSites` dry-run and real-run happy paths reconstruct, compile and report measured dirty state; multi-failure `continueOnFailure=true` reports every failure and no longer labels a clean or already-saved asset as unattempted; `WireSelectionWorkflow` refuses a two-node leftover function as incomplete while still passing an idempotent re-run of four fully scaffolded functions.

Also landed alongside E2: the Unreal log reports agent actions instead of pipe plumbing. Because the adapter opens one connection per request, the old log emitted a connect/disconnect pair plus repeated `initialize`/`tools/list` handshakes for every single call, none of which said what was being asked. Now one bounded line per real tool call is logged at `Log` (`MCP GetBlueprintOverview [objectPath=/Game/A_BadActor.A_BadActor] -> ok (94 ms)`), failures log at `Warning` with the error code and message, and the handshakes and connect/disconnect pair moved to `Verbose`. A connection that sends no request at all still logs once, since nothing else would record it. The adapter now forwards the MCP client's name and version in the native `initialize` params, and the plugin logs `MCP client attached: <name> <version> via <adapter>` only when that identity changes, so the caller is named once rather than on every reconnect.

The compile-failure rollback branch is now covered by `UnrealMCP.Blueprint.Authoring.CallSiteRollback`, and `graphRestored`/`dirtyRestored` are proven rather than only code-reviewed. The failure is genuine, not injected: the fixture removes a function parameter while a caller still has that pin linked, so reconstruction orphans the pin and the caller legitimately fails to compile. No test-only field was added to the public schema. To make this testable the reconstruction/compile/rollback engine moved to `RefreshBlueprintCallSitesEngine.{h,cpp}`, leaving the tool as the index query over it — necessary because the SQLite index is built from `AssetRegistry.GetAllAssets` and never sees the transient packages automation fixtures create. The previous full suite passed 20 of 20 with zero errors. The new save regression brings the registered total to 21; final focused runs of save, call-site rollback and exec splice each passed with zero errors and warnings through the compact adapter gateway. The full 21-test suite was not rerun.

Final verification for this follow-up: the canonical `UE_544_MCPEditor Win64 Development` build passed; an isolated adapter Release build passed with zero errors/warnings; all 49 simulated-pipe adapter checks passed, including both explicit non-prefix mutations. The normal checkout adapter executable was locked by active MCP clients, so those processes must restart before the normal Release output can be replaced. The rebuilt Editor was reopened and attached to `UE_544_MCP`; no PIE or index schema rebuild was used.

Not yet covered, and required before E2 can be called complete: the mutation review across every exposed tool, and the disconnection/replay/expired-ID/rollback-failure tests listed above.
