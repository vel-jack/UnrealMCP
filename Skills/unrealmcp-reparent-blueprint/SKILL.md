---
name: unrealmcp-reparent-blueprint
description: Reparent an Unreal Blueprint and port inherited members, defaults, components, and callers through UnrealMCP.
---

# Unrealmcp Reparent Blueprint

1. Verify the exact project and live tool schemas through the UnrealMCP adapter. Capture parent, effective class and component defaults, and a `CompareBlueprintGraphPreservation` baseline.
2. Dry-run `ReparentBlueprint`. Review lost members, inherited defaults, overrides, interfaces and breaking nodes. Use `FindClassUsages` for relevant descendants and callers. Keep the original graph and defaults evidence until readback.
3. Apply the reviewed parent change and use native member, defaults, component and graph tools for approved ports. Repoint broken references by exact graph/node GUID and preserve pin links. Use `RefreshBlueprintCallSites` for signature changes where needed.
4. Compile the owner and affected dependents, then compare graph preservation and re-read defaults. Use `SaveValidatedBlueprints` for the explicitly intended packages. Report compile, save and remaining dirty package results separately. Each package save has its own failure boundary.
5. Record any remaining runtime checks without treating static evidence as runtime proof.
