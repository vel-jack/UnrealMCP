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

Milestone 9 Phase 5, Enhanced Input Asset Authoring, is **complete and live-verified** (September 3, 2026). `CreateInputAction`, `CreateInputMappingContext`, `AddInputMappingContextMapping`, `RemoveInputMappingContextMapping`, and `GetInputMappingContextMappings` were verified end to end through the adapter against real `/Game/UnrealMCP_Test/` fixtures — dry-run/real creation, idempotent add, exact read-back, remove-then-verify-empty, correct zero-match rejection on a second remove, and correct rejection of an invalid `FKey` name. See `ROADMAP.md` Milestone 9 Status for full detail. A formal `UnrealMCP.*` automation test for this phase is still outstanding.

That verification pass surfaced a real adapter bug, now the top priority: `UnrealSessionManager.GetMirroredToolListAsync` (`Adapter/UnrealMCP.Adapter/UnrealSessionManager.cs`) only attempts a live tool-catalog refresh when `_activeProject` is already set, but `_activeProject` is only ever set by an in-session attach call, which necessarily happens *after* the MCP client's one-and-only `tools/list` handshake at session start. A newly registered native tool therefore can never reach a connected agent's tool manifest through normal usage, no matter how many times the session restarts — confirmed live across two independent agent sessions. The only workaround found was manually overwriting the adapter's on-disk cache (`%LocalAppData%\UnrealMCP\cache\unreal-tools.json`) with a fresh `ListTools` result from an attached session, which is not a real fix. Fix this properly: either have the adapter eagerly auto-attach to the single unambiguous discoverable project before answering its first `tools/list`, or add an MCP `notifications/tools/list_changed` push plus an in-session "refresh tool manifest" action once any attach succeeds. See Milestone 20's Universal MCP Distribution checklist.

After that, resume Milestone 9 Phase 4C's two remaining bullets (Phase 4A, 4B, 4B.1, 4B.2, and Phase 4C's helper-function scaffolding are already implemented per the Status Snapshot — do not re-do them):

- wire the pointer-valid/modifier-held branch into an existing project hit-test event
- the group-transform workflow (one primary target plus a bounded per-update transform delta, preserving relative offsets)

The immediate acceptance scenario spans `AC_DragShapes`, `AC_ContextMenu`, and `BP_GIS_AdvancedPawn`. Do not hardcode those assets into generic plugin tools.

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
6. Launch or attach to the `UE_544_MCP` test project through the adapter when live Unreal verification is required.
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

Use the Unreal 5.4 editor target for `UE_544_MCP` and run the focused `UnrealMCP.*` automation tests relevant to the changed tool. Do not claim a native change is verified from C# build success alone.

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

