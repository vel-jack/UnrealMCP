---
name: unrealmcp-retire-class
description: Retire or replace a Blueprint class by finding exact usages, reconciling callers, and checking references before deletion.
---

# Unrealmcp Retire Class

1. Attach to the exact project and use `FindClassUsages` for the exact class. Review subclasses, defaults, pin references and DataTable use. Check coverage flags and inspect candidate graphs from live evidence.
2. Confirm the replacement direction and each asset authorized for change. Reparent descendants or retarget nodes and fix references before removing the old class. Validate relevant tables and compile owners and callers.
3. Dry-run `ConsolidateAndDeleteAsset` when replacing equivalent references, or `DeleteAsset` for a dependency-free asset. Review blocked referencers and any changes to package ownership. Run the authorized operation, then verify the old asset is gone. Use scoped `FixupRedirectors` when needed and inspect dirty referencers before authorizing their saves.
4. Recheck class usages and saved packages. List touched, saved and dirty packages. Treat failures on later packages separately from earlier saves.
