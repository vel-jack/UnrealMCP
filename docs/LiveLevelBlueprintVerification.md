# Live Level Blueprint verification — September 4, 2026

## Result

The latest `UnrealEditor-UnrealMCP-9043.dll` passed `UnrealMCP.Blueprint.LiveInspection.EmbeddedLevelAndTrace` with zero errors and zero warnings after editor restart. Live MCP inspection resolved the embedded Level Blueprint in `BaseLevel_3D_AR_Collab` through current-level context, map path, Blueprint object path, and direct graph object path.

All 11 graphs were enumerated. The entire EventGraph (154 nodes) and the `ApplicationOpenedUsingStartLevelFlag` macro (8 nodes) were inspected without pagination gaps. Results reported `source=live_editor` and `indexUsed=false`.

## Verified camera-position wiring

The exact EventGraph execution route is:

1. `ReceiveBeginPlay` (`39d1e202-48ec-a9d2-962f-b28b05b205d8`).
2. Two Sequence nodes and a reroute.
3. Delay **2 seconds** (`00b1a8ff-4074-9279-73f8-6f9b437c19f5`).
4. `GetActorOfClass`, with `APlanDataManager_C` as its authored class.
5. `AC_GameState.ExtractGameStateFromJson` (`bd8cd01b-4c27-e0d9-8d0a-c6bba3ad0fba`).
6. Delay **1 second** (`79ebf9be-4835-df2e-efcc-02a206962156`).
7. `K2_SetWorldLocation` (`19d50f28-4ed3-d07f-757f-848427c9a048`).
8. Reroute (`5493a8a1-4600-2776-b727-f0a06cc8a9dc`).
9. `ApplicationOpenedUsingStartLevelFlag` macro (`beebb777-4d00-9994-e2d2-ff8d5c202560`). Its graph checks command-line arguments for the `startmap=` prefix.
10. The macro's `then` output links to `APlanDataManager.Load Plan` (`796bcfd1-43b6-1f67-f50b-cbae6ce0ddfe`). Its `else` output and the call's outgoing `then` pin are unconnected in the Level Blueprint.

`SetWorldLocation.self` comes from `GetPlayerPawn(0).RootComponent`, with an exact linked target pin. This establishes the player pawn's root scene component as the object being moved; it does not establish the runtime pawn instance or a separate camera component as the direct target.

`NewLocation` is wired through a reroute from `ExtractGameStateFromJson.OutputPin`. That function's independent live trace completed with **48 nodes, 65 edges, no truncation, no unresolved boundaries**, and `coverageComplete=true`. The graph reads the project Saved directory plus `GameState.json`, parses `pawnTransform.translation` x/y/z into the returned vector, and supplies `savedfilepath` to the subsequent `Load Plan` call. Rotation/scale parsing and a `PawnTransform` assignment are also present in the function; this does not prove that this Level Blueprint branch applies camera rotation.

## Coverage and read-only qualification

- Full transitive BeginPlay expansion reached the configured 1,000-node limit (1,716 edges); a second trace starting at `SetWorldLocation` also reached 1,000 nodes (1,907 edges). Both correctly reported truncation/incomplete coverage. Expansion enters the large plan-loading and shape-restoration implementation, including dynamic interface boundaries. Exhaustive transitive coverage of those external systems is **not claimed**.
- Every downstream execution node in the Level Blueprint after `SetWorldLocation`, its macro body, and the saved-position extraction function were inspected. Their exact links and native/cross-Blueprint call boundaries are available through MCP.
- All 11 Level Blueprint graph revisions remained identical before and after testing. The selected actor remained `Cesium3DTileset_0` with the same reported transform.
- The map started clean and was dirty by the first broad trace. The editor log records a Cesium tileset URL change at 07:15:26 UTC (between the initial snapshot and the trace), followed by another tileset reload at 07:15:57 UTC. The user subsequently confirmed they were experimenting with Cesium settings during the checks. The dirty-state observation is therefore explained by concurrent user editing, rather than evidence of an inspection defect. Later inspections preserved its already-dirty state; `AC_GameState` remained clean.
- No PIE, Blueprint compilation, explicit mutation, saving, or index rebuild/refresh was invoked by these inspection calls. Graph revisions and selection stayed unchanged, and the controlled regression passed clean/dirty preservation checks. This live run is not a controlled clean-to-clean comparison because the user was editing concurrently. No dirty flag was cleared to conceal the change.

The raw MCP response captures are local temporary verification artifacts named `unreal-camera-*.json` in the user's local Temp directory. Runtime camera behavior requires separate testing.
