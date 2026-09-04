#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SceneComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/AutomationTest.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/InspectLiveBlueprintTool.h"
#include "Tools/TraceLiveBlueprintFlowTool.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    template <typename T>
    T* AddNode(UEdGraph* Graph, const TCHAR* Name)
    {
        T* Node = NewObject<T>(Graph, FName(Name), RF_Transient);
        Node->NodeGuid = FGuid::NewGuid();
        Graph->Nodes.Add(Node);
        return Node;
    }

    UEdGraphPin* Pin(UEdGraphNode* Node, EEdGraphPinDirection Direction, FName Category, const TCHAR* Name)
    {
        return Node->CreatePin(Direction, Category, FName(Name));
    }

    bool ContainsNode(const TSharedPtr<FJsonObject>& Result, UEdGraphNode* Node)
    {
        if (!Result) return false;
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Result->TryGetArrayField(TEXT("nodes"), Nodes)) return false;
        for (const auto& Value : *Nodes)
        {
            if (Value->AsObject()->GetStringField(TEXT("nodePath")) == Node->GetPathName()) return true;
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPLiveBlueprintTest,
    "UnrealMCP.Blueprint.LiveInspection.EmbeddedLevelAndTrace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealMCPLiveBlueprintTest::RunTest(const FString& Parameters)
{
    // No asset factory, compilation, registration, saving, world initialization or editor switch.
    // The transient object tree reproduces the map/level/embedded Blueprint ownership shape.
    const FString PackageName = TEXT("/Temp/UnrealMCPLiveInspection_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TStrongObjectPtr<UPackage> Package(CreatePackage(*PackageName));
    Package->SetFlags(RF_Transient);
    TStrongObjectPtr<UWorld> World(NewObject<UWorld>(Package.Get(), NAME_None, RF_Transient));
    World->WorldType = EWorldType::Editor;
    ULevel* Level = NewObject<ULevel>(World.Get(), TEXT("PersistentLevel"), RF_Transient);
    Level->OwningWorld = World.Get();
    World->PersistentLevel = Level;
    auto* Blueprint = NewObject<ULevelScriptBlueprint>(Level, TEXT("EmbeddedLevelBlueprint"), RF_Transient);
    Level->LevelScriptBlueprint = Blueprint;
    Blueprint->GeneratedClass = AActor::StaticClass();
    Blueprint->SkeletonGeneratedClass = AActor::StaticClass();
    Blueprint->FriendlyName = TEXT("KeepThisFriendlyName");
    UEdGraph* Graph = NewObject<UEdGraph>(Blueprint, TEXT("EventGraph"), RF_Transient);
    Graph->Schema = UEdGraphSchema_K2::StaticClass();
    Graph->GraphGuid = FGuid::NewGuid();
    Blueprint->UbergraphPages.Add(Graph);

    auto* Begin = AddNode<UK2Node_Event>(Graph, TEXT("Begin"));
    Begin->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
    UEdGraphPin* BeginOut = Pin(Begin, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("then"));
    auto* Delay = AddNode<UK2Node_CallFunction>(Graph, TEXT("Delay"));
    Delay->FunctionReference.SetExternalMember(TEXT("Delay"), UKismetSystemLibrary::StaticClass());
    BeginOut->MakeLinkTo(Pin(Delay, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("execute")));
    auto* Duration = Pin(Delay, EGPD_Input, UEdGraphSchema_K2::PC_Real, TEXT("Duration"));
    Duration->DefaultValue = TEXT("0.35");
    auto* Call = AddNode<UK2Node_CallFunction>(Graph, TEXT("RestoreCamera"));
    Call->FunctionReference.SetSelfMember(TEXT("RestoreSavedCamera"));
    Pin(Delay, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("then"))->MakeLinkTo(Pin(Call, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("execute")));
    auto* SetLocation = AddNode<UK2Node_CallFunction>(Graph, TEXT("SetLocation"));
    SetLocation->FunctionReference.SetExternalMember(TEXT("K2_SetWorldLocation"), USceneComponent::StaticClass());
    Pin(Call, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("then"))->MakeLinkTo(Pin(SetLocation, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("execute")));
    auto* TargetPin = Pin(SetLocation, EGPD_Input, UEdGraphSchema_K2::PC_Object, TEXT("self"));
    TargetPin->PinType.PinSubCategoryObject = USceneComponent::StaticClass();
    auto* Producer = AddNode<UEdGraphNode>(Graph, TEXT("CameraComponent"));
    auto* ComponentPin = Pin(Producer, EGPD_Output, UEdGraphSchema_K2::PC_Object, TEXT("Camera"));
    ComponentPin->PinType.PinSubCategoryObject = USceneComponent::StaticClass();
    ComponentPin->MakeLinkTo(TargetPin);
    auto* Unrelated = AddNode<UEdGraphNode>(Graph, TEXT("UnrelatedExec"));
    Pin(Producer, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("then"))->MakeLinkTo(Pin(Unrelated, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("execute")));
    auto* After = AddNode<UEdGraphNode>(Graph, TEXT("AfterSetWorldLocation"));
    Pin(SetLocation, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("then"))->MakeLinkTo(Pin(After, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("execute")));
    Pin(After, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("loop"))->MakeLinkTo(Delay->FindPin(TEXT("execute")));

    UEdGraph* FunctionGraph = NewObject<UEdGraph>(Blueprint, TEXT("RestoreSavedCamera"), RF_Transient);
    FunctionGraph->GraphGuid = FGuid::NewGuid();
    FunctionGraph->Schema = UEdGraphSchema_K2::StaticClass();
    Blueprint->FunctionGraphs.Add(FunctionGraph);
    auto* Entry = AddNode<UK2Node_FunctionEntry>(FunctionGraph, TEXT("Entry"));
    auto* Inside = AddNode<UEdGraphNode>(FunctionGraph, TEXT("InsideRestore"));
    Pin(Entry, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("then"))->MakeLinkTo(Pin(Inside, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("execute")));
    auto* Exit = AddNode<UK2Node_FunctionResult>(FunctionGraph, TEXT("Return"));
    Pin(Inside, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("then"))->MakeLinkTo(Pin(Exit, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("execute")));

    UEdGraph* MacroGraph = NewObject<UEdGraph>(Blueprint, TEXT("CameraMacro"), RF_Transient);
    MacroGraph->GraphGuid = FGuid::NewGuid();
    MacroGraph->Schema = UEdGraphSchema_K2::StaticClass();
    Blueprint->MacroGraphs.Add(MacroGraph);
    auto* MacroEntry = AddNode<UK2Node_Tunnel>(MacroGraph, TEXT("Inputs"));
    MacroEntry->bCanHaveOutputs = true;
    MacroEntry->bCanHaveInputs = false;
    auto* MacroExit = AddNode<UK2Node_Tunnel>(MacroGraph, TEXT("Outputs"));
    MacroExit->bCanHaveInputs = true;
    MacroExit->bCanHaveOutputs = false;
    auto* MacroInner = AddNode<UEdGraphNode>(MacroGraph, TEXT("InsideMacro"));
    Pin(MacroEntry, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("In"))->MakeLinkTo(Pin(MacroInner, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("execute")));
    Pin(MacroInner, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("then"))->MakeLinkTo(Pin(MacroExit, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("Out")));
    auto* Macro = AddNode<UK2Node_MacroInstance>(Graph, TEXT("MacroCall"));
    Macro->SetMacroGraph(MacroGraph);
    Pin(After, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("macro"))->MakeLinkTo(Pin(Macro, EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("In")));
    Pin(Macro, EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("Out"));

    const FString Before = UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
    const bool bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
    const EBlueprintStatus StatusBefore = Blueprint->Status;
    UnrealMCP::FMCPRequest Request;
    Request.Id = TEXT("live-inspection-regression");
    Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
    const FInspectLiveBlueprintTool Inspector;
    auto Inventory = Inspector.Execute(Request);
    if (!TestFalse(TEXT("Embedded Blueprint resolves without an asset index"), Inventory.Error.IsSet())) return false;
    TestEqual(TEXT("Graph count"), Inventory.Result->GetArrayField(TEXT("graphs")).Num(), 3);
    TestEqual(TEXT("Provenance"), Inventory.Result->GetStringField(TEXT("source")), FString(TEXT("live_editor")));
    TestFalse(TEXT("No index"), Inventory.Result->GetBoolField(TEXT("indexUsed")));

    Request.Params->SetStringField(TEXT("objectPath"), Graph->GetPathName());
    Request.Params->SetStringField(TEXT("mode"), TEXT("nodes"));
    Request.Params->SetNumberField(TEXT("limit"), 1);
    auto Page = Inspector.Execute(Request);
    if (!TestFalse(TEXT("Direct graph path resolves"), Page.Error.IsSet())) return false;
    TestTrue(TEXT("Page truncation visible"), Page.Result->GetBoolField(TEXT("hasMore")));
    TestFalse(TEXT("Partial page is not complete"), Page.Result->GetBoolField(TEXT("coverageComplete")));
    Request.Params->SetStringField(TEXT("nodeGuid"), Delay->NodeGuid.ToString());
    auto DelayResult = Inspector.Execute(Request);
    if (!TestFalse(TEXT("Exact GUID inspection"), DelayResult.Error.IsSet())) return false;
    const auto DelayJson = DelayResult.Result->GetArrayField(TEXT("nodes"))[0]->AsObject();
    bool bDefaultFound = false, bLinkFound = false;
    for (const auto& Value : DelayJson->GetArrayField(TEXT("pins")))
    {
        const auto P = Value->AsObject();
        if (P->GetStringField(TEXT("pinName")) == TEXT("Duration")) bDefaultFound = P->GetStringField(TEXT("literalDefaultValue")) == TEXT("0.35");
        if (P->GetStringField(TEXT("pinName")) == TEXT("then")) bLinkFound = P->GetArrayField(TEXT("links")).Num() == 1;
    }
    TestTrue(TEXT("Authored pin default preserved"), bDefaultFound);
    TestTrue(TEXT("Exact linked endpoint exposed"), bLinkFound);

    Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
    const FTraceLiveBlueprintFlowTool Tracer;
    auto Trace = Tracer.Execute(Request);
    if (!TestFalse(TEXT("Live BeginPlay trace succeeds"), Trace.Error.IsSet())) return false;
    TestTrue(TEXT("Follows Delay"), ContainsNode(Trace.Result, SetLocation));
    TestTrue(TEXT("Follows every downstream node after SetWorldLocation"), ContainsNode(Trace.Result, After));
    TestTrue(TEXT("Includes camera target producer"), ContainsNode(Trace.Result, Producer));
    TestFalse(TEXT("Does not follow execution from a mere data dependency"), ContainsNode(Trace.Result, Unrelated));
    TestTrue(TEXT("Expands local function body"), ContainsNode(Trace.Result, Inside));
    TestTrue(TEXT("Expands macro body"), ContainsNode(Trace.Result, MacroInner));
    bool bEntryBinding = false, bReturnBinding = false;
    for (const auto& Expansion : Trace.Result->GetArrayField(TEXT("expansions")))
    {
        for (const auto& Binding : Expansion->AsObject()->GetArrayField(TEXT("pinBindings")))
        {
            const FString Kind = Binding->AsObject()->GetStringField(TEXT("kind"));
            bEntryBinding |= Kind == TEXT("argument_or_entry");
            bReturnBinding |= Kind == TEXT("return_or_exit");
        }
    }
    TestTrue(TEXT("Call entry pin bindings exposed"), bEntryBinding);
    TestTrue(TEXT("Call return pin bindings exposed"), bReturnBinding);
    TestTrue(TEXT("Cycle deduplicated"), Trace.Result->GetIntegerField(TEXT("revisitedNodeCount")) > 0);
    TestTrue(TEXT("Unbounded fixture coverage complete"), Trace.Result->GetBoolField(TEXT("coverageComplete")));
    Request.Params->SetNumberField(TEXT("maxNodes"), 2);
    auto Limited = Tracer.Execute(Request);
    TestEqual(TEXT("Node budget respected"), Limited.Result->GetIntegerField(TEXT("nodeCount")), 2);
    TestTrue(TEXT("Frontier exposed"), Limited.Result->GetArrayField(TEXT("frontier")).Num() > 0);
    TestFalse(TEXT("Limited trace is not complete"), Limited.Result->GetBoolField(TEXT("coverageComplete")));
    Request.Params->RemoveField(TEXT("maxNodes"));
    Request.Params->SetNumberField(TEXT("maxCallDepth"), 0);
    auto NoCalls = Tracer.Execute(Request);
    TestFalse(TEXT("Call depth respected"), ContainsNode(NoCalls.Result, Inside));
    TestTrue(TEXT("Caller continuation survives call depth bound"), ContainsNode(NoCalls.Result, After));
    TestTrue(TEXT("Call bound marks truncation"), NoCalls.Result->GetBoolField(TEXT("truncated")));
    Request.Params->RemoveField(TEXT("maxCallDepth"));
    Request.Params->SetNumberField(TEXT("maxDepth"), 0);
    auto NoDepth = Tracer.Execute(Request);
    TestEqual(TEXT("Depth zero includes only start"), NoDepth.Result->GetIntegerField(TEXT("nodeCount")), 1);
    Request.Params->SetStringField(TEXT("mapPath"), TEXT("/Game/NotLoaded"));
    TestTrue(TEXT("Conflicting targets rejected"), Inspector.Execute(Request).Error.IsSet());
    Request.Params->RemoveField(TEXT("objectPath"));
    TestTrue(TEXT("Absent map rejected without loading"), Inspector.Execute(Request).Error.IsSet());
    Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
    Request.Params->SetStringField(TEXT("graphPath"), TEXT("/Temp/OtherBlueprint.EventGraph"));
    TestTrue(TEXT("Foreign graph rejected"), Inspector.Execute(Request).Error.IsSet());
    Request.Params->RemoveField(TEXT("graphPath"));
    Package->SetDirtyFlag(true);
    auto DirtyRead = Inspector.Execute(Request);
    TestFalse(TEXT("Dirty package remains inspectable"), DirtyRead.Error.IsSet());
    TestTrue(TEXT("Dirty state remains dirty"), Package->IsDirty());
    Package->SetDirtyFlag(bDirtyBefore);
    TestEqual(TEXT("Graph revision unchanged"), UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph), Before);
    TestEqual(TEXT("Dirty state preserved"), Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
    TestEqual(TEXT("Compile status preserved"), static_cast<int32>(Blueprint->Status), static_cast<int32>(StatusBefore));
    TestEqual(TEXT("Friendly name preserved"), Blueprint->FriendlyName, FString(TEXT("KeepThisFriendlyName")));
    return true;
}

#endif
