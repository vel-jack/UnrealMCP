# Blueprint defaults tooling handover

Target project: `C:\dev\Unreal\UE_5.8\UGitClientSample\UGitClientSample.uproject`. No DSM project or plugin checkout was changed.

## Available tools

- `GetBlueprintClassDefaults` reads paged CDO values, parent values, declaring classes, pin types, and override state.
- `SetBlueprintClassDefaults` validates Unreal export text on a transient copy, refuses pre-existing dirty packages unless `allowDirty=true`, requires `confirm=true`, compiles by default, and saves only with `save=true`. It supports inherited properties and updates a Blueprint's declared-variable default.
- `ResetBlueprintClassDefaults` sets inherited properties back to their parent CDO values through the same guarded setter.
- `CompareBlueprintClassDefaults` compares values and properties present on only one side.
- `SetBlueprintLocalVariableDefault` validates export text against the compiled local property's reflected type, including enum-keyed maps.
- `TraceBlueprintFlow` refreshes only the selected Blueprint's index and retries once when indexed start nodes are missing; the result reports `indexRefreshed`.
- `InspectLiveBlueprint` accepts `outputMode="compact"` in nodes mode. Each node includes its GUID, graph, title, member, and visible `pin -> title.pin` links, without a pin dump.
- `unreal.adapter.RequestUnrealShutdown` accepts `discardDirty=true` to explicitly discard unsaved package changes before closing the selected editor. If shutdown remains pending, it restores the dirty flags. The default leaves Unreal's save prompt in control.

## Sample fixtures and verification

The local sample project has `Content/Data/BP_DefaultsParent`, `BP_DefaultsChild`, and `BP_DefaultsShadow`. The parent has `ModelScale=1`, `ShouldSample=True`, `TargetClass=None`, and an empty enum-keyed `TagMap`. The child overrides `ShouldSample=False`. The shadow declares its own `ModelScale=0`.

The canonical UE 5.8 editor build passed. Focused `ClassDefaults.Live`, `LocalVariables.Live`, `LiveInspection.EmbeddedLevelAndTrace`, and `SaveBlueprint.Live` automation tests passed. The adapter regression suite passed. Saved CDO values were read after an editor restart, including the inherited bool and enum-keyed map. A stale indexed trace found a newly added sample node after one targeted refresh. The sample adapter's discard shutdown closed the editor without changing the fixture file hash; the unsaved node was absent after reopening. The final bundled native catalog contains 139 tools.

## Adapter handoff

Build output: `Adapter/UnrealMCP.Adapter/bin/Release/net9.0-windows/UnrealMCP.Adapter.exe`. Reconnect the client to this executable to load the new adapter behavior. Select the exact UE 5.8 sample `.uproject` before native calls. The existing adapter process will keep its old code until reopened.

No DSM asset edit, merge action, or PIE test was performed. The three fixture `.uasset` files are local to UGitClientSample; its outer Git repository has no remote, so they are not part of this plugin repository's push. The plugin automation test recreates equivalent transient fixtures.
