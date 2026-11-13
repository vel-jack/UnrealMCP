# MCP Tool Index

Generated metadata only; fetch the exact live schema before execution.

Custom catalog captured: 2026-11-13T09:49:50.1183121Z. Official catalog captured: 2026-10-30T09:56:38.641Z.
Contains 139 custom tools and 838 official tools across 53 toolsets.

Search this file with `rg -i 'interface|delete.*node|merge'`. Descriptions are shortened; a missing keyword is not proof a capability is absent.

Custom route: `unreal.adapter.SearchTools` -> `GetToolSchema` -> `CallTool` (name is the native tool name). Full adapter mode also exposes native tools directly.
Official route: `describe_toolset(toolset_name)` for the exact schema, then `call_tool(toolset_name, tool_name, arguments)`.
If missing: refresh the custom manifest and re-list, or list official toolsets and describe the relevant one. Cached metadata does not prove current availability.

Refresh from snapshots: `python Scripts/refresh_tool_index.py`. Refresh official metadata live: add `--official-url <MCP HTTP endpoint>`. To use a freshly captured custom catalog, add `--custom-catalog <path>`.

| Server | Toolset | Tool | Short description |
|---|---|---|---|
| Custom | `native registry` | `AddBlueprintArrayOperationNode` | Adds a typed Blueprint array operation node and resolves its wildcard array and item pins. |
| Custom | `native registry` | `AddBlueprintBranchNode` | Adds a collision-aware Branch node to a stable Blueprint graph. Supports explicit or relative placement, dry-run, and optional save. |
| Custom | `native registry` | `AddBlueprintCastNode` | Adds a typed Dynamic Cast node with optional pure mode, collision-aware placement, dry-run, and optional save. |
| Custom | `native registry` | `AddBlueprintCommentNode` | Adds a bounded Blueprint graph comment with explicit text, collision-aware placement, size, dry-run, and optional save. |
| Custom | `native registry` | `AddBlueprintComponent` | Transactionally adds one component to a Blueprint SCS with optional editable template property defaults. Idempotent by component name. |
| Custom | `native registry` | `AddBlueprintComponents` | Prevalidates and transactionally adds an ordered component set to one Blueprint, then optionally compiles, saves, and partially refreshes the... |
| Custom | `native registry` | `AddBlueprintCustomEventNode` | Adds a uniquely named Custom Event to a stable Blueprint graph with collision-aware layout, dry-run, and optional save. |
| Custom | `native registry` | `AddBlueprintDelegateBroadcastNode` | Adds a Call Delegate node for an exact Blueprint-callable multicast delegate property. |
| Custom | `native registry` | `AddBlueprintDelegateEventNode` | Adds a uniquely named Custom Event whose parameter pins exactly match a reflected Blueprint-assignable multicast delegate signature. |
| Custom | `native registry` | `AddBlueprintDelegateNode` | Adds a Bind or Unbind node for an exact Blueprint-assignable multicast delegate property on a reflected owner class. |
| Custom | `native registry` | `AddBlueprintFunctionCallNode` | Adds a function-call node from an exact owner class and function name with collision-aware placement, dry-run, and optional save. |
| Custom | `native registry` | `AddBlueprintFunctionParameter` | Transactionally adds one typed input or output parameter to a Blueprint function graph identified by graphGuid. |
| Custom | `native registry` | `AddBlueprintInterface` | Attaches an exact Blueprint interface transactionally, with dry-run, duplicate/inherited detection, collision refusal, compile rollback and... |
| Custom | `native registry` | `AddBlueprintInterfaceEventNode` | Adds an implemented Blueprint interface function as an event node in an exact Ubergraph, with idempotent duplicate detection, dry-run, and... |
| Custom | `native registry` | `AddBlueprintInterfaceFunctionGraph` | Creates the signature-correct function-graph implementation for a non-event Blueprint interface function, including output and return signatures. |
| Custom | `native registry` | `AddBlueprintLocalVariable` | Transactionally adds a typed local variable to one Blueprint function graph (type/containerType/valueType as in AddBlueprintVariable). Idempotent... |
| Custom | `native registry` | `AddBlueprintMacroNode` | Adds a macro instance from a Blueprint macro library, including StandardMacros such as ForEachLoop. |
| Custom | `native registry` | `AddBlueprintMapOperationNode` | Adds a persistent typed Blueprint Map operation node with independently resolved key and value pins. |
| Custom | `native registry` | `AddBlueprintOverrideEventNode` | Adds an overrideable parent-class Blueprint event to an exact Blueprint graph with collision-aware placement, dry-run, duplicate rejection, and... |
| Custom | `native registry` | `AddBlueprintRerouteNode` | Adds a wildcard Reroute node with collision-aware placement. Its pin type specializes when connected. |
| Custom | `native registry` | `AddBlueprintSequenceNode` | Adds a collision-aware Sequence node to a stable Blueprint graph. Supports dry-run and optional save. |
| Custom | `native registry` | `AddBlueprintSetOperationNode` | Adds a persistent typed Blueprint Set operation node with resolved element pins. |
| Custom | `native registry` | `AddBlueprintTypedOperatorNode` | Adds an exact object, Boolean, or Vector operator node without ambiguous function lookup. |
| Custom | `native registry` | `AddBlueprintVariable` | Transactionally adds a typed Blueprint member variable with optional default, instance-editable metadata, compile, and save. |
| Custom | `native registry` | `AddBlueprintVariableGetNode` | Adds a variable Get node for an exact Blueprint member with collision-aware placement, dry-run, and optional save. |
| Custom | `native registry` | `AddBlueprintVariableSetNode` | Adds a variable Set node for an exact Blueprint member with collision-aware placement, dry-run, and optional save. |
| Custom | `native registry` | `AddDataTableRow` | Transactionally edits one DataTable row in memory; leaves the asset dirty for an explicit save. |
| Custom | `native registry` | `AddEnhancedInputActionNode` | Adds an Enhanced Input Action event node for an exact InputAction asset without requiring Enhanced Input as a hard plugin dependency. |
| Custom | `native registry` | `AddInputMappingContextMapping` | Adds one key-mapping row to an existing InputMappingContext. Idempotent if the identical Action+Key row already exists. |
| Custom | `native registry` | `AnalyzeBlueprintCoupling` | Ranks project Blueprint assets by indexed package dependency coupling. This is static package-reference analysis, not runtime call coupling. |
| Custom | `native registry` | `AnalyzeFeatureBoundary` | Analyzes static indexed package dependencies crossing a required /Game feature boundary. It does not infer runtime architecture. |
| Custom | `native registry` | `AnalyzeProjectArchitecture` | Returns a compact, fault-tolerant static architecture overview composed from the indexed cycle, coupling, broken-reference, unused-candidate, and... |
| Custom | `native registry` | `ApplyBlueprintGraphPatch` | Atomically preflights and applies a bounded declarative Blueprint graph patch with deterministic node identities, exact data-link replacement,... |
| Custom | `native registry` | `ApplyBlueprintInteractionPlan` | Preflights and idempotently applies Blueprint interaction nodes, exact named-pin connections, and safe Sequence splices that preserve an existing... |
| Custom | `native registry` | `AssetExists` | Checks whether an asset exists by object path without loading it. |
| Custom | `native registry` | `BuildProjectIndex` | Expensive manual action that builds or rebuilds the local UnrealMCP project-understanding index. Prefer user-triggered rebuilds instead of... |
| Custom | `native registry` | `CompareBlueprintClassDefaults` | Compares two Blueprint CDOs by property name, including properties present on one side only. |
| Custom | `native registry` | `CompileAllBlueprints` | Compiles multiple Blueprint assets under a package path and returns aggregate results. |
| Custom | `native registry` | `CompileBlueprint` | Compiles one Blueprint asset and returns structured status, errors[{graph,nodeGuid,message}] and warnings[] plus the raw message list. |
| Custom | `native registry` | `ConnectBlueprintPins` | Validates and transactionally connects Blueprint pins, returning their resolved post-connection type model. |
| Custom | `native registry` | `ConsolidateAndDeleteAsset` | Editor 'Replace References' equivalent: repoints every referencer of oldAssetPath to replacementAssetPath, then deletes the old asset (leaving a... |
| Custom | `native registry` | `CreateBlueprintAsset` | Creates a Blueprint asset with an explicit parent class. Rejects overwrite and supports dry-run and optional saving. |
| Custom | `native registry` | `CreateBlueprintFunctionGraph` | Creates a uniquely named user Blueprint function graph and returns stable graph, entry, and return identities. Supports dry-run and optional save. |
| Custom | `native registry` | `CreateInputAction` | Creates a new UInputAction asset with an explicit value type. Rejects overwrite and supports dry-run and optional saving. |
| Custom | `native registry` | `CreateInputMappingContext` | Creates a new UInputMappingContext asset. Rejects overwrite and supports dry-run and optional saving. |
| Custom | `native registry` | `DeleteBlueprintNode` | Deletes one Blueprint graph node by stable GUID. Requires confirm=true, supports dry-run, and reports affected pin links. |
| Custom | `native registry` | `DiscardDirtyPackagesForShutdown` | Clears dirty flags on explicitly discarded editor packages before shutdown, without saving them. |
| Custom | `native registry` | `DisconnectBlueprintPins` | Disconnects one exact Blueprint pin link or all links on one pin. Requires confirmation unless dry-run and reports affected links. |
| Custom | `native registry` | `ExplainBlueprintRole` | Returns a structured indexed summary that helps explain a Blueprint's likely role, neighbors, and composition inside the project. |
| Custom | `native registry` | `ExplainBlueprintTrace` | Explains an indexed Blueprint trace in a higher-level, token-efficient summary with confidence markers and cross-Blueprint notes. |
| Custom | `native registry` | `ExplainFeatureWorkflow` | Explains a feature query by ranking likely entry assets, then summarizing each candidate's role and nearby indexed flow. |
| Custom | `native registry` | `FindActorsUsingBlueprint` | Finds actors in the active editor world whose class was generated from a specific Blueprint asset. |
| Custom | `native registry` | `FindAssetsByClass` | Finds assets by Asset Registry class path, optionally scoped to a package path. |
| Custom | `native registry` | `FindAssetsByPath` | Finds assets under a package path, optionally filtered by class. |
| Custom | `native registry` | `FindBlueprintNodeReferences` | Finds indexed Blueprint nodes that reference a component variable, function, event, or other member across the project. |
| Custom | `native registry` | `FindBlueprintTraceStartPoints` | Finds likely indexed Blueprint graph nodes to use as trace starting points, ranked for tracing workflows. |
| Custom | `native registry` | `FindBlueprintVariableUsage` | Finds indexed Blueprint nodes that appear to read or write a variable, and summarizes their local connection counts. |
| Custom | `native registry` | `FindBrokenBlueprintReferences` | Finds project Blueprint references that are provably unresolved in the current project index. This is static index evidence, not a live compiler... |
| Custom | `native registry` | `FindCircularDependencies` | Finds deterministic circular dependency groups among indexed project Blueprints. Use the reported internal edges to identify dependency seams to... |
| Custom | `native registry` | `FindClassUsages` | Finds every use of a class/struct/enum/asset inside one Blueprint, tagged by kind (cast_target, function_call, variable_access, pin_type,... |
| Custom | `native registry` | `FindCrossBlueprintCalls` | Finds indexed Blueprint function call nodes that target other Blueprint assets, with compact or full output modes. |
| Custom | `native registry` | `FindDataTableReferences` | Finds Blueprint nodes with a direct DataTable pin default, scanning registry referencers or one specified Blueprint. |
| Custom | `native registry` | `FindFeatureEntryPoints` | Finds likely starting assets for a feature by ranking indexed assets that match a query, optionally boosted by a related context asset. |
| Custom | `native registry` | `FindUnusedBlueprintAssets` | Finds conservative project Blueprint candidates with no indexed incoming package dependencies. Results are leads for review, never proof that an... |
| Custom | `native registry` | `GetActorInfo` | Returns detailed information about one actor from the active editor world, including transform, components, and attached actors. |
| Custom | `native registry` | `GetAssetInfo` | Returns detailed Asset Registry information for one asset without loading it. |
| Custom | `native registry` | `GetAssetReferencers` | Returns indexed package referencers for one asset or package. |
| Custom | `native registry` | `GetAssetReferences` | Returns indexed package references for one asset or package. |
| Custom | `native registry` | `GetBlueprintClassDefaults` | Reads paged Blueprint class default values and parent overrides without modifying the asset. |
| Custom | `native registry` | `GetBlueprintComponentDefaults` | Reads safely editable defaults from one existing Blueprint component template without mutating or compiling the asset. |
| Custom | `native registry` | `GetBlueprintComponentHierarchy` | Returns an indexed component hierarchy summary for one Blueprint asset. |
| Custom | `native registry` | `GetBlueprintDependencies` | Returns an indexed dependency summary for one Blueprint asset. |
| Custom | `native registry` | `GetBlueprintInfo` | Returns summary information about a Blueprint asset. |
| Custom | `native registry` | `GetBlueprintOverview` | Inspect a Blueprint asset in one bounded live read: parent, compile/dirty state, graph, variable, component and interface inventory. Filter and... |
| Custom | `native registry` | `GetDependencies` | Returns Asset Registry package dependencies for one asset or package. |
| Custom | `native registry` | `GetImplementedInterfaces` | Lists interfaces implemented by a Blueprint, with optional inherited interfaces. |
| Custom | `native registry` | `GetIndexStatus` | Returns status information for the local UnrealMCP project index. |
| Custom | `native registry` | `GetInputMappingContextMappings` | Reads back every key-mapping row (action, key, triggers, modifiers) in an InputMappingContext without loading it for editing. |
| Custom | `native registry` | `GetLevelActorDependencies` | Returns actor-to-asset relationships for one placed actor, including class source, attached actors, and referenced component assets. |
| Custom | `native registry` | `GetMutationRequestStatus` | Returns the durable in-editor state and terminal response for one mutation operation ID. |
| Custom | `native registry` | `GetParentBlueprint` | Returns parent class information and resolves the parent Blueprint asset when applicable. |
| Custom | `native registry` | `GetReferencers` | Returns Asset Registry package referencers for one asset or package. |
| Custom | `native registry` | `GetServerInfo` | Returns plugin, protocol, and engine version information. |
| Custom | `native registry` | `HealthCheck` | Returns plugin health and readiness information. |
| Custom | `native registry` | `InspectBlueprintNode` | Explains one or more indexed Blueprint nodes, including immediate previous and next connected nodes plus trace hints. |
| Custom | `native registry` | `InspectDataTableRows` | Reads paged DataTable rows as export text and reports soft references that cannot resolve, without changing the table. |
| Custom | `native registry` | `InspectEnhancedInputActionWiring` | Finds Enhanced Input Action event nodes and reports exact Started/Triggered/Ongoing/Canceled/Completed execution routes. |
| Custom | `native registry` | `InspectLiveBlueprint` | Read-only live graph/node/pin inspection, including a map's embedded Level Blueprint. Defaults to the current editor level. No index, loading,... |
| Custom | `native registry` | `LayoutBlueprintNodes` | Deterministically lays out an explicit Blueprint node set by execution-flow depth with configurable spacing, collision avoidance, dry-run, and... |
| Custom | `native registry` | `ListActors` | Lists actors from the active editor world so agents can inspect current level composition without modifying it. |
| Custom | `native registry` | `ListAssets` | Lists assets under a package path without loading them. |
| Custom | `native registry` | `ListBlueprintGraphs` | Lists indexed graphs for one Blueprint with stable graph identity, GUID, type, entry node, and node count. |
| Custom | `native registry` | `ListBlueprintLocalVariables` | Lists the local variables (name, GUID, type, default) of one Blueprint function graph. |
| Custom | `native registry` | `ListBlueprintNodePins` | Lists live pins for one Blueprint node by stable graph and node GUID, including IDs, directions, types, defaults, and exact linked endpoints. |
| Custom | `native registry` | `ListChildBlueprints` | Lists child Blueprints whose parent class is this Blueprint's generated class. |
| Custom | `native registry` | `ListComponents` | Lists Blueprint simple-construction-script components and hierarchy data. |
| Custom | `native registry` | `ListFolders` | Lists known Asset Registry package folders under a base path. |
| Custom | `native registry` | `ListFunctions` | Lists Blueprint-declared function graphs and interface graphs. |
| Custom | `native registry` | `ListSelectedActors` | Lists the currently selected actors from the active editor world. |
| Custom | `native registry` | `ListTools` | Lists all registered Unreal MCP tools and schemas. |
| Custom | `native registry` | `ListUnrealMCPAutomationTests` | Lists registered UnrealMCP editor automation tests available to the bounded static regression runner. |
| Custom | `native registry` | `ListVariables` | Lists Blueprint-defined variables and their pin types. |
| Custom | `native registry` | `MoveBlueprintNode` | Moves a Blueprint node using explicit or relative collision-aware placement. Supports dry-run and optional save. |
| Custom | `native registry` | `PlanProjectRefactor` | Builds a deterministic, evidence-backed, non-mutating Blueprint refactor plan from indexed architecture analyses. It never changes assets and... |
| Custom | `native registry` | `RefreshBlueprintCallSites` | Finds every same-Blueprint and cross-Blueprint call site of a Blueprint function via the project index and reconstructs each call node to match... |
| Custom | `native registry` | `RefreshProjectIndex` | Refreshes only changed, added, deleted, or explicitly requested project assets in the existing UnrealMCP index. This is cheaper than... |
| Custom | `native registry` | `RemoveBlueprintLocalVariable` | Removes one local variable from a Blueprint function graph. Requires confirm=true unless dryRun=true; reports how many Get/Set nodes still... |
| Custom | `native registry` | `RemoveDataTableRow` | Transactionally edits one DataTable row in memory; leaves the asset dirty for an explicit save. |
| Custom | `native registry` | `RemoveInputMappingContextMapping` | Removes one exact existing Action+Key mapping row from an InputMappingContext. Fails clearly rather than guessing when zero or more than one row... |
| Custom | `native registry` | `ResetBlueprintClassDefaults` | Resets inherited Blueprint class defaults to their parent CDO values. |
| Custom | `native registry` | `RetargetBlueprintNode` | Retargets a cast node (including a 'Bad cast node' whose class was deleted) to newClassPath without needing the old class. Output links whose pins... |
| Custom | `native registry` | `RunUnrealMCPAutomationTest` | Runs one exact synchronous UnrealMCP.* editor automation test and returns structured execution evidence. |
| Custom | `native registry` | `RunUnrealMCPAutomationTests` | Runs a bounded explicit list of synchronous UnrealMCP editor automation tests and returns one suite result. |
| Custom | `native registry` | `SaveAllDirtyPackages` | Saves every dirty package without prompting, so a subsequent editor shutdown does not block on a save-confirmation dialog. Supports dry-run to... |
| Custom | `native registry` | `SaveBlueprint` | Explicitly saves one Blueprint asset, refusing by default to write a Blueprint whose compile is not up to date, and reports its measured package... |
| Custom | `native registry` | `SaveDataTable` | Saves exactly one DataTable package after explicit row edits and reports the resulting dirty state. |
| Custom | `native registry` | `SaveValidatedBlueprints` | Saves an ordered list of already-compiled, dirty Blueprint assets one at a time, stopping at the first asset that is not saveable and reporting... |
| Custom | `native registry` | `SearchAssets` | Searches the Asset Registry without loading matching assets. |
| Custom | `native registry` | `SetBlueprintClassDefaults` | Preflights and transactionally sets Blueprint class defaults, including inherited properties. |
| Custom | `native registry` | `SetBlueprintComponentDefaults` | Transactionally validates and updates safely editable defaults on one existing Blueprint component template. |
| Custom | `native registry` | `SetBlueprintFunctionMetadata` | Idempotently sets category, description, pure, and const metadata on a Blueprint function graph. |
| Custom | `native registry` | `SetBlueprintLocalVariableDefault` | Sets the export-text default of an existing Blueprint function local variable. |
| Custom | `native registry` | `SetBlueprintNodeComment` | Sets a Blueprint node comment and optional comment-bubble visibility state. Idempotent, transactional, and supports dry-run and optional save. |
| Custom | `native registry` | `SetBlueprintParameter` | Changes the type and/or name of one parameter on a custom event, function entry or function result node, updating call sites in one undoable... |
| Custom | `native registry` | `SetBlueprintPinDefaultObject` | Sets an exact UObject or UClass default on an unconnected Blueprint input pin using stable graph, node, and pin identity. Supports dry-run and... |
| Custom | `native registry` | `SetBlueprintPinDefaultValue` | Sets a validated literal default on an unconnected Blueprint input pin using stable node/pin identity. Supports dry-run and optional save. |
| Custom | `native registry` | `SetBlueprintPinSplit` | Splits a Blueprint struct pin into member sub-pins, or explicitly recombines it, using stable graph/node/pin identifiers. |
| Custom | `native registry` | `SetBlueprintSequenceOutputs` | Sets a Sequence node to 2-32 execution outputs. Removing outputs requires explicit confirmation, with separate confirmation for linked pins. |
| Custom | `native registry` | `SetDataTableRowFields` | Transactionally edits one DataTable row in memory; leaves the asset dirty for an explicit save. |
| Custom | `native registry` | `SetInputMappingContextMappingKey` | Replaces the key of one exact Action+Key row, preserving its position, action, inline objects and metadata. Static asset editing only; no runtime... |
| Custom | `native registry` | `SpliceBlueprintExecFlow` | Atomically inserts an ordered chain of existing nodes into one exact Blueprint execution link, with revision checks, idempotent retry, rollback,... |
| Custom | `native registry` | `SummarizeBlueprintCluster` | Summarizes the nearby indexed asset cluster around a starting Blueprint or asset. |
| Custom | `native registry` | `TraceBlueprintFlow` | Traces Blueprint graph flow from the project index; refreshes the selected Blueprint once if start nodes are missing. |
| Custom | `native registry` | `TraceFeatureFlow` | Traces a small indexed feature flow around a starting Blueprint or asset using dependencies and referencers. |
| Custom | `native registry` | `TraceLiveBlueprintFlow` | Trace live Blueprint execution and data links from an event or node, including embedded Level Blueprints, delays, and resident function/macro... |
| Custom | `native registry` | `ValidateBlueprint` | Runs a no-save compile validation for one Blueprint and returns structured diagnostics and dirty state. |
| Custom | `native registry` | `WireBlueprintEventToFunction` | Idempotently ensures a custom event calls one exact Blueprint-callable function, optionally through a Blueprint component/member target. Refuses... |
| Custom | `native registry` | `WireEnhancedInputActionToComponent` | Idempotently inserts one component function call into an exact Enhanced Input action phase while preserving its existing execution continuation. |
| Custom | `native registry` | `WireSelectionWorkflow` | Idempotently scaffolds four collection-backed helper functions (SelectOne, ToggleSelection, ClearSelection, ContainsSelection) over a named... |
| Official | `AutomationTestToolset.AutomationTestToolset` | `DiscoverTests` | Initialize automation worker discovery and load the test list. Must be called once before ListTests or RunTests. Takes several seconds as it... |
| Official | `AutomationTestToolset.AutomationTestToolset` | `GetTestResults` | Get detailed results for the current or most recent test run. Requires DiscoverTests() to have completed. Returns a JSON object with per-test... |
| Official | `AutomationTestToolset.AutomationTestToolset` | `GetTestStatus` | Get a lightweight status snapshot of the automation controller. Requires DiscoverTests() to have completed. Returns a JSON object with the... |
| Official | `AutomationTestToolset.AutomationTestToolset` | `ListTests` | List available automation tests. Requires DiscoverTests() to have completed. Returns a JSON object: {"tests": ["path1", ...], "total": N,... |
| Official | `AutomationTestToolset.AutomationTestToolset` | `RunTests` | Run a set of automation tests by name. Requires DiscoverTests() to have completed. Starts executing the specified tests and returns an async... |
| Official | `AutomationTestToolset.AutomationTestToolset` | `RunTestsByFilter` | Run automation tests selected by a filter expression. Requires DiscoverTests() to have completed. Much faster than RunTests when targeting a large... |
| Official | `AutomationTestToolset.AutomationTestToolset` | `StopTests` | Stop all currently running tests. Requires DiscoverTests() to have completed. If a RunTests async result is pending, it will be completed with an... |
| Official | `ConfigSettingsToolset.ConfigSettingsToolset` | `GetSectionPropertyValues` | Returns the current values of the specified properties as a JSON object. Raises an error if the section does not exist, has no settings object, or... |
| Official | `ConfigSettingsToolset.ConfigSettingsToolset` | `GetSectionSchema` | Returns a JSON Schema describing the user-visible properties of a settings section. The schema maps each property name to its type, description,... |
| Official | `ConfigSettingsToolset.ConfigSettingsToolset` | `ListCategories` | Lists the names of all categories within a settings container, sorted alphabetically. Raises an error if the container does not exist. |
| Official | `ConfigSettingsToolset.ConfigSettingsToolset` | `ListContainers` | Lists the names of all known settings containers, sorted alphabetically. Common containers are "Editor" and "Project". |
| Official | `ConfigSettingsToolset.ConfigSettingsToolset` | `ListSections` | Lists the names of all sections within a settings category, sorted alphabetically. Raises an error if the container or category does not exist. |
| Official | `ConfigSettingsToolset.ConfigSettingsToolset` | `ResetSectionToDefaults` | Resets the settings in a section to their default values. Raises an error if the section does not exist or reset is not supported. |
| Official | `ConfigSettingsToolset.ConfigSettingsToolset` | `SaveSection` | Saves the settings in a section. Raises an error if the section does not exist or saving is not supported. |
| Official | `ConfigSettingsToolset.ConfigSettingsToolset` | `SetSectionProperties` | Sets one or more properties on a settings section from a JSON object and saves. PropertiesJson must be a JSON object mapping property names to new... |
| Official | `DataRegistryToolset.DataRegistryTools` | `GetItems` | Returns cached item data. Items must be loaded in the registry cache to be returned. |
| Official | `DataRegistryToolset.DataRegistryTools` | `GetRegistryInfo` | Returns detailed information about a specific registry. |
| Official | `DataRegistryToolset.DataRegistryTools` | `GetSchema` | Returns the item struct schema as JSON. |
| Official | `DataRegistryToolset.DataRegistryTools` | `ListDataSources` | Returns the editor-defined sources configured on a Data Registry. These are the sources as authored on the registry asset, before any runtime... |
| Official | `DataRegistryToolset.DataRegistryTools` | `ListItems` | Returns all item names in a Data Registry. |
| Official | `DataRegistryToolset.DataRegistryTools` | `ListRegistries` | Returns the names of all registered Data Registries. |
| Official | `DataRegistryToolset.DataRegistryTools` | `ListRuntimeSources` | Returns the runtime sources for a Data Registry. This is the expanded list including transient child sources generated from meta sources. Will... |
| Official | `DataflowAgent.DataflowAgentToolset` | `AddCommentBox` | Adds a comment box around the given nodes. |
| Official | `DataflowAgent.DataflowAgentToolset` | `AddNode` | Adds a node of the given type to the Dataflow graph. |
| Official | `DataflowAgent.DataflowAgentToolset` | `AddVariable` | Adds a new variable to the Dataflow graph. |
| Official | `DataflowAgent.DataflowAgentToolset` | `AssignDataflowTemplate` | Assigns a Dataflow template to an existing Dataflow-compatible asset by duplicating the template graph and embedding it. Replaces any existing... |
| Official | `DataflowAgent.DataflowAgentToolset` | `ConnectNodePins` | Connects an output pin of one node to an input pin of another. |
| Official | `DataflowAgent.DataflowAgentToolset` | `CreateDataflowCompatibleAsset` | Creates a new Dataflow-compatible asset (e.g. ChaosClothAsset, GeometryCollection, FleshAsset, GroomAsset) with an empty embedded Dataflow graph. |
| Official | `DataflowAgent.DataflowAgentToolset` | `CreateDataflowCompatibleAssetFromTemplate` | Creates a new Dataflow-compatible asset and initialises its embedded Dataflow graph from a registered template in one step. |
| Official | `DataflowAgent.DataflowAgentToolset` | `CreateGraph` | Creates a new saved Dataflow graph asset. |
| Official | `DataflowAgent.DataflowAgentToolset` | `DisconnectNodePins` | Removes the connection between two node pins. |
| Official | `DataflowAgent.DataflowAgentToolset` | `GetGraphStructure` | Returns the complete structure of a Dataflow graph including all nodes and connections. |
| Official | `DataflowAgent.DataflowAgentToolset` | `GetNodeInfo` | Returns information about a node as a JSON object (name, type, position, pins). |
| Official | `DataflowAgent.DataflowAgentToolset` | `GetNodeTypeSchema` | Returns the schema for a Dataflow node type including its input/output pins and editable UPROPERTY parameters. |
| Official | `DataflowAgent.DataflowAgentToolset` | `ListDataflowCompatibleAssetTypes` | Returns a JSON list of every UClass that can host an embedded Dataflow graph (i.e. implements IDataflowInstanceInterface). Each entry has... |
| Official | `DataflowAgent.DataflowAgentToolset` | `ListDataflowTemplatesForAssetClass` | Returns a JSON list of Dataflow templates registered for the given asset class. Templates registered for parent classes are included (class... |
| Official | `DataflowAgent.DataflowAgentToolset` | `ListNodeTypes` | Returns a JSON list of all registered Dataflow node types. |
| Official | `DataflowAgent.DataflowAgentToolset` | `ListVariables` | Returns all variables defined on the Dataflow graph as a JSON array. Each entry contains "name", "type", and "value" fields. |
| Official | `DataflowAgent.DataflowAgentToolset` | `RemoveCommentBox` | Removes a comment box node from the graph. |
| Official | `DataflowAgent.DataflowAgentToolset` | `RemoveNode` | Removes a node and all its connections from the Dataflow graph. |
| Official | `DataflowAgent.DataflowAgentToolset` | `RemoveVariable` | Removes a variable from the Dataflow graph. |
| Official | `DataflowAgent.DataflowAgentToolset` | `RepositionNode` | Moves a node to a new position in the graph editor. |
| Official | `DataflowAgent.DataflowAgentToolset` | `SetVariable` | Sets the value of an existing variable using its serialized string representation. The format depends on the variable's type (e.g., "3.14" for... |
| Official | `DataflowAgent.DataflowAgentToolset` | `UpdateNode` | Updates an existing node's editable properties via JSON. |
| Official | `EditorToolset.EditorAppToolset` | `CaptureAssetImage` | Renders a thumbnail for the specified asset (e.g. static meshes, skeletal meshes, skeletons, animations, montages, materials, textures). |
| Official | `EditorToolset.EditorAppToolset` | `CaptureEditorImage` | Captures an image of the entire editor application as the user sees it. |
| Official | `EditorToolset.EditorAppToolset` | `CaptureViewport` | Captures the level viewport with optional annotations. |
| Official | `EditorToolset.EditorAppToolset` | `FocusOnActors` | Repositions the level editor camera to focus on the specified actors. Cannot be called while PIE is active. |
| Official | `EditorToolset.EditorAppToolset` | `GetCameraTransform` | Returns the position and rotation of the level viewport camera. |
| Official | `EditorToolset.EditorAppToolset` | `GetContentBrowserPath` | Gets the current path of the active content browser. |
| Official | `EditorToolset.EditorAppToolset` | `GetOpenAssets` | Gets the list of assets currently open in asset editors. |
| Official | `EditorToolset.EditorAppToolset` | `GetSelectedActors` | Gets the currently selected actors in the level editor. |
| Official | `EditorToolset.EditorAppToolset` | `GetSelectedAssets` | Gets the list of assets selected in the content browser. |
| Official | `EditorToolset.EditorAppToolset` | `GetVisibleActors` | Returns all actors in the current level whose bounds intersect the viewport frustum. |
| Official | `EditorToolset.EditorAppToolset` | `IsPIERunning` | Returns whether a Play In Editor session is currently running. |
| Official | `EditorToolset.EditorAppToolset` | `OpenEditorForAsset` | Opens an asset editor for the specified asset. |
| Official | `EditorToolset.EditorAppToolset` | `ScreenCoordsToWorld` | Finds the world position of the nearest solid object at a given set of normalized view space coords. |
| Official | `EditorToolset.EditorAppToolset` | `SearchCVars` | Finds all console variables that contain a given name. |
| Official | `EditorToolset.EditorAppToolset` | `SelectActors` | Selects the specified actors in the current scene. |
| Official | `EditorToolset.EditorAppToolset` | `SelectAssets` | Selects the specified assets in the content browser. Completes once the content browser has applied the selection. |
| Official | `EditorToolset.EditorAppToolset` | `SetCameraTransform` | Sets the position and rotation of the level viewport camera. |
| Official | `EditorToolset.EditorAppToolset` | `SetContentBrowserPath` | Navigates the active content browser to the specified folder path. |
| Official | `EditorToolset.EditorAppToolset` | `StartPIE` | Starts a Play-In-Editor or Simulate-In-Editor session using the current level. Completes after the engine fires PostPIEStarted (session fully... |
| Official | `EditorToolset.EditorAppToolset` | `StopPIE` | Stops the currently running play session (PIE or Simulate). Raises an error if no play session is running. |
| Official | `EditorToolset.EditorAppToolset` | `WorldPosToScreenCoords` | Converts a world-space position into normalized screen space based on the editor viewport camera. |
| Official | `EditorToolset.LogsToolset` | `GetLogCategories` | Returns a sorted list of registered log categories. |
| Official | `EditorToolset.LogsToolset` | `GetLogEntries` | Returns log entries from the current session's log file. |
| Official | `EditorToolset.LogsToolset` | `GetVerbosity` | Returns the current verbosity level for a log category. |
| Official | `EditorToolset.LogsToolset` | `SetVerbosity` | Sets the verbosity level for a log category. |
| Official | `GASToolsets.AbilitySystemInspectorToolset` | `GetActiveEffects` | Returns all gameplay effects currently active on the actor's AbilitySystemComponent. |
| Official | `GASToolsets.AbilitySystemInspectorToolset` | `GetActiveTags` | Returns the gameplay tags currently owned by the actor's AbilitySystemComponent (includes loose tags, effect-granted tags, etc.). |
| Official | `GASToolsets.AbilitySystemInspectorToolset` | `GetAttributeValues` | Returns the current base and modified values of all gameplay attributes on the actor's AbilitySystemComponent. |
| Official | `GASToolsets.AbilitySystemInspectorToolset` | `GetGrantedAbilities` | Returns all abilities granted to the actor's AbilitySystemComponent. |
| Official | `GASToolsets.AttributeSetToolset` | `FindAttributeSetClasses` | Returns all AttributeSet subclasses found in the project, including their attributes. Covers both native C++ subclasses and Blueprint subclasses... |
| Official | `GASToolsets.AttributeSetToolset` | `ListAttributes` | Returns the gameplay attributes defined on a specific AttributeSet class. |
| Official | `GASToolsets.GameplayCueToolset` | `AddCueTag` | Adds a new gameplay cue tag to the project. This should ONLY be called after getting explicit direction or permission from the user. |
| Official | `GASToolsets.GameplayCueToolset` | `CreateCueNotifyAsset` | Creates a new GameplayCueNotify Blueprint asset at the specified content browser location. This should ONLY be called after getting explicit... |
| Official | `GASToolsets.GameplayCueToolset` | `ExecuteCueOnSelectedActor` | Executes a gameplay cue non-replicated on the currently selected actor in the editor. Useful for previewing cue effects without network... |
| Official | `GASToolsets.GameplayCueToolset` | `FindCueNotifyAssets` | Returns all GameplayCueNotify assets found in the project via the asset registry. |
| Official | `GASToolsets.GameplayCueToolset` | `FindCueTagsWithoutNotifies` | Returns gameplay cue tags that have no corresponding GameplayCueNotify asset in the project. Tags without notifies produce no visible effect when... |
| Official | `GASToolsets.GameplayCueToolset` | `GetCueInfo` | Returns information about a specific gameplay cue, including its notify asset. |
| Official | `GASToolsets.GameplayCueToolset` | `ListCues` | Returns gameplay cue tags registered in the project. |
| Official | `GASToolsets.GameplayCueToolset` | `RemoveCueTag` | Removes a gameplay cue tag from the project. This should ONLY be called after getting explicit direction or permission from the user. |
| Official | `GameFeaturesToolset.GameFeaturesToolset` | `GetGameFeatureState` | Gets the current state of a Game Feature Plugin. |
| Official | `GameFeaturesToolset.GameFeaturesToolset` | `IsGameFeatureActive` | Checks whether a Game Feature Plugin is active. Raises an error if the subsystem is unavailable or the plugin is not found. Use... |
| Official | `GameFeaturesToolset.GameFeaturesToolset` | `IsGameFeaturePlugin` | Return whether or not a plugin is a Game Feature Plugin. Will error if no plugin of this name can be found by the Plugin Manager. |
| Official | `GameFeaturesToolset.GameFeaturesToolset` | `ListDiscoveredGameFeaturePlugins` | Lists all discovered Game Feature Plugins sorted by name. This includes enabled and disabled plugins. Only enabled plugins are known by the Game... |
| Official | `GameFeaturesToolset.GameFeaturesToolset` | `ListEnabledGameFeaturePlugins` | Lists all enabled Game Feature Plugins sorted by name. Enabled plugins are the only plugins known by the Game Features system beyond identifying... |
| Official | `GameFeaturesToolset.GameFeaturesToolset` | `RequestActivateGameFeature` | Requests activation of a Game Feature Plugin. Returns true if the activation request was submitted successfully. The actual activation happens... |
| Official | `GameFeaturesToolset.GameFeaturesToolset` | `RequestDeactivateGameFeature` | Requests deactivation of a Game Feature Plugin. Returns true if the deactivation request was submitted successfully. The actual deactivation... |
| Official | `GameplayTagsToolset.GameplayTagsToolset` | `AddTag` | Adds a new gameplay tag to the project. This should ONLY be called after getting explicit direction or permission from the user. |
| Official | `GameplayTagsToolset.GameplayTagsToolset` | `FindReferencersByTag` | Returns assets that reference a gameplay tag. |
| Official | `GameplayTagsToolset.GameplayTagsToolset` | `GetTagInfo` | Returns detailed information about a specific gameplay tag. |
| Official | `GameplayTagsToolset.GameplayTagsToolset` | `ListTags` | Returns gameplay tags registered in the project. |
| Official | `GameplayTagsToolset.GameplayTagsToolset` | `RemoveTag` | Removes a gameplay tag from the project. This should ONLY be called after getting explicit direction or permission from the user. |
| Official | `GameplayTagsToolset.GameplayTagsToolset` | `RenameTag` | Renames a gameplay tag, updating all references in the project. This should ONLY be called after getting explicit direction or permission from the... |
| Official | `GraphXPortConflictToolset.GraphXPortConflictToolset` | `ApplyMergeResolution` | Apply the stored plan with stale-state guards and native transactional compile/rollback. Save and Stage are explicit; Stage requires Save. Retry... |
| Official | `GraphXPortConflictToolset.GraphXPortConflictToolset` | `CloseBlueprintMerge` | Release session evidence; never unload packages held by the merge UI. Sessions also expire after ten minutes. |
| Official | `GraphXPortConflictToolset.GraphXPortConflictToolset` | `CompareRevisions` | Compare graph changes from Before to After. Use base->current for outgoing and base->incoming for incoming. Revisions accept... |
| Official | `GraphXPortConflictToolset.GraphXPortConflictToolset` | `InspectMergeNodes` | Read only the requested node GUIDs, including pins and owner/member identities, from an unchanged session revision. NodeGuidsJson is a JSON string... |
| Official | `GraphXPortConflictToolset.GraphXPortConflictToolset` | `InspectRevision` | Read graph inventory (Graph empty) or nodes/pins of a named graph. Revision accepts base/current/incoming/HEAD/full commit hash. Path is... |
| Official | `GraphXPortConflictToolset.GraphXPortConflictToolset` | `ListConflicts` | List unmerged paths with stage 1/2/3 blob identities. Never stages or saves. |
| Official | `GraphXPortConflictToolset.GraphXPortConflictToolset` | `OpenBlueprintMerge` | Open a bounded merge review. Caches revision reads, records stage/disk identities and dirty state, returns outgoing/incoming graph changes. Does... |
| Official | `GraphXPortConflictToolset.GraphXPortConflictToolset` | `PreviewMergeResolution` | Preflight an exact ApplyBlueprintGraphPatch JSON plan against the session. Captures graph revision and stores the plan; returns PlanId and compact... |
| Official | `NiagaraToolsets.NiagaraToolset_Assets` | `FindNiagaraScripts` | Searches for UNiagaraScript assets matching the given filters. |
| Official | `NiagaraToolsets.NiagaraToolset_Assets` | `GetAssetDiscoveryInfo` | Returns the project's configured asset discovery groups. Each group describes a content directory's purpose and paths. |
| Official | `NiagaraToolsets.NiagaraToolset_Assets` | `GetNiagaraScriptDigest` | Returns the decoded asset-registry tag metadata for a Niagara script asset. |
| Official | `NiagaraToolsets.NiagaraToolset_Blueprint` | `ConstructNiagaraBPWrapperFromComponent` | Creates a Blueprint actor wrapper from a Niagara Component. This generates a new Blueprint actor and preserves all component property values and... |
| Official | `NiagaraToolsets.NiagaraToolset_Blueprint` | `ConstructNiagaraBPWrapperFromSystem` | Creates a Blueprint actor wrapper around a Niagara System. This generates a new Blueprint actor with a Niagara component configured to use the... |
| Official | `NiagaraToolsets.NiagaraToolset_Component` | `GetUserVariables` | Returns all user variable values currently set on the component. This retrieves the current values of all user-exposed parameters that can be... |
| Official | `NiagaraToolsets.NiagaraToolset_Component` | `GetVariable` | Gets the current value of a specific user variable on the component. This retrieves the current value of a user-exposed parameter, including any... |
| Official | `NiagaraToolsets.NiagaraToolset_Component` | `SetSystem` | Sets the Niagara System for a component. Use this instead of setting the Asset property directly to ensure proper initialization. |
| Official | `NiagaraToolsets.NiagaraToolset_Component` | `SetVariable` | Sets the value of a user variable on the component. This overrides the default value of a user-exposed parameter on a specific component instance. |
| Official | `NiagaraToolsets.NiagaraToolset_Info` | `UEnum_Info` | Returns information about a UEnum and all its values. ALWAYS call this when working with a UEnum type to see valid values. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `AddEmitter` | Adds an emitter to a Niagara System. The new emitter will be based on the template emitter, inheriting its configuration and modules. Returns the... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `AddModule` | Adds a module to a script stack. The module will be inserted into the specified script's execution stack. Returns the module topology with all... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `AddRenderer` | Adds a renderer to an emitter. Creates a new renderer of the specified type and adds it to the emitter's renderer list. Returns an... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `AddSetParameterEntry` | Adds a single parameter to an existing SetParameters module. The module referenced by ModuleRef must be a SetParameters (UNiagaraNodeAssignment)... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `AddSetParametersModule` | Adds a SetParameters module to a script stack. Unlike AddModule which requires a script asset, a SetParameters module dynamically assigns values... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `AddUserVariables` | Adds or updates user variables on a system. If a variable with the same name already exists, it will be replaced with the new definition. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `ApplyStackIssueFix` | Applies a Fix-style stack issue fix identified by IssueId and FixId. Link-style fixes are rejected. The fix is undoable via the editor undo stack.... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `CreateNiagaraSystem` | Creates a new Niagara System asset. The new system will be based on the template system, inheriting its configuration and emitters. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetAvailableDynamicInputs` | Returns all available Dynamic Input Module assets compatible with the given type. Dynamic inputs provide procedural value generation for module... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetDataInterfaceSchema` | Returns property schema for a specific Data Interface class. Describes all available properties and their types for the given data interface type. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetDynamicInputChain` | Returns the full recursive chain for a dynamic input: topology metadata and resolved values at every level. The starting input must have value... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetDynamicInputSchema` | Returns schema for a dynamic input module in the stack. Describes the inputs and configuration for a procedural value generator. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetDynamicInputSchemaFromAsset` | Returns schema for a dynamic input asset. Standalone function that doesn't require a system context - useful for browsing available dynamic inputs. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetEmitterData` | Returns emitter property values as a single JSON-string blob in PropertyValues. The blob contains the full FVersionedNiagaraEmitterData... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetEmitterInputValues` | Returns all resolved input values for every module across all four emitter script stacks. One FNiagaraExt_ModuleInputValues entry per module, each... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetEmitterSchema` | Returns property schema for Niagara Emitter. Describes all available properties and their types that can be set on a Niagara Emitter. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetEmitterSummary` | Returns lightweight emitter metadata: name, enabled state, sim target, renderer classes. Use this when you only need to check metadata without... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetEmitterTopology` | Returns full emitter topology: four script stacks with all modules and inputs, renderer references. All fields always populated. The returned... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetModuleInputValues` | Returns resolved input values for a single module. Use when you need values for one specific module without walking the whole emitter. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetModuleSchema` | Returns schema for a module and all its inputs. Call this after seeing a module in topology to understand what inputs it exposes. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetModuleSchemaFromAsset` | Returns schema for a module asset. Standalone function that doesn't require a system context - useful for browsing available modules. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetModuleTopology` | Returns module topology: metadata and all inputs (name/type/visibility only, no values). All fields always populated. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetRendererData` | Returns renderer property values. Retrieves the current values of all configurable renderer properties. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetRendererSchema` | Returns property schema for a specific Renderer class. Describes all available properties and their types for the given renderer type. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetScriptStackInputValues` | Returns all resolved input values for every module in the given script stack. One FNiagaraExt_ModuleInputValues entry per module. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetScriptStackTopology` | Returns script stack topology: all modules and their inputs in execution order. All fields always populated. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetStackInputData` | Returns the value of a stack module input. Retrieves the current value and configuration for a specific module input parameter. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetStackInputSchema` | Returns schema for a single module input in the stack. Describes the type, metadata, and configuration options for a specific input parameter. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetStackInputTopology` | Returns stack input topology: name, type, visibility, editability. No value payload. For the resolved value call GetStackInputData. For a... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetStackIssues` | Returns all stack issues (errors, warnings, info) from the Niagara module stack, including dismissed ones. Waits for any in-flight compile to... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetSystemCompileState` | Returns the current compile state of a Niagara System: aggregate status, per-script compile events, and summary flags. Waits for any in-flight... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetSystemData` | Returns system property values. Retrieves the current values of all configurable system-level properties. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetSystemDependencies` | Returns the four Used* sets (renderers, data interfaces, modules, dynamic inputs) gathered across all emitters and system scripts. These sets are... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetSystemSchema` | Returns property schema for Niagara System. Describes all available properties and their types that can be set on a Niagara System. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetSystemSummary` | Returns lightweight system metadata: name, user variables, and one summary entry per emitter. Use this for first contact with an unfamiliar... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `GetUserVariables` | Returns all user variables defined on the system. User variables are parameters exposed for external control and can be overridden per component... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `RemoveEmitter` | Removes an emitter from a system. Deletes the specified emitter and all its associated scripts, modules, and renderers. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `RemoveModule` | Removes a module from a script stack. Deletes the specified module and all its inputs from the script's execution stack. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `RemoveRenderer` | Removes a renderer from an emitter. Deletes the specified renderer from the emitter's renderer list. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `RemoveSetParameterEntry` | Removes a parameter from an existing SetParameters module by name. The module referenced by ModuleRef must be a SetParameters... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `RemoveUserVariables` | Removes user variables from a system. Deletes the specified user variables from the system's user parameter collection. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `SetEmitterData` | Sets property values on a Niagara Emitter. Applies new values to emitter-level properties based on the provided data structure. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `SetModuleEnabled` | Sets whether a module is enabled. Disabled modules remain in the stack but don't execute. Current state is visible in module topology. |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `SetRendererData` | Sets property values on a Niagara Renderer. Applies new values to renderer properties based on the provided data structure. Payload shape varies... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `SetStackInputData` | Sets the value of a stack module input and returns the resulting stored value. Updates the value and configuration for a specific module input... |
| Official | `NiagaraToolsets.NiagaraToolset_System` | `SetSystemData` | Sets property values on a Niagara System. Applies new values to system-level properties based on the provided data structure. |
| Official | `PCGToolset.PCGSpatialToolset` | `RunPCGInstantGraph` | Runs an instant PCG graph with the specified parameters in fire-and-forget mode (Should be called directly: Not callable in a python context... |
| Official | `PCGToolset.PCGToolset` | `AddCommentBox` | Adds a comment box around the given nodes. Note: If the bounding box of the nodes include other nodes, they will be included in the comment. |
| Official | `PCGToolset.PCGToolset` | `AddNode` | Adds a native node to the graph. |
| Official | `PCGToolset.PCGToolset` | `AddSubgraphNode` | Adds a subgraph node to the graph. |
| Official | `PCGToolset.PCGToolset` | `ConnectNodePins` | Add an edge between two nodes connected to the specified pins. |
| Official | `PCGToolset.PCGToolset` | `CreateGraph` | Creates a new saved PCG graph asset. |
| Official | `PCGToolset.PCGToolset` | `DisconnectNodePins` | Removes the edge between two nodes connected to the specified pins. |
| Official | `PCGToolset.PCGToolset` | `DrawSpline` | Triggers the user to draw a spline in the viewport to be used later in the world building. Waits for the user to be done. |
| Official | `PCGToolset.PCGToolset` | `ExecuteGraphInstance` | Executes the graph instance and returns any issues encountered during execution. |
| Official | `PCGToolset.PCGToolset` | `GetGraphDescription` | Returns the description of a PCG graph. |
| Official | `PCGToolset.PCGToolset` | `GetGraphInstanceParams` | Gets the graph instance params of a specific actor, actor MUST have a graph instance |
| Official | `PCGToolset.PCGToolset` | `GetGraphSchema` | Returns the schema for a PCG Graph's graph parameters |
| Official | `PCGToolset.PCGToolset` | `GetGraphStructure` | Returns the complete structure of a PCG graph including all nodes, connections, exposed parameters, and comment boxes. |
| Official | `PCGToolset.PCGToolset` | `GetNativeNodeSchema` | Returns the schema for a PCG node type including input/output pins, parameters, and their types. |
| Official | `PCGToolset.PCGToolset` | `GetNodeDataView` | Returns a JSON Data View of a specific node's output data from the last graph execution. On first call, enables inspection so future... |
| Official | `PCGToolset.PCGToolset` | `GetNodeInfo` | Returns node details including name, position, and all parameter values. |
| Official | `PCGToolset.PCGToolset` | `ListAvailableSubgraphs` | Lists the PCG graphs that can be used with the Subgraph native node. Only these graphs should be used with the Subgraph native node. |
| Official | `PCGToolset.PCGToolset` | `ListGraphInstances` | Gets all actors with a PCG graph instance in the scene. |
| Official | `PCGToolset.PCGToolset` | `ListNativeNodes` | Returns a list of available native PCG node type names. |
| Official | `PCGToolset.PCGToolset` | `RemoveCommentBox` | Removes a comment box from the graph. Does not affect the nodes it contains. |
| Official | `PCGToolset.PCGToolset` | `RemoveGraphParams` | Removes graph parameters to a specific PCG graph, such that they are not overridable anymore. |
| Official | `PCGToolset.PCGToolset` | `RemoveNode` | Removes the node from the graph, will also remove edges connected to the node. |
| Official | `PCGToolset.PCGToolset` | `RepositionNode` | Change the position of node. |
| Official | `PCGToolset.PCGToolset` | `ResetGraphInstanceParams` | Resets the given graph instance params back to the graph's default values. Actor MUST have a graph instance. |
| Official | `PCGToolset.PCGToolset` | `SetGraphDescription` | Set the description of a PCGGraph |
| Official | `PCGToolset.PCGToolset` | `SetGraphInstanceParams` | Sets the graph instance params of a specific actor, actor MUST have a graph instance |
| Official | `PCGToolset.PCGToolset` | `SetGraphParams` | Adds one or more graph user parameters to a specific PCG graph, such that they will be overridable in per graph instance. |
| Official | `PCGToolset.PCGToolset` | `SetNodeComment` | Change the comment on the specified node. |
| Official | `PCGToolset.PCGToolset` | `SpawnGraphInstance` | Spawns a PCG Volume with associated Graph Instance into the scene, optionally with Graph Param overrides. |
| Official | `PCGToolset.PCGToolset` | `UpdateCommentBox` | Updates an existing comment box with new nodes and value. Note: If the bounding box of the nodes include other nodes, they will be included in the... |
| Official | `PCGToolset.PCGToolset` | `UpdateNode` | Updates a node by changing its params and/or title. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `AddBody` | Adds a new empty body for the given bone. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `AddConstraint` | Adds a new constraint between two bodies. Both bodies must already exist. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `CreateFromMesh` | Creates a physics asset from a skeletal mesh, auto-generating collision bodies for each bone. The asset is placed in the same folder as the mesh... |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `GetBodyMassScale` | Returns the mass-scale multiplier for the given body. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `GetBodyNames` | Returns the bone name for each rigid body in a physics asset. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `GetBodyPhysicsMode` | Returns the physics simulation mode for the given body. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `GetBodyShapes` | Returns all collision shapes assigned to a body. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `GetConstraints` | Returns all constraints in the physics asset with their current angular limits. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `RemoveBody` | Removes the body for the given bone along with any constraints that reference it. Raises a script error if PhysicsAsset is null or no body exists... |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `RemoveConstraint` | Removes the constraint between two bodies. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `RemoveShape` | Removes a collision primitive from a body by name. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `SetBodyMassScale` | Sets the mass-scale multiplier for the given body. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `SetBodyPhysicsMode` | Sets the physics simulation mode for the given body. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `SetBox` | Adds or replaces a box collision primitive on a body. If any shape with the given name already exists on the body it is removed first. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `SetCapsule` | Adds or replaces a capsule collision primitive on a body. If any shape with the given name already exists on the body it is removed first. The... |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `SetConstraintLimits` | Updates the angular limits for an existing constraint. |
| Official | `PhysicsToolsets.PhysicsAssetToolset` | `SetSphere` | Adds or replaces a sphere collision primitive on a body. If any shape with the given name already exists on the body it is removed first. |
| Official | `PluginToolset.PluginToolset` | `AddPluginDependency` | Adds a dependency entry to a plugin's Plugins array in its .uplugin file. No-ops if a dependency with that name already exists with matching... |
| Official | `PluginToolset.PluginToolset` | `CreatePlugin` | Creates a new plugin from a template and loads it into the editor. Use GetPluginTemplateDescriptions to obtain a valid TemplateInfo. |
| Official | `PluginToolset.PluginToolset` | `GetPluginDependencies` | Returns the dependency entries from a plugin's Plugins array in its .uplugin file. |
| Official | `PluginToolset.PluginToolset` | `GetPluginDependents` | Returns the names of all discovered plugins that declare a dependency on the given plugin. |
| Official | `PluginToolset.PluginToolset` | `GetPluginDescriptor` | Gets the editable descriptor fields for a discovered plugin. |
| Official | `PluginToolset.PluginToolset` | `GetPluginForAsset` | Returns the name of the enabled plugin whose content mount point contains the given asset path. Accepts full asset paths or mount point prefixes... |
| Official | `PluginToolset.PluginToolset` | `GetPluginInfo` | Gets metadata for a discovered plugin, including description, version, base directory, content directory, descriptor path, and mounted asset path. |
| Official | `PluginToolset.PluginToolset` | `GetPluginTemplateDescriptions` | Returns the list of available plugin templates. Pass one of the results to CreatePlugin to create a new plugin from that template. |
| Official | `PluginToolset.PluginToolset` | `IsEnabled` | Checks whether a discovered plugin is currently enabled. |
| Official | `PluginToolset.PluginToolset` | `IsPluginCreationAllowed` | Checks whether the editor settings permit plugin creation from the plugin browser. |
| Official | `PluginToolset.PluginToolset` | `IsPluginModificationAllowed` | Checks whether the editor settings permit modifying plugins from the plugin browser. |
| Official | `PluginToolset.PluginToolset` | `ListDiscoveredPlugins` | Lists the names of all discovered plugins (enabled and disabled), sorted alphabetically. |
| Official | `PluginToolset.PluginToolset` | `ListEnabledPlugins` | Lists the names of all enabled plugins, sorted alphabetically. |
| Official | `PluginToolset.PluginToolset` | `RemovePluginDependency` | Removes a dependency entry from a plugin's Plugins array in its .uplugin file. |
| Official | `PluginToolset.PluginToolset` | `SetPluginEnabled` | Enables or disables a plugin in the project config. The change takes effect on the next editor restart. |
| Official | `PluginToolset.PluginToolset` | `UpdatePluginDescriptor` | Updates a plugin's descriptor fields and writes them to its .uplugin file. Checks out the file via source control if source control is enabled.... |
| Official | `PluginToolset.PluginToolset` | `ValidateNewPluginNameAndLocation` | Validates that PluginName and RelativePluginLocation are acceptable for a new plugin. |
| Official | `SemanticSearchToolset.SemanticSearchToolset` | `FindSimilar` | Find assets whose embeddings are semantically similar to the given asset's embedding. Vector-only (no BM25). The source asset must already be... |
| Official | `SemanticSearchToolset.SemanticSearchToolset` | `Search` | Run a semantic search over the Content Browser assets indexed by the SemanticSearch plugin. |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `Click` | Click a Slate widget identified by its ref. |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `Drag` | Drag from one Slate widget to another (mouse down, move, release). |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `FillForm` | Fill multiple Slate form fields at once. |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `Hover` | Hover over a Slate widget, triggering any hover state or tooltip. |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `ListObservers` | List all active observers as a JSON array for debugging. Each entry includes the observer identifier, whether it is the root observer, the root... |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `Observe` | Register an observer on a widget subtree so its refs are continuously kept up to date (~100ms tick). Call this on the window or panel you are... |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `PressKey` | Press and release a keyboard key on the currently focused Slate widget. Supports modifier prefixes: "Ctrl+C", "Shift+1". |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `Screenshot` | Screenshot a Slate widget or the active editor window. Prefer this over SceneTools.take_screenshot for Editor UI; use SceneTools only for 3D viewport. |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `SelectOption` | Select an option in a Slate combobox by its text label. Opens the dropdown, finds the matching text, and clicks it. |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `Snapshot` | Capture a Slate UI accessibility snapshot. Use this to read the current widget tree and discover refs for action tools (Click, Type, Hover, etc.).... |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `Type` | Type text into a Slate text input widget. Focuses the widget first, then sends one key event per character. |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `Unobserve` | Remove an observer by its identifier. |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `WaitFor` | Check if text is present or absent in the Slate widget tree. Non-blocking: checks once and returns immediately. Poll to wait. |
| Official | `SlateInspectorToolset.SlateInspectorToolset` | `Windows` | List, select, or close top-level Slate editor windows. |
| Official | `ToolsetRegistry.AgentSkillToolset` | `CreateSkill` | Creates a new AgentSkill. This should ONLY be called after getting explicit direction or permission from the user. |
| Official | `ToolsetRegistry.AgentSkillToolset` | `GetSkills` | Returns detailed information about a specific set of AgentSkills. |
| Official | `ToolsetRegistry.AgentSkillToolset` | `ListSkills` | Gets a summary of all AgentSkills in the project. |
| Official | `ToolsetRegistry.AgentSkillToolset` | `UpdateSkill` | Updates an existing AgentSkill. This should ONLY be called after getting explicit direction or permission from the user. |
| Official | `UMGToolSet.UMGToolSet` | `AddUIComponent` | Adds a UI component of the given class to the named widget. |
| Official | `UMGToolSet.UMGToolSet` | `AddWidget` | Adds a widget to the tree at the specified position. Returns full widget info including Slot pointer. When ParentWidget is null and no root... |
| Official | `UMGToolSet.UMGToolSet` | `BindToEventProperty` | Adds a Blueprint event handler graph node bound to a widget's multicast delegate event, |
| Official | `UMGToolSet.UMGToolSet` | `CompileWidgetBlueprint` | Compiles a widget blueprint. Returns false with error details if compilation fails. Errors include missing BindWidget bindings, type mismatches,... |
| Official | `UMGToolSet.UMGToolSet` | `CreateWidgetBlueprint` | Creates a new Widget Blueprint asset. Returns the blueprint or nullptr on failure. |
| Official | `UMGToolSet.UMGToolSet` | `GetNamedSlots` | Returns named slot bindings (separate from tree hierarchy). |
| Official | `UMGToolSet.UMGToolSet` | `GetWidgetClassInfo` | Returns the Category, Description and if it's a Panel for a single widget class. Same per-entry data as ListWidgetClasses, but lets callers query... |
| Official | `UMGToolSet.UMGToolSet` | `GetWidgetDescription` | Full property dump of every widget in the tree. Each line: [N] Type Name Prop:Value ... slot:(SlotProp:Value ...) N is the 0-based index into... |
| Official | `UMGToolSet.UMGToolSet` | `GetWidgetTreeDepth` | Returns the maximum depth of the widget tree. Depth: root with no children = 0; root + children = 1; etc. |
| Official | `UMGToolSet.UMGToolSet` | `GetWidgets` | Returns blueprint info and all widgets in depth-first order. Children within each parent are in their panel slot order - this is the hierarchy... |
| Official | `UMGToolSet.UMGToolSet` | `ListWidgetBlueprints` | Lists widget blueprints in a content folder. |
| Official | `UMGToolSet.UMGToolSet` | `ListWidgetClasses` | Lists available widget classes, optionally filtered by name substring. |
| Official | `UMGToolSet.UMGToolSet` | `MoveUIComponent` | Moves a UI component before or after another component on the same widget. |
| Official | `UMGToolSet.UMGToolSet` | `MoveWidget` | Moves a widget to a new parent panel at the specified position. Returns updated widget info with new Slot. |
| Official | `UMGToolSet.UMGToolSet` | `RemoveUIComponent` | Removes a UI component of the given class from the named widget. |
| Official | `UMGToolSet.UMGToolSet` | `RemoveWidget` | Removes a widget and its children from the tree. |
| Official | `UMGToolSet.UMGToolSet` | `RenameWidget` | Renames a widget. Returns updated widget info or empty on failure. |
| Official | `UMGToolSet.UMGToolSet` | `ReplaceWidgetWithChild` | Replaces a panel widget with its first child, removing the panel from the tree. The widget to replace must be a UPanelWidget with only one child. |
| Official | `UMGToolSet.UMGToolSet` | `ReplaceWidgetWithNamedSlot` | Replaces a host widget with the content of one of its named slots. The host must implement INamedSlotInterface (e.g., a UUserWidget exposing named... |
| Official | `UMGToolSet.UMGToolSet` | `ReplaceWidgetWithTemplate` | Replaces a widget instance in the blueprint's widget tree with a new instance created from a different template widget class. Preserves references... |
| Official | `UMGToolSet.UMGToolSet` | `SetNamedSlotContent` | Sets content for a named slot. Returns full widget info including Slot pointer. |
| Official | `UMGToolSet.UMGToolSet` | `ToggleWidgetAsVariable` | Sets the bIsVariable flag. |
| Official | `UMGToolSet.UMGToolSet` | `WrapWidgets` | Wraps one or more widgets in a new panel widget of the specified class. Only the root-most widgets in the selection are wrapped — children of... |
| Official | `WorldConditionsToolset.WorldConditionTools` | `GetConditionDescription` | Returns a human-readable description of a single world condition. The condition must be passed as an FInstancedStruct containing an... |
| Official | `WorldConditionsToolset.WorldConditionTools` | `GetQueryDescription` | Returns a human-readable description of a world condition query. |
| Official | `aimodule_toolset.toolsets.behavior_tree.BehaviorTreeTools` | `get_blackboard` | Returns the blackboard asset for this behavior tree. |
| Official | `aimodule_toolset.toolsets.behavior_tree.BehaviorTreeTools` | `get_children` | Returns direct child nodes of a composite node. |
| Official | `aimodule_toolset.toolsets.behavior_tree.BehaviorTreeTools` | `get_node_depth` | Returns the tree depth of a node by its list_nodes index. |
| Official | `aimodule_toolset.toolsets.behavior_tree.BehaviorTreeTools` | `get_node_depths` | Returns tree depths for all nodes, matching list_nodes order. |
| Official | `aimodule_toolset.toolsets.behavior_tree.BehaviorTreeTools` | `get_root_decorators` | Returns root-level decorators on this tree. |
| Official | `aimodule_toolset.toolsets.behavior_tree.BehaviorTreeTools` | `get_subtree` | Returns the sub-BT asset referenced by a RunBehavior task. |
| Official | `aimodule_toolset.toolsets.behavior_tree.BehaviorTreeTools` | `list_nodes` | Returns a flat list of all node UObjects in tree order. |
| Official | `animation_toolset.toolsets.conditions.SequencerConditionTools` | `clear_section_condition` | Remove the condition from a section. |
| Official | `animation_toolset.toolsets.conditions.SequencerConditionTools` | `clear_track_condition` | Remove the condition from a track. |
| Official | `animation_toolset.toolsets.conditions.SequencerConditionTools` | `clear_track_row_condition` | Remove the condition from a specific track row. |
| Official | `animation_toolset.toolsets.conditions.SequencerConditionTools` | `get_section_condition` | Get the condition on a section. |
| Official | `animation_toolset.toolsets.conditions.SequencerConditionTools` | `get_track_condition` | Get the track-level condition. |
| Official | `animation_toolset.toolsets.conditions.SequencerConditionTools` | `get_track_row_condition` | Get the condition on a specific track row. |
| Official | `animation_toolset.toolsets.conditions.SequencerConditionTools` | `set_section_condition` | Set a condition on a section. |
| Official | `animation_toolset.toolsets.conditions.SequencerConditionTools` | `set_track_condition` | Set a condition on a track. |
| Official | `animation_toolset.toolsets.conditions.SequencerConditionTools` | `set_track_row_condition` | Set a condition on a specific track row. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_backward_solve_graph` | Create a backward solve graph with InverseExecution event. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_bone` | Add a bone to the Control Rig hierarchy. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_control` | Add a control to the hierarchy. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_element` | Add a bone or null element to the Control Rig hierarchy. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_event_graph` | Create a new graph with the specified event type. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_event_node` | Add an event node to the graph. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_graph` | Create a new empty graph in the Control Rig. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_interaction_graph` | Create an interaction graph with InteractionExecution event. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_null` | Add a null (locator) to the Control Rig hierarchy. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_variable` | Add a member variable to the Control Rig. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `add_variable_node` | Create a variable getter or setter node. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `change_variable_type` | Change the type of an existing variable. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `connect_pins` | Connect two pins together. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `create` | Creates a new Control Rig at the given location. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `create_node` | Create a new RigUnit node in the graph. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `delete_node` | Delete a node from the graph. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `disconnect_pins` | Disconnect two pins. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_all_bones` | Get all bones in the Control Rig hierarchy. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_all_controls` | Get all controls in the Control Rig hierarchy. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_all_nulls` | Get all nulls (locators) in the Control Rig hierarchy. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_backward_solve_graph` | Get the backward solve graph. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_children` | Get children of a hierarchy element. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_connected_pins` | Get all pins connected to this pin. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_elements` | Get hierarchy elements of the specified type. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_event_graph` | Get a graph containing the specified event type. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_forward_solve_graph` | Get the forward solve graph (main execution graph). |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_global_transform` | Get global transform of a hierarchy element. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_graph` | Get a specific graph from the Control Rig by name. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_interaction_graph` | Get the interaction graph. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_local_transform` | Get local transform of a hierarchy element. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_node_position` | Get the position of a node in the graph editor. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_parent` | Get the parent of a hierarchy element. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_pin_value` | Get the default value of a pin. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `get_variable` | Get a specific variable by name. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `import_bones_from_asset` | Import bones from the given skeletal mesh to the Control Rig hierarchy. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `list_graphs` | List all graphs in the Control Rig. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `list_nodes` | List all nodes in a graph. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `list_pins` | List all pins on a node. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `list_variables` | List all member variables in the Control Rig. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `remove_variable` | Remove a member variable from the Control Rig. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `set_global_transform` | Set global transform of a hierarchy element. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `set_local_transform` | Set local transform of a hierarchy element. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `set_node_position` | Set the position of a node in the graph editor. |
| Official | `animation_toolset.toolsets.controlrig.ControlRigTools` | `set_pin_value` | Set the default value of a pin. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `add_layer_from_selection` | Add an animation layer from the currently selected objects in Sequencer. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `bake_space` | Bake Control Rig controls' space over a frame range. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `bake_to_control_rig` | Bake existing animation on a binding into a Control Rig track. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `blend_values_on_selected` | Perform a blend operation on selected keys or controls. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `clear_selection` | Clear the current Control Rig control selection. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `collapse_anim_layers` | Collapse all sections and layers on a Control Rig track into one section. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `delete_anim_layer` | Delete an animation layer at the specified index. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `delete_space` | Delete a space-switch key at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `duplicate_anim_layer` | Duplicate an animation layer at the specified index. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `export_fbx_from_rig` | Export an FBX file from a Control Rig section. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `find_or_create_track` | Add a Control Rig track to a binding using a Control Rig asset. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `frame_selection` | Frame the viewport to the current Control Rig control selection. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_actor_transform_at_frame` | Get an actor's world transform at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_anim_layers` | Get all animation layers from the active Sequencer. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_anim_mode_gizmo_scale` | Get the editor's transform gizmo size. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_anim_mode_hide_manips` | Get whether Animation Mode hides all manipulators. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_anim_mode_hierarchy` | Get whether the Animation Mode draws hierarchy lines/dots. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_anim_mode_local_spaces` | Get whether multi-select transforms act in each control's own space. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_anim_mode_nulls` | Get whether the Animation Mode draws nulls. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_anim_mode_only_rig_sel` | Get whether Animation Mode restricts viewport selection to rig controls. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_bool` | Get a bool control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_control_rigs` | Get all Control Rigs currently in the sequence. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_controls_info` | Get all controls on a Control Rig with their names and types. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_controls_mask` | Check if a control is visible (unmasked) on a section. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_euler_transform` | Get an EulerTransform control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_float` | Get a float control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_int` | Get an integer control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_position` | Get a position control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_priority_order` | Get the evaluation priority order of a Control Rig track. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_rotator` | Get a rotator control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_scale` | Get a scale control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_selected_controls` | Get the currently selected controls on a Control Rig. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_transform` | Get the transform value of a Control Rig control at a frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_vector2d` | Get a Vector2D control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `get_world_transform` | Get a control's world-space transform at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `hide_all_controls` | Hide all controls on a Control Rig section (mask everything). |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `import_fbx_to_rig` | Import an FBX file onto a Control Rig track. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `is_fk_control_rig` | Check if a Control Rig is an FK Control Rig. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `is_layered_control_rig` | Check if a Control Rig in the sequence is in layered mode. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `key_controls` | Key the specified controls on the section at the current Sequencer time. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `key_controls_at_frames` | Key the specified controls at specific frame numbers. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `load_anim_into_rig` | Load an animation sequence into a Control Rig section. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `merge_anim_layers` | Merge specified animation layers into one. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `mirror_selected_controls` | Apply a mirrored pose to the currently selected controls. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `move_space` | Move a space-switch key from one frame to another. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `reorder_anim_layers` | Move an animation layer from one index to another. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `select_control` | Select or deselect a control on a Control Rig. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `select_mirrored_controls` | Select the mirrored counterparts of the currently selected controls. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_anim_mode_gizmo_scale` | Set the editor's transform gizmo size. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_anim_mode_hide_manips` | Toggle whether Animation Mode hides all manipulators. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_anim_mode_hierarchy` | Toggle the Animation Mode hierarchy lines/dots display. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_anim_mode_local_spaces` | Toggle multi-select transforms acting in each control's own space. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_anim_mode_nulls` | Toggle the Animation Mode nulls display. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_anim_mode_only_rig_sel` | Toggle Animation Mode restricting viewport selection to rig controls. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_bool` | Set a bool control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_controls_mask` | Set the visibility mask for the specified controls on a section. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_euler_transform` | Set an EulerTransform control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_float` | Set a float control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_int` | Set an integer control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_layered_mode` | Set a Control Rig track to layered or absolute mode. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_position` | Set a position control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_priority_order` | Set the evaluation priority order of a Control Rig track. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_rotator` | Set a rotator control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_scale` | Set a scale control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_space` | Set the space for a Control Rig control at a given frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_transform` | Set a transform value on a Control Rig control and optionally key it. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_vector2d` | Set a Vector2D control value at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `set_world_transform` | Set a control's world-space transform at a specific frame. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `show_all_controls` | Show all controls on a Control Rig section (unmask everything). |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `snap_control_rig` | Snap Control Rig controls to a target actor over a frame range. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `tween_control_rig` | Perform a tween operation on a Control Rig at the current Sequencer time. |
| Official | `animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools` | `zero_transforms` | Reset Control Rig transforms to their default (usually zero) values. |
| Official | `animation_toolset.toolsets.custom_bindings.SequencerCustomBindingTools` | `change_actor_template_class` | Set the actor class for a spawnable or replaceable template. |
| Official | `animation_toolset.toolsets.custom_bindings.SequencerCustomBindingTools` | `convert_to_custom_binding` | Convert a binding to a custom binding type. |
| Official | `animation_toolset.toolsets.custom_bindings.SequencerCustomBindingTools` | `convert_to_possessable` | Convert a spawnable binding to a possessable. |
| Official | `animation_toolset.toolsets.custom_bindings.SequencerCustomBindingTools` | `convert_to_spawnable` | Convert a possessable binding to a spawnable. |
| Official | `animation_toolset.toolsets.custom_bindings.SequencerCustomBindingTools` | `get_custom_binding_objects` | Get the custom binding instances for a binding. |
| Official | `animation_toolset.toolsets.custom_bindings.SequencerCustomBindingTools` | `get_custom_binding_type` | Get the custom binding class for a binding. |
| Official | `animation_toolset.toolsets.custom_bindings.SequencerCustomBindingTools` | `get_custom_bindings_of_type` | Find all bindings of a given custom type in the current sequence. |
| Official | `animation_toolset.toolsets.custom_bindings.SequencerCustomBindingTools` | `save_default_spawnable_state` | Save the current state of a spawnable as its default. |
| Official | `animation_toolset.toolsets.import_export.SequencerImportExportTools` | `export_anim_sequence` | Export animation from a sequence binding to an AnimSequence asset. |
| Official | `animation_toolset.toolsets.import_export.SequencerImportExportTools` | `export_fbx` | Export a level sequence to FBX. |
| Official | `animation_toolset.toolsets.import_export.SequencerImportExportTools` | `get_linked_anim_sequences` | Get content paths of all AnimSequences linked to a LevelSequence. |
| Official | `animation_toolset.toolsets.import_export.SequencerImportExportTools` | `get_linked_level_sequence` | Get the content path of the LevelSequence linked to an AnimSequence. |
| Official | `animation_toolset.toolsets.import_export.SequencerImportExportTools` | `import_fbx` | Import FBX data into a level sequence. |
| Official | `animation_toolset.toolsets.import_export.SequencerImportExportTools` | `link_anim_sequence` | Link an AnimSequence asset to a level sequence binding. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `add_key_bool` | Add a bool key to a channel on a section. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `add_key_float` | Add a float key to a channel on a section. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `add_key_integer` | Add an integer key to a channel on a section. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `add_key_string` | Add a string key to a channel on a section. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `bake_channel_keys` | Bake a channel's values over a frame range. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `close_curve_editor` | Close the Sequencer Curve Editor panel. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `curve_editor_empty_selection` | Clear all key selection in the Curve Editor. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `curve_editor_select_keys` | Select keys by index in the Curve Editor. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `get_channel_names` | Get the names of all channels on a section. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `get_curve_editor_selected_keys` | Get selected key indices for a channel in the Curve Editor. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `get_default_value` | Get the default value of a float channel. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `get_keys` | Get all keys on a channel, returned as a JSON array. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `get_keys_by_index` | Get specific keys on a channel by their indices, returned as JSON. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `get_selected_channels` | Get the currently selected channels in the Sequencer editor. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `get_selected_key_channels` | Get channels that have selected keys in the Curve Editor. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `is_curve_editor_open` | Check whether the Curve Editor panel is currently open. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `is_curve_shown` | Check if a curve is visible in the Curve Editor. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `open_curve_editor` | Open the Sequencer Curve Editor panel. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `remove_key_at_frame` | Remove a key at a specific frame from a channel. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `select_channels` | Set the channel selection in the Sequencer editor. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `set_default_value` | Set the default value of a channel. |
| Official | `animation_toolset.toolsets.keyframing.SequencerKeyframingTools` | `show_curve` | Show or hide a curve in the Curve Editor. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_deactivated_nodes` | Get the currently deactivated outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_locked_nodes` | Get the currently locked outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_muted_nodes` | Get the currently muted outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_node_label` | Get the display label of an outliner node. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_outliner_children` | Get child nodes of an outliner node. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_outliner_selection` | Get the currently selected nodes in the outliner. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_outliner_tree` | Get a full snapshot of the Sequencer outliner tree. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_pinned_nodes` | Get the currently pinned outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_sections_for_nodes` | Get sections associated with the given outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `get_soloed_nodes` | Get the currently soloed outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `is_node_expanded` | Check whether an outliner node is expanded. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `set_node_deactivated` | Deactivate or reactivate outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `set_node_expanded` | Expand or collapse outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `set_node_locked` | Lock or unlock outliner nodes for editing. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `set_node_muted` | Mute or unmute outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `set_node_pinned` | Pin or unpin outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `set_node_solo` | Solo or unsolo outliner nodes. |
| Official | `animation_toolset.toolsets.outliner.SequencerOutlinerTools` | `set_outliner_selection` | Set the outliner selection. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_actors` | Add actors from the level to the currently open sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_actors_by_name` | Add actors to the sequence by their names in the level. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_actors_to_binding` | Add actors to an existing binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_binding_to_folder` | Add a binding into a folder for organization. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_event_repeater_section` | Add an event repeater section to an event track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_event_trigger_section` | Add an event trigger section to an event track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_marked_frame` | Add a marked frame (bookmark) to the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_root_folder` | Create a new root-level folder in the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_section` | Add a new section to a track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_spawnable_from_class` | Create a spawnable binding from an actor class. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_spawnable_from_instance` | Create a spawnable binding from an existing object instance. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_track_to_binding` | Add a track of the given type to a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_track_to_folder` | Add a track into a folder for organization. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `add_track_to_sequence` | Add a sequence-level (master) track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `bake_transform` | Bake transforms for the given bindings at every frame. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `close_sequence` | Close the currently open level sequence editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `copy_bindings` | Copy one or more bindings to the Sequencer clipboard. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `copy_folders` | Copy one or more folders to the Sequencer clipboard. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `copy_sections` | Copy one or more sections to the Sequencer clipboard. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `copy_tracks` | Copy one or more tracks to the Sequencer clipboard. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `create_camera` | Create a new cine camera actor in the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `create_level_sequence` | Create a new Level Sequence asset. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `delete_all_marked_frames` | Delete all marked frames from the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `delete_marked_frame` | Delete a marked frame by index. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `empty_selection` | Clear all selection in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `find_binding_by_name` | Find a binding by its display name. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `find_binding_by_tag` | Find the first binding with the given tag in the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `find_bindings_by_tag` | Find all bindings with the given tag in the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `find_marked_frame_by_label` | Find a marked frame by label. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `find_tracks_by_type` | Find all tracks of a specific type on a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `fix_actor_references` | Attempt to auto-fix broken actor references in the current sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `focus_parent_sequence` | Navigate up one level in the sub-sequence hierarchy. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `focus_sub_sequence` | Navigate into a sub-sequence via its sub-section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `force_evaluate` | Force the Sequencer to evaluate and update the viewport. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_all_binding_tags` | Get every tag name registered in the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_binding_id` | Get the binding ID for a binding proxy. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_binding_name` | Get the display name of a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_binding_tags` | Get the tags currently attached to a specific binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_bindings` | Get all bindings in the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_bound_objects` | Get the objects currently resolved by a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_child_possessables` | Get component bindings under an actor binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_clock_source` | Get the clock source for the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_current_sequence` | Get the root level sequence currently open in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_display_rate` | Get the display frame rate of a sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_evaluation_type` | Get the evaluation type of a sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_focused_sequence` | Get the currently focused level sequence in the hierarchy. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_folder_contents` | Get the tracks and bindings inside a folder. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_loop_mode` | Get the current loop playback mode. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_marked_frames` | Get all marked frames (bookmarks) in the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_playback_range` | Get the playback start and end frames of a sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_playback_speed` | Get the current playback speed multiplier. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_playhead_frame` | Get the current playhead position in display rate frames. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_root_folders` | Get all root-level folders in the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_section_blend_type` | Get the blend type of a section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_section_completion_mode` | Get the completion mode of a section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_section_ease_in` | Get the effective ease-in duration of a section in frames. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_section_ease_out` | Get the effective ease-out duration of a section in frames. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_section_post_roll_frames` | Get the number of post-roll frames configured on a section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_section_pre_roll_frames` | Get the number of pre-roll frames configured on a section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_section_properties` | Get all common properties of a section in a single call. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_section_range` | Get the frame range of a section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_section_to_key` | Get the active section that receives new keys on a track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_sections` | Get all sections on a track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_selected_bindings` | Get the currently selected bindings in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_selected_folders` | Get the currently selected folders in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_selected_sections` | Get the currently selected sections in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_selected_tracks` | Get the currently selected tracks in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_selection_range` | Get the selection range (green bar) start and end frames. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_sequence_lock_state` | Check whether the current level sequence is locked. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_sub_sequence_hierarchy` | Get the current sub-sequence hierarchy path. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_tick_resolution` | Get the internal tick resolution of a sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_track_display_name` | Get the display name of a track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_track_filter_names` | Get all available track filter names. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_tracks_on_binding` | Get all tracks on a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_tracks_on_sequence` | Get all sequence-level (master) tracks. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_view_range` | Get the visible time range in the Sequencer timeline. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `get_work_range` | Get the work range of the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `has_section_end_frame` | Check if a section has a bounded end frame (vs infinite). |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `has_section_start_frame` | Check if a section has a bounded start frame (vs infinite). |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `is_camera_cut_locked` | Check if the camera cut is locked to the viewport. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `is_playback_range_locked` | Check if the playback range is locked. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `is_playing` | Check whether the sequence is currently playing. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `is_sequence_locked` | Check if the current sequence and its descendants are locked. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `is_track_filter_active` | Check whether a track filter is currently active. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `open_sequence` | Open a level sequence asset in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `paste_bindings` | Paste bindings from the clipboard (or a token returned by copy_bindings). |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `paste_folders` | Paste folders from the clipboard into the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `paste_sections` | Paste sections from the clipboard onto the given tracks. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `paste_tracks` | Paste tracks from the clipboard onto the given bindings. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `pause` | Pause playback of the current sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `play` | Start playback of the current sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `play_to` | Play from the current position to a specific frame, then stop. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `rebind_component` | Rebind component bindings to a named component. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `refresh_sequence` | Force refresh the Sequencer editor UI on the next tick. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `remove_actors_from_binding` | Remove specific actors from a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `remove_all_bindings` | Remove all bound actors from a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `remove_binding` | Remove a binding from the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `remove_binding_tag` | Remove a tag from the sequence entirely. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `remove_invalid_bindings` | Remove missing or broken actor references from a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `remove_root_folder` | Remove a root-level folder from the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `remove_section` | Remove a section from a track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `remove_track` | Remove a track from a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `remove_track_from_sequence` | Remove a sequence-level (master) track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `replace_binding_with_actors` | Replace all bound actors on a binding with new ones. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `select_bindings` | Set the binding selection in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `select_folders` | Set the folder selection in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `select_sections` | Set the section selection in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `select_tracks` | Set the track selection in the Sequencer editor. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_binding_name` | Set the display name of a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_byte_track_enum` | Configure a byte track to use a specific enum type. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_camera_cut_binding` | Set which camera a camera cut section uses. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_camera_lock` | Lock or unlock the camera cut to the viewport. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_clock_source` | Set the clock source for the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_display_rate` | Set the display frame rate of a sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_evaluation_type` | Set the evaluation type of a sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_loop_mode` | Enable or disable loop playback. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_playback_range` | Set the playback start and end frames of a sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_playback_range_locked` | Lock or unlock the playback range. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_playback_speed` | Set the playback speed multiplier. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_playhead_frame` | Set the playhead position in display rate frames. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_property_name_and_path` | Configure a property track to animate a specific UProperty. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_animation` | Set the animation asset on a skeletal animation section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_blend_type` | Set the blend type of a section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_completion_mode` | Set the completion mode of a section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_ease_in` | Set the ease-in duration of a section in frames. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_ease_out` | Set the ease-out duration of a section in frames. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_end_bounded` | Set whether the section end frame is bounded or infinite. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_post_roll_frames` | Set the number of frames to post-roll this section after it ends. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_pre_roll_frames` | Set the number of frames to pre-roll this section before it starts. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_range` | Set the frame range of a section. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_section_start_bounded` | Set whether the section start frame is bounded or infinite. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_selection_range` | Set the selection range (green bar) start and end frames. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_sequence_locked` | Lock or unlock the current sequence and its descendants. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_tick_resolution` | Set the internal tick resolution of a sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_track_display_name` | Set the display name of a track. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_track_filter_active` | Enable or disable a track filter. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_view_range` | Set the visible time range in the Sequencer timeline. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `set_work_range` | Set the work range of the sequence. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `tag_binding` | Attach a tag to a binding. |
| Official | `animation_toolset.toolsets.sequencer.SequencerTools` | `untag_binding` | Remove a tag from a binding. |
| Official | `conversation_toolset.toolsets.conversation.ConversationTools` | `get_all_nodes` | Returns all reachable nodes in the conversation. |
| Official | `conversation_toolset.toolsets.conversation.ConversationTools` | `get_node_by_guid` | Returns a conversation node by its GUID. |
| Official | `conversation_toolset.toolsets.conversation.ConversationTools` | `get_node_connections` | Returns output connection GUIDs for a conversation node. |
| Official | `conversation_toolset.toolsets.conversation.ConversationTools` | `get_node_guids` | Returns GUIDs of all reachable nodes, in map iteration order. |
| Official | `conversation_toolset.toolsets.conversation.ConversationTools` | `get_sub_nodes` | Returns sub-nodes (requirements, choices) attached to a task node. |
| Official | `conversation_toolset.toolsets.conversation.ConversationTools` | `list_entry_points` | Returns entry points (FConversationEntryList structs). |
| Official | `conversation_toolset.toolsets.conversation.ConversationTools` | `list_speakers` | Returns speaker/participant information. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `add_component` | Adds a component to an actor instance or blueprint. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `add_tag` | Adds a tag to an actor. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `get_actor_bounds` | Returns the bounding box of an actor. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `get_actor_transform` | Returns the position, rotation, and scale of an actor. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `get_component_actor` | Returns the actor that owns the specified component. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `get_components` | Returns the components that an actor contains. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `get_label` | Returns the actor's human friendly name as it appears in the editor. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `get_parent_component` | Returns the parent component that this component is attached to, if any. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `get_root_component` | Returns the root component of an actor, if any. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `get_tags` | Returns the list of tags on an actor. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `has_tag` | Returns whether an actor has a specific tag. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `look_at` | Rotates an actor so its forward vector points at a world-space position. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `remove_component` | Removes a component from an actor instance or blueprint. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `remove_tag` | Removes a tag from an actor. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `set_actor_transform` | Updates the position, rotation, and/or scale of an actor. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `set_label` | Sets the human-friendly name of the actor. |
| Official | `editor_toolset.toolsets.actor.ActorTools` | `set_parent_component` | Sets the parent for the specified scene component. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `can_edit_asset` | Checks whether an asset can be edited. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `create_folder` | Creates a folder at the specified path. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `delete` | Deletes an asset or folder. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `duplicate` | Makes a copy of a folder or asset. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `exists` | Determines if a folder or asset exists. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `find_assets` | Searches the project for assets that match specific criteria. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `get_asset_class` | Gets the class of an asset. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `get_asset_tags` | Gets the asset registry tags for an asset. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `get_dependencies` | Lists assets that the specified asset depends on. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `get_metadata_tags` | Gets the metadata tags for an asset. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `get_plugin_content_paths` | Returns the root content paths for plugins that have content. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `get_referencers` | Lists assets that reference the specified asset. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `is_checked_out` | Checks whether an asset is checked out by the current user. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `is_dirty` | Checks whether an asset has unsaved changes. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `list_folders` | Lists the folders contained within a folder. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `load_asset` | Loads an asset from the project. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `move` | Moves or renames an asset or folder. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `read_file` | Reads a text file from disk and returns its contents. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `save_assets` | Saves assets to disk. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `update_metadata_tags` | Sets or removes metadata tags on an asset. |
| Official | `editor_toolset.toolsets.asset.AssetTools` | `write_file` | Writes text content to a file on disk. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_component_bound_event` | Creates a component bound event node in the event graph. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_event` | Adds an event node to the Blueprint's event graph. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_event_dispatcher` | Adds an event dispatcher to a Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_function_graph` | Adds a function graph to the Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_function_param` | Adds an input or output to a function or event dispatcher |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_node_pin` | Adds a pin to a node that supports dynamic pin addition. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_object_function_param` | Adds an object reference input or output to a function or event dispatcher. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_object_variable` | Adds a member or local variable that holds an object reference to a Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_struct_function_param` | Adds a struct input or output to a function or event dispatcher. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_struct_variable` | Adds a member or local variable of a struct type to a Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `add_variable` | Adds a member or local variable to a Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `arrange_nodes` | Arranges a list of nodes in a readable left-to-right layout. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `break_pins` | Breaks the connection between two pins. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `compile_blueprint` | Compiles the given Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `connect_pins` | Makes a connection between source (output) and dest (input) pins. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `create` | Creates a new Blueprint asset in the project. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `create_node` | Adds a new node to the graph. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `delete_node` | Deletes the node from its graph. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `find_node_categories` | Retrieves a list of available node categories in a Blueprint graph, optionally filtered by compatible input and/or output pin types |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `find_node_types` | Finds node types that can be created in a particular graph meeting the search criteria. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `find_nodes` | Finds nodes in a graph by title, class, and/or execution role. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_connected_subgraph` | Returns detailed information for all nodes connected to the given node. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_create_event_function` | Returns the function currently bound to a Create Event node. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_default_object` | Returns the Class Default Object (CDO) for a Blueprint's. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_graph` | Retrieves a specific graph from a Blueprint asset by name. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_graph_dsl_docs` | Returns the full syntax reference for write_graph_dsl. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_node_infos` | Retrieves detailed information for a list of Blueprint graph nodes. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_node_type_pins` | Returns the pin names and types for a node type. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_parent` | Returns the parent class of a Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_pin_value` | Gets the value of a Blueprint graph pin. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_variable_category` | Gets the user-defined category of a Blueprint member variable. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `get_variable_replication` | Gets the replication mode of a Blueprint member variable. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `list_compatible_event_functions` | Lists functions that can be bound to a Create Event node. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `list_component_events` | Lists the bindable delegate events available on a component. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `list_event_dispatchers` | Lists all event dispatchers defined on a Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `list_events` | Lists all events visible on the Blueprint — locally defined custom events plus inheritable events from the parent class chain and implemented... |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `list_functions` | Lists all functions visible on the Blueprint — locally defined plus inheritable ones from the parent class chain and implemented interfaces. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `list_graphs` | Lists all graphs in the Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `list_variables` | Lists member or local variables defined on a Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `read_graph_dsl` | Reads a Blueprint graph and returns a DSL script. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `remove_function_graph` | Removes a function graph or event dispatcher from the Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `remove_function_param` | Removes an input or output from a function or event dispatcher. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `remove_node_pin` | Removes a specific pin from a node that supports dynamic pin removal. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `remove_variable` | Removes a member or local variable from a Blueprint. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `retarget_node_class` | Replaces a node's baked-in class reference from old_class to new_class in place. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `set_create_event_function` | Binds a function to a Create Event node. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `set_node_position` | Sets a new position for the node. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `set_parent` | Reparents a Blueprint to a new parent class. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `set_pin_value` | Sets the value of a Blueprint graph pin. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `set_variable_category` | Sets the user-defined category on a Blueprint member variable. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `set_variable_instance_editable` | Sets whether a member variable is editable per-instance on actors placed in the level. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `set_variable_replication` | Sets the replication mode on a Blueprint member variable. |
| Official | `editor_toolset.toolsets.blueprint.BlueprintTools` | `write_graph_dsl` | Populates a Blueprint graph with nodes from a DSL script and compiles the Blueprint. |
| Official | `editor_toolset.toolsets.curve_table.CurveTableTools` | `add_key` | Adds a key to a row. |
| Official | `editor_toolset.toolsets.curve_table.CurveTableTools` | `add_row` | Adds a new row to the curve table with an optional default value. |
| Official | `editor_toolset.toolsets.curve_table.CurveTableTools` | `create` | Creates a new CurveTable asset. |
| Official | `editor_toolset.toolsets.curve_table.CurveTableTools` | `get_keys` | Returns all keys for a row. |
| Official | `editor_toolset.toolsets.curve_table.CurveTableTools` | `import_file` | Imports a file from disk as a CurveTable asset. |
| Official | `editor_toolset.toolsets.curve_table.CurveTableTools` | `list_rows` | Lists the names of all rows in the curve table. |
| Official | `editor_toolset.toolsets.curve_table.CurveTableTools` | `remove_row` | Removes a row from the curve table. |
| Official | `editor_toolset.toolsets.curve_table.CurveTableTools` | `rename_row` | Renames a row in the curve table. |
| Official | `editor_toolset.toolsets.curve_table.CurveTableTools` | `set_keys` | Replaces all keys in a row with the provided list. |
| Official | `editor_toolset.toolsets.data_asset.DataAssetTools` | `create` | Creates a new DataAsset asset in the project. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `add_rows` | Adds new rows with default values to the data table. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `create` | Creates a new DataTable asset with the specified column schema. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `get_rows` | Returns the column values for one or more rows as a JSON string. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `get_schema` | Returns the column schema of the data table as a JSON string. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `import_file` | Imports a file from disk as a DataTable asset. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `list_rows` | Lists the names of all rows in the data table. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `remove_rows` | Removes rows from the data table. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `rename_rows` | Renames one or more rows in the data table. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `search_row_structs` | Finds structs that can be used as a DataTable schema. |
| Official | `editor_toolset.toolsets.data_table.DataTableTools` | `set_rows` | Sets column values for one or more rows. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `add_expression` | Adds a new expression node to a Material or MaterialFunction graph. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `connect_expressions` | Connects an expression node's output pin to another expression node's input pin. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `connect_to_output` | Connects an expression node's output to one of the material's output properties. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `create_function` | Creates a new empty MaterialFunction asset. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `create_material` | Creates a new empty Material asset. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `create_parameter_collection` | Creates a new empty MaterialParameterCollection (MPC) asset. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `delete_expression` | Removes an expression node from a Material or MaterialFunction graph. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `delete_parameter_group` | Removes a parameter group, ungrouping all parameters that belong to it. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `delete_unused_expressions` | Deletes all expression nodes not connected to any material output. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `disconnect_expressions` | Disconnects the input pin of an expression node, removing whatever is connected to it. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `disconnect_from_output` | Disconnects the expression currently connected to a material output property. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `get_expression_input_names` | Returns the names of all input pins on a material expression node. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `get_expression_inputs` | Returns the current wiring of each input pin on a material expression. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `get_expression_output_names` | Returns the names of all output pins on a material expression node. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `get_expressions` | Returns all expression nodes in a Material or MaterialFunction graph. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `get_property_input` | Returns the expression and output pin feeding a material output property. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `get_referencing_materials` | Returns asset data for all Materials that reference this MaterialFunction. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `layout_expressions` | Automatically arranges all expression nodes in a Material or MaterialFunction graph. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `list_expression_classes` | Returns MaterialExpression subclasses valid for the given context. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `list_parameter_groups` | Returns the unique parameter group names defined in a Material or MaterialFunction. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `recompile` | Recompiles a Material or MaterialFunction after edits. |
| Official | `editor_toolset.toolsets.material.MaterialTools` | `rename_parameter_group` | Renames a parameter group across all parameter expressions in a Material or MaterialFunction. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `clear_parameters` | Clears all parameter overrides on a material instance, reverting to parent defaults. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `create` | Creates a new MaterialInstanceConstant asset derived from a parent material. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `get_scalar_parameter` | Gets the current value of a scalar parameter on a material instance. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `get_static_switch_parameter` | Gets the value of a static switch parameter on a material instance. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `get_texture_parameter` | Gets the texture assigned to a texture parameter on a material instance. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `get_vector_parameter` | Gets the current value of a vector parameter on a material instance. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `list_parameters` | Returns all parameters exposed by a material or instance, with their names and types. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `set_parameter_override` | Enables or disables a parameter override on a material instance. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `set_parent` | Changes the parent of a material instance. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `set_scalar_parameter` | Sets the value of a scalar parameter on a material instance. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `set_static_switch_parameter` | Sets the value of a static switch parameter on a material instance. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `set_texture_parameter` | Assigns a texture to a texture parameter on a material instance. |
| Official | `editor_toolset.toolsets.material_instance.MaterialInstanceTools` | `set_vector_parameter` | Sets the value of a vector parameter on a material instance. |
| Official | `editor_toolset.toolsets.object.ObjectTools` | `get_class` | Returns the class of an Unreal object. |
| Official | `editor_toolset.toolsets.object.ObjectTools` | `get_properties` | Returns the values of one or more properties on an object. |
| Official | `editor_toolset.toolsets.object.ObjectTools` | `list_properties` | Returns a list of properties that are on the specified object. |
| Official | `editor_toolset.toolsets.object.ObjectTools` | `reset_properties` | Resets one or more properties on an object to their default values, removing any per-instance overrides. |
| Official | `editor_toolset.toolsets.object.ObjectTools` | `search_subclasses` | Finds all subclasses of a given class. |
| Official | `editor_toolset.toolsets.object.ObjectTools` | `set_properties` | Sets the values of properties on an object. |
| Official | `editor_toolset.toolsets.primitive.PrimitiveTools` | `add_cone` | Adds a cone-shaped StaticMeshComponent to an actor. |
| Official | `editor_toolset.toolsets.primitive.PrimitiveTools` | `add_cube` | Adds a cube-shaped StaticMeshComponent to an actor. |
| Official | `editor_toolset.toolsets.primitive.PrimitiveTools` | `add_cylinder` | Adds a cylinder-shaped StaticMeshComponent to an actor. |
| Official | `editor_toolset.toolsets.primitive.PrimitiveTools` | `add_sphere` | Adds a sphere-shaped StaticMeshComponent to an actor. |
| Official | `editor_toolset.toolsets.programmatic.ProgrammaticToolset` | `execute_tool_script` | Execute a Python script against the toolset APIs. |
| Official | `editor_toolset.toolsets.programmatic.ProgrammaticToolset` | `get_execution_environment` | Get details about execution environment. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `add_to_scene_from_asset` | Creates a new actor in the scene from an asset. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `add_to_scene_from_class` | Creates a new instance of the specified object at the specified transform. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `can_edit` | Checks whether an actor can be edited. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `commit_level_instance` | Saves or discards edits to a level instance and exits edit mode. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `create_level_instance` | Creates a Level Instance actor in the scene referencing an existing level asset. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `delete_folder` | Deletes a folder from the outliner. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `edit_level_instance` | Opens a level instance for editing. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `find_actors` | Searches the scene for actors that match specific criteria. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `get_actors_in_folder` | Returns the actors in the specified outliner folder. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `get_collision_channels` | Returns all available collision channels for use with find_actors. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `get_current_level` | Returns the path to the current level asset. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `get_folders` | Returns all folder paths currently in use in the outliner. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `is_checked_out` | Checks whether an actor is checked out by the current user. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `load_level` | Loads a level in the editor. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `merge_actors` | Merges multiple StaticMesh actors into a single mesh asset and actor. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `remove_from_scene` | Deletes an actor from the scene. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `rename_folder` | Renames a folder in the outliner. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `save_actor` | Saves the actor to disk. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `set_actor_folder` | Assigns an actor to the specified folder in the outliner. |
| Official | `editor_toolset.toolsets.scene.SceneTools` | `trace_world` | Traces a line through the world and returns the distance to the first hit. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `add_socket` | Adds a named socket to a skeletal mesh attached to a bone. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `assign_physics_asset` | Assigns a physics asset to a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_bone_children` | Returns the direct children of a bone. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_bone_names` | Returns the names of all bones in a skeletal mesh in hierarchy order. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_bone_parent` | Returns the name of a bone's parent, or an empty string for the root bone. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_bounds` | Returns the local-space bounding volume of a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_lod_count` | Returns the number of LODs in a skeletal mesh asset. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_material` | Returns the material assigned to a named slot on a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_material_slots` | Returns the names of all material slots in a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_morph_target_names` | Returns the names of all morph targets on a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_physics_asset` | Returns the physics asset assigned to a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_section_count` | Returns the number of sections in a specific LOD of a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_skeleton` | Returns the skeleton asset associated with a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_socket_bone` | Returns the name of the bone that a socket is attached to. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_socket_names` | Returns the names of all sockets on a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_socket_transform` | Returns the local transform of a socket relative to its parent bone. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `get_vertex_count` | Returns the number of vertices in a specific LOD of a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `import_file` | Imports a mesh file from disk as a SkeletalMesh asset. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `remove_socket` | Removes a named socket from a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `rename_socket` | Renames a socket on a skeletal mesh. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `set_material` | Assigns a material to a named slot on a skeletal mesh asset. |
| Official | `editor_toolset.toolsets.skeletal_mesh.SkeletalMeshTools` | `set_socket_transform` | Sets the local transform of a socket relative to its parent bone. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `generate_convex_collisions` | Generates convex hull collision shapes for a static mesh. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `generate_lods` | Auto-generates LODs for a static mesh using triangle reduction. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `get_bounds` | Returns the local-space bounding box of a static mesh. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `get_lod_count` | Returns the number of LODs in a static mesh asset. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `get_lod_thresholds` | Returns the screen-size thresholds at which each LOD becomes active. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `get_material` | Returns the material assigned to a named slot on a static mesh. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `get_material_slots` | Returns the names of all material slots in a static mesh. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `get_triangle_count` | Returns the number of triangles in a specific LOD of a static mesh. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `get_vertex_count` | Returns the number of vertices in a specific LOD of a static mesh. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `import_file` | Imports a mesh file from disk as a StaticMesh asset. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `is_nanite_enabled` | Returns whether Nanite is enabled for a static mesh. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `remove_collisions` | Removes all collision shapes from a static mesh. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `remove_lods` | Removes all auto-generated LODs from a static mesh, keeping only LOD 0. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `set_lod_thresholds` | Sets the screen-size thresholds at which each LOD becomes active. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `set_material` | Assigns a material to a named slot on a static mesh asset. |
| Official | `editor_toolset.toolsets.static_mesh.StaticMeshTools` | `set_nanite_enabled` | Enables or disables Nanite for a static mesh. |
| Official | `editor_toolset.toolsets.string_table.StringTableTools` | `create` | Creates a new StringTable asset. |
| Official | `editor_toolset.toolsets.string_table.StringTableTools` | `get_entry` | Returns the source string for a specific key. |
| Official | `editor_toolset.toolsets.string_table.StringTableTools` | `get_namespace` | Returns the namespace of a StringTable asset. |
| Official | `editor_toolset.toolsets.string_table.StringTableTools` | `get_table_id` | Returns the table ID for a StringTable asset. |
| Official | `editor_toolset.toolsets.string_table.StringTableTools` | `import_file` | Imports a file from disk as a StringTable asset. |
| Official | `editor_toolset.toolsets.string_table.StringTableTools` | `list_keys` | Lists all keys in the string table. |
| Official | `editor_toolset.toolsets.string_table.StringTableTools` | `remove_entry` | Removes an entry from the string table. |
| Official | `editor_toolset.toolsets.string_table.StringTableTools` | `set_entry` | Adds or updates an entry in the string table. |
| Official | `editor_toolset.toolsets.texture.TextureTools` | `get_size` | Returns the dimensions of a Texture2D in pixels. |
| Official | `editor_toolset.toolsets.texture.TextureTools` | `import_file` | Imports an image file from disk as a Texture2D asset. |
| Official | `state_tree_toolset.toolsets.state_tree.StateTreeTools` | `get_children` | Returns child states of a state. |
| Official | `state_tree_toolset.toolsets.state_tree.StateTreeTools` | `get_editor_data` | Returns the editor data for a StateTree asset. |
| Official | `state_tree_toolset.toolsets.state_tree.StateTreeTools` | `get_enter_conditions` | Returns enter conditions on a state. |
| Official | `state_tree_toolset.toolsets.state_tree.StateTreeTools` | `get_evaluators` | Returns global evaluators. |
| Official | `state_tree_toolset.toolsets.state_tree.StateTreeTools` | `get_global_tasks` | Returns global tasks that run across all states. |
| Official | `state_tree_toolset.toolsets.state_tree.StateTreeTools` | `get_node_description` | Returns a human-readable description for a node. |
| Official | `state_tree_toolset.toolsets.state_tree.StateTreeTools` | `get_root_states` | Returns top-level states of a StateTree. |
| Official | `state_tree_toolset.toolsets.state_tree.StateTreeTools` | `get_tasks` | Returns tasks on a state. |
| Official | `state_tree_toolset.toolsets.state_tree.StateTreeTools` | `get_transitions` | Returns transitions on a state. |
