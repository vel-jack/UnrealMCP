---
name: unrealmcp-resolve-blueprint-conflict
description: Review and selectively resolve binary Unreal Blueprint Git conflicts using standalone UnrealMCP discovery, replay, apply and staging.
---

# Unrealmcp Resolve Blueprint Conflict

1. Verify the selected project and live schema. Run `ListBlueprintGitConflicts` and `CompareBlueprintMergeStructure` with graph bodies. Page via `nextCursor` until all evidence is reviewed. Keep stage identifiers and paths. Name missing dependencies and seek a deliberate decision for absent or unreadable stages.
2. Use `PreviewBlueprintSideReplay` for compatible selected graph edits. Review blockers and author the remaining targeted steps. Use `InspectBlueprintCommit` for older logic by exact commit/path if needed. Whole-side replacement requires explicit approval for the exact asset and side.
3. Keep the original `ApplyBlueprintMergePlan` payload and UUID. Preview and confirm each checkpoint against the reviewed revision. Check compile, save and dirty results. Earlier checkpoint saves survive a later failure. On timeout, use the same operation ID to inspect status. Treat an indeterminate receipt as requiring inspection.
4. Compare saved wiring, defaults and references against the intended result. Use capture or runtime validation where authorized. On explicit approval, preview `ApplyBlueprintConflictStage` and apply with the exact token, then confirm resolution with `ListBlueprintGitConflicts`. Stage only reviewed assets. Commit only when explicitly requested.
