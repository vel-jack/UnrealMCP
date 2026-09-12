# UnrealMCP Development Guide

## Start Here

This directory is the standalone UnrealMCP plugin Git repository. Read these files before changing code:

1. `AGENTS.md` for architecture, safety, and development rules.
2. `ROADMAP.md` for status, priorities, acceptance criteria, and pending work.
3. `README.md` for the implemented tool surface and user integration instructions.

Do not infer milestone status from old chat summaries when the roadmap provides a newer status.

## Mission

UnrealMCP lets MCP-compatible coding agents understand and safely edit Unreal Engine projects. Unreal editor logic stays inside a native editor plugin; a small external adapter provides the stable MCP stdio entry point and editor lifecycle/recovery behavior.

The default target is Unreal Engine 5.4.4 on Windows. Native plugin code uses Unreal C++20. Python is optional fallback only and is not required for normal operation.

## Architecture

```text
Codex / Claude Code / Cursor / Cline / Antigravity
                         |
                         | standard MCP over stdio
                         v
              UnrealMCP.Adapter (.NET)
                         |
                         | project-specific Windows named pipe
                         v
               UnrealMCP Editor Plugin
                         |
       Asset Registry / Blueprint API / Editor APIs
       Project Index / Worlds / Build / Diagnostics
```

### UnrealMCP Editor Plugin

The native plugin is the authoritative Unreal execution engine. It owns:

- Unreal-facing tool registration and execution
- direct UObject, Blueprint, Asset Registry, world, and editor access
- game-thread dispatch for operations that load or mutate Unreal objects
- named-pipe server transport
- project indexing and indexed graph analysis
- editor transactions, compilation, saving, and partial index refresh

Tool implementations must not depend on stdio or a specific coding client.

### UnrealMCP Adapter

`Adapter/UnrealMCP.Adapter` is the only MCP server entry configured in coding agents. It owns:

- the official MCP C# SDK stdio transport
- project discovery and selection
- Unreal Editor path/version resolution and lifecycle tools
- project-specific pipe selection, attachment, forwarding, and reconnect behavior
- structured transport/session errors while Unreal is unavailable
- client setup, `help`, `doctor`, dry-run configuration, and guarded uninstall
- a cached authoritative Unreal tool catalog so schemas remain discoverable while the editor is closed

The adapter does not reimplement Unreal editor tools. It mirrors and forwards the plugin's tool definitions and calls.

By default, client configuration points to the adapter executable in its current directory. Never copy the adapter automatically. Copying is allowed only when the user explicitly supplies `--install-dir <path>`.

## Current Priority

The active roadmap is organized around capability and context efficiency (E1-E8), not historical M1-M20 order. E1 introduces progressive adapter discovery and bounded Blueprint overviews. E2 safety hardening gates additional compound mutations. Read ROADMAP.md for current implementation and acceptance status; docs/LEGACY_ROADMAP.md retains historical evidence and unfinished tasks. Map/level editing is deferred. Preserve existing level/selection context and do not use Computer Use; hand unavoidable UI actions to the user.

The adapter now attempts an unambiguous initial project attach before tools/list, without launching Unreal. Late successful attachment of a changed catalog emits notifications/tools/list_changed and persists the catalog immediately. unreal.adapter.RefreshToolManifest provides explicit refresh. Clients must re-list after notifications; already-running old adapter processes need to load the updated binary once. No manual cache reseeding is part of this workflow.

Remove-prefixed native tools receive adapter operation IDs and non-retryable uncertain-timeout responses. RemoveInputMappingContextMapping now requires confirm=true outside dry-run. Add/remove reject ambiguous duplicate Action+Key rows. SetInputMappingContextMappingKey preserves the exact row except its key, validates a detached preview, uses one undoable transaction, and separates save failure from successful in-memory edits. Its static editor regression covers settings preservation and undo; explicit save/reload and injected mutation/save-failure coverage remain pending.

Prioritize fewer schemas, smaller complete evidence sets, fewer round trips, and reliable server-side workflows over raw tool count. Compact adapter mode exposes lifecycle plus SearchTools/GetToolSchema/CallTool; full mode remains available. Unreal semantics stay native. GetBlueprintOverview is the preferred first live asset inspection, with bounded members and no pin dump. Preserve mutation/error/replay evidence through the gateway. Enhanced Input Phase 5A, deeper graph coverage and generic workflows remain tracked in E3/E4; deprecated input config APIs remain excluded. Never hardcode host-project asset names into generic tools.

## Tool Design

Expose high-level deterministic operations rather than raw Unreal APIs.

Every tool must provide:

- verb-first stable name
- clear description and JSON input schema
- structured success and error payloads
- bounded output with coverage/truncation metadata when applicable
- idempotent behavior or an explicit idempotency key for mutations
- dry-run for mutations when meaningful
- stable asset, graph, node, and pin identities
- explicit compile, save, and index-refresh controls

Never return plain `Done` text when structured evidence is available.

Tool implementations belong in separate header/source files under `Public/Tools` and `Private/Tools`. Shared Blueprint editing behavior belongs in the `Tools/BlueprintEditToolUtils`, `Tools/BlueprintGraphEditToolUtils`, and `Tools/BlueprintToolUtils` modules rather than duplicated across tools. `Public/Blueprint`/`Private/Blueprint` hold Blueprint-specific K2Node subclass implementations, not shared editing utilities.

## Blueprint Mutation Safety

- Resolve and inspect the target asset before editing.
- Execute UObject loading and mutation on the Unreal game thread.
- Use editor transactions for all Blueprint mutations.
- Preflight graph, node, pin, type, link, and expected-revision constraints before mutation.
- Never silently replace occupied data pins, linked nodes, assets, or existing implementation paths.
- Roll back the complete bounded patch when any operation or compilation fails.
- Compile before saving. Compilation failure must leave the asset unsaved and the patch rolled back.
- Save and partially refresh the index only after successful compilation and only when requested.
- Return final node GUIDs, pin IDs, changed links, compile status, save status, and index evidence.
- Never compile all Blueprints or build the C++ project implicitly from an edit tool.
- Destructive operations require reference analysis and explicit confirmation.

Cold-load timeouts must not leave mutation state ambiguous. A timed-out mutation needs an operation ID and queryable status; agents must not blindly retry an unknown mutation.

## Project Index Policy

The SQLite project index lives under the host project's `Saved/UnrealMCP` directory and is never source-controlled.

- Keep schema version `1` during development unless the user explicitly changes this policy.
- Do not add legacy schema migration compatibility during active development.
- When schema structure changes, close Unreal if required, delete the old SQLite database, and rebuild a fresh index.
- Full rebuilds are expensive and manual by default. Agents should not repeatedly trigger them.
- Prefer partial refresh for touched assets after successful saved edits.
- The user normally performs a full manual rebuild when requested; once per day is generally sufficient unless a schema change requires a fresh database.
- Index tools should distinguish project content from engine/plugin content and report scope/counts clearly.

## Runtime Interaction Policy

The user owns:

- PIE and multiplayer playtesting
- mouse/touch clicking and dragging
- camera behavior and interaction feel
- visual quality and final acceptance

UnrealMCP owns static inspection, indexed tracing, safe editor mutation, compilation, bounded diagnostics, and explicitly requested deterministic tests. Do not use generic desktop automation as a substitute for project-specific runtime validation.

Do not use Computer Use unless the user explicitly requests it. If a required manual editor action cannot be performed through UnrealMCP, stop and tell the user the exact action to perform.

## Development Workflow

1. Inspect the relevant roadmap section and existing implementation.
2. Search for existing utilities and tests before adding new abstractions.
3. Plan the smallest generic capability that satisfies the acceptance case.
4. Implement native plugin and adapter changes in their proper layers.
5. Build after C++ or C# source changes.
6. Launch or attach to the explicitly authorized host through the adapter when live Unreal verification is required. For this iteration use `work_dsm/AR_GIS_MAP.uproject`; do not synchronize another checkout.
7. Run focused editor automation tests and live MCP calls.
8. Gracefully close only the test editor when rebuilding locked plugin binaries.
9. Update `README.md` for user-facing behavior and `ROADMAP.md` for status/remaining work.
10. Report verification, residual risks, and any manual test required from the user.

When independent work can be safely parallelized, subagents may be used for bounded, disjoint tasks. Close them after their work is integrated.

## Build And Verification

### Adapter

```powershell
dotnet build .\Adapter\UnrealMCP.Adapter\UnrealMCP.Adapter.csproj -c Release
.\Adapter\UnrealMCP.Adapter\bin\Release\net9.0-windows\UnrealMCP.Adapter.exe help
.\Adapter\UnrealMCP.Adapter\bin\Release\net9.0-windows\UnrealMCP.Adapter.exe doctor --json
```

The adapter must keep stdout protocol-only in `serve` mode; logs belong on stderr. Standard stdio uses newline-delimited JSON-RPC through the official MCP SDK.

### Native Plugin

Use the Unreal 5.4 editor target for the authorized host and run the focused `UnrealMCP.*` automation tests relevant to the changed tool. For `work_dsm`, run from the host root:

```powershell
& "C:\Program Files\Epic Games\UE_5.4\Engine\Build\BatchFiles\Build.bat" AR_GIS_MAPEditor Win64 Development -Project="$PWD\AR_GIS_MAP.uproject" -WaitMutex -NoHotReloadFromIDE
```

Use the unreal-rebuild-reopen skill for canonical DLL changes. Adapter shutdown must not save unrelated dirty packages; it may require the user to resolve Unreal's prompt. Do not claim a native change is verified from C# build success alone.

After a tool-schema change, connect to a live test editor and refresh the adapter's authoritative tool catalog. Public release artifacts must include a freshly captured catalog.

## Source And File Rules

- Follow Unreal coding conventions and use `TArray`, `TMap`, `TSet`, `TOptional`, `FString`, `FName`, and `FGuid` appropriately.
- Avoid raw `new`/`delete`; use Unreal/shared ownership patterns.
- Prefer Asset Registry metadata and indexed queries over loading every asset.
- Never iterate all UObjects when a registry/index query can answer the question.
- Keep transport code isolated from tool behavior.
- Keep source files focused; split files when responsibilities can be separated cleanly.
- Add comments only for non-obvious invariants, transaction boundaries, or Unreal-specific behavior.
- Preserve unrelated user changes in dirty worktrees.

## Repository And Synchronization Rules

- This directory has its own Git repository and is the canonical public plugin source.
- The surrounding `UE_544_MCP` repository is a local test project and intentionally ignores this nested repository.
- Do not commit or push unless the user explicitly requests it. Never push automatically.
- Before proposing a commit message, inspect both tracked and untracked plugin changes.
- Copies of UnrealMCP may exist in AXIS/DSM projects for integration testing. Do not modify or synchronize those copies unless the user explicitly requests it.
- When synchronization is requested, first confirm the source-of-truth direction and preserve project-specific files such as local update scripts.

## Error Handling And Performance

- Never allow an MCP request to crash or freeze Unreal Editor.
- Return machine-usable `errorCode`, message, session state, retry safety, and recommended action.
- Distinguish editor process loss, pipe loss, plugin unavailability, timeout, and a still-running mutation.
- Avoid infinite retries. At most one immediate reconnect is appropriate for a transient transport failure.
- Long operations need progress/state reporting and bounded results.
- Prefer overview-first and server-side filtered responses to reduce latency and token usage.

## Handoff Checklist

Before ending a substantial development task:

- update status and pending acceptance criteria in `ROADMAP.md`
- update user-facing commands/tool behavior in `README.md`
- record what was built and live-tested
- state whether Unreal Editor is currently open or closed
- state whether an index rebuild or schema reset is required
- list uncommitted files or provide a commit message when requested
- identify the exact next milestone task

### Current Development Handoff (September 12, 2026, later session)

- E2 started. Two of its reliability items are implemented and live-verified: the `RefreshBlueprintCallSites` transaction/evidence fix and the `SaveValidatedBlueprints` recovery-metadata fix. See ROADMAP.md's E2 section for exactly what each now guarantees.
- `RefreshBlueprintCallSites` previously called `GEditor->UndoTransaction()` while its `FScopedTransaction` was still open, so the advertised rollback could never restore anything; the transaction now closes in its own scope first, matching `ApplyBlueprintGraphPatchTool`. It also no longer returns `success: true` after silently skipping call sites it could not resolve.
- Verification host for this session was `UE_544_MCP`, not `work_dsm`: that is the project this session's adapter is bound to and the checkout the work was done in. `UE_544_MCPEditor Win64 Development` built clean (both edited files compiled, zero warnings) and every check ran through the compact adapter gateway against a real attached Editor.
- Live evidence: stale-index call site correctly fails the asset with `call_site_refresh_incomplete`; dry-run and real-run happy paths reconstruct/compile with measured dirty state; `continueOnFailure=true` with two failures reports both; a clean asset is reported as `skippedCleanObjectPaths`, no longer as unattempted-and-dirty.
- `WireSelectionWorkflow` and `SaveAllDirtyPackages` are fixed too. The workflow is explicitly resumable, not atomic: it composes independently mutating sub-tools, so on failure it returns the per-function record in the error payload rather than pretending it can roll them back as one unit. `FMCPToolBase::BuildError` gained a `Data` overload for exactly this; prefer it in any tool that can fail after changing something.
- Unreal log now shows agent actions, not pipe plumbing. `FMCPServer::HandleJsonRequest` logs one bounded `MCP <tool> [target] -> ok (N ms)` line per real tool call at `Log`, failures at `Warning` with code and message; `initialize`/`tools/list`/`ping` and the connect/disconnect pair are `Verbose`. The adapter opens one connection per request, so at `Log` level those handshakes produced roughly five noise lines per actual call and buried the one line that mattered — do not promote them back.
- The adapter forwards the MCP client name/version in native `initialize` params (`UnrealSessionManager.SetConnectedClient`, captured from the SDK handshake in `SdkMcpServer`), and the plugin logs `MCP client attached: ...` only when that identity changes. Before this the plugin had no idea who was calling, since initialize params were empty.
- Known gap, deliberately not claimed as done: the compile-failure rollback branch has no fault-injection seam, so `graphRestored`/`dirtyRestored` are implemented but never exercised live. Building that seam is the next E2 task.
- `/Game/A_BadActor.A_BadActor` was left dirty and unsaved in the running Editor by the real-run test, by design (compile without save). The `work_dsm` checkout was not touched or synchronized.

### Previous Development Handoff (September 12, 2026)

- E1: default compact discovery, exact schema lookup, native gateway and GetBlueprintOverview. Full tool-surface compatibility remains available; no native tool names were removed.
- Adapter Release builds (isolated and normal checkout output) and 33 simulated-pipe regression checks passed. Canonical `AR_GIS_MAPEditor Win64 Development` build passed. Native `UnrealMCP.Blueprint.Overview.ReadOnly` passed through compact production stdio with zero errors/warnings.
- Measured live catalog: full 117,096 bytes; compact 4,305 bytes (96.3% smaller). UTF-8 serialization comparison, not a model-token or whole-workflow saving claim.
- Startup-state follow-up: internal pipe timeouts are distinct from caller cancellation; closed status returns immediately; accepted launches report `editor_starting` as a successful lifecycle request until ready/exit/deadline. Forty-one adapter regression checks pass, and a real closed `work_dsm` launch produced `editor_starting` with its tracked PID before a later status reached `ready`.
- Editor reopened and attached to the exact `work_dsm` project. No PIE or full index rebuild/schema change. Test fixtures are transient and unsaved; no user Blueprint compilation or asset saving was needed.
- Configured clients may still run the adapter from the separate `UE_544_MCP` checkout. That checkout is not updated automatically. Reload/configure this checkout's tested adapter to use compact mode; do not kill unrelated client processes.
- Next: E2 transaction/recovery fixes and E1 authoritative metadata/workflow benchmarks, followed by E3 query/evidence improvements and E4 declarative authoring.

### Historical Development Handoff (September 3, 2026)

- Scope: Phase 5A reliability prerequisites, formal original-tool regression, and exact mapping-key replacement. Remaining action/settings/modifier/trigger/batch scope is tracked in ROADMAP.md.
- Verification: native Development and isolated adapter Release builds passed; 17 adapter regression checks passed; EnhancedInput.AssetAuthoring.Live passed four consecutive final runs through the adapter, and MutationRequestReplay passed. Fresh live tools/list discovered the new native tool without an earlier attach. Full-suite, save/reload/fault-injection, and runtime acceptance were not run.
- Unsaved input test fixtures are unregistered and moved to the transient package without forced GC. Do not apply this cleanup strategy to compiled Blueprint fixtures without a separate lifetime review.
- Configured adapter processes had the executable locked; the latest verified build is in `%LocalAppData%/Temp/UnrealMCPAdapterRegression`. Normal adapter rebuild/restart is needed to deploy this source to already-connected clients. No client configuration or another project's plugin was changed.
- No index schema change or full rebuild is required. No Computer Use, PIE, or gameplay testing was performed. No commit/push was made.
- The UE_544_MCP test Editor was gracefully closed through the adapter at handoff; process absence was confirmed. All source/document changes remain uncommitted.
