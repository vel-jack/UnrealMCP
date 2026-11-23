#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/EngineTypes.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchString.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "ObjectTools.h"
#include "Tools/Blueprint/Graph/AddBlueprintOverrideEventNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintVariableGetNodeTool.h"
#include "Tools/Blueprint/Graph/AddBlueprintVariableSetNodeTool.h"
#include "Tools/Blueprint/Graph/ApplyBlueprintGraphPatchTool.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Members/BlueprintMemberGraphTools.h"

namespace InheritedGraphTests
{
    namespace GraphEdit = UnrealMCP::BlueprintGraphEditToolUtils;
    bool Good(FAutomationTestBase& Test, const UnrealMCP::FMCPResponse& Response, const TCHAR* Context)
    {
        if (Response.Error.IsSet()) { Test.AddError(FString(Context) + TEXT(": ") + Response.Error->Message); return false; }
        return Test.TestTrue(FString(Context) + TEXT(" returns a result"), Response.Result.IsValid());
    }
    UnrealMCP::FMCPResponse Run(const FMCPToolBase& Tool, UBlueprint* BP, UEdGraph* Graph, TFunctionRef<void(FJsonObject&)> Fill)
    {
        UnrealMCP::FMCPRequest R; R.Id = TEXT("inherited-graph-test"); R.Params = MakeShared<FJsonObject>();
        R.Params->SetStringField(TEXT("objectPath"), BP->GetPathName());
        if (Graph) { R.Params->SetStringField(TEXT("graphName"), Graph->GetName()); R.Params->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower)); }
        Fill(*R.Params); return Tool.Execute(R);
    }
    TSharedRef<FJsonObject> Node(const TCHAR* Id, const TCHAR* Kind)
    { auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("id"), Id); J->SetStringField(TEXT("kind"), Kind); return J; }
    TSharedPtr<FJsonValue> Link(const FString& Source, const FString& SourcePin, const FString& Target, const FString& TargetPin)
    {
        auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("sourceNodeId"), Source); J->SetStringField(TEXT("sourcePinName"), SourcePin);
        J->SetStringField(TEXT("targetNodeId"), Target); J->SetStringField(TEXT("targetPinName"), TargetPin); return MakeShared<FJsonValueObject>(J);
    }
    UEdGraphNode* Find(UEdGraph* Graph, const FString& GuidText)
    {
        FGuid Guid; FGuid::Parse(GuidText, Guid);
        for (UEdGraphNode* N : Graph->Nodes) if (N && N->NodeGuid == Guid) return N;
        return nullptr;
    }
    UEdGraphNode* PatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Result, const TCHAR* Id)
    {
        for (const auto& V : Result->GetArrayField(TEXT("nodes")))
            if (V->AsObject()->GetStringField(TEXT("id")) == Id) return Find(Graph, V->AsObject()->GetStringField(TEXT("nodeGuid")));
        return nullptr;
    }
    bool Compile(FAutomationTestBase& Test, UBlueprint* BP)
    {
        TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
        const bool bOk = UnrealMCP::BlueprintEditToolUtils::CompileWithLocatedMessages(BP, Errors, Warnings);
        for (const auto& E : Errors) Test.AddError(E->AsObject()->GetStringField(TEXT("message")));
        return Test.TestTrue(TEXT("Blueprint compiles"), bOk);
    }
    struct FFixture
    {
        FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        TArray<FString> Paths;
        UBlueprint* Base = nullptr; UBlueprint* Mid = nullptr; UBlueprint* Child = nullptr;
        UEdGraph* Use = nullptr;
        UBlueprint* Create(const TCHAR* Prefix, UClass* Parent)
        {
            const FString AssetName = FString(Prefix) + TEXT("_") + Suffix;
            auto* BP = FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(*(TEXT("/Game/UnrealMCP_Automation/") + AssetName)),
                FName(*AssetName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
            FAssetRegistryModule::AssetCreated(BP); Paths.Add(BP->GetPathName()); return BP;
        }
        template <typename T> T* Place(UEdGraph* Graph)
        { auto* N = NewObject<T>(Graph); GraphEdit::PlaceNewNode(Graph, N, {}); return N; }
        UEdGraph* Function(UBlueprint* BP, const TCHAR* Name, const TCHAR* Input = nullptr, bool bReturnsString = false)
        {
            auto* Graph = FBlueprintEditorUtils::CreateNewGraph(BP, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            FBlueprintEditorUtils::AddFunctionGraph<UFunction>(BP, Graph, true, nullptr);
            TArray<UK2Node_FunctionEntry*> Entries; Graph->GetNodesOfClass(Entries);
            FEdGraphPinType String; String.PinCategory = UEdGraphSchema_K2::PC_String;
            if (Input) Entries[0]->CreateUserDefinedPin(Input, String, EGPD_Output);
            if (bReturnsString)
            {
                auto* Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entries[0]);
                Result->CreateUserDefinedPin(TEXT("ReturnValue"), String, EGPD_Input);
                GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Result->FindPin(TEXT("ReturnValue")), TEXT("base"));
                GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Entries[0]->FindPin(UEdGraphSchema_K2::PN_Then), Result->FindPin(UEdGraphSchema_K2::PN_Execute));
            }
            return Graph;
        }
        bool Initialize(FAutomationTestBase& Test)
        {
            Base = Create(TEXT("BP_InhBase"), AActor::StaticClass());
            FEdGraphPinType String; String.PinCategory = UEdGraphSchema_K2::PC_String;
            FBlueprintEditorUtils::AddMemberVariable(Base, TEXT("ParentVar"), String, TEXT("base"));
            USCS_Node* Component = Base->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("ParentWidget"));
            Base->SimpleConstructionScript->AddNode(Component);
            Function(Base, TEXT("SetParentVar"), TEXT("NewValue")); Function(Base, TEXT("SetRenamedVar"), TEXT("EntityValue"));
            Function(Base, TEXT("Describe"), nullptr, true);
            auto* Event = NewObject<UK2Node_CustomEvent>(Base->UbergraphPages[0]); Event->CustomFunctionName = TEXT("MulticastPing");
            GraphEdit::PlaceNewNode(Base->UbergraphPages[0], Event, {});
            FEdGraphPinType Int; Int.PinCategory = UEdGraphSchema_K2::PC_Int; Event->CreateUserDefinedPin(TEXT("Index"), Int, EGPD_Output);
            if (!Compile(Test, Base)) return false;
            Mid = Create(TEXT("BP_InhMid"), Base->GeneratedClass); FBlueprintEditorUtils::AddMemberVariable(Mid, TEXT("MidVar"), Int);
            if (!Compile(Test, Mid)) return false;
            Child = Create(TEXT("BP_InhGrandChild"), Mid->GeneratedClass); FBlueprintEditorUtils::AddMemberVariable(Child, TEXT("ChildVar"), String);
            Use = Function(Child, TEXT("Use"));
            return Compile(Test, Child);
        }
        ~FFixture()
        {
            for (int32 I = Paths.Num() - 1; I >= 0; --I) if (UObject* O = FindObject<UObject>(nullptr, *Paths[I]))
            { UPackage* P = O->GetPackage(); FAssetRegistryModule::AssetDeleted(O); ObjectTools::DeleteObjectsUnchecked({O}); P->SetDirtyFlag(false); }
        }
    };
}
using namespace InheritedGraphTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPInheritedVariableTest, "UnrealMCP.Blueprint.Authoring.InheritedVariables.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPInheritedVariableTest::RunTest(const FString& Parameters)
{
    FFixture F; if (!F.Initialize(*this)) return false;
    const FAddBlueprintVariableGetNodeTool Get;
    for (const TCHAR* Variable : {TEXT("ParentVar"), TEXT("MidVar"), TEXT("ParentWidget"), TEXT("Tags")})
    {
        const auto R = Run(Get, F.Child, F.Use, [&](FJsonObject& P) { P.SetStringField(TEXT("variableName"), Variable); });
        if (!Good(*this, R, TEXT("Inherited standalone getter"))) return false;
        TestTrue(TEXT("Getter reports self context"), R.Result->GetBoolField(TEXT("selfContext")));
        auto* N = Cast<UK2Node_VariableGet>(Find(F.Use, R.Result->GetStringField(TEXT("nodeGuid"))));
        if (!TestNotNull(TEXT("Getter exists"), N)) return false;
        TestTrue(TEXT("Reference is self context"), N->VariableReference.IsSelfContext());
        const auto* Target = N->FindPin(UEdGraphSchema_K2::PN_Self);
        TestTrue(TEXT("Target hidden or absent"), !Target || Target->bHidden);
    }
    const auto Ancestor = Run(Get, F.Child, F.Use, [&](FJsonObject& P)
    { P.SetStringField(TEXT("variableName"), TEXT("ParentVar")); P.SetStringField(TEXT("ownerClassPath"), F.Base->GeneratedClass->GetPathName()); });
    if (!Good(*this, Ancestor, TEXT("Explicit ancestor getter"))) return false;
    TestTrue(TEXT("Explicit ancestor remains self context"), Ancestor.Result->GetBoolField(TEXT("selfContext")));
    TestEqual(TEXT("Declaring class reported"), Ancestor.Result->GetStringField(TEXT("declaredIn")), F.Base->GeneratedClass->GetPathName());
    const auto Set = Run(FAddBlueprintVariableSetNodeTool(), F.Child, F.Use, [](FJsonObject& P) { P.SetStringField(TEXT("variableName"), TEXT("ParentVar")); });
    if (!Good(*this, Set, TEXT("Inherited standalone setter"))) return false;
    auto* Setter = Cast<UK2Node_VariableSet>(Find(F.Use, Set.Result->GetStringField(TEXT("nodeGuid"))));
    if (!TestNotNull(TEXT("Setter exists"), Setter)) return false;
    TestNotNull(TEXT("Setter Output_Get exists"), Setter->FindPin(TEXT("Output_Get")));
    auto Read = Node(TEXT("read"), TEXT("variableGet")); Read->SetStringField(TEXT("variableName"), TEXT("ParentVar"));
    auto Write = Node(TEXT("write"), TEXT("variableSet")); Write->SetStringField(TEXT("variableName"), TEXT("ParentVar"));
    const auto Patch = Run(FApplyBlueprintGraphPatchTool(), F.Child, F.Use, [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("patchId"), TEXT("inherited-var-patch")); P.SetBoolField(TEXT("compileAfterEdit"), true);
        P.SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(Read), MakeShared<FJsonValueObject>(Write)});
        P.SetArrayField(TEXT("connections"), {Link(TEXT("read"), TEXT("ParentVar"), TEXT("write"), TEXT("ParentVar"))});
    });
    if (!Good(*this, Patch, TEXT("Inherited variable patch"))) return false;
    TestTrue(TEXT("Inherited patch compiles"), Patch.Result->GetBoolField(TEXT("compileSucceeded")));
    TestTrue(TEXT("Patch getter self context"), CastChecked<UK2Node_Variable>(PatchNode(F.Use, Patch.Result, TEXT("read")))->VariableReference.IsSelfContext());
    TestNotNull(TEXT("Patch setter Output_Get exists"), PatchNode(F.Use, Patch.Result, TEXT("write"))->FindPin(TEXT("Output_Get")));
    return Compile(*this, F.Child) && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPMemberRetargetSwitchTest, "UnrealMCP.Blueprint.Authoring.RetargetMemberSwitch.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPMemberRetargetSwitchTest::RunTest(const FString& Parameters)
{
    FFixture F; if (!F.Initialize(*this)) return false;
    UEdGraph* Graph = F.Child->UbergraphPages[0];
    auto* Getter = NewObject<UK2Node_VariableGet>(Graph); Getter->VariableReference.SetSelfMember(TEXT("ChildVar")); GraphEdit::PlaceNewNode(Graph, Getter, {});
    auto* Call = NewObject<UK2Node_CallFunction>(Graph); Call->SetFromFunction(F.Base->GeneratedClass->FindFunctionByName(TEXT("SetParentVar"))); GraphEdit::PlaceNewNode(Graph, Call, {});
    const auto* Schema = GetDefault<UEdGraphSchema_K2>();
    TestTrue(TEXT("Fixture data link connected"), Schema->TryCreateConnection(Getter->FindPin(TEXT("ChildVar")), Call->FindPin(TEXT("NewValue"))));
    Getter->NodePosX = 123; Getter->NodePosY = 456; Getter->NodeComment = TEXT("keep this comment");
    const FGuid GuidBefore = Getter->NodeGuid;
    auto Retarget = [&](FJsonObject& P, bool bDry)
    {
        P.SetStringField(TEXT("nodeGuid"), GraphEdit::GetNodeGuid(Getter)); P.SetStringField(TEXT("newMemberName"), TEXT("ParentVar"));
        P.SetBoolField(TEXT("dryRun"), bDry); P.SetBoolField(TEXT("confirm"), !bDry);
    };
    F.Child->GetPackage()->SetDirtyFlag(false);
    const FString Revision = GraphEdit::ComputeGraphRevision(Graph); const auto Status = F.Child->Status;
    const auto Preview = Run(FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit::RetargetMember), F.Child, Graph, [&](FJsonObject& P) { Retarget(P, true); });
    if (!Good(*this, Preview, TEXT("Linked retarget preview"))) return false;
    TestEqual(TEXT("Retarget dry run preserves revision"), GraphEdit::ComputeGraphRevision(Graph), Revision);
    TestFalse(TEXT("Retarget dry run preserves clean package"), F.Child->GetPackage()->IsDirty());
    TestTrue(TEXT("Retarget dry run preserves status"), F.Child->Status == Status);
    TestTrue(TEXT("Retarget dry run preserves data link"), Getter->FindPin(TEXT("ChildVar"))->LinkedTo.Contains(Call->FindPin(TEXT("NewValue"))));
    // The old owner no longer declares the member, as after reparenting; retain its original pins and links.
    Getter->VariableReference.SetExternalMember(TEXT("ChildVar"), F.Base->GeneratedClass);
    TestNull(TEXT("Old getter member is unresolved"), Getter->VariableReference.ResolveMember<FProperty>(F.Child->SkeletonGeneratedClass));
    const auto Changed = Run(FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit::RetargetMember), F.Child, Graph, [&](FJsonObject& P) { Retarget(P, false); });
    if (!Good(*this, Changed, TEXT("Broken getter retarget"))) return false;
    TestEqual(TEXT("GUID retained"), Getter->NodeGuid, GuidBefore); TestEqual(TEXT("Position retained"), Getter->NodePosX, 123);
    TestEqual(TEXT("Comment retained"), Getter->NodeComment, FString(TEXT("keep this comment")));
    TestTrue(TEXT("Renamed getter link retained"), Getter->FindPin(TEXT("ParentVar"))->LinkedTo.Contains(Call->FindPin(TEXT("NewValue"))));
    const FGuid CallGuid = Call->NodeGuid;
    const auto RetargetCall = Run(FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit::RetargetMember), F.Child, Graph, [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("nodeGuid"), GraphEdit::GetNodeGuid(Call)); P.SetStringField(TEXT("newMemberName"), TEXT("SetRenamedVar")); P.SetBoolField(TEXT("confirm"), true);
        auto Map = MakeShared<FJsonObject>(); Map->SetStringField(TEXT("NewValue"), TEXT("EntityValue")); P.SetObjectField(TEXT("pinMap"), Map);
    });
    if (!Good(*this, RetargetCall, TEXT("Function member retarget"))) return false;
    TestEqual(TEXT("Function call GUID retained"), Call->NodeGuid, CallGuid);
    TestTrue(TEXT("Mapped function argument retains link"), Call->FindPin(TEXT("EntityValue"))->LinkedTo.Contains(Getter->FindPin(TEXT("ParentVar"))));

    auto* Switch = NewObject<UK2Node_SwitchString>(Graph); Switch->PinNames = {TEXT("A"), TEXT("B")}; Switch->bHasDefaultPin = true;
    GraphEdit::PlaceNewNode(Graph, Switch, {}); TestTrue(TEXT("Fixture exec link connected"), Schema->TryCreateConnection(Switch->FindPin(TEXT("A")), Call->GetExecPin()));
    auto Cases = [&](FJsonObject& P, bool bDry)
    {
        P.SetStringField(TEXT("nodeGuid"), GraphEdit::GetNodeGuid(Switch)); P.SetArrayField(TEXT("cases"), {MakeShared<FJsonValueString>(TEXT("C")), MakeShared<FJsonValueString>(TEXT("B"))});
        auto Renames = MakeShared<FJsonObject>(); Renames->SetStringField(TEXT("A"), TEXT("C")); P.SetObjectField(TEXT("renames"), Renames);
        P.SetBoolField(TEXT("dryRun"), bDry); P.SetBoolField(TEXT("confirm"), !bDry);
    };
    F.Child->GetPackage()->SetDirtyFlag(false); const FString SwitchRevision = GraphEdit::ComputeGraphRevision(Graph); const auto SwitchStatus = F.Child->Status;
    const auto SwitchPreview = Run(FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit::SwitchCases), F.Child, Graph, [&](FJsonObject& P) { Cases(P, true); });
    if (!Good(*this, SwitchPreview, TEXT("Linked switch preview"))) return false;
    TestEqual(TEXT("Switch preview preserves revision"), GraphEdit::ComputeGraphRevision(Graph), SwitchRevision);
    TestFalse(TEXT("Switch preview preserves package dirty state"), F.Child->GetPackage()->IsDirty());
    TestTrue(TEXT("Switch preview preserves Blueprint status"), F.Child->Status == SwitchStatus);
    const auto Switched = Run(FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit::SwitchCases), F.Child, Graph, [&](FJsonObject& P) { Cases(P, false); });
    if (!Good(*this, Switched, TEXT("Switch case rename"))) return false;
    TestTrue(TEXT("Renamed case keeps exec link"), Switch->FindPin(TEXT("C"))->LinkedTo.Contains(Call->GetExecPin()));
    TestNotNull(TEXT("Default retained"), Switch->FindPin(TEXT("Default")));
    const FString BeforeCollision = GraphEdit::ComputeGraphRevision(Graph);
    const auto Collision = Run(FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit::SwitchCases), F.Child, Graph, [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("nodeGuid"), GraphEdit::GetNodeGuid(Switch)); P.SetBoolField(TEXT("confirm"), true);
        P.SetArrayField(TEXT("cases"), {MakeShared<FJsonValueString>(TEXT("B"))});
        auto Map = MakeShared<FJsonObject>(); Map->SetStringField(TEXT("C"), TEXT("B")); P.SetObjectField(TEXT("renames"), Map);
    });
    TestTrue(TEXT("Ambiguous case merge rejected"), Collision.Error.IsSet());
    TestEqual(TEXT("Rejected case merge preserves graph"), GraphEdit::ComputeGraphRevision(Graph), BeforeCollision);
    const auto Removed = Run(FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit::SwitchCases), F.Child, Graph, [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("nodeGuid"), GraphEdit::GetNodeGuid(Switch)); P.SetBoolField(TEXT("confirm"), true);
        P.SetArrayField(TEXT("cases"), {MakeShared<FJsonValueString>(TEXT("B")), MakeShared<FJsonValueString>(TEXT("D"))});
    });
    if (!Good(*this, Removed, TEXT("Remove linked case and append new case"))) return false;
    TestEqual(TEXT("Removed case reports one dropped link"), Removed.Result->GetArrayField(TEXT("droppedLinks")).Num(), 1);
    TestTrue(TEXT("Dropped endpoint identifies its node"), Removed.Result->GetArrayField(TEXT("droppedLinks"))[0]->AsObject()->GetStringField(TEXT("otherNodeGuid")) == GraphEdit::GetNodeGuid(Call));
    TestNotNull(TEXT("New case appended"), Switch->FindPin(TEXT("D")));
    TestNotNull(TEXT("Removal retains Default"), Switch->FindPin(TEXT("Default")));
    auto* DefaultCall = NewObject<UK2Node_CallFunction>(Graph);
    DefaultCall->SetFromFunction(F.Base->GeneratedClass->FindFunctionByName(TEXT("SetParentVar"))); GraphEdit::PlaceNewNode(Graph, DefaultCall, {});
    DefaultCall->FindPin(TEXT("NewValue"))->DefaultValue = TEXT("old function default");
    DefaultCall->FindPin(TEXT("NewValue"))->AutogeneratedDefaultValue = TEXT("old function default");
    const auto DefaultRetarget = Run(FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit::RetargetMember), F.Child, Graph, [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("nodeGuid"), GraphEdit::GetNodeGuid(DefaultCall)); P.SetStringField(TEXT("newMemberName"), TEXT("SetRenamedVar")); P.SetBoolField(TEXT("confirm"), true);
        auto Map = MakeShared<FJsonObject>(); Map->SetStringField(TEXT("NewValue"), TEXT("EntityValue")); P.SetObjectField(TEXT("pinMap"), Map);
    });
    if (!Good(*this, DefaultRetarget, TEXT("Retarget inherited default argument"))) return false;
    TestEqual(TEXT("Old argument default preserved"), DefaultCall->FindPin(TEXT("EntityValue"))->DefaultValue, FString(TEXT("old function default")));

    return Compile(*this, F.Child) && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPParentAndAdvancedPatchTest, "UnrealMCP.Blueprint.Authoring.ParentAndAdvancedPatch.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPParentAndAdvancedPatchTest::RunTest(const FString& Parameters)
{
    FFixture F; if (!F.Initialize(*this)) return false;
    for (UBlueprint* BP : {F.Mid, F.Child})
    {
        const auto Override = Run(FOverrideBlueprintFunctionTool(), BP, nullptr, [](FJsonObject& P) { P.SetStringField(TEXT("functionName"), TEXT("Describe")); P.SetBoolField(TEXT("addParentCall"), true); });
        if (!Good(*this, Override, TEXT("Function override with parent"))) return false;
        UEdGraph* Graph = nullptr;
        for (UEdGraph* G : BP->FunctionGraphs) if (G && G->GetFName() == TEXT("Describe")) Graph = G;
        if (!TestNotNull(TEXT("Describe override graph created"), Graph)) return false;
        TArray<UK2Node_CallParentFunction*> Parents; Graph->GetNodesOfClass(Parents);
        if (!TestEqual(TEXT("One parent implementation call"), Parents.Num(), 1)) return false;
        TestNotNull(TEXT("Parent return signature preserved"), Parents[0]->FindPin(TEXT("ReturnValue")));
        TestTrue(TEXT("Parent exec wired"), !Parents[0]->GetExecPin()->LinkedTo.IsEmpty());
    }
    UEdGraph* Graph = F.Child->UbergraphPages[0];
    for (const TCHAR* EventName : {TEXT("MulticastPing"), TEXT("ReceiveBeginPlay")})
    {
        // Actor templates can supply a disabled BeginPlay placeholder, which is not an override made by this test.
        TArray<UK2Node_Event*> ExistingEvents; Graph->GetNodesOfClass(ExistingEvents);
        for (auto* Event : ExistingEvents)
            if (Event->EventReference.GetMemberName() == EventName && Event->GetDesiredEnabledState() == ENodeEnabledState::Disabled)
                FBlueprintEditorUtils::RemoveNode(F.Child, Event, true);
        F.Child->GetOutermost()->SetDirtyFlag(false);
        const FString BeforeEventRevision = GraphEdit::ComputeGraphRevision(Graph);
        const EBlueprintStatus BeforeEventStatus = F.Child->Status;
        const auto EventPreview = Run(FAddBlueprintOverrideEventNodeTool(), F.Child, Graph, [&](FJsonObject& P)
        { P.SetStringField(TEXT("functionName"), EventName); P.SetBoolField(TEXT("addParentCall"), true); P.SetBoolField(TEXT("dryRun"), true); });
        if (!Good(*this, EventPreview, TEXT("Event override dry run"))) return false;
        TestEqual(TEXT("Event dry run preserves graph"), GraphEdit::ComputeGraphRevision(Graph), BeforeEventRevision);
        TestFalse(TEXT("Event dry run preserves clean package"), F.Child->GetOutermost()->IsDirty());
        TestEqual(TEXT("Event dry run preserves status"), F.Child->Status, BeforeEventStatus);
        const auto Override = Run(FAddBlueprintOverrideEventNodeTool(), F.Child, Graph, [&](FJsonObject& P)
        { P.SetStringField(TEXT("functionName"), EventName); P.SetBoolField(TEXT("addParentCall"), true); });
        if (!Good(*this, Override, TEXT("Event override with parent"))) return false;
        TestTrue(TEXT("Parent event call added"), Override.Result->GetBoolField(TEXT("parentCallAdded")));
        auto* Parent = Cast<UK2Node_CallParentFunction>(Find(Graph, Override.Result->GetStringField(TEXT("parentCallNodeGuid"))));
        if (!TestNotNull(TEXT("Parent event node exists"), Parent)) return false;
        TestTrue(TEXT("Parent event execution wired"), !Parent->GetExecPin()->LinkedTo.IsEmpty());
        if (FString(EventName) == TEXT("MulticastPing")) TestTrue(TEXT("Event argument passed through"), !Parent->FindPin(TEXT("Index"))->LinkedTo.IsEmpty());
    }
    auto* Start = NewObject<UK2Node_CustomEvent>(Graph); Start->CustomFunctionName = TEXT("AdvancedStart"); GraphEdit::PlaceNewNode(Graph, Start, {});
    const FString MacroPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:IsValid");
    UEdGraph* MacroGraph = LoadObject<UEdGraph>(nullptr, *MacroPath);
    if (!TestNotNull(TEXT("Standard IsValid macro loaded"), MacroGraph)) return false;
    auto* Probe = NewObject<UK2Node_MacroInstance>(Graph); Probe->SetMacroGraph(MacroGraph); Probe->AllocateDefaultPins();
    FString MacroExec, MacroObject, MacroThen;
    for (const auto* P : Probe->Pins)
    {
        if (P->Direction == EGPD_Input && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) MacroExec = P->PinName.ToString();
        if (P->Direction == EGPD_Input && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Object) MacroObject = P->PinName.ToString();
        if (P->Direction == EGPD_Output && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && MacroThen.IsEmpty()) MacroThen = P->PinName.ToString();
    }
    auto Self = Node(TEXT("self"), TEXT("self")); auto Cast = Node(TEXT("cast"), TEXT("cast"));
    Cast->SetStringField(TEXT("targetClassPath"), TEXT("/Script/Engine.Actor")); Cast->SetBoolField(TEXT("pure"), true);
    auto Macro = Node(TEXT("valid"), TEXT("macro")); Macro->SetStringField(TEXT("macroPath"), MacroPath);
    auto Switch = Node(TEXT("switch"), TEXT("switchString")); Switch->SetArrayField(TEXT("cases"), {MakeShared<FJsonValueString>(TEXT("A")), MakeShared<FJsonValueString>(TEXT("B"))});
    auto Enum = Node(TEXT("enum"), TEXT("switchEnum")); Enum->SetStringField(TEXT("enumPath"), StaticEnum<ECollisionChannel>()->GetPathName());
    auto Existing = Node(TEXT("start"), TEXT("existingNode")); Existing->SetStringField(TEXT("nodeGuid"), GraphEdit::GetNodeGuid(Start));
    auto Parent = Node(TEXT("parent"), TEXT("parentCall")); Parent->SetStringField(TEXT("functionName"), TEXT("MulticastPing"));
    const auto FillPatch = [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("patchId"), TEXT("advanced-inherited-patch")); P.SetBoolField(TEXT("compileAfterEdit"), true);
        P.SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(Self), MakeShared<FJsonValueObject>(Cast), MakeShared<FJsonValueObject>(Macro),
            MakeShared<FJsonValueObject>(Switch), MakeShared<FJsonValueObject>(Enum), MakeShared<FJsonValueObject>(Existing), MakeShared<FJsonValueObject>(Parent)});
        P.SetArrayField(TEXT("connections"), {Link(TEXT("self"), TEXT("self"), TEXT("cast"), TEXT("Object")), Link(TEXT("cast"), TEXT("AsActor"), TEXT("valid"), MacroObject),
            Link(TEXT("start"), TEXT("then"), TEXT("valid"), MacroExec), Link(TEXT("valid"), MacroThen, TEXT("switch"), TEXT("execute")),
            Link(TEXT("switch"), TEXT("A"), TEXT("parent"), TEXT("execute"))});
    };
    F.Child->GetOutermost()->SetDirtyFlag(false);
    const FString BeforePatchRevision = GraphEdit::ComputeGraphRevision(Graph);
    const EBlueprintStatus BeforePatchStatus = F.Child->Status;
    const auto Preview = Run(FApplyBlueprintGraphPatchTool(), F.Child, Graph, [&](FJsonObject& P)
    { FillPatch(P); P.SetBoolField(TEXT("dryRun"), true); });
    if (!Good(*this, Preview, TEXT("Advanced connected patch dry run"))) return false;
    TestEqual(TEXT("Patch dry run preserves graph"), GraphEdit::ComputeGraphRevision(Graph), BeforePatchRevision);
    TestFalse(TEXT("Patch dry run preserves clean package"), F.Child->GetOutermost()->IsDirty());
    TestEqual(TEXT("Patch dry run preserves status"), F.Child->Status, BeforePatchStatus);
    TestTrue(TEXT("Patch dry run leaves existing event unconnected"), Start->FindPin(TEXT("then"))->LinkedTo.IsEmpty());
    const auto Patch = Run(FApplyBlueprintGraphPatchTool(), F.Child, Graph, FillPatch);
    if (!Good(*this, Patch, TEXT("Atomic advanced patch"))) return false;
    TestTrue(TEXT("Atomic patch compiles"), Patch.Result->GetBoolField(TEXT("compileSucceeded")));
    TestTrue(TEXT("Enum switch created"), PatchNode(Graph, Patch.Result, TEXT("enum"))->IsA<UK2Node_SwitchEnum>());
    TestNotNull(TEXT("Enum switch Default pin exposed"), PatchNode(Graph, Patch.Result, TEXT("enum"))->FindPin(TEXT("Default")));
    TestTrue(TEXT("Parent-call patch node created"), PatchNode(Graph, Patch.Result, TEXT("parent"))->IsA<UK2Node_CallParentFunction>());
    TestTrue(TEXT("Macro object connected"), !PatchNode(Graph, Patch.Result, TEXT("valid"))->FindPin(*MacroObject)->LinkedTo.IsEmpty());
    return Compile(*this, F.Child) && !HasAnyErrors();
}
#endif
