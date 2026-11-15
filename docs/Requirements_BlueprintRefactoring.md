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
