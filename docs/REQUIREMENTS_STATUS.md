# Consolidated UnrealMCP requirements and implementation status

Updated **2026-11-22**. Reviewed baseline: **`6561ef0`**, `master`; **157 native tools**, catalog captured 2026-11-15.

This is the reconciled tracker for the requirement files and earlier tool requests. Stable `MCP-xx` IDs distinguish overlapping source IDs such as `R1`. The superseded source files were removed after their complete text was retained in the historical sections below.

## Scope and evidence

- The original 2026-11-22 consolidation was read-only: source/schema/test/handover inspection without editor or asset changes. Subsequent implementation evidence is recorded per entry and in the follow-up section below.
- The recorded UE 5.8 handover passed **50/50 native tests** and exercised basic graph capture through the adapter. This is dated evidence, not a fresh November 22 run or proof of every DSM acceptance case.
- The November 22 weapon report independently verifies local creation/typed nodes, targeted rewiring, compile counts, targeted validated saving, live node/pin inspection and explicit-window screenshots. It does not re-test local removal, DSL round-tripping, consolidation, broken casts or binary merges.
- Native tools reached through `unreal-mcp-adapter` and Epic's official MCP/DSL tools are separate implementations. A native alternative does not establish an upstream fix, and an old official failure does not establish a missing native tool.
- Historical instructions embedded in reports are context, **not current authorization** to select DSM, take a conflict side, save other work, stage, commit or push. Plugin development remains in UGitClientSample unless the user changes scope.
- [ROADMAP.md](../ROADMAP.md) retains development order and history; this document reconciles the request statuses. The [tool index](MCP-Tool-Index.md) is discovery metadata, not acceptance evidence.

## Source register

Source names below identify the original files. Links open their preserved text in this document. The new merge-readiness source is also retained in the sample project.

| Prefix | Original source | Identifiers |
|---|---|---|
| GEN | [UnrealMCP_Requirements.md](#source-gen) | R1–R14 |
| WEAPON | [UnrealMCP_WeaponMigration_Requirements.md](#source-weapon) | R1–R5; working-capability evidence dated November 22 |
| TOOL | [UnrealMCP_ToolingRequirements.md](#source-tool) | R1–R15, S1–S3 |
| MERGE | [DSM_Merge_DataTable_Gap_Report.md](#source-merge) | G1–G5; fixtures and DSM follow-ups |
| DEFAULT | [ToolRequest_BlueprintVariableDefaults.md](#source-default) | T1–T5; X1/X2/X3 below denote section 3 tracing/compact-output/shutdown subfixes |
| ASSET | [ToolRequest_AssetMoveReparent.md](#source-asset) | A1–A5; F1 below denotes its inherited-variable follow-up |
| INHERIT | [ToolRequest_InheritedMembersAndGraphEditing.md](#source-inherit) | Sections 1–5 and 6.1–6.3 |
| LOCAL | [ToolRequest_BlueprintLocalVariableNodes_DSM639.md](#source-local) | `nodes`, `visual`, `functional` name its three requested areas |
| REF | [Requirements_BlueprintRefactoring.md](#source-ref) | Q1–Q18; F1/F2/F3 denote section F reparent/retirement/save-format workflows |
| READY | [UnrealMCP_MergeReadiness_Requirements.md](#source-ready) | M1–M7; DSM review dated November 23 |
| EXTRA | Earlier cleanup and struct-value-preservation requests, recorded in [ROADMAP.md](../ROADMAP.md) and commits `a2be901`, `bb50171` | `cleanup`, `struct-values` |

## Status legend

| Status | Meaning |
|---|---|
| Implemented | Native capability exists with recorded source/test/handover evidence. Listed scope limits and project/runtime acceptance still apply. |
| Partial | A baseline or workaround exists; the complete requested contract is not delivered or proved. |
| Reported bug | Open for current-build reproduction. Source corroboration is stated where found; not marked fixed. |
| Open | No implementation of the requested dedicated capability was identified in this baseline. |
| External / unverified | Official/upstream behavior has not been re-tested against its current build. |
| Separate project work | Tooling support does not establish completion of the DSM migration/gameplay task. |

Priority retains the highest relevant source priority. An implemented P1 entry is not proposed new work. Source items may map to multiple entries when they combine native, official and safety requirements.

## Session, safety and reading

| ID | Priority | Status | Requirement, current coverage and remaining work | Sources |
|---|---|---|---|---|
| MCP-01 | P1 | Implemented | **Connection independent of tab/title/background.** Exact absolute project command-line discovery, retained attached-process handles and pre-request named-pipe owner PID validation replace title matching. Adapter regression tests cover lost-window discovery/reconnect, same-name projects, wrong-PID pipes and duplicate-launch refusal. Real sample attachment verified 2026-11-23; externally launched editor discovery requires Windows WMI inspection and fails closed when unavailable. | WEAPON:R1 GEN:R13 |
| MCP-02 | P2 | Open | **Wait for ready/exit and released file handles.** Startup status and bounded shutdown polling exist. No named WaitForUnrealReady/WaitForUnrealExit tools or package-file-handle-release guarantee. Separate from window-title detection. | TOOL:R14 REF:Q3 |
| MCP-03 | P1 | Implemented | **Explicit dirty-work shutdown policy.** The adapter defaults to fail and returns the dirty package list. Explicit discard drops changes and retains pending-shutdown recovery. Explicit save reports the native save receipt and requires a follow-up clean-state check before closing. Legacy discardDirty remains supported. UE 5.8 verified named fail, discard without a disk write, and save before exit using disposable sample assets. Adapter regressions verified partial-save refusal and pending recovery. | MERGE:G4 DEFAULT:X3 REF:Q2 |
| MCP-04 | P1 | Implemented | **Crash-safe index refresh after bulk deletion.** Fixed double-counted slow-task progress and switched MCP refresh to the UE 5.8-safe game-thread dispatcher. The live UGitClientSample regression populated SQLite with 200 connected collapsed-graph nodes, deleted and compiled them, refreshed the same asset, and verified node, pin and edge counts against current extracted rows. Fixture file and index entries were removed. Passed with zero errors and warnings. The original DSM crash was not reproduced. | REF:Q1 |
| MCP-05 | P2 | Implemented (native indexed readers) | **Freshness after edits.** Shared indexed queries refresh tracked assets and dirty resident project Blueprints before querying SQLite. TraceBlueprintFlow also requests refresh of its starting Blueprint. The UE 5.8 adapter found a newly added unsaved node with InspectBlueprintNode and followed its new exec link with TraceBlueprintFlow. A dedicated cross-Blueprint fixture remains unverified. | GEN:R14 DEFAULT:X1 |
| MCP-06 | P2 | Implemented (native live reads) | **Large Blueprint reads.** UE 5.8 automation invoked through the adapter returned all 100 graphs exactly once across five revision-bound pages. The same test rejects a stale revision after an in-memory edit. Live node pages are capped at 200. DSL and script reads require separate coverage under MCP-26. | GEN:R12 TOOL:R9 |
| MCP-07 | P1 | Implemented | **Find class/asset usages and package imports.** FindClassUsages reports graph/node/pin references, reflected defaults, owned subobjects, component templates and object-path literals. It reads saved imports even when no linker is attached. The optional scanBlueprints mode pages /Game Blueprint assets, including loaded unsaved assets, independently of the dependency index. The fast registry-candidate route remains available. Embedded Level Blueprints, plugin content and other asset types are outside the live project scan. | GEN:R2 TOOL:R7 |
| MCP-08 | P2 | Implemented | **Fast member/pin-type search.** FindNodesByPinType and FindNodesByMember scan live Blueprint graphs and page exact locations without serializing entire graphs or using the index. Pin hits include pin ID/name; function calls are node-level hits. The focused UE 5.8 regression passed. | REF:Q16 |
| MCP-09 | P2 | Implemented (native) | **Complete, accurate graph reading.** Native full inspection includes reroutes and exact pin links; compact output exists. Resolved inherited variables report the declaring property class. Macro instances report the exact body path and GUID when resolved. ReadBlueprintGraphDsl returns the exact live node and pin evidence and a revision-bound patch document for one selected graph. UE 5.8 verified collapsed-graph DSL inspection and function local declaration/default evidence. Local getter/setter evidence identifies its declaration GUID and scope. | GEN:R4 GEN:R6 TOOL:R8 |
| MCP-10 | P1 | Implemented | **Native collapsed/nested graph access.** GetAllGraphs and owning-Blueprint traversal support native reading/editing; collapsed-graph automation exists. This does not prove Epic DSL/script fixes or every arbitrary-depth/macro/function parity case. | GEN:R1 TOOL:R1 |

## Graph editing

| ID | Priority | Status | Requirement, current coverage and remaining work | Sources |
|---|---|---|---|---|
| MCP-11 | P1 | Implemented | **Local-variable CRUD/default reads.** ListBlueprintLocalVariables, AddBlueprintLocalVariable and RemoveBlueprintLocalVariable exist. Removal has source/test evidence although the latest refactor did not re-test it. DSL declaration/round-trip remains separate. | GEN:R5 TOOL:R3 |
| MCP-12 | P1 | Implemented | **Typed local getter/setter nodes and patches.** Primitives, patches and retargeting use selected-function declaration GUID/scope. Tests cover same names in different functions, fresh uncompiled Actor arrays, dry runs, idempotence and real compile-failure rollback. No owner means local-first; explicit owner selects class members. | LOCAL:nodes |
| MCP-13 | P1 | Implemented | **Inherited self-context access.** Blueprint/native/SCS ancestors work in primitives and patches; self/ancestor owners remain self context, unrelated owners external. Old blanket inherited-variable failures are superseded for the native route. | GEN:R8 TOOL:R4 REF:Q5 ASSET:F1 INHERIT:1 |
| MCP-14 | P1 | Implemented | **Broken dynamic-cast repair.** RetargetBlueprintNode needs only newClassPath, tolerates a missing old target and preserves compatible links. Not a general class-reference retargeter; dry-run link prediction remains optimistic. | TOOL:R2 |
| MCP-15 | P2 | Implemented | **Persistent member/function/event retargeting.** RetargetBlueprintMemberNode handles broken get/set/ordinary calls, pin maps, identity/layout/default/link preservation and dropped endpoints. UE 5.8 adapter verification retargeted an inherited event and compiled and saved it with its original GUID and exec link. A full editor restart confirmed the new event member, original GUID and exact execution link persisted. | REF:Q6 INHERIT:2 |
| MCP-16 | P1 | Implemented | **Parent calls and function/event overrides.** Patch parentCall, OverrideBlueprintFunction and event addParentCall exist and are tested. A legal parent implementation is required; an interface declaration alone does not supply one. Official DSL support is not established. | GEN:R6 INHERIT:3 |
| MCP-17 | P2 | Implemented | **Exact node variants.** Exact function owner/name, macroPath, self and cast(targetClassPath,pure) support atomic wiring. Generic function purity follows its signature; an arbitrary function isPure override is not supplied. | REF:Q7 INHERIT:5 |
| MCP-18 | P2 | Implemented | **Named switches.** String/Enum creation and String SetSwitchCases work, retaining renamed links and Default and reporting removals. Enum cases derive from enum metadata; arbitrary enum case edits are intentionally refused. UE 5.8 adapter verification created Switch on Name and renamed a case while retaining incoming execution, the renamed case link, and the Default link. | REF:Q10 INHERIT:4 |
| MCP-19 | P1 | Implemented (native) | **Targeted patches and exec splicing.** ApplyBlueprintGraphPatch supports confirmed spliceExec operations, runs disconnections before reconnection, and allows multiple incoming exec links where the K2 schema permits them. Existing-node anchors and exact data replacement support targeted event/function edits. Revision-bound removeNodeGuids batches node removal with rewiring in the same patch. The collapse option groups new patch nodes and derives input/output tunnel pins from their external links. UE 5.8 sample validation verified preview parity, compilation, and exact nested wiring. DSM UE 5.8 verified exact disconnected link preview, successful compile, and restoration of the removed node and link on compile rollback. Official single-event DSL insert/replace remains tracked separately under MCP-26. | REF:Q4 GEN:R7 MERGE:G5 |
| MCP-20 | P3 | Implemented | **Defaults and array wildcards in patches.** Guarded replacement and wildcard preflight support arrayAddUnique via Unreal's specialized array node. UE 5.8 adapter verification resolved wildcard pins from an Actor array during preview, applied and compiled the patch, and verified exact graph rollback after a compile failure. | MERGE:G5 |
| MCP-21 | P2 | Implemented | **Resize/style existing comment boxes.** ApplyBlueprintGraphPatch accepts width, height, RGB and bubble visibility on an existing comment GUID, preserves omitted styles and reports the preview or applied values. Styles participate in graph revision checks. Non-comment targets return an error. UE 5.8 live regression passed. | WEAPON:R2 |
| MCP-22 | P2 | Implemented | **Prune unreachable logic.** PruneUnreachableNodes previews reachable execution and upstream data, preserves structural nodes and comments, and removes the remainder in one undoable edit after revision confirmation. UE 5.8 adapter apply removed only a disconnected two-node chain while preserving reachable nodes and data dependencies. UE 5.8 editor Undo restored the exact original graph revision. | REF:Q8 |
| MCP-23 | P2 | Implemented | **One-operation validity guard.** WrapWithIsValid inserts a branch fed by IsValid, splices one incoming execution link, and optionally routes invalid execution to another input. Uses the atomic graph patch engine. UE 5.8 adapter apply verified the valid and invalid links against the live graph. UE 5.8 editor Undo restored the exact original graph revision. | REF:Q9 |
| MCP-24 | P3 | Implemented | **Whole-graph arrangement.** Omit nodeGuids in LayoutBlueprintNodes to arrange the whole graph, capped at 2,048 nodes. Verified a dry run through the UE 5.8 adapter. WriteBlueprintGraphDsl supports arrangeAfterWrite as a separate undo step after a successful apply. UE 5.8 verified dry-run preservation, layout of every node, and the final graph revision. | GEN:R11 TOOL:R6 |
| MCP-25 | P3 | Implemented | **Standalone Refresh All Nodes.** RefreshBlueprintNodes invokes Unreal Editor refresh inside an undo transaction, requires explicit confirmation, and does not save. Live execution verified through the UE 5.8 adapter on a disposable Blueprint. UE 5.8 editor Undo restored the exact original graph revision. | REF:Q18 |

## DSL and interoperability

| ID | Priority | Status | Requirement, current coverage and remaining work | Sources |
|---|---|---|---|---|
| MCP-26 | P2 | Implemented (UE 5.8 native) | **Native Blueprint DSL support.** Implement reader/writer tooling in UnrealMCP and reuse its native graph patch engine for targeted writes. The native patch engine supports MakeArray positional inputs and unambiguous pin display names. A three-input array was created and wired through the UE 5.8 adapter. ReadBlueprintGraphDsl and WriteBlueprintGraphDsl implement the unrealmcp.graph-patch/1 JSON document and atomic targeted edits. UE 5.8 verified read, unchanged preview, targeted apply, reread and stale revision refusal. UE 5.8 also verified a local declaration and getter in one transaction, an explicit interface message, string equality with operator ==, rejection of inline expressions, and a collapsed-graph read/replay. Portable export supports branches, reroutes, self nodes, variable nodes and their locals, function calls, interface messages, parent calls, macros, and MakeArray and reports external links for explicit rebinding. UE 5.8 verified a branch fragment compiled in a second Blueprint with exact internal and boundary links, its authored default, and stale-document refusal. UE 5.8 also verified a portable integer local and getter compiled in a second Blueprint and retained the declaration default. A full editor restart preserved the imported local declaration and exact getter/setter link. Additional imports compiled with MakeArray, a function call, an interface message, a standard macro, a typed reroute, and a parent call. Unsupported node classes are rejected explicitly. Parent calls require an inherited implementation. UE 5.4.x verification remains on hold. | GEN:R1 GEN:R4 GEN:R5 GEN:R6 GEN:R7 TOOL:R1 TOOL:R3 TOOL:R5 TOOL:R8 |
| MCP-27 | P2 | Partial | **Consistent arguments and identities across servers.** Compact SearchTools/GetToolSchema/CallTool and native GUID/path results are implemented. No common argument-name normalization or universal GUID/refPath acceptance. Exact owning-server schemas remain authoritative; deployed contracts need compatibility planning before renaming. | GEN:R9 TOOL:R10 |

## Defaults and data assets

| ID | Priority | Status | Requirement, current coverage and remaining work | Sources |
|---|---|---|---|---|
| MCP-28 | P1 | Implemented | **Safe class-default reads.** GetBlueprintClassDefaults reads existing CDO values, parent values, declaring class/type/editability and overrides with paging, without construction-script execution. This is a native alternative; the official list_properties/get_properties failure was not fixed or re-tested here. | GEN:R10 DEFAULT:T1 |
| MCP-29 | P1 | Implemented | **Set/reset own and inherited defaults.** SetBlueprintClassDefaults and ResetBlueprintClassDefaults support typed export text, dirty guards, transactions, confirmation, compile/save and before/after values. Recorded sample/save-reload evidence exists. Actual DSM ModelScale/trajectory acceptance remains project work. | DEFAULT:T2 DEFAULT:T3 |
| MCP-30 | P1 | Implemented | **Containers and local defaults.** Reflected import/export supports arrays, sets, maps, enums, structs and object/class references. Read locals through ListBlueprintLocalVariables; write through SetBlueprintLocalVariableDefault using `defaultValue`, not proposed `exportText`. Exact sparse DSM enum/map entries are not newly verified. | DEFAULT:T4 MERGE:G2 REF:Q12 |
| MCP-31 | P3 | Implemented | **Defaults comparison.** CompareBlueprintClassDefaults matches properties by name, reports values/types and one-side-only properties. Scope is editable/Blueprint-visible properties, not every reflected implementation field. | DEFAULT:T5 |
| MCP-32 | P3 | Implemented | **Declaring-class filter.** GetBlueprintClassDefaults(declaredInOnly=classPath) filters exact authoritative ownership before pagination. | INHERIT:6.3 |
| MCP-33 | P1 | Implemented | **DataTable rows.** InspectDataTableRows, SetDataTableRowFields, AddDataTableRow and RemoveDataTableRow provide authored field-name resolution and transactional edits. SaveDataTable is explicit. Earlier proposed GetDataTableRows/SetDataTableRowField names are not the shipped API. | MERGE:G1 |
| MCP-34 | P1 | Implemented (native) | **Struct/enum schema editing and dependent safety.** Native tools inspect, add, remove, rename and change struct defaults, and inspect, append and relabel enum entries. Referenced edits require explicit approval. Optional dependent Blueprint compilation returns before/after pin locations and compiler diagnostics; only the edited schema can be saved. UE 5.8 tests passed for stable struct GUIDs/defaults, stable enum names/values, dependent compile and pin evidence, and existing struct migration. Coverage is limited to discovered dependents; enum reordering is unavailable. | REF:Q11 |
| MCP-35 | P1 | Implemented | **Preserve struct values before removing fields.** MigrateUserDefinedStructMemberValues and RestoreStructMemberValueBackup provide compatible mappings, durable backups, stale guards and guarded removal for DataTables/CDOs/nested containers. Graph literals/placed instances are excluded; restore requires compatible unchanged layout. | EXTRA:struct-values |

## Asset lifecycle, diagnostics and output

| ID | Priority | Status | Requirement, current coverage and remaining work | Sources |
|---|---|---|---|---|
| MCP-36 | P1 | Implemented | **Reparent preflight/apply.** ReparentBlueprint reports breaking usages, dropped overrides/interfaces/components and changed/lost defaults, with located compile errors and default failed-compile save refusal. onlyUsed filters usage/overrides; descendant scan is capped at 256 with completeness evidence. | ASSET:A1 INHERIT:6.2 |
| MCP-37 | P1 | Implemented | **Move/rename and scoped redirector fixup.** MoveAsset/FixupRedirectors report saved/unsaved/newly-dirty packages and guard pre-existing dirty referencers. Pre-rename snapshots distinguish the move's own dirtiness. One packagePath per call, not proposed paths[] batching. Source-control lock/failure coverage is not universal. | ASSET:A2 ASSET:A3 REF:Q14 REF:Q15 INHERIT:6.1 |
| MCP-38 | P2 | Implemented | **Duplicate assets.** DuplicateAsset supports source/destination/name, dry-run, overwrite refusal and explicit save. | ASSET:A4 |
| MCP-39 | P1 | Implemented | **Verify ordinary package deletion.** DeleteAsset checks the original package file after the engine deletion attempt. It reports surviving files and read-only state and returns an error on incomplete deletion. UE 5.8 saved-asset deletion passed. Source-control lock failures were not reproduced. | ASSET:A5 REF:Q13 |
| MCP-40 | P1 | Implemented | **Consolidate/replace references and delete.** ConsolidateAndDeleteAsset provides dry-run, referencers, confirmation, reference/type rewriting and touched-package reporting. Referencer compile/save remain explicit separate steps. Exact retired DSM-class cleanup is not newly verified. | GEN:R3 TOOL:R12 |
| MCP-41 | P2 | Implemented | **Blueprint member/function cleanup.** RemoveBlueprintVariable and RemoveBlueprintFunction include usage/inherited/reference guards, dry-run, confirmation and compile/rollback behavior. Reference acknowledgement is not permission to discard project behavior. | EXTRA:cleanup |
| MCP-42 | P1 | Implemented | **Located compile errors and warnings.** CompileBlueprint/shared native utilities return status and error/warning arrays with graph/node/pin locations. Patch failures also include located diagnostics. Equivalent reporting on every official compile tool is unestablished. | TOOL:R11 REF:Q17 |
| MCP-43 | P3 | Implemented | **Explicit warning before saving broken nodes.** SaveBlueprint and SaveValidatedBlueprints inspect live graphs when the compile guard is disabled. They report graph paths and node GUIDs for visible missing cast classes, unresolved calls, existing compiler errors and orphaned pins. UE 5.8 verified the missing-cast warning in batch preview without a file write and in a successful explicit save. | TOOL:R13 |
| MCP-44 | P2 | Implemented | **Compact live inspection.** InspectLiveBlueprint(outputMode=compact) returns identities and visible exact links without full pin dumps. Reader completeness/owner qualifications remain under MCP-09. | DEFAULT:X2 |
| MCP-45 | P2 | Open | **Guarded Python/console escape hatch.** No ExecuteEditorPython/arbitrary-console tool identified. Decide whether this stays an intentional restriction or becomes an explicitly guarded feature; do not imply an undisclosed fallback exists. | MERGE:G3 |

## Visual review, preservation and workflows

| ID | Priority | Status | Requirement, current coverage and remaining work | Sources |
|---|---|---|---|---|
| MCP-46 | P2 | Implemented | **Basic explicit native UI review.** GetEditorWidgetSnapshot, CaptureEditorWidget and FrameBlueprintGraph exist. Recorded sample handover captured BP_Door via adapter; the latest report also verifies explicit official-window captures. Native weak/session-local refs and project-local PNG output are implemented. Screenshot capability is not missing. | LOCAL:visual |
| MCP-47 | P2 | Implemented | **Readable regions, connected data nodes and tiles.** Native framing supports graph rectangles, upstream pure data expansion, padding and minimum zoom. Capture reports actual viewport bounds, zoom and tile/region clipping. UE 5.8 verified an unclipped selected conversion node with its data source and overlapping 1.0-zoom tiles. | WEAPON:R3 |
| MCP-48 | P2 | Implemented | **Deterministic background capture and frame waiting.** Adapter capture waits within a configurable settling budget and reports exact project/graph/widget identity, dimensions and completion. Background framing uses an already visible graph tab without activating it. UE 5.8 verified background capture with the same foreground process before and after, plus the explicit-window capture route and dry-run regressions. | WEAPON:R4 |
| MCP-49 | P2 | Implemented | **Graph preservation snapshot/diff.** CompareBlueprintGraphPreservation snapshots a live Blueprint and reports changed graphs and paged exact wiring, execution destinations, literals, local declarations and normalized input sources. It follows reroutes and unique-writer locals, and flags caching and ambiguous writers for review. UE 5.8 verified branch and literal changes, reroute normalization, untouched-graph exclusion and a zero-diff live BP_Door round trip. Structural evidence only. | WEAPON:R5 |
| MCP-50 | P2 | Implemented | **Guided binary conflict resolution.** Standalone UnrealMCP discovers and compares conflicts, previews a selected side's compatible existing-node link and layout replay, applies authored checkpoints, and stages approved keep/delete resolutions for same-path, asymmetric and split-rename conflicts. Unsupported replay changes and overlaps are reported for explicit authoring. Split-rename staging supports one kept Blueprint and explicitly approved absent paths. UE 5.8 verified historical link replay preview and staging for same-path, add/add, modify/delete and split renames using an isolated index with GraphXPort and Epic MCP disabled. | TOOL:R15 |
| MCP-51 | P2 | Implemented | **Packaged end-to-end workflows.** Four bundled skills cover reparent/member port, class retirement, binary Blueprint conflict resolution, and save-format migration. Skill frontmatter, packaging paths and native tool references were validated. Project-specific runtime and converter acceptance remains separate. | TOOL:S1 TOOL:S2 TOOL:S3 REF:F1 REF:F2 REF:F3 |
| MCP-52 | Project work | Separate project work | **Actual DSM migration/gameplay fixes.** The local-array/AddUnique(Self) pattern is tested in the sample. DSM edits, suspected self-hit/PIE verification, registry/default/map repairs, merge staging/commit, converter/offline builds and trajectory/color tests are not implied complete. Do not reconnect the disconnected ForEach on the report's evidence. | LOCAL:functional |
| MCP-53 | P1 | Implemented | **Project-bound merge readiness.** Verified on UE 5.8 in UGitClientSample and the exact DSM AR_GIS_MAP editor. Native discovery distinguished live schemas from unverified disk cache while DSM was closed. DSM's adapter attached and reconnected to the same PID. The same process owned the official HTTP listener on port 8000 and exposed the GraphXPort conflict schema. Both conflict routes returned zero entries with another window in the foreground throughout. No DSM assets were edited. | READY:M1; MCP-01/MCP-50 |
| MCP-54 | P1 | Implemented | **Whole-Blueprint three-way evidence.** Native CompareBlueprintMergeStructure pages changes to declarations, signatures and locals, editable settings and defaults, parent/interfaces, component trees/overrides and saved direct asset references. Optional graph rows identify nodes, pins, and links. Historical Git LFS reads verify locally cached objects by SHA-256. Standalone UE 5.8 fixtures verified exact per-side evidence, stale-page rejection, valid/missing/corrupt LFS objects, and unchanged working bytes, dirty flags and Git stages. Missing /Game dependencies can be recovered from verified Base/Ours/Theirs commits under temporary package names. DSM's APlanDataManager comparison loaded all stages after recovering seven packages for Base and Theirs. Present active packages are reused; exact historical dependency closure, arbitrary native serialization and runtime behavior stay outside coverage. | READY:M2; MCP-49/MCP-50 |
| MCP-55 | P1 | Implemented | **Guarded non-graph resolutions.** Native Class Defaults, component defaults/overrides, interface add/remove, variable add/rename/type/remove, local function parameter type changes, compatible reparenting and local scene-component moves. Structural batches bind the preview to the three target stages, operation order, working file and reviewed dependent packages. Apply compiles the owner and reconstructs/compiles callers before an optional save, reports per-package outcomes and rolls back unsafe edits. UE 5.8 staged fixtures verified mixed apply, caller reconstruction, dirty-caller and stale-token rejection, rollback of an incompatible signature, and unchanged Git stages. Breaking edits and unsupported operations require an explicit separate repair plan. | READY:M3; MCP-19/MCP-50 |
| MCP-56 | P2 | Implemented | **Large and asymmetric conflict review.** Native cursors bind each page to exact Git slots, paths, graph scope and evidence with legacy offset paging retained. Add/add and modify/delete report absent stages, rename review supports explicit side paths, and unreadable sides report missing direct package dependencies or other load errors without returning misleading absence rows. UE 5.8 fixtures verified full traversal beyond 200 rows, asymmetric stages, cross-path lookup, and named missing dependencies. Graph-scoped comparison exposes bounded named execution links by stage and exact input/output links around a selected node. Live DSM verification passed for BP_BasePoint's shared IsValid node and APlanDataManager's nested MarkerSpawning graph, including cursor continuation. | READY:M5; MCP-50 |
| MCP-57 | P2 | Implemented | **Multi-graph review lifetime and transaction boundaries.** ApplyBlueprintMergePlan uses a client-held sequence of graph and structural steps and durable per-step receipts. Each checkpoint verifies the original stages, clean working state and previous saved file, then preflights and optionally applies on explicit confirmation. Checkpoints save independently. Replay returns stored success or failure and interrupted intents block reapplication. UE 5.8 fixtures verified two graph saves, dirty/disk/stage guards, later structural rollback and durable replay. | READY:M6; MCP-50/MCP-51 |
| MCP-58 | P1 | Implemented | **Inspect historical commits for selective merge edits.** Native anchored history and exact-commit file changes work independently of working-file and index state. Exact filters retain both rename endpoints, with paged text patch output. Historical Blueprint inspection pages graphs, nodes, pins and exact linked endpoints by commit and identity, reads SHA-256-verified local LFS objects, and names missing direct dependency packages. UE 5.8 fixtures verified anchored paging, both rename paths, a historical pin link across rename, cached LFS and missing dependencies. A live adapter call verified a sample text patch. | EXTRA:commit-history |

### Merge-readiness crosswalk

Native merge evidence is designed for agent consumption. `CompareBlueprintMergeStructure` defaults to compact column-ordered rows, preserving exact stage values and overlap flags. Full object rows remain available on request. The 65-row UE 5.8 fixture produced 9,242 compact JSON bytes versus 12,942 full JSON bytes.

| Source | Tracking | Acceptance boundary |
|---|---|---|
| READY:M4 | MCP-52, supported by MCP-49 and MCP-55 | Preserve separate guns/mortars arrays and types, ordinary iconEntity save/load, native migration and its ambiguity handling, APlanDataManager signatures/callers, spawn registry, networking, property panel and marker dependencies, and both military-symbol initialization refactors. Compile affected dependents and verify authorized save/load/spawn behavior separately. |
| READY:M7 | MCP-42 and MCP-46–48, integrated under MCP-50 | Review exact post-edit nodes, pins and defaults. Require readable, unclipped background captures and explicit pending/empty evidence. Report structural, compile, visual and runtime checks separately, with distinct save, dirty and stage results. Staging requires authorization. |

The source review found no unmerged paths at the time and had only cached adapter evidence. The UE 5.8 sample build subsequently verified native live search and schema discovery and reconnect on the same PID. The sample's port 8001 exposed both new GraphXPort tools, and their live calls rejected missing stages and a missing review session. Full staged acceptance and DSM acceptance remain pending. Native UnrealMCP continues to work independently of Epic's toolsets.

UnrealMCP's merge-readiness acceptance requires a standalone native route through its adapter. GraphXPort's tools are optional integration evidence. The native MCP-54 comparison and MCP-55 defaults, structural-batch and caller-reconstruction fixtures passed with GraphXPort and Epic disabled on UE 5.8, using a separate Git index. The new comparison source compiled in the UE 5.4 package build, which stopped on the existing three-argument `UDataTable::AddRow` call in `DataTableRowMutationTool.cpp`. UE 5.4 live acceptance remains open.

## Acceptance fixtures and historical project follow-ups

- The defaults request's parent/child/shadow assets exist in the recorded sample as BP_DefaultsParent, BP_DefaultsChild and BP_DefaultsShadow, with equivalent automation fixtures. These host binaries are separate from the plugin repository.
- Asset/reparent, inherited editing, local-node, struct/container and visual-safety tests create temporary sample equivalents. This is not a claim that every requested permanent Content/Data fixture is installed or committed.
- The merge report's sparse enum, DataTable/map and broken-class-reference fixtures need their own acceptance evidence when reopened. Generic container tests do not prove those precise DSM values.
- Defaults/asset/inherited/merge/local reports contain DSM follow-ups: verify ModelScale/height-sampling defaults, registry rows/map entries, port members/components, compare old/new values, integrate both conflict sides, compile/save intended packages and perform authorized gameplay/converter/release tests. The November 22 refactor proves one later outcome, not completion of all historical DSM steps.
- The muzzle self-hit remains a hypothesis pending PIE hit-actor/path-data evidence. Compilation, structural checks and screenshots cannot establish runtime equivalence.

## Platform backlog inherited from E1–E8

These remain part of the overall plan, not newly missing tools established by the two November 22 reports. Completed slices must not be reopened wholesale.

| Milestone | Implemented baseline | Remaining scope |
|---|---|---|
| E1: discovery/context | Compact discovery, exact schemas and bounded overviews. | Authoritative tool metadata, capability recipes, consistent output profiles and whole-workflow measurements. |
| E2: reliability | Transactions, scoped rollback/save evidence and operation-ID/replay tests. | Real identity targeting (MCP-01), disconnect/restart/expired-ID/rollback-failure coverage and stronger compound guarantees. |
| E3: query/evidence | Index/live inspection, traces and class usage. | Complete search/freshness, revision-bound paging, evidence handles/deltas, bounded read composition, latent/runtime boundaries and structural comparison. |
| E4: authoring | Native graph, member/component/interface/delegate/collection and asset edits. | Missing operations above, dependency-ordered multi-asset plans and non-atomic-save evidence. Enhanced Input Phase 5A properties/discovery/modifiers/triggers/player metadata/batches remain partial. |
| E5: diagnose/build/verify | Structured compile and synchronous automation hooks; operational rebuild/reopen skill. | Cursor/filter/grouped logs, explicit build-job lifecycle/artifacts/cancellation, verification/preload recipes and broader automation batching. A skill is not a native build-job API. |
| E6: reflection/migration | Internal reflected API use. | Dedicated compiled API inspection, ownership/dependency analysis, exact node/API mapping, dependency-aware rewiring and structural/runtime acceptance contracts. |
| E7: asset breadth | DataTable rows, struct-member operations and value migration. | Enum editing, wider Data Asset/gameplay-tag coverage, Widget Blueprint authoring, materials/meshes/textures and separately scoped animation/Niagara/GAS/ControlRig/MetaSound/source-control work. Level authoring remains deferred. |
| E8: compatibility/release | Recorded catalog-byte measurements, native/adapter regressions and refreshed catalogs. | Whole-task measurements, large/cold/ambiguous-target stress, version/client matrices and public-distribution documentation/release gates. No universal UE 5.4/runtime/client claim. |

Historical M1–M20 details remain in [LEGACY_ROADMAP.md](LEGACY_ROADMAP.md); E1–E8 is the active order. This consolidation does not declare unchecked historical tasks complete.

## Recommended next order

1. **Merge acceptance and scale.** Use MCP-52 for separately authorized DSM weapon-contract checks. Track validation receipts under MCP-42 and MCP-46–50.
2. **Other planned work.** MCP-26 is implemented on UE 5.8 with verified same-graph edits and portable fragments. MCP-49 and MCP-47/48 are available for merge validation.

Reproduce reported issues on the selected sample build before implementing them. Distinguish native and official routes; do not recreate delivered local/default/asset tools.

## Evidence pointers

- Contracts: [native catalog](../Adapter/UnrealMCP.Adapter/unreal-tools.json), [registration](../Source/UnrealMCP/Private/UnrealMCPModule.cpp), [README](../README.md).
- Identity: [ProcessDiscovery](../Adapter/UnrealMCP.Adapter/UnrealSessionManager.ProcessDiscovery.cs), [PipeAttachment](../Adapter/UnrealMCP.Adapter/UnrealSessionManager.PipeAttachment.cs), [session/shutdown manager](../Adapter/UnrealMCP.Adapter/UnrealSessionManager.cs).
- Comments/exec/patch behavior: [ApplyBlueprintGraphPatchTool.cpp](../Source/UnrealMCP/Private/Tools/Blueprint/Graph/ApplyBlueprintGraphPatchTool.cpp). Reader/declaring-owner behavior: [LiveBlueprintToolUtils.cpp](../Source/UnrealMCP/Private/Tools/Blueprint/Shared/LiveBlueprintToolUtils.cpp).
- Local/inherited resolution: [BlueprintMemberToolUtils.cpp](../Source/UnrealMCP/Private/Tools/Blueprint/Shared/BlueprintMemberToolUtils.cpp); retargeting: [BlueprintMemberGraphTools.cpp](../Source/UnrealMCP/Private/Tools/Blueprint/Members/BlueprintMemberGraphTools.cpp).
- Usage coverage: [FindClassUsagesTool.cpp](../Source/UnrealMCP/Private/Tools/Blueprint/Inspection/FindClassUsagesTool.cpp). Move/fixup/plain deletion: [AssetManagementTools.cpp](../Source/UnrealMCP/Private/Tools/Assets/AssetManagementTools.cpp).
- Defaults: [BlueprintClassDefaultsTools.cpp](../Source/UnrealMCP/Private/Tools/Blueprint/Members/BlueprintClassDefaultsTools.cpp). [Defaults handover](BlueprintVariableDefaults_Handover.md) is historical; its 139-tool count is not the current catalog count.
- Visual framing/settling: [EditorVisualTools.cpp](../Source/UnrealMCP/Private/Tools/Editor/EditorVisualTools.cpp), [visual tests](../Source/UnrealMCP/Private/Tests/UnrealMCPEditorVisualTests.cpp), [local-node tests](../Source/UnrealMCP/Private/Tests/UnrealMCPLocalVariableNodeTests.cpp), [inherited tests](../Source/UnrealMCP/Private/Tests/UnrealMCPInheritedGraphEditingTests.cpp).
- Recorded test families include ToolingP1, CollapsedGraph, GraphPatch, ClassDefaults, LocalVariables/LocalVariableNodes, Asset/RedirectorFixup, MemberRemoval, Struct/ValueMigration and Editor/Visual. Test-source presence alone is not a fresh passing run.
- Separate GraphXPort integration: [ConflictToolset.md](../../GraphXPort/docs/ConflictToolset.md). Its conflict toolset depends on UnrealMCP; UnrealMCP does not require GraphXPort. Core export/merge and conflict-toolset integration are distinct.

## Implementation follow-up: 2026-11-23

- MCP-01 is implemented and adapter-regression tested, with real UGitClientSample launch/attachment and live read-only gateway verification.
- MCP-03 defaults to dirty-package refusal and supports explicit discard and save policies.
- **MCP-53: Editor launch mode.** The adapter starts editors interactively by default. Callers can opt in to unattended mode per launch.
- Native plugin/catalog remain unchanged at 157 tools. No new native-suite run was required for this adapter-only change; the prior native evidence remains dated. No DSM, PIE or gameplay changes were made. Other open requirements remain open.

## Historical source specifications

These collapsed sections preserve the complete superseded source documents, including acceptance criteria, proposed schemas and fixtures. They are historical evidence, not new instructions or permission to operate on DSM. The reconciled statuses above take precedence over old claims that a capability is missing.

<a id="source-gen"></a>

### GEN — UnrealMCP_Requirements.md

<details>
<summary>Original specification: UnrealMCP_Requirements.md</summary>

`````markdown
# UnrealMCP Requirements (unreal-mcp-adapter and unreal-mcp)

These come from gaps found while converting DSM vehicle Blueprints (UE 5.8, project `AR_GIS_MAP`). Each item says what blocked or caused trouble.

## Must have

### R1. Read nested graphs
`read_graph_dsl`, `find_node_types` and the scripting tool fail on graphs inside collapsed graphs (`Cannot cast type 'K2Node_Composite' to 'Blueprint'`). They must work at any nesting depth (e.g. `Load Plan → Spawnning Drone → SetWeaponData`).
*Blocked:* locating the last `BP_Vehicle` reference in `APlanDataManager`.

### R2. Find what references an asset or class, down to the node or pin
Given an asset or class, list every Blueprint, graph and node that uses it: local variables, function parameters, pin types, macros, collapsed graphs. Also report leftover import entries that keep a package referenced after the last using node is gone.
*Blocked:* `APlanDataManager` still imports `BP_Vehicle`; neither the index nor live tools show where.

### R3. Delete an asset and replace its references
Equivalent of the editor's Delete → Replace References: delete an asset and retarget its users to another class, with a dry run that lists what would change.
*Blocked:* deleting `BP_Vehicle` (left as a manual step).

### R4. Show reroute nodes when reading a graph
The DSL reader drops reroute (knot) nodes, so connected branches look empty. Output must show reroute chains or follow them to the real target.
*Caused:* a false bug report (`OnUIPropertyChanged` → `Is Not Valid` looked empty but was wired).

### R5. Add and remove function-local variables
Neither server can create a local variable, and the graph DSL cannot declare one.
*Blocked:* refactoring `InitialisePayloads` with locals.

## Should have

### R6. Fuller graph-writing language
- Create parent calls for interface functions and events.
- `MakeArray` with any number of inputs (currently one).
- `==` for strings (today needs `Equal (Case Insensitive)`).
- Inline impure nodes inside an expression: keep them or raise an error, never drop them silently.
- Reader shows which output pin is used (e.g. Split Left vs Right) and the real owner class of inherited members (it mislabels them, e.g. `BPSplinePointDataParent`, `BPVehicle`).

### R7. Edit one event without rewriting the graph
`write_graph_dsl` replaces the whole graph. Need insert/replace of a single event or function, or a patch that appends nodes after an existing node.

### R8. Inherited variables in the patch tool
`ApplyBlueprintGraphPatch` fails with "variable not found" for inherited self variables (`IconTexture`, `EntityName`). It must resolve them through the parent class.

### R9. Consistent argument names across both servers
Today they differ: `toolset`/`toolset_name`, `tool`/`tool_name`, `pos`/`position`, `output_pin`/`source`, `input_param`/`is_input`. Schemas should be discoverable per tool without dumping a 72 KB toolset description.

### R10. Read class defaults safely
`list_properties`/`get_properties` on `Default__BP_Vehicle_drone_C` failed with `Accessed None ... CreateDynamicMaterialInstance`. Reading defaults must not run construction logic or throw.

## Nice to have

### R11. Auto-arrange a whole graph
`arrange_nodes` needs an explicit node list. Accept a graph or function and lay it out like the editor's straighten command.

### R12. No timeouts on large Blueprints
Scripted reads across about 100 graphs in `APlanDataManager` timed out. Need pagination or longer timeouts for batch reads.

### R13. Stable project selection
The adapter silently switched to `UGitClientSample` between calls. Keep the selected project for the session, or warn when it changes.

### R14. Report index staleness
The index kept showing deleted nodes until the Blueprint was saved. Results should say the index is stale since an edit, or refresh after each change.
`````

</details>

<a id="source-weapon"></a>

### WEAPON — UnrealMCP_WeaponMigration_Requirements.md

<details>
<summary>Original specification: UnrealMCP_WeaponMigration_Requirements.md</summary>

`````markdown
# Unreal MCP requirements from the DSM weapon migration

Date: 2026-11-22

Scope: limitations observed while migrating DSM weapons and refactoring `APlanDataManager.InitializeMilitarySymbol` and `InitializeNewMilitarySymbol` through `unreal-mcp-adapter` and `unreal-mcp`. This is an implementation backlog, not a claim that every capability is absent from the latest plugin. Reproduce each remaining issue against the current build before implementing it. No plugin code was changed for this report.

## R1 — P1: Keep the editor connection when its active tab changes

Observed: the adapter reported no project window or `unreal_not_running` while the user confirmed the editor was open. The official MCP route remained usable during parts of this work. Reattaching to the same project sometimes recovered the adapter.

Requirement: identify an attached editor by its project, process and transport connection, independently of the foreground window, window title or selected editor tab. Distinguish a missing window from an exited process and a disconnected plugin.

Acceptance:

- Changing Blueprint tabs, moving Unreal into the background and returning to the main editor must preserve the selected project and tool access.
- If the transport disconnects, report the project, process ID, connection state and a recoverable reconnect action.
- Reconnect to the existing process without launching another editor or silently selecting another project.

## R2 — P2: Apply comment-box dimensions reliably in graph patches

Observed: attempts to resize existing comments through `ApplyBlueprintGraphPatch` did not produce the requested dimensions during the two-function refactor. The workaround was to replace those comment nodes with new boxes. This observation needs reproduction on the current build.

Requirement: applying `width`, `height`, colour and `showBubbleWhenZoomed` to an existing comment must either update those properties or return an explicit unsupported-property error. Do not report a successful mutation while ignoring requested properties.

Acceptance:

- Resize an existing comment without deleting it or changing its GUID.
- Read back the actual bounds, colour and bubble setting after the operation.
- Preserve contained nodes and their positions unless movement is explicitly requested.

## R3 — P2: Frame and capture graph regions with usable text

Observed: `FrameBlueprintGraph` followed by a screenshot could omit the data nodes below selected execution nodes. Including a larger region reduced the zoom until text was hard to read. Several separate captures were needed for review.

Requirement: support framing an explicit graph rectangle or a node selection with optional connected data nodes, padding and a minimum readable zoom. If the region cannot fit at that zoom, provide multiple captures rather than treating a distant overview as sufficient.

Acceptance:

- Capture a call and its supporting local getters without clipping either.
- Report the actual framed bounds and zoom, plus whether requested content was clipped.
- Offer bounded, overlapping captures for a large function without changing graph layout.

## R4 — P2: Make graph capture deterministic in the background

Observed: an empty-ref Slate screenshot could return an empty image while Unreal was in the background. An explicit window ref discovered through `SlateInspectorToolset.Snapshot` worked. Framing also returned `framingPending`, requiring a later observation before capture.

Requirement: allow a graph/widget-specific capture that waits for its pending frame operation and resolves the correct editor window. Return a clear error for an invalid or unavailable render target rather than an empty successful image.

Acceptance:

- Capture the requested graph while Unreal is in the background, without bringing it forward or using an OS screenshot.
- Return the captured project, graph/widget identity, dimensions and framing completion state.
- Preserve the existing explicit-window-ref route; it is a verified working workaround.

## R5 — P2: Provide a bounded graph-preservation check for wiring refactors

Observed: proving that the local-variable refactor preserved behavior required a custom in-memory comparison of the original and edited graphs. Compilation alone cannot establish that the original execution branches or input sources survived.

Requirement: provide a read-only before/after graph comparison that reports changes to original nodes, exact pin links, literal defaults, execution destinations and local-variable declarations. A refactor mode should trace through newly inserted local setters/getters and reroutes, while explicitly identifying any caching that changes when a pure expression is evaluated.

Acceptance:

- Detect a removed branch, changed literal or replaced input source even when the Blueprint compiles.
- List changed graphs so a two-function refactor can demonstrate that other graphs were untouched.
- Describe this as structural verification, not proof of runtime equivalence.

## Capabilities verified working — do not implement as missing features

- Function-local variable creation and typed local getters/setters worked through the adapter, including object, vector and numeric locals. The blanket local-variable limitations in `UnrealMCP_Requirements.md` R5 and `UnrealMCP_ToolingRequirements.md` R3 are outdated for the adapter route. Removal and DSL round-tripping were not established by this refactor.
- Targeted declarative patches, exact data-link replacement, execution reroutes and split vector getter pins worked.
- Targeted compilation returned structured error and warning counts. The final Blueprint compiled with zero errors and zero warnings. This does not establish equivalent reporting on every official MCP compile tool.
- `SaveValidatedBlueprints` saved only the requested Blueprint; a subsequent read confirmed the package was clean.
- Live node/pin inspection and graph revisions supported preservation checks. Only the two requested function graphs changed.
- Official Slate screenshots using an explicit window ref supported background review. Screenshot capability itself was not missing.

## Validation boundary

The latest refactor was compiled, structurally compared, visually reviewed and saved. PIE was not run, so runtime equivalence remains unverified. Older requirements concerning asset consolidation, broken casts, nested graphs and binary merges were not re-tested in this refactor and should retain their own evidence and status rather than being presented as newly confirmed gaps here.
`````

</details>

<a id="source-tool"></a>

### TOOL — UnrealMCP_ToolingRequirements.md

<details>
<summary>Original specification: UnrealMCP_ToolingRequirements.md</summary>

`````markdown
# UnrealMCP / unreal-mcp-adapter: tooling requirements

Gaps found while doing a real Blueprint refactor in the DSM project (UE 5.8): reparenting `BP_Marker` children, building a vehicle hierarchy, deleting legacy assets, and merging binary `.uasset` conflicts from `dev_5.8`. Each item gives the problem we hit, what is needed, and how to tell it is done.

Priority: **P1** blocked work or cost the most time · **P2** caused wrong conclusions or workarounds · **P3** quality of life.

---

## 1. Graph editing

### R1 (P1): node tools work inside collapsed graphs
- **Problem:** `BlueprintTools.create_node`, `find_node_types` and the script tooling fail inside collapsed graphs (`K2Node_Composite`, e.g. `Load Plan.K2Node_Composite_7.Spawnning Drone`) with `Cannot cast type 'K2Node_Composite' to 'Blueprint'`. Logic in collapsed graphs can't be edited.
- **Need:** resolve the owning Blueprint by walking the graph's outer chain (`FBlueprintEditorUtils::FindBlueprintForGraph`) in every tool that takes a graph.
- **Done when:** create, connect, delete and find work the same in a collapsed graph, a macro graph and a function graph.

### R2 (P1): retarget a broken cast or class reference
- **Problem:** `retarget_node_class` needs `old_class` to still exist. Once a class is deleted, the cast becomes a "Bad cast node" and can't be repaired. The workaround was restoring the asset from git and restarting the editor.
- **Need:** `retarget_node` that takes a node and only a `new_class`, works on broken nodes, and keeps every link whose pin still exists.
- **Done when:** a cast to a deleted class can be retargeted and its output links survive.

### R3 (P1): function local variables
- **Problem:** no tool can add, list, read or remove a function's local variables. The DSL reader shows them as member getters.
- **Need:** `list_local_variables(function_graph)`, `add_local_variable(graph, name, type, default)`, `remove_local_variable`. DSL support: `(local name type)` plus getters and setters.
- **Done when:** a function can be refactored to use locals without the editor UI.

### R4 (P2): getters for inherited variables
- **Problem:** the adapter's `ApplyBlueprintGraphPatch` `variableGet` fails for inherited self variables ("Variable not found"). With `ownerClassPath` it creates a non-self node that needs a target pin.
- **Need:** look the variable up through the class hierarchy and create a self-context getter or setter when no target is wired.
- **Done when:** `variableGet IconTexture` on a child Blueprint gives a self getter for the inherited variable.

### R5 (P2): DSL writer limits
- Make Array takes only one positional input. Allow N inputs and add pins automatically.
- It can't create parent calls for interface functions or events.
- Struct make/break pins need generated names (`PropertyName_20_FA36…`). Accept display names (`PropertyName`).
- `write_graph_dsl` replaces the whole graph. Add an insert or replace mode for a single event or function body.
- **Done when:** these four cases work without falling back to node-by-node edits.

### R6 (P3): arrange a whole graph
- **Problem:** `arrange_nodes` needs an explicit node list.
- **Need:** `arrange_graph(graph)`, and an option to auto-arrange after `write_graph_dsl`.

---

## 2. Reading and searching

### R7 (P1): dependency search to the node and pin
- **Problem:** `AssetTools.get_referencers` said `APlanDataManager` uses `BP_Vehicle` but not where. The index showed no node or pin, and reading every graph timed out.
- **Need:** `find_class_usages(blueprint, class_or_asset_path)` returning every hit, each tagged by kind: graph, node GUID, pin, local or member variable, function parameter, default value, struct field, or import-only (stale).
- **Done when:** a hidden reference is found in one call, including stale import-table entries.

### R8 (P2): reader output complete and accurate
- Reroute nodes are hidden. This produced a false "Is Not Valid branch is empty" finding.
- The owner class of members is mislabelled (`Class|BPSplinePointDataParent|GetPropertyList` for a `BP_BasePoint` member).
- Macro instances print as empty functions.
- Multi-output pins aren't identified (Split Left vs Right).
- **Need:** a verbose mode that lists every node and every link with exact pin names and the class that declares each member.
- **Done when:** the reader output can be used to judge whether logic is correct.

### R9 (P2): big Blueprints without timeouts
- **Problem:** reading all graphs of a large Blueprint (`APlanDataManager`, ~96 graphs, one with 1,000+ nodes) in one script times out.
- **Need:** paged or streamed graph reads and search with server-side filtering, plus a configurable timeout.

### R10 (P2): one node identifier for both servers
- **Problem:** adapter tools take node GUIDs. BlueprintTools returns refPaths without GUIDs. GUIDs had to be fetched from a possibly stale SQLite index.
- **Need:** include `nodeGuid` in every BlueprintTools node result, and accept either a GUID or a refPath everywhere.

---

## 3. Compile, save, assets

### R11 (P1): structured compile results
- **Problem:** `compile_blueprint` returns `null` on success and one text blob on error. Warnings are invisible.
- **Need:** `{status, errors:[{graph, nodeGuid, message}], warnings:[…]}`.

### R12 (P1): delete with replace references (consolidate)
- **Problem:** `AssetTools.delete` force-deletes. There's no equivalent of the editor's "Replace References" dialog.
- **Need:** `consolidate_and_delete(old_asset, replacement_asset)`. It repoints every referencer, leaves a redirector or fixes one up, and reports the touched packages.
- **Done when:** deleting `BP_Vehicle` with replacement `BP_Vehicles` leaves no references and nothing broken.

### R13 (P3): save without a compile check, made explicit
- `SaveValidatedBlueprints` with `requireUpToDateCompile=false` saved a Blueprint whose cast pointed at a missing class, and the empty class was stored for good. Warn when a save would persist broken nodes.

---

## 4. Editor lifecycle

### R14 (P2): reliable ready and exit waits
- **Problem:** `RequestUnrealShutdown` can report "not running" while the process is still exiting. A git file write then failed with "Device or resource busy". There's also no blocking "wait until ready".
- **Need:** `WaitForUnrealReady(timeout)` and `WaitForUnrealExit(timeout)` that check both the process and its file handles.

---

## 5. Source control and binary assets

### R15 (P2): merging binary Blueprint conflicts
- **Problem:** `.uasset` conflicts (here `APlanDataManager`) can only be resolved as ours or theirs, and one side's edits then have to be redone by hand.
- **Need:** export base, ours and theirs graphs to text, diff them by node GUID, and replay a chosen side's node-level edits onto the other. Check whether `GraphXPortConflictToolset` already covers part of this.
- **Done when:** taking "theirs" and replaying "ours" is one guided operation with a review step.

---

## 6. Skills (packaged workflows)

### S1: reparent a Blueprint
Record child defaults and component overrides → `set_parent` → map old fields to new (e.g. `MarkerName → EntityName`) → rebuild inherited self getters → compile → save only touched assets.

### S2: retire an asset safely
Find usages to the pin (R7) → rewire or delete → consolidate and delete (R12) → recompile all referencers → dry run to confirm nothing else is dirty.

### S3: resolve a binary merge
Take theirs → replay our listed edits (R15) → compile → check for stale references → commit.

---

## Suggested order
R1, R7, R12, R11, R3, R2 first. Then R8, R10, R9, R4, R5, R14, R15. Then R6, R13 and the skills.
`````

</details>

<a id="source-merge"></a>

### MERGE — DSM_Merge_DataTable_Gap_Report.md

<details>
<summary>Original specification: DSM_Merge_DataTable_Gap_Report.md</summary>

`````markdown
# DSM dev_5.8 merge: what the unreal-mcp-adapter could not do

**Date:** 2026-11-13
**Project:** DSM `AR_GIS_MAP` (UE 5.8), `C:\Users\LTX_MSI\Documents\AXIS\work_dsm`
**Branch:** `bp_marker-conversion-v2`, part of the DSM-639 weapon migration

This report covers merging `origin/dev_5.8` (`2e13b6799`) into the branch. The merge is still in progress (`--no-commit`). It records what the user asked for, what was searched and done, which tool gaps blocked the work, and what is still needed. It extends `UnrealMCP_ToolingRequirements.md` in this folder.

---

## 1. User instructions

- Work only in `work_dsm`. Ignore the other checkout at `C:\Users\LTX_MSI\Documents\AXIS\dsm`. Select the `work_dsm` uproject in the adapter.
- Commit the local work, then merge the latest `dev_5.8` into the branch. Resolve conflicts without losing other developers' fixes, and keep our weapon structure and reparenting:
  ```
  BP_IconEntity
  └── BP_BaseWeapon
      ├── BP_Mortars → BP_Mortar_81mm, BP_51mm
      └── BP_Guns    → BP_AGS, BP_MMG
  ```
- Use **unreal-mcp-adapter only**. Do not use unreal-mcp (Epic's server). The adapter may close, launch and restart the editor.
- Commit only when the user says so. Never push. Do not take either side of a Blueprint wholesale without the user's confirmation.
- Bugs reported by the user:
  - The weapon trajectory was not toggling or visible. Fixed upstream in `55a57e0c` (PR #255) and ported.
  - Icon and text colour does not work. Not yet investigated.
  - Weapons spawn correctly from saved plans. A **newly created** weapon spawns as a default icon with the title "Header". This report is about that bug.

---

## 2. What was done

| Asset | Resolution | State |
|---|---|---|
| `BP_GIS_Map_PlayerController` | Took dev_5.8, then re-added our IsValid guard in `UpdateEntityPreview` | Compiled and saved; conflict still marked (UU) |
| `APlanDataManager` | Kept ours; added PR #256's new `EntityName` string input to all 33 `Server_CreateShape` calls, each fed by a literal or by `FeatureModeToEntityName` | Compiled with 0 errors and saved; UU |
| `BFL_ShapePropertiesLibrary` | Took dev_5.8 (`git checkout --theirs`) after an in-memory edit could not be undone through the adapter | Restored on disk; UU |
| `IconEntity/Weapon/BP_BaseWeapon` | Ported #255: the trajectory Branch now reads only `isSimulating`; the AND with `TrajectoryInvisible` was removed | Saved; modified |
| Old `FirstPerson/.../BP_BaseWeapon` | Kept deleted (`git rm`) | — |
| `Content/__MergeTemp/` | Scratch copies of the base and incoming versions, used for comparison | Deleted |

**Comparison method (adapter only):**
1. Extract the base and incoming versions with `git cat-file` into `Content/__MergeTemp/{base,theirs}`.
2. Load them in the editor and read them with `InspectLiveBlueprint` / `GetBlueprintOverview`.
3. Diff the results with a script.

The copies were never saved.

---

## 3. Root cause of the "default icon / Header" bug

1. New-entity creation runs `AC_CreateShapes` → `Create_Shape`.
2. That graph reads the `EntityName` variable, converts it with `Conv_StringToName`, and calls `GetDataTableRow(DT_EntityRegistry)` to get the class to spawn.
3. `DT_EntityRegistry` came in with PR #249/#256 (EntitySaving). Its weapon rows still point at classes that no longer exist on this branch:

| Row | Class in the table (missing) | Should be |
|---|---|---|
| `WEAPON` (81mm, NE8) | `/Game/Assets/Weaponary/81_mm_Mortar/BP_BaseWeapon_Mortar` | `.../MarkerBP/IconEntity/Weapon/Mortar/BP_Mortar_81mm` |
| `AGS` (NE27) | `.../MarkerBP/MortarandGuns/BP_AGS` | `.../MarkerBP/IconEntity/Weapon/Gun/BP_AGS` |
| `MMG` (NE28) | `.../MarkerBP/MortarandGuns/BP_MMG` | `.../MarkerBP/IconEntity/Weapon/Gun/BP_MMG` |
| `51mmMortar` (NE29) | `.../MarkerBP/MortarandGuns/BP_51mm` | `.../MarkerBP/IconEntity/Weapon/Mortar/BP_51mm` |

The table also references `MarkerBP/BP_Marker_Weapon`, probably from the `SymbolWeapon` row. That needs checking.

A missing class falls back to `BP_IconEntity`, which gives the default icon and the "Header" title. Saved plans use a different spawn path that doesn't read this table, which is why they spawn correctly.

**Evidence:**
- Live inspection of the `Create_Shape` nodes (`K2Node_VariableGet_56` → `Conv_StringToName` → `K2Node_GetDataTableRow_0`).
- `strings` on the `.uasset`.
- The old folders `MortarandGuns/` and the old `BP_BaseWeapon_Mortar` are absent on disk.

---

## 4. What was searched in the adapter

The adapter was refreshed (`RefreshToolManifest`) and reconnected after the editor restart (PID 20836). The catalog still has 127 tools.

| Query | Result |
|---|---|
| `DataTable`, `DataTable row`, `data table rows read`, `table` | Nothing that reads or edits DataTable rows (`table` matched only node and variable tools) |
| `set asset property`, `property`, `row struct asset edit set` | Only component and delegate tools |
| `local variable default value map` | Nothing |
| `python execute console command` | Nothing |

---

## 5. What could not be done (tool gaps)

### G1 (P1): edit DataTable rows
- **Problem:** no adapter tool can read or write `UDataTable` rows. The four broken class references above can't be fixed, which blocks the merge.
- **Need:**
  - `GetDataTableRows(assetPath, rowNames?)`: returns each row's fields as export text.
  - `SetDataTableRowField(assetPath, rowName, fieldName, value, save?)`: matches fields by display name, so the `Class_15_<GUID>` suffix of user-defined structs is hidden.
  - Optional: `AddDataTableRow` / `RemoveDataTableRow`.
  - All of these should be transactional, mark the package dirty, and save only on request.
- **Done when:** the `Class` field of row `AGS` in `DT_EntityRegistry` can be changed to a different Blueprint class and saved, and reading it back shows the new path.

### G2 (P1): local variable whose type is a map with an enum key
- **Problem:** in `BFL_ShapePropertiesLibrary.FeatureToBasePoint`, the local variable `FeatureBasePoinrMap` is a `TMap<E_FeatureModes, TSubclassOf<BP_BasePoint>>`.
  - `AddBlueprintLocalVariable` rejects an enum key (`byte`, `enum` and the enum object path all fail).
  - No tool can set a local variable's default value.
  - After `RemoveBlueprintLocalVariable` there was no way to recreate it. The asset had to be restored from git.
- **Need:**
  - Container types (Array, Set, Map) with any key and value type, including user enums, structs and class references, in `AddBlueprintLocalVariable` and `AddBlueprintVariable`.
  - `SetBlueprintLocalVariableDefault(graph, name, exportText)`.
- **Done when:** that map variable can be recreated and its four weapon entries (NE8, NE27, NE28, NE29) set.

### G3 (P2): run editor Python or a console command
- **Need:** a guarded `ExecuteEditorPython` (or console command) tool, as an escape hatch for edits that have no dedicated tool.

### G4 (P2): RequestUnrealShutdown saves dirty packages
- **Problem:** shutdown silently saved the dirty in-memory BFL, so it had to be restored from git afterwards.
- **Need:** a `discardDirty=true` option, or a refusal when packages are dirty unless the caller confirms.

### G5 (P3): pin defaults and array wildcards in patches
- **Problems:**
  - `ApplyBlueprintGraphPatch` refuses to overwrite an existing pin default.
  - It refuses a second exec link or self link into an occupied pin.
  - Array functions created as `functionCall` leave unresolved wildcard pins.

---

## 6. Test fixture schemas (to recreate in this sample project)

The DSM assets were **not** copied as binaries. Their `/Game/AR_GISMap/...` references would break in this project, and their row values can't be stripped from a `.uasset` outside the editor. Recreate equivalents in `Content/Data` (next to `E_ItemRarity`, `S_ItemData` and `DT_ItemTextStyles`) with no values:

**`E_FeatureModesTest`** (UserDefinedEnum)
- Sparse display names like the DSM enum's `NewEnumerator0..39`; the DSM enum has gaps at 4, 26 and 38.
- At least 5 entries. Gaps are needed to test how name and index are resolved.

**`ST_EntityRegistryTest`** (UserDefinedStruct)
- `Category`: `E_FeatureModesTest` (byte enum)
- `Class`: Class reference to `Actor`, matching DSM's `Class_15_<GUID>`; check whether DSM uses a hard or soft class reference.

**`DT_EntityRegistryTest`** (DataTable, row struct `ST_EntityRegistryTest`)
- Rows `WEAPON`, `AGS`, `MMG`, `51mmMortar` with the Class left empty.
- One row should point at a since-deleted Blueprint to test a broken reference.

**`BFL_MapLocalTest`** (Blueprint Function Library)
- One function with an empty local variable of type `Map<E_FeatureModesTest, Class<Actor>>`, for testing G2.

---

## 7. Still needed to finish the DSM merge

1. Fix the four `DT_EntityRegistry` rows (§3), either by hand or with G1. Save, then test creating new weapons in PIE.
2. If new weapons still spawn wrongly, set the four `FeatureBasePoinrMap` entries (G2 or by hand).
3. Recompile `APlanDataManager`.
4. `git add` the three UU files and `BP_BaseWeapon`, and confirm no unmerged paths remain.
5. Commit only after the user's go-ahead. Do not push.
6. PIE test: trajectory toggle, entity creation and saving.
7. Investigate the icon/text colour bug.
8. Remaining DSM-639 steps: two-slide save test, converter run on a real old plan, rebuild the offline EXE, Jira update.
`````

</details>

<a id="source-default"></a>

### DEFAULT — ToolRequest_BlueprintVariableDefaults.md

<details>
<summary>Original specification: ToolRequest_BlueprintVariableDefaults.md</summary>

`````markdown
# Tool request: read and set Blueprint class defaults (unreal-mcp-adapter)

**Date:** 2026-11-13
**Project:** DSM `AR_GIS_MAP` (UE 5.8), `C:\Users\LTX_MSI\Documents\AXIS\work_dsm`
**Related:** `Docs/DSM_Merge_DataTable_Gap_Report.md`, `UnrealMCP_ToolingRequirements.md`

## 1. Why this is needed (the real case)

Newly created mortar weapons showed no trajectory line, and the "Toggle Trajectory" property did nothing visible.

**Root cause:** the line is drawn by spline-mesh components whose thickness comes from `SetStartScale(ModelScale)`.
- In `dev_5.8`, the weapon Blueprint inherits `ModelScale` from `BP_Marker`, where the class default is **1.0**.
- After the weapons were moved under `BP_IconEntity`, `BP_BaseWeapon` declares its own `ModelScale`, and the default is **0**. Nothing in its CDO sets it.
- The spline meshes are therefore scaled to 0, so the line is invisible.

**The fix is one value:** `BP_BaseWeapon` → Class Defaults → `ModelScale = 1.0`.

No adapter tool could:
- **(a) read that value.** I had to write a script that parses the `.uasset` name table and property tags to decode the default from bytes.
- **(b) set it.** The user had to do it by hand in the editor.

The same gap came up for `ShouldSampleHeightForPositioning` (bool). It is overridden to true on `BP_IconEntity` and false on `BP_Mortar_81mm`; reading those values also needed byte-level parsing.

Existing tools only partly cover this:

| Tool | Limitation |
|---|---|
| `GetBlueprintOverview` | Lists variables and their types, but no default values |
| `AddBlueprintVariable` | Can set a default only when creating a variable; on an existing name it just reports `alreadyExists` |
| `Get` / `SetBlueprintComponentDefaults` | Component templates only, not the class's own variables |
| `ListBlueprintLocalVariables` | Shows defaults, but for function locals only |
| Inherited variables | No tool reads or overrides a parent class's variable in a child CDO |

## 2. Requested tools

### T1 (P1): `GetBlueprintClassDefaults`
Read class-default (CDO) values for one Blueprint.

**Input:**

| Field | Type | Notes |
|---|---|---|
| `objectPath` | string | Exact Blueprint object path |
| `propertyNames` | string[] | Optional; when omitted, return all |
| `includeInherited` | bool | Default true |
| `onlyOverridden` | bool | Default false; true returns only values that differ from the parent CDO |
| `offset`, `limit` | int | Paging |

**Output, per property:**

| Field | Notes |
|---|---|
| `name` | Display name, with no GUID suffix |
| `declaringClass` | Path of the class that declares the property |
| `type` | Pin-type style: category, subCategory, objectPath, container, key/value types |
| `value` | Export text, e.g. `1.000000`, `True`, `/Script/Engine.BlueprintGeneratedClass'/Game/...'`, `(X=1,Y=2,Z=3)` |
| `parentValue` | The parent CDO's value |
| `isOverridden` | Whether `value` differs from `parentValue` |
| `editable` | Whether the property is `CPF_Edit` or Blueprint-visible |

Read-only. It must not load other assets unless they are needed to resolve the class, must not compile, and must leave the dirty state unchanged.

**Done when:** for `BP_BaseWeapon`, `ModelScale` reads `0.0` (declared in `BP_BaseWeapon`); for `BP_Marker` it reads `1.0`; `BP_Mortar_81mm.ShouldSampleHeightForPositioning` reads `False` with parent value `True` and `isOverridden=true`.

### T2 (P1): `SetBlueprintClassDefaults`
Set one or more CDO values on one Blueprint, including **inherited** properties. Inherited properties are overridden in the child CDO; the parent is not changed.

**Input:**

| Field | Type | Notes |
|---|---|---|
| `objectPath` | string | Exact Blueprint object path |
| `values` | map | Property name → export-text value |
| `dryRun` | bool | |
| `confirm` | bool | Required unless `dryRun` |
| `compile` | bool | Default true |
| `save` | bool | Default false |
| `operationId` | string | Optional stable retry ID |

**Behaviour:**
- Transactional (undoable), using `FBlueprintEditorUtils` / CDO modify, then mark the Blueprint modified.
- Validate types: parse export text with `ImportText`, and reject on parse failure with the expected format.
- For a variable declared in this Blueprint, also update `FBPVariableDescription::DefaultValue` so the "My Blueprint" panel and the compile result agree.
- Return the previous and new value for each property.
- Fail when the Blueprint has unsaved changes unless `allowDirty=true`, to avoid saving unrelated edits.

**Done when:** setting `ModelScale=1.0` on `BP_BaseWeapon` compiles, saves only that package, reads back `1.0` through T1, and in PIE a newly created 81mm mortar shows the trajectory line.

### T3 (P2): `ResetBlueprintClassDefaults`
Remove a child override so the property inherits the parent value again (the editor's "Reset to default").

Input: `objectPath`, `propertyNames[]`, `dryRun`, `confirm`, `compile`, `save`.

### T4 (P2): container and object values in T1 and T2
Values for Array, Set and Map, with any key or value type (including user-defined enums like `E_FeatureModes`, structs, and class/soft-class references), must work as export text in both directions.

The same parser should also back `SetBlueprintLocalVariableDefault(objectPath, graphName, variableName, exportText)`. That fixes the open case `BFL_ShapePropertiesLibrary.FeatureToBasePoint.FeatureBasePoinrMap` (`TMap<E_FeatureModes, TSubclassOf<BP_BasePoint>>`), where the four weapon entries (`NewEnumerator8`/`27`/`28`/`29`) are still `None`.

### T5 (P3): defaults diff between two Blueprints
`CompareBlueprintClassDefaults(objectPathA, objectPathB, onlyDifferent=true)` should match properties by name, and list properties present on one side only.

Use case: compare a moved or reparented Blueprint with its old version (for example `/Game/__MergeTemp/theirs/BP_BaseWeapon` against `IconEntity/Weapon/BP_BaseWeapon`). This would have found the `ModelScale` 1.0 → 0 regression in one call.

## 3. Also worth fixing (seen in this session)

1. **`TraceBlueprintFlow` reads only the cached index.** On a stale index it returns `matchedStartNodes:false` with nothing traced. Add `source: live` (walk live pins), or refresh the index for that one Blueprint automatically.
2. **`InspectLiveBlueprint` in nodes mode is huge** (up to about 950 KB per 200 nodes). Add a compact mode: one line per node with its title, member, and links as `title.pin`, skipping hidden pins.
3. **`RequestUnrealShutdown` saves dirty packages silently.** In this session that overwrote a restored `BFL_ShapePropertiesLibrary`, and the library then failed to compile. Add `discardDirty=true`, or refuse when packages are dirty unless the caller confirms.

## 4. Fixture for automation tests (UGitClientSample)

Put these in `Content/Data`:

**`BP_DefaultsParent`** (Actor). Variables and their defaults:

| Variable | Type | Default |
|---|---|---|
| `ModelScale` | double | 1.0 |
| `ShouldSample` | bool | true |
| `TargetClass` | class<Actor> | None |
| `TagMap` | `Map<E_ItemRarity, Class<Actor>>` | empty |

**`BP_DefaultsChild`** (parent `BP_DefaultsParent`): overrides `ShouldSample = false`.

**`BP_DefaultsShadow`** (parent Actor): declares its own `ModelScale`, double, default 0. This mimics the reparent case.

**Tests:**
- T1 reads the values above, including `isOverridden` on the child.
- T2 sets `BP_DefaultsShadow.ModelScale=1.0` and an inherited `ShouldSample` on the child; it compiles and reads back the new values.
- T3 resets the child, and `ShouldSample` returns to `true`.
- T4 sets `TagMap` from export text, e.g. `((NewEnumerator0, "/Script/Engine.BlueprintGeneratedClass'/Game/Data/BP_DefaultsChild.BP_DefaultsChild_C'"))`.
- T5 lists `ModelScale` as different between the parent and the shadow.

## 5. Once the tools exist, the DSM follow-ups are

1. T1: confirm `BP_BaseWeapon.ModelScale` (and its children) is 1.0, or set it with T2 if the user hasn't yet.
2. T1: list every `ModelScale` and `ShouldSampleHeightForPositioning` default under `IconEntity/Weapon/**`, and compare them with the `dev_5.8` weapons using T5.
3. T4: set the four `FeatureBasePoinrMap` weapon entries.
4. Compile, save single packages, delete `Content/__MergeTemp/`, stage the merge, and commit when the user approves.
`````

</details>

<a id="source-asset"></a>

### ASSET — ToolRequest_AssetMoveReparent.md

<details>
<summary>Original specification: ToolRequest_AssetMoveReparent.md</summary>

`````markdown
# Tool request: reparent, move/rename, duplicate assets (unreal-mcp-adapter)

**Date:** 2026-11-15
**Project:** DSM `AR_GIS_MAP` (UE 5.8). Branch `bp_weapon-icon-entity-migration`, fresh from `dev_5.8`.
**Related:** `ToolRequest_BlueprintVariableDefaults.md` (implemented), `DSM_Merge_DataTable_Gap_Report.md`

## Why

The weapon migration (DSM-639) is being redone from scratch. It needs these operations, applied to six Blueprints:

| Source on dev_5.8 | Target | Operation |
|---|---|---|
| `/Game/FirstPerson/Blueprints/BP_BaseWeapon` | `.../MarkerBP/IconEntity/Weapon/BP_BaseWeapon` | move, then reparent `BP_Marker` → `BP_IconEntity` |
| — | `.../IconEntity/Weapon/Mortar/BP_Mortars` | create, parent `BP_BaseWeapon` (`CreateBlueprintAsset` already covers this) |
| — | `.../IconEntity/Weapon/Gun/BP_Guns` | create, parent `BP_BaseWeapon` (already covered) |
| `/Game/Assets/Weaponary/81_mm_Mortar/BP_BaseWeapon_Mortar` | `.../Weapon/Mortar/BP_Mortar_81mm` | move/rename, then reparent → `BP_Mortars` |
| `.../MarkerBP/MortarandGuns/BP_51mm` | `.../Weapon/Mortar/BP_51mm` | move, then reparent → `BP_Mortars` |
| `.../MortarandGuns/BP_AGS`, `BP_MMG` | `.../Weapon/Gun/BP_AGS`, `BP_MMG` | move, then reparent → `BP_Guns` |

Last time these were done with Epic's unreal-mcp `BlueprintTools.set_parent` plus manual moves. The user has now restricted work to **unreal-mcp-adapter only**, and the adapter (139 tools) has none of these. A search for "reparent", "parent", "rename", "move", "duplicate", "redirector" and "asset" finds only read tools, `CreateBlueprintAsset` and `ConsolidateAndDeleteAsset`.

## Requested tools

### A1 (P1): `ReparentBlueprint`

**Input:**

| Field | Type | Notes |
|---|---|---|
| `objectPath` | string | |
| `newParentClassPath` | string | |
| `dryRun` | bool | |
| `confirm` | bool | |
| `compile` | bool | Default true |
| `save` | bool | Default false |
| `operationId` | string | |

**Behaviour:**
- Use `FBlueprintEditorUtils` / `UBlueprint::ParentClass` change, then refresh all nodes and compile.
- **Preflight / dry-run report:**
  - Variables, functions, events, interfaces and components the Blueprint's graphs use that come from the *old* parent but don't exist on the *new* parent. These are the nodes that will break; give the graph, node GUID and member name for each.
  - Old-parent overrides that will be dropped, for example `ReceiveBeginPlay` calling `Parent: BeginPlay`, and overridden functions.
  - CDO values that are currently inherited from the old parent and will change. For example, `ModelScale` came from `BP_Marker` as 1.0, and if the Blueprint recreates the variable itself it starts at 0. **This exact case caused the trajectory bug last time.**
- **Apply:** report compile errors with graph and node GUID. Never save when compile fails unless `allowSaveWithErrors=true`.

**Done when:** `BP_BaseWeapon` is reparented to `BP_IconEntity` in dry-run, and the report lists the `BP_Marker` members it uses (`ModelScale`, `Deflection`, `Rotate3dMortar`, `Marker Icon Name`, `MarkerName`, the "Rotate 3D Mortar" RPC, etc.) with their node GUIDs.

### A2 (P1): `RenameAsset` / `MoveAsset`

**Input:**

| Field | Type | Notes |
|---|---|---|
| `sourceObjectPath` | string | |
| `destinationPackagePath` | string | e.g. `/Game/.../Weapon/Mortar` |
| `newName` | string | Optional |
| `fixupRedirectors` | bool | Default true |
| `dryRun` | bool | |
| `confirm` | bool | |
| `save` | bool | |

**Behaviour:**
- Use `IAssetTools::RenameAssets`. Then, when `fixupRedirectors` is set, fix up the redirector so referencers point at the new path (resave referencers and delete the redirector).
- Return the list of referencer packages that were dirtied or saved, and any that failed (checked out or locked).
- The dry-run lists referencers.

**Done when:** `BP_BaseWeapon_Mortar` is moved and renamed to `.../Weapon/Mortar/BP_Mortar_81mm`; every referencer (APlanDataManager, DT_EntityRegistry, BFL_ShapePropertiesLibrary, the PlayerController, etc.) loads the new path; no redirector is left.

### A3 (P2): `FixupRedirectors`
- **Input:** `packagePath` (recursive), `dryRun`, `confirm`.
- Equivalent to the editor's "Fix Up Redirectors in Folder". Returns the redirectors removed and the referencers resaved.

### A4 (P2): `DuplicateAsset`
- **Input:** `sourceObjectPath`, `destinationPackagePath`, `newName`, `dryRun`, `save`.
- Lets an old Blueprint be kept for comparison while its copy is changed.

### A5 (P3): `DeleteAsset`
- **Input:** `objectPath`, `dryRun`, `confirm`, plus `requireNoReferencers` (default true).
- Refuses when indexed referencers exist, unless `force=true`. A plain delete, as opposed to `ConsolidateAndDeleteAsset`'s replace-references.

## Safety notes
- Each of these must leave other packages' dirty state alone, or report every package it dirtied.
- They must not depend on `RequestUnrealShutdown` saving changes.
- Dry-run must never mutate, including the redirector fixup.

## Test fixture (UGitClientSample `Content/Data`)
- `BP_ReparentOldParent` (Actor): declares `ModelScale` (double, default 1) and a custom event `RotateRPC`.
- `BP_ReparentNewParent` (Actor): declares nothing.
- `BP_ReparentChild`, with parent `BP_ReparentOldParent`: its EventGraph reads `ModelScale` and calls `RotateRPC`.

**Tests:**
- A1 dry-run to `BP_ReparentNewParent` lists the `ModelScale` getter and the `RotateRPC` call as breaking, and the `ModelScale` value as changing from 1 to unset.
- A1 apply reports compile errors at those node GUIDs.
- A2 moves `BP_ReparentChild` to a `Moved/` subfolder; a Blueprint that casts to it still compiles, and no redirector is left.

---

## Follow-up (2026-11-15): self-context getters/setters for INHERITED variables (P1, blocking)

**Found while reparenting `BP_BaseWeapon` → `BP_IconEntity`.**

| Call | Result |
|---|---|
| `ApplyBlueprintGraphPatch` node `{kind: variableGet, variableName: "EntityName"}` (no owner) | `Variable 'EntityName' was not found.` (EntityName is declared in grand-parent `BP_BasePoint`) |
| same with `ownerClassPath: /Game/.../BP_BasePoint.BP_BasePoint_C` | node is created, but as a **non-self** getter with a visible, unconnected `self` (Target) pin; compile fails: `Variable node Get EntityName uses an invalid target. It may depend on a node that is not connected to the execution chain, and got purged.` |
| `AddBlueprintVariableGetNode` `variableName: "EntityName"` | `Variable was not found in the target Blueprint member variables.` |

**Need:**
- When the variable is a member of the Blueprint's own class *or any ancestor class*, create the getter/setter with `FMemberReference::SetSelfMember` (self context, Target pin hidden), exactly like dragging an inherited variable from "My Blueprint → Show Inherited Variables" in the editor.
- Apply this to `variableGet` / `variableSet` in `ApplyBlueprintGraphPatch` and to `AddBlueprintVariableGetNode` / `AddBlueprintVariableSetNode`. `ownerClassPath` should only be needed for genuinely external (other-object) access.
- Optional alternative: a `selfReference` node kind in `ApplyBlueprintGraphPatch` (K2Node_Self), so an explicit Self can be wired into the Target pin.

**Done when:**
- In a child of `BP_BasePoint`, `variableGet EntityName` (no owner) compiles with a hidden self target.
- The same holds for `IconTexture` (`BP_BasePoint`) and `DisplayWidget` (`BP_IconEntity`), from a grandchild.

This blocks the weapon migration. The reparented `BP_BaseWeapon` uses ~3 inherited getters now, and many more are expected in the children and in the EventGraph.
`````

</details>

<a id="source-inherit"></a>

### INHERIT — ToolRequest_InheritedMembersAndGraphEditing.md

<details>
<summary>Original specification: ToolRequest_InheritedMembersAndGraphEditing.md</summary>

`````markdown
# Tool request: inherited members, parent calls and graph-editing gaps (unreal-mcp-adapter)

**Date:** 2026-11-15
**Requested by:** DSM weapon migration (DSM-639) redo, branch `bp_weapon-icon-entity-migration` (UE 5.8)
**Related:** `ToolRequest_AssetMoveReparent.md` (implemented: Reparent, Move, Fixup, Duplicate, Delete), `ToolRequest_BlueprintVariableDefaults.md` (implemented)

## 0. Context: what is being done and where it stopped

The six weapon Blueprints are being moved from `BP_Marker` to the icon-entity hierarchy:

```
BP_BasePoint            (declares EntityName, EntityDescription, IconTexture, IconColor, TextColor,
 │                        PropertyList, UpdateProperty, MulticastUpdateProperty, ...)
 └─ BP_IconEntity       (declares DisplayWidget, DisplayWidgetWBP, PivotMovableMeshComponent,
     │                    As BP GIS Map Player Controller, SetEntityName, SetEntityDescription,
     │                    SetIconColor, SetTextColor, MulticastUpdatePropertyFromButton handling, ...)
     └─ BP_BaseWeapon   (reparented from BP_Marker with ReparentBlueprint)
         ├─ BP_Mortars → BP_Mortar_81mm, BP_51mm
         └─ BP_Guns    → BP_AGS, BP_MMG
```

After `ReparentBlueprint`, the graphs of `BP_BaseWeapon` must be rewired from `BP_Marker` members to `BP_BasePoint` / `BP_IconEntity` members. **The tools cannot create a working getter or setter for a variable declared in an ancestor class.** That is the blocker (section 1). Sections 2–6 are other gaps the remaining steps will hit; adding them now avoids further stops.

Current editor state (do not lose): `BP_BaseWeapon` is reparented in memory and **unsaved**, with 3 compile errors, all caused by section 1.

---

## 1. (P1, BLOCKING) Self-context getters/setters for inherited variables

### What happens today

| Call | Result |
|---|---|
| `ApplyBlueprintGraphPatch` `{kind:"variableGet", variableName:"EntityName"}` in `BP_BaseWeapon` | `Variable 'EntityName' was not found.` |
| same with `ownerClassPath:"/Game/.../BP_BasePoint.BP_BasePoint_C"` | Creates a non-self getter with a visible, unconnected Target pin. Compile error: *"Variable node Get EntityName uses an invalid target. It may depend on a node that is not connected to the execution chain, and got purged."* |
| `AddBlueprintVariableGetNode` `variableName:"EntityName"` | `Variable was not found in the target Blueprint member variables.` |

The same failures apply to `IconTexture` (in `BP_BasePoint`) and `DisplayWidget` (in `BP_IconEntity`), and to `variableSet` / `AddBlueprintVariableSetNode`.

### What is needed
- Resolve `variableName` against the Blueprint's generated class **and all ancestor classes**, both Blueprint and native: `FindFProperty` on `SkeletonGeneratedClass` walking `GetSuperClass()`.
- When the property is found on self or an ancestor, and no explicit `ownerClassPath` of an unrelated class is given:
  - build the node with `VariableReference.SetSelfMember(PropertyName)`, the way the editor does when you drag a variable from "My Blueprint → Show Inherited Variables";
  - leave the Target pin hidden, so the node compiles without a target.
- Keep `ownerClassPath` for external access (another object's variable), as today. If `ownerClassPath` names an ancestor of the edited Blueprint, still build a self-context node.
- Apply the same to `ApplyBlueprintGraphPatch` (`variableGet`, `variableSet`), `AddBlueprintVariableGetNode` and `AddBlueprintVariableSetNode`.
- Report in the result whether the node is `selfContext: true` and which class declares the variable.

### Done when (fixture in section 7)
- In `BP_GrandChild`, `variableGet ParentVar` (declared two levels up) with no owner compiles with no Target pin.
- `variableSet ParentVar` also compiles, and the setter's `Output_Get` pin exists.
- In DSM: `variableGet EntityName`, `IconTexture` and `DisplayWidget` in `BP_BaseWeapon` compile.

---

## 2. (P1) Retarget an existing variable or function node to a different member, keeping its links

After a reparent, many existing nodes reference members that were renamed or moved. Recreating them by delete+add loses pin links and positions. The EventGraph of `BP_BaseWeapon` alone has ~470 nodes.

**Examples:**

| Old member (`BP_Marker`) | New member |
|---|---|
| `Get MarkerName` | `Get EntityName` (`BP_BasePoint`) |
| `Get Widget` | `Get DisplayWidget` (`BP_IconEntity`) |
| `Get MovableMeshComponent` | `Get PivotMovableMeshComponent` (`BP_IconEntity`) |
| `UpdateName(NewMarkerName)` | `SetEntityName(EntityName)` (`BP_IconEntity`) |

**Need: `RetargetBlueprintMemberNode`**

| Field | Type | Notes |
|---|---|---|
| `objectPath` | string | |
| `graphName` | string | |
| `nodeGuid` | string | |
| `newMemberName` | string | |
| `newOwnerClassPath` | string | Optional; self or ancestor gives a self-context node |
| `pinMap` | object | Optional `{oldPinName: newPinName}` for renamed parameters |
| `dryRun` / `confirm` | bool | |

**Behaviour:**
- Works for `K2Node_VariableGet`, `K2Node_VariableSet` and `K2Node_CallFunction`, including nodes whose current member no longer resolves, such as ones left broken by a reparent.
- Keeps the node position, comment and every link whose pin still exists; pins renamed via `pinMap` keep their links.
- Reports dropped links: pin name plus the other end's node and pin.

**Done when:** the existing `Get Widget` node in `BP_BaseWeapon.UpdateProperty` is retargeted to `DisplayWidget` and keeps its link to `UpdateIconVisibility.DisplayWidget`.

---

## 3. (P1) Parent-function call nodes and function overrides

The weapon must forward common properties to its parent, so the icon-entity code handles Name, ColorPicker, TextColor, IconSize and Lock, and the weapon only handles its own properties. This needs **"Parent: X" call nodes** (`K2Node_CallParentFunction`), which no tool or patch kind can create today.

**Need:**
- A new `ApplyBlueprintGraphPatch` node kind `parentCall`, with `functionName`. It creates the parent call for the function or event the graph overrides, and validates that the parent class implements that function.
- `OverrideBlueprintFunction(objectPath, functionName)`: creates a function-override graph for a parent Blueprint function that is overridable and not an event. The entry and result signatures come from the parent; optionally a "Parent: X" call is wired through.
- Event overrides already exist (`AddBlueprintOverrideEventNode`). It needs an option `addParentCall: true` that wires `Event X → Parent: X`.

**Done when:**
- In `BP_BaseWeapon.UpdateProperty`, a `parentCall UpdateProperty` node can be connected from the Switch's `Default` exec pin, with PropertyName / Value / PropertyType passed through.
- `Parent: MulticastUpdatePropertyFromButton` can be added in the EventGraph.

---

## 4. (P2) Switch on String / Switch on Enum nodes: create and edit cases

The weapon's `UpdateProperty` uses a `Switch on String` whose case names must change to the icon-entity property names: `Colors` → `ColorPicker`, `Icon Colors` → `TextColor`, `Description` → `DETAILS`, plus new cases. Today there is no node kind or tool to create a switch or to add, rename or remove case pins.

**Need:**
- New patch kinds `switchString` (with `cases[]`) and `switchEnum` (with `enumPath`).
- **`SetSwitchCases(objectPath, graphName, nodeGuid, cases[], renames{old:new})`:**
  - renames keep their exec links;
  - removed cases report their dropped links;
  - added cases are appended;
  - the `Default` pin is kept.

**Done when:** an existing switch case `Colors` renamed to `ColorPicker` keeps its outgoing exec link.

---

## 5. (P2) Self-reference and macro nodes in `ApplyBlueprintGraphPatch`

**Need:**
- `kind: "self"` (`K2Node_Self`), to wire a Self pin explicitly. This is a fallback when section 1 does not apply.
- `kind: "macro"` with `macroPath`, for engine standard macros. The IsValid guards use `/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:IsValid`, and `ForEachLoop` / `DoOnce` are expected later.
- `kind: "cast"` with `targetClassPath` and `pure`, inside a patch, so a cast can be created and wired atomically (`AddBlueprintCastNode` exists but is standalone).

---

## 6. (P3) Quality of life seen in this session

1. **`MoveAsset` with `fixupRedirectors: true` fails on its own side effect.** The rename dirties all referencers, and fixup then refuses with *"Some referencers have unsaved edits"*, although they were clean before the call (verified with `SaveAllDirtyPackages dryRun` = 0). The move still happened (`moved: true`), leaving a redirector. Fix: snapshot dirty packages before the rename; only packages that were dirty *before* count as user edits. Workaround used: `FixupRedirectors allowSavingDirtyReferencers=true`.
2. **`ReparentBlueprint` result size:** `defaultChanges` lists every lost `BP_Marker` property (~40 rows). Add `onlyUsed: true` to report only properties that the Blueprint's graphs read or write, and properties overridden in the Blueprint or its children.
3. **`GetBlueprintClassDefaults`** needs a `declaredInOnly: "<classPath>"` filter. Today getting one class's own variables means paging through 278 rows of `AActor` properties.

---

## 7. Test fixture (UGitClientSample `Content/Data`)

| Blueprint | Parent | Declares |
|---|---|---|
| `BP_InhBase` | Actor | `ParentVar` (string, default "base"), `ParentWidget` (`WidgetComponent` component), function `SetParentVar(NewValue)`, function `Describe() -> string`, event `MulticastPing(Index)` |
| `BP_InhMid` | `BP_InhBase` | `MidVar` (int); overrides `Describe` with `Parent: Describe` |
| `BP_InhGrandChild` | `BP_InhMid` | its own `ChildVar`; one function graph `Use` and an EventGraph |

**Tests:**
1. **Section 1:** in `BP_InhGrandChild.Use`, `variableGet ParentVar` / `MidVar` / `ParentWidget` with no owner compile as self-context. `variableSet ParentVar` compiles.
2. **Section 2:** a `Get ChildVar` node wired to a string pin, retargeted to `ParentVar`, keeps the link. `SetParentVar` called via a `K2Node_CallFunction` is retargeted to a renamed function with a `pinMap`.
3. **Section 3:** `OverrideBlueprintFunction BP_InhGrandChild Describe` creates an override; a `parentCall Describe` node is wired and compiles. `AddBlueprintOverrideEventNode MulticastPing addParentCall=true` compiles.
4. **Section 4:** create a `switchString` with cases `A`, `B`; rename `A`→`C`; the link survives.
5. **Section 5:** `self`, `macro IsValid` and `cast` nodes are created and wired in one patch and compile.

## 8. After this lands, the DSM steps that continue

1. Fix the 3 remaining `BP_BaseWeapon` compile errors (section 1), compile and save.
2. EventGraph and `UpdateProperty`: forward common properties to the parent (section 3) and rename switch cases (section 4). This includes the icon/text colour bug fix: the weapons send `Colors` / `Icon Colors`, but `BP_IconEntity` handles `ColorPicker` / `TextColor`.
3. Retarget the remaining `BP_Marker`-era nodes (section 2) in `BP_BaseWeapon`, the child weapons and the referencing widgets and components.
4. Create `BP_Mortars` / `BP_Guns`, move and reparent the 81mm, 51mm, AGS and MMG, then compare class defaults with the dev_5.8 baseline (`work_dsm/Saved/WeaponMigration/dev58_weapon_baseline.md`).
`````

</details>

<a id="source-local"></a>

### LOCAL — ToolRequest_BlueprintLocalVariableNodes_DSM639.md

<details>
<summary>Original specification: ToolRequest_BlueprintLocalVariableNodes_DSM639.md</summary>

`````markdown
# DSM-639: local variable nodes through unreal-mcp-adapter

## Blocking capability

`AddBlueprintLocalVariable` works, but `AddBlueprintVariableGetNode`, `AddBlueprintVariableSetNode`, and `ApplyBlueprintGraphPatch` variableGet/variableSet cannot resolve that local. Their descriptions explicitly advertise local-variable support.

Reproduction in `C:\Users\LTX_MSI\Documents\AXIS\work_dsm\AR_GIS_MAP.uproject`:

- Blueprint: `/Game/AR_GISMap/Blueprints/Features/MarkerShape/MarkerBP/IconEntity/Weapon/BP_BaseWeapon.BP_BaseWeapon`
- Function: `ProjectilePathPrediction`, GUID `c8e25862-4de8-c804-59c6-e6b4518f3d8e`
- Add local `PredictionActorsToIgnore`, object `/Script/Engine.Actor`, container array: succeeds.
- Add a get/set of that local in the same graph: fails with `Variable 'PredictionActorsToIgnore' is missing, inaccessible or read-only for this operation.`
- Current `BlueprintMemberToolUtils::ResolveVariable` looks only for a class FProperty; `CreateVariableNode` uses SetSelfMember, never SetLocalMember.

Please resolve function locals using the selected graph, set scoped local member references with the declaration GUID, retain external/class variable handling, and support both primitive node tools and graph patches. Cover same-named locals in different functions, actor arrays, get/set wiring and compilation, and dry-run/rollback behavior.

## Intended functional patch after tool support

Copy `ProjectilesArray` into a function-local actor array; AddUnique Self to the copy; pass the copy to Predict Projectile Path (Advanced). Preserve the original projectile array because destruction code iterates it. Do not add Self to the original array.

Predict node: `3a92d2e7-4e26-59d7-d25f-57adb6948ae3`; ignore input: `PredictParams_ActorsToIgnore`. Current source is `ProjectilesArray` getter `0f51bfe3-4641-5dd7-b5da-ae867a375e7d`. Execute after ClearSplineMesh (`45cf4def-4f04-8c9c-b826-9fb959d21406`) and before prediction.

Evidence for suspected self-hit: after toggling trajectory, the user's screenshot shows a line terminating at the muzzle; prediction traces WorldStatic with radius 10 and ignores projectiles only. The new BP_IconEntity parent adds PivotMovableMeshComponent with BlockAllDynamic, QueryAndPhysics. Self-hit remains a hypothesis pending PIE hit-actor/path-data verification. Preserve terrain collision and trajectory visibility defaults.

Do not reconnect the disconnected ForEach in ProjectilePathPrediction on this evidence: the existing DrawPredictedPath event already populates Spline_0 and performs decal/distance updates. Reconnecting would duplicate that work, including during GetAnalysisPoint sampling.

## Visual validation gap

The attached adapter catalog (154 tools) has no SlateInspectorToolset Snapshot/Screenshot or graph framing route. Expose a read-only explicit-widget/window capture and graph framing capability through the adapter so edited graph areas can be reviewed without computer use.

## State left by investigation

The temporary local variable and unconnected AddUnique node used for capability testing were removed. The original ProjectilePathPrediction graph revision and 37-node count were restored; BP_BaseWeapon compiled with 0 errors and 0 warnings. No functional patch was saved, staged or committed. The git index hash stayed unchanged.
`````

</details>

<a id="source-ref"></a>

### REF — Requirements_BlueprintRefactoring.md

<details>
<summary>Original specification: Requirements_BlueprintRefactoring.md</summary>

`````markdown
# Requirements: large-scale Blueprint refactoring through MCP

Gaps observed when an agent used `unreal-mcp-adapter` together with Epic's official
`BlueprintTools` to refactor a real Blueprint-heavy project: reparenting class hierarchies,
moving and renaming assets, deleting retired classes, changing a save format, and fixing the
resulting compile and runtime errors. Each item states the failure, the requirement and an
acceptance check. Cross-references to `ROADMAP.md` items (R1-R15) are given where related; verify
the current build before reopening an item marked done there.

Priority: **P1** crash, data loss or blocked task · **P2** workaround or manual UI step needed ·
**P3** quality of life.

---

## A. Editor stability

### Q1 (P1) Index refresh must not crash after bulk graph edits
- **Failure:** after deleting ~150+ nodes inside a collapsed (composite) graph, the index refresh
  run by `CompileBlueprint` (`HandleRefreshProjectIndex`) crashed the editor; unsaved edits were
  lost. Partial refresh also hits `Ensure: Work overflow in slow task` in `RefreshProjectIndex`.
- **Requirement:** refresh is crash-safe after node deletion (no stale node/pin pointers), slow
  task work is accounted correctly, and callers can skip the refresh.
- **Accept:** automation test deletes 200 nodes in a composite graph, compiles, refreshes; no
  crash, no ensure.

### Q2 (P1) Shutdown must not save unrequested packages
- **Failure:** `RequestUnrealShutdown` wrote every dirty package, including assets dirtied only by
  loading/compiling and a Blueprint with compile errors.
- **Requirement:** `RequestUnrealShutdown(dirtyPolicy: fail | discard | save)`, default `fail`,
  returning the dirty package list.
- **Accept:** with an unsaved edit pending, default shutdown refuses and lists it; `discard` exits
  without writing.

### Q3 (P2) Reliable wait primitives (R14)
- **Failure:** shutdown reported "not running" while the process still held file handles; a file
  write failed with "Device or resource busy", and a second editor started too early.
- **Requirement:** `WaitForUnrealExit(timeout)` (process and package file handles released) and
  `WaitForUnrealReady(timeout)`.

---

## B. Graph editing

### Q4 (P1) Exec rewiring in one declarative patch
- **Failure:** `ApplyBlueprintGraphPatch` rejected "execution target already has a route" when
  (a) the same patch disconnects A→B and connects A→N→B (routes are validated before the patch's
  own disconnections), and (b) a second exec link enters an input pin (legal in Blueprints).
- **Requirement:** apply disconnections before route validation; allow multiple exec inputs;
  add a `spliceExec` operation (insert node N between exec pins A and B).
- **Accept:** "insert a validity gate before node X, false path jumps to Y" is a single patch.

### Q5 (P2) Inherited variable get/set in patches (R4)
- **Failure:** `variableGet`/`variableSet` of a variable declared on a parent class fails; with
  `ownerClassPath` it creates a non-self node that needs a target.
- **Requirement:** resolve variables through the class hierarchy; create self-context nodes.

### Q6 (P2) Retarget function-call, variable and event nodes, persistently (extends R2)
- **Failure:** official `retarget_node_class` moved a call node to a new owner class; the
  Blueprint compiled and saved, but after reload the node referred to the old class again.
- **Requirement:** native retarget for `CallFunction`, variable get/set and event nodes that
  rewrites `FMemberReference`, keeps links whose pins still exist and reports dropped links.
- **Accept:** test retargets, saves, reloads the package and re-reads the node's owner.

### Q7 (P2) Exact node variant on creation
- **Failure:** names that exist as both a pure function and an exec macro (for example
  `IsValid`) produced the macro even when a declaring class was given.
- **Requirement:** create by exact owner + function name or macro path; optional `isPure`.

### Q8 (P2) Prune unreachable nodes
- **Failure:** rewriting a function body with the DSL left the previous nodes in the graph as an
  orphaned, still-connected chain that kept producing compile errors.
- **Requirement:** `PruneUnreachableNodes(graph, dryRun)` — keep nodes reachable from entry
  points via exec plus their data dependencies; report and remove the rest.

### Q9 (P2) Validity guard helper
- **Failure:** fixing "Accessed None" runtime errors required manual splice + IsValid wiring each
  time.
- **Requirement:** `WrapWithIsValid(node, objectSource, elseTarget?)`.

### Q10 (P3) Named Switch-on-String/Name cases
- **Failure:** the DSL cannot create named cases on a new switch node.
- **Requirement:** `SetSwitchCases(node, names[])` / add named case pin.

---

## C. Data assets

### Q11 (P1) Edit `UserDefinedStruct` and `UserDefinedEnum`
- **Failure:** could not add a member to a Blueprint struct or a value to a Blueprint enum, which
  forced a C++ workaround for a save-format change and reuse of an unrelated enum value.
- **Requirement:** add/remove/rename struct members and set defaults; append enum values and set
  display names; recompile dependents and report affected pins. Enum reordering stays refused.
- **Accept:** add a struct member and an enum value; dependent Blueprints compile; existing
  serialized data still loads.

### Q12 (P2) Local-variable defaults (extends R3)
- **Failure:** a lookup table stored as a function-local `TMap` default could not be read; the
  agent had to parse `.uasset` bytes.
- **Requirement:** `GetLocalVariableDefault` / `SetLocalVariableDefault`, including map, set and
  array literals.

---

## D. Assets and references

### Q13 (P1) Delete must delete, or say why not
- **Failure:** deleting an asset with no referencers returned `true`, yet the file stayed on disk
  and the asset still existed.
- **Requirement:** native delete that verifies the package file is gone and otherwise returns the
  reason (loaded, in use, source-control lock).

### Q14 (P2) Scoped Fix Up Redirectors
- **Failure:** no tool existed; a cleanup triggered elsewhere removed redirectors outside the
  intended folders.
- **Requirement:** `FixUpRedirectors(paths[], dryRun)` limited to the given paths, returning
  redirectors removed and referencers re-saved.

### Q15 (P2) Move/rename with a change report
- **Failure:** moving assets silently dirtied many referencers.
- **Requirement:** `MoveAsset` returns redirectors created and referencers updated, with an option
  to save them in the same transaction.

### Q16 (P2) Fast node search inside one large Blueprint (relates to R7/R9)
- **Failure:** reading every graph of a ~100-graph Blueprint timed out; only a possibly stale
  index could locate nodes by pin type.
- **Requirement:** `FindNodesByPinType(blueprint, class)` and `FindNodesByMember(...)` returning
  graph, node GUID and pin without serializing whole graphs.

---

## E. Compile diagnostics

### Q17 (P2) Node-level compile messages including warnings (verify R11)
- **Failure:** compile errors arrived as text only (no graph/node/pin); the official compile tool
  returns `null` on success and hides warnings.
- **Requirement:** each message `{severity, text, graph, nodeGuid, pin}`; warnings included.

### Q18 (P3) Refresh all nodes
- **Failure:** after a parent change, stale errors remained that no node search could locate and
  only an editor restart cleared.
- **Requirement:** `RefreshAllNodes(blueprint)` (reconstruct every node) before compiling.

---

## F. Packaged workflows (skills)

- **Reparent with member port:** snapshot CDO overrides → change parent → re-add or remap
  variables the old parent supplied → rebuild self getters → compile children → save touched only.
- **Retire a class:** find usages to the pin (Q16) → rewire or remove branches → delete (Q13) →
  fix up redirectors (Q14) → compile referencers → confirm no unexpected dirty packages.
- **Save-format migration:** native struct and parser → save/load graph functions → legacy load
  fallback → offline converter with tests.

## Suggested order
Q1, Q2, Q4, Q11, Q13 → Q6, Q17, Q5, Q8, Q14, Q16 → remaining items.
`````

</details>

<a id="source-ready"></a>

### READY — UnrealMCP_MergeReadiness_Requirements.md

# Unreal MCP: DSM dev merge readiness requirements

Reviewed: 2026-11-23. Target project: `C:/Users/LTX_MSI/Documents/AXIS/work_dsm`.

## Evidence and current state

- `git ls-files -u` returned no conflicts. The merge has not been started by this agent; there is no actual conflict reproduction yet.
- The worktree contains modified/deleted assets and source files plus untracked weapon assets and C++ migration files. Preserve this work before merging; do not automatically reset, stash, stage or commit it.
- Official `unreal-mcp-8001/list_toolsets` failed to connect to `http://127.0.0.1:8001/mcp`. The adapter search returned `source: disk_cache`, `availabilityVerified: false`. Live availability and exact runtime schemas remain unverified.
- Local evidence: `Plugins/GraphXPort/docs/ConflictToolset.md`, its `GraphXPortConflictToolset.h`, and `Plugins/UnrealMCP/docs/MCP-Tool-Index.md` in work_dsm. Source presence proves implementation/API presence, not that the editor loaded it.
- The earlier `UnrealMCP_WeaponMigration_Requirements.md` was absent from this destination on this review. This report is a new file; no existing report was replaced.

## Existing capabilities to retain and verify live

GraphXPort's official Toolset Registry route already declares:

- `ListConflicts`: paths and Git stage identities.
- `InspectRevision`, `CompareRevisions`: historical graph inventory, nodes/pins and directional graph differences.
- `CompareBlueprintDefaults`: changed CDO property paths and before/after values.
- `OpenBlueprintMerge`, `InspectMergeNodes`: bounded review and selected-node evidence.
- `PreviewMergeResolution`, `ApplyMergeResolution`: explicit native graph patch, revision guards, compile/rollback and separate optional save/stage.
- `CloseBlueprintMerge`: release review evidence.
- `StartFromRevision`: explicit wholesale current/incoming replacement. This is not an automatic conflict-resolution step and requires user confirmation for that asset and side under DSM rules.

These workflow tools are intentionally exposed through the official server, not mirrored in the adapter catalog. A zero-result adapter search does not prove they are missing. Current header/docs contain more methods than the older index lists; discovery must use the live toolset schema.

## Requirements and acceptance criteria

### M1 — P1: reliable project-bound readiness and recovery

Need: diagnose process, selected project, loaded plugin and HTTP transport separately; reconnect to the existing editor without opening another project or changing selection.

Pass: both catalogs can be queried for the exact work_dsm editor; results identify live versus cached evidence. Failure reports the specific unavailable layer. Background windows and tab changes do not lose the session.

### M2 — P1: whole-Blueprint three-way evidence

Documented gap: graph and CDO comparisons do not cover variable declarations, component trees, class settings or arbitrary asset properties. Zero graph/default differences cannot establish whole-asset equivalence.

Need: compare base/current/incoming declarations and defaults, function signatures and locals, parent class, interfaces, component hierarchy and component overrides, relevant class settings and asset references. Return stable identities, exact changed values and explicit unsupported coverage.

Pass: detect a parent change, interface addition, parameter type change and component override change even when graph differences are empty. Historical reads must not mutate working packages.

### M3 — P1: guarded non-graph resolutions

Documented boundary: the merge workflow delegates an authored graph patch; that does not establish support for resolving every declaration, class/default or component conflict transactionally.

Need: preflight and apply explicit compatible changes beyond graphs, with source-stage identities, dirty-state checks and rollback. Support dependency-ordered operations when a changed signature or parent requires caller reconstruction.

Pass: preserve compatible edits from both sides; on failure restore the affected state and report any unsaved/touched packages. An unsupported operation must stop explicitly rather than silently omit part of a resolution. Do not resolve by taking an entire side unless the user approves that asset and side.

### M4 — P1: preserve weapon migration contracts during dev merge

Need: focused checks for the contracts below, using declarations/defaults and live graph wiring rather than serialized strings alone:

- Separate top-level `guns[]` and `mortars[]`, with their separate data structs and correct object types.
- Ordinary `iconEntity` save/load behavior remains intact.
- Native runtime plan migration and its ambiguity/data-preservation behavior survive.
- APlanDataManager's native parent/API and Blueprint call sites remain compatible.
- Weapon spawn registry, networking parameter types, property panel integration and required marker dependencies retain compatible changes from dev.
- The local-variable refactors in `InitializeMilitarySymbol` and `InitializeNewMilitarySymbol` preserve original calls, branches and input sources.

Pass: reviewers can account for both sides' changes for each affected contract. Compile all affected dependencies and callers; use focused save/load/spawn runtime checks after resolution when authorized. Compilation alone is not runtime proof.

### M5 — P2: large, add/delete and missing-dependency conflicts

Documented limits: bounded review rejects add/delete cases and more than 200 graph/default differences per side. Historical loads may fail when dependencies are missing. Legacy paged reads exist; they are a fallback, not a missing feature.

Need: resumable paged review with stable stage identities; clear support/fallback for absent stages, renames and missing historical dependencies. Never fabricate an empty asset from a failed historical load.

Pass: a large Blueprint can be fully reviewed without truncation being mistaken for completeness. Add/delete conflicts explicitly identify the missing side and require a deliberate resolution. Missing dependencies are named and unresolved evidence stays marked incomplete.

### M6 — P2: multi-graph review lifetime and atomicity

Documented limits: at most two sessions, ten-minute idle expiry, and bounded single-graph patch plans.

Need: renew or reopen a review without losing authored decisions; validate a sequence of graph/declaration changes against the same stage identities. Explicitly report the transaction boundary when multiple patches cannot be atomic.

Pass: stale stages, disk edits or new unsaved changes invalidate execution. A later patch failure cannot leave earlier saved changes described as a fully rolled-back merge. Retries return accurate receipts without duplicate mutation.

### M7 — P2: inspectable layout and validation receipts

Need: exact nodes/pins/defaults after edits, readable explicit-window Slate captures of affected regions, structured compile diagnostics, and separate save/dirty/stage results. Captures must work in the background and distinguish pending framing from an empty capture.

Pass: edited graph text is readable, no important nodes are clipped, comments/layout are inspected, and the saved asset is clean. The receipt distinguishes structural, compile, visual and runtime checks. No staging or commit occurs in this task; any later staging requires authorization.

## Procedure once conflicts actually exist

1. Record unmerged paths and stage blobs; inspect editor dirty state without discarding work.
2. Discover both live routes and fetch exact schemas. Use GraphXPort to inspect base/current/incoming, including graph and default changes; supplement documented coverage gaps.
3. Explain behavioral conflicts and obtain decisions where intent is unclear. Preserve compatible changes from both sides. Seek the required separate approval before any optional Blueprint cleanup.
4. Preflight targeted edits, apply through MCP, verify wiring/defaults/declarations and readable graph views, compile, then save only reviewed assets.
5. Report unresolved evidence and runtime limitations. Do not mark the Git conflict resolved merely because the Blueprint compiles; do not stage or commit without authorization.

No merge, asset edit, build, PIE, staging or commit was performed for this report. Runtime schemas and actual conflict-specific gaps must be verified after the editor connection and merge are available.
