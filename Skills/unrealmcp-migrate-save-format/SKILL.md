---
name: unrealmcp-migrate-save-format
description: Implement and verify a reversible save-format migration in an Unreal project, including legacy load, Blueprint caller updates and an offline converter.
---

# Unrealmcp Migrate Save Format

1. Identify the old and new schemas, read existing fixtures, and record the field mappings, unknown data, duplicates and ambiguity rules. Keep original test saves and define a rollback path before changing a format.
2. Locate native data declarations, parsers, Blueprint save/load entry points and callers. Use UnrealMCP class usages and graph snapshots to document affected assets and behavior. Keep editor package edits in the selected project.
3. Add the native declaration and parser, then update the Blueprint save/load paths. Retain legacy loading with explicit detection. Have the offline converter use the same mapping rules and write new output before any authorized replacement of originals.
4. Test legacy load, new-format round trips, repeat conversion, corrupt input, ambiguous records and unchanged unrelated records. Compile and check Blueprint call boundaries; run runtime or converter tests only where authorized.
5. Save reviewed packages and report receipts individually. Deliver converter usage and test results alongside any remaining runtime checks.
