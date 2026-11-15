#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "ObjectTools.h"
#include "Tools/AddBlueprintVariableGetNodeTool.h"
#include "Tools/AddBlueprintVariableSetNodeTool.h"
#include "Tools/ApplyBlueprintGraphPatchTool.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"

namespace LocalVariableNodeTests
{
    namespace Edit = UnrealMCP::BlueprintGraphEditToolUtils;
    UnrealMCP::FMCPResponse Run(const FMCPToolBase& Tool, UBlueprint* BP, UEdGraph* Graph, TFunctionRef<void(FJsonObject&)> Fill)
    {
        UnrealMCP::FMCPRequest R; R.Id = TEXT("local-node-test"); R.Params = MakeShared<FJsonObject>();
        R.Params->SetStringField(TEXT("objectPath"), BP->GetPathName());
        R.Params->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Fill(*R.Params); return Tool.Execute(R);
    }
    bool Good(FAutomationTestBase& T, const UnrealMCP::FMCPResponse& R, const TCHAR* Label)
    {
        if (R.Error.IsSet())
        {
            T.AddError(FString(Label) + TEXT(": ") + R.Error->Message);
            const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
            if (R.Error->Data && R.Error->Data->TryGetArrayField(TEXT("errors"), Errors))
                for (const auto& Error : *Errors) T.AddError(Error->AsObject()->GetStringField(TEXT("message")));
            return false;
        }
        return T.TestTrue(Label, R.Result.IsValid());
    }
    UEdGraphNode* Find(UEdGraph* G, const FString& Text)
    { FGuid Id; FGuid::Parse(Text, Id); for (UEdGraphNode* N : G->Nodes) if (N && N->NodeGuid == Id) return N; return nullptr; }
    TSharedRef<FJsonObject> Node(const TCHAR* Id, const TCHAR* Kind, const TCHAR* Variable = nullptr)
    {
        auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("id"), Id); J->SetStringField(TEXT("kind"), Kind);
        if (Variable) J->SetStringField(TEXT("variableName"), Variable); return J;
    }
    TSharedPtr<FJsonValue> Link(const TCHAR* Source, const TCHAR* Out, const TCHAR* Target, const TCHAR* In)
    {
        auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("sourceNodeId"), Source); J->SetStringField(TEXT("sourcePinName"), Out);
        J->SetStringField(TEXT("targetNodeId"), Target); J->SetStringField(TEXT("targetPinName"), In); return MakeShared<FJsonValueObject>(J);
    }
    bool Compile(FAutomationTestBase& T, UBlueprint* BP)
    {
        TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
        const bool Ok = UnrealMCP::BlueprintEditToolUtils::CompileWithLocatedMessages(BP, Errors, Warnings);
        for (const auto& E : Errors) T.AddError(E->AsObject()->GetStringField(TEXT("message")));
        return T.TestTrue(TEXT("Fixture compiles"), Ok);
    }
    struct FFixture
    {
        UBlueprint* BP; UEdGraph* First; UEdGraph* Second; UEdGraph* Predict;
        FEdGraphPinType Int, String, Actors;
        FFixture()
        {
            const FString Name = TEXT("BP_LocalNodes_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*(TEXT("/Game/UnrealMCP_Automation/") + Name)),
                *Name, BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
            FAssetRegistryModule::AssetCreated(BP);
            Int.PinCategory = UEdGraphSchema_K2::PC_Int; String.PinCategory = UEdGraphSchema_K2::PC_String;
            Actors.PinCategory = UEdGraphSchema_K2::PC_Object; Actors.PinSubCategoryObject = AActor::StaticClass(); Actors.ContainerType = EPinContainerType::Array;
            FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Shared"), Int);
            FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("ProjectilesArray"), Actors);
            First = Function(TEXT("First")); Second = Function(TEXT("Second")); Predict = Function(TEXT("PredictProxy"));
            Entry(Predict)->CreateUserDefinedPin(TEXT("ActorsToIgnore"), Actors, EGPD_Output);
            FBlueprintEditorUtils::AddLocalVariable(BP, First, TEXT("Shared"), Int);
            FBlueprintEditorUtils::AddLocalVariable(BP, Second, TEXT("Shared"), String);
            FBlueprintEditorUtils::AddLocalVariable(BP, First, TEXT("PredictionActorsToIgnore"), Actors);
        }
        UEdGraph* Function(const TCHAR* Name)
        {
            auto* G = FBlueprintEditorUtils::CreateNewGraph(BP, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            FBlueprintEditorUtils::AddFunctionGraph<UFunction>(BP, G, true, nullptr); return G;
        }
        static UK2Node_FunctionEntry* Entry(UEdGraph* G)
        { TArray<UK2Node_FunctionEntry*> Entries; G->GetNodesOfClass(Entries); return Entries[0]; }
        ~FFixture()
        { UPackage* P = BP->GetPackage(); FAssetRegistryModule::AssetDeleted(BP); ObjectTools::DeleteObjectsUnchecked({BP}); P->SetDirtyFlag(false); }
    };
}
using namespace LocalVariableNodeTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPLocalVariableStandaloneTest, "UnrealMCP.Blueprint.Authoring.LocalVariableNodes.Standalone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPLocalVariableStandaloneTest::RunTest(const FString& Parameters)
{
    FFixture F; if (!Compile(*this, F.BP)) return false;
    // Simulate a declaration just added by another tool, before any skeleton regeneration.
    FBPVariableDescription Fresh; Fresh.VarName = TEXT("FreshActors"); Fresh.VarGuid = FGuid::NewGuid(); Fresh.VarType = F.Actors;
    Fresh.PropertyFlags |= CPF_BlueprintVisible;
    Fresh.FriendlyName = TEXT("Fresh Actors"); Fresh.Category = UEdGraphSchema_K2::VR_DefaultCategory;
    FFixture::Entry(F.First)->LocalVariables.Add(Fresh);
    for (bool Set : {false, true})
    {
        const auto Preview = Run(Set ? static_cast<const FMCPToolBase&>(FAddBlueprintVariableSetNodeTool()) : static_cast<const FMCPToolBase&>(FAddBlueprintVariableGetNodeTool()), F.BP, F.First,
            [](FJsonObject& P) { P.SetStringField(TEXT("variableName"), TEXT("FreshActors")); P.SetBoolField(TEXT("dryRun"), true); });
        if (!Good(*this, Preview, TEXT("Fresh uncompiled local preview"))) return false;
        bool Found = false;
        for (const auto& V : Preview.Result->GetArrayField(TEXT("pins")))
        {
            const auto J = V->AsObject(); if (J->GetStringField(TEXT("pinName")) != TEXT("FreshActors")) continue;
            Found = true; TestEqual(TEXT("Fresh preview object category"), J->GetStringField(TEXT("category")), FString(TEXT("object")));
            TestEqual(TEXT("Fresh preview array container"), J->GetStringField(TEXT("containerType")), FString(TEXT("Array")));
            TestEqual(TEXT("Fresh preview Actor class"), J->GetStringField(TEXT("subCategoryObjectPath")), AActor::StaticClass()->GetPathName());
        }
        TestTrue(TEXT("Fresh uncompiled local exposes value pin"), Found);
    }
    const auto FreshGet = Run(FAddBlueprintVariableGetNodeTool(), F.BP, F.First, [](FJsonObject& P) { P.SetStringField(TEXT("variableName"), TEXT("FreshActors")); });
    const auto FreshSet = Run(FAddBlueprintVariableSetNodeTool(), F.BP, F.First, [](FJsonObject& P) { P.SetStringField(TEXT("variableName"), TEXT("FreshActors")); });
    if (!Good(*this, FreshGet, TEXT("Fresh local live getter")) || !Good(*this, FreshSet, TEXT("Fresh local live setter"))) return false;
    auto* FreshGetter = Find(F.First, FreshGet.Result->GetStringField(TEXT("nodeGuid")));
    auto* FreshSetter = Find(F.First, FreshSet.Result->GetStringField(TEXT("nodeGuid")));
    if (!TestNotNull(TEXT("Fresh live getter value pin"), FreshGetter->FindPin(TEXT("FreshActors")))
        || !TestNotNull(TEXT("Fresh live setter value pin"), FreshSetter->FindPin(TEXT("FreshActors")))) return false;
    TestTrue(TEXT("Fresh live local Actor array type preserved"), FreshGetter->FindPin(TEXT("FreshActors"))->PinType == F.Actors);
    TestTrue(TEXT("Fresh uncompiled local nodes connect"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(FreshGetter->FindPin(TEXT("FreshActors")), FreshSetter->FindPin(TEXT("FreshActors"))));
    TArray<FGuid> Guids;
    for (UEdGraph* G : {F.First, F.Second})
    {
        F.BP->GetOutermost()->SetDirtyFlag(false); const auto Status = F.BP->Status; const auto Revision = Edit::ComputeGraphRevision(G);
        const auto Preview = Run(FAddBlueprintVariableSetNodeTool(), F.BP, G, [](FJsonObject& P)
        { P.SetStringField(TEXT("variableName"), TEXT("Shared")); P.SetBoolField(TEXT("dryRun"), true); });
        if (!Good(*this, Preview, TEXT("Local setter dry run"))) return false;
        TestEqual(TEXT("Dry run preserves revision"), Edit::ComputeGraphRevision(G), Revision);
        TestEqual(TEXT("Dry run preserves status"), F.BP->Status, Status); TestFalse(TEXT("Dry run preserves clean package"), F.BP->GetOutermost()->IsDirty());
        const auto Get = Run(FAddBlueprintVariableGetNodeTool(), F.BP, G, [](FJsonObject& P) { P.SetStringField(TEXT("variableName"), TEXT("Shared")); });
        const auto Set = Run(FAddBlueprintVariableSetNodeTool(), F.BP, G, [](FJsonObject& P) { P.SetStringField(TEXT("variableName"), TEXT("Shared")); });
        if (!Good(*this, Get, TEXT("Local getter")) || !Good(*this, Set, TEXT("Local setter"))) return false;
        TestEqual(TEXT("Local scope reported"), Get.Result->GetStringField(TEXT("variableScope")), FString(TEXT("local")));
        TestEqual(TEXT("Selected function scope reported"), Get.Result->GetStringField(TEXT("scopeName")), G->GetName());
        auto* Getter = Cast<UK2Node_VariableGet>(Find(G, Get.Result->GetStringField(TEXT("nodeGuid"))));
        auto* Setter = Cast<UK2Node_VariableSet>(Find(G, Set.Result->GetStringField(TEXT("nodeGuid"))));
        if (!TestNotNull(TEXT("Local getter node exists"), Getter) || !TestNotNull(TEXT("Local setter node exists"), Setter)) return false;
        const auto& Declaration = FFixture::Entry(G)->LocalVariables[0]; Guids.Add(Declaration.VarGuid);
        FGuid ReportedGuid; FGuid::Parse(Get.Result->GetStringField(TEXT("variableGuid")), ReportedGuid);
        TestEqual(TEXT("Getter reports declaration GUID"), ReportedGuid, Declaration.VarGuid);
        TestEqual(TEXT("Getter references declaration GUID"), Getter->VariableReference.GetMemberGuid(), Declaration.VarGuid);
        TestEqual(TEXT("Setter references declaration GUID"), Setter->VariableReference.GetMemberGuid(), Declaration.VarGuid);
        TestEqual(TEXT("Function determines local pin type"), Getter->FindPin(TEXT("Shared"))->PinType.PinCategory, G == F.First ? F.Int.PinCategory : F.String.PinCategory);
        const auto* Schema = GetDefault<UEdGraphSchema_K2>();
        TestTrue(TEXT("Local get connects to set"), Schema->TryCreateConnection(Getter->FindPin(TEXT("Shared")), Setter->FindPin(TEXT("Shared"))));
        if (G == F.First)
        {
            TestTrue(TEXT("Fresh Actor array assignment is executable"), Schema->TryCreateConnection(FFixture::Entry(G)->FindPin(TEXT("then")), FreshSetter->FindPin(TEXT("execute"))));
            TestTrue(TEXT("Local setter exec follows fresh assignment"), Schema->TryCreateConnection(FreshSetter->FindPin(TEXT("then")), Setter->GetExecPin()));
        }
        else TestTrue(TEXT("Local setter exec connects"), Schema->TryCreateConnection(FFixture::Entry(G)->FindPin(TEXT("then")), Setter->GetExecPin()));
        TestNotNull(TEXT("Setter exposes Output_Get"), Setter->FindPin(TEXT("Output_Get")));
    }
    TestTrue(TEXT("Same-named declarations have different GUIDs"), Guids[0] != Guids[1]);
    const auto Member = Run(FAddBlueprintVariableGetNodeTool(), F.BP, F.Second, [&](FJsonObject& P)
    { P.SetStringField(TEXT("variableName"), TEXT("Shared")); P.SetStringField(TEXT("ownerClassPath"), F.BP->GeneratedClass->GetPathName()); });
    if (!Good(*this, Member, TEXT("Explicit owner selects member over local"))) return false;
    TestEqual(TEXT("Explicit member scope reported"), Member.Result->GetStringField(TEXT("variableScope")), FString(TEXT("member")));
    auto* MemberNode = Cast<UK2Node_VariableGet>(Find(F.Second, Member.Result->GetStringField(TEXT("nodeGuid"))));
    TestTrue(TEXT("Class member retains self context"), MemberNode && MemberNode->VariableReference.IsSelfContext());
    TestEqual(TEXT("Class member retains int type despite string local"), MemberNode->FindPin(TEXT("Shared"))->PinType.PinCategory, F.Int.PinCategory);
    const auto External = Run(FAddBlueprintVariableGetNodeTool(), F.BP, F.First, [](FJsonObject& P)
    { P.SetStringField(TEXT("variableName"), TEXT("Tags")); P.SetStringField(TEXT("ownerClassPath"), TEXT("/Script/Engine.Actor")); });
    if (!Good(*this, External, TEXT("Native member regression"))) return false;
    const auto Foreign = Run(FAddBlueprintVariableGetNodeTool(), F.BP, F.First, [](FJsonObject& P)
    { P.SetStringField(TEXT("variableName"), TEXT("ComponentTags")); P.SetStringField(TEXT("ownerClassPath"), TEXT("/Script/Engine.ActorComponent")); });
    if (!Good(*this, Foreign, TEXT("External member regression"))) return false;
    auto* ForeignNode = Cast<UK2Node_VariableGet>(Find(F.First, Foreign.Result->GetStringField(TEXT("nodeGuid"))));
    TestTrue(TEXT("External member retains external context"), ForeignNode && !ForeignNode->VariableReference.IsSelfContext());
    TestNotNull(TEXT("External member keeps target pin"), ForeignNode->FindPin(UEdGraphSchema_K2::PN_Self));
    return Compile(*this, F.BP) && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPLocalVariablePatchTest, "UnrealMCP.Blueprint.Authoring.LocalVariableNodes.Patch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPLocalVariablePatchTest::RunTest(const FString& Parameters)
{
    FFixture F; if (!Compile(*this, F.BP)) return false;
    const auto SourceResult = Run(FAddBlueprintVariableGetNodeTool(), F.BP, F.First, [](FJsonObject& P) { P.SetStringField(TEXT("variableName"), TEXT("ProjectilesArray")); });
    if (!Good(*this, SourceResult, TEXT("Original projectiles getter"))) return false;
    auto* Source = Cast<UK2Node_VariableGet>(Find(F.First, SourceResult.Result->GetStringField(TEXT("nodeGuid"))));
    auto* Original = NewObject<UK2Node_CallFunction>(F.First); Original->SetFromFunction(F.BP->GeneratedClass->FindFunctionByName(TEXT("PredictProxy")));
    Edit::PlaceNewNode(F.First, Original, {});
    if (!TestTrue(TEXT("Original consumer linked"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Source->FindPin(TEXT("ProjectilesArray")), Original->FindPin(TEXT("ActorsToIgnore"))))) return false;
    auto Existing = Node(TEXT("entry"), TEXT("existingNode")); Existing->SetStringField(TEXT("nodeGuid"), Edit::GetNodeGuid(FFixture::Entry(F.First)));
    auto OldSource = Node(TEXT("source"), TEXT("existingNode")); OldSource->SetStringField(TEXT("nodeGuid"), Edit::GetNodeGuid(Source));
    auto Get = Node(TEXT("local"), TEXT("variableGet"), TEXT("PredictionActorsToIgnore"));
    auto Set = Node(TEXT("copy"), TEXT("variableSet"), TEXT("PredictionActorsToIgnore"));
    auto Self = Node(TEXT("self"), TEXT("self"));
    // Array custom thunks require the editor's specialized node, matching the existing DSM graph fixture.
    auto* AddUnique = NewObject<UK2Node_CallArrayFunction>(F.First);
    AddUnique->SetFromFunction(UKismetArrayLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_AddUnique)));
    Edit::PlaceNewNode(F.First, AddUnique, {});
    auto Add = Node(TEXT("unique"), TEXT("existingNode")); Add->SetStringField(TEXT("nodeGuid"), Edit::GetNodeGuid(AddUnique));
    auto Predict = Node(TEXT("predict"), TEXT("functionCall")); Predict->SetStringField(TEXT("functionName"), TEXT("PredictProxy"));
    Predict->SetStringField(TEXT("ownerClassPath"), F.BP->GeneratedClass->GetPathName());
    auto Fill = [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("patchId"), TEXT("local-array-copy"));
        P.SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(Existing), MakeShared<FJsonValueObject>(OldSource), MakeShared<FJsonValueObject>(Get),
            MakeShared<FJsonValueObject>(Set), MakeShared<FJsonValueObject>(Self), MakeShared<FJsonValueObject>(Add), MakeShared<FJsonValueObject>(Predict)});
        P.SetArrayField(TEXT("connections"), {Link(TEXT("entry"), TEXT("then"), TEXT("copy"), TEXT("execute")),
            Link(TEXT("source"), TEXT("ProjectilesArray"), TEXT("copy"), TEXT("PredictionActorsToIgnore")),
            Link(TEXT("copy"), TEXT("then"), TEXT("unique"), TEXT("execute")), Link(TEXT("local"), TEXT("PredictionActorsToIgnore"), TEXT("unique"), TEXT("TargetArray")),
            Link(TEXT("self"), TEXT("self"), TEXT("unique"), TEXT("NewItem")), Link(TEXT("unique"), TEXT("then"), TEXT("predict"), TEXT("execute")),
            Link(TEXT("local"), TEXT("PredictionActorsToIgnore"), TEXT("predict"), TEXT("ActorsToIgnore"))});
    };
    F.BP->GetOutermost()->SetDirtyFlag(false); const auto Status = F.BP->Status; const auto Before = Edit::ComputeGraphRevision(F.First);
    const auto LocalGuid = FFixture::Entry(F.First)->LocalVariables.Last().VarGuid;
    const auto Preview = Run(FApplyBlueprintGraphPatchTool(), F.BP, F.First, [&](FJsonObject& P) { Fill(P); P.SetBoolField(TEXT("dryRun"), true); });
    if (!Good(*this, Preview, TEXT("Local array connected patch dry run"))) return false;
    TestEqual(TEXT("Patch preview leaves graph unchanged"), Edit::ComputeGraphRevision(F.First), Before);
    TestEqual(TEXT("Patch preview leaves status unchanged"), F.BP->Status, Status); TestFalse(TEXT("Patch preview preserves clean package"), F.BP->GetOutermost()->IsDirty());
    const auto Applied = Run(FApplyBlueprintGraphPatchTool(), F.BP, F.First, Fill);
    if (!Good(*this, Applied, TEXT("Local array copy patch"))) return false;
    TestTrue(TEXT("Local array patch compiles"), Applied.Result->GetBoolField(TEXT("compileSucceeded")));
    TestTrue(TEXT("Original array consumer remains linked"), Source->FindPin(TEXT("ProjectilesArray"))->LinkedTo.Contains(Original->FindPin(TEXT("ActorsToIgnore"))));
    const FString After = Edit::ComputeGraphRevision(F.First); const int32 Count = F.First->Nodes.Num();
    const auto Again = Run(FApplyBlueprintGraphPatchTool(), F.BP, F.First, Fill);
    if (!Good(*this, Again, TEXT("Local patch idempotent reuse"))) return false;
    TestTrue(TEXT("Second application reports complete"), Again.Result->GetBoolField(TEXT("alreadyComplete")));
    TestEqual(TEXT("Idempotent patch adds no nodes"), F.First->Nodes.Num(), Count);
    TestEqual(TEXT("Idempotent patch preserves revision"), Edit::ComputeGraphRevision(F.First), After);
    F.BP->GetOutermost()->SetDirtyFlag(false); const auto BeforeRejectedStatus = F.BP->Status;
    const auto Invalid = Run(FApplyBlueprintGraphPatchTool(), F.BP, F.First, [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("patchId"), TEXT("invalid-local-link"));
        auto A = Node(TEXT("local"), TEXT("variableGet"), TEXT("PredictionActorsToIgnore")); auto B = Node(TEXT("set"), TEXT("variableSet"), TEXT("Shared"));
        P.SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(A), MakeShared<FJsonValueObject>(B)});
        P.SetArrayField(TEXT("connections"), {Link(TEXT("local"), TEXT("PredictionActorsToIgnore"), TEXT("set"), TEXT("Shared"))});
    });
    TestTrue(TEXT("Incompatible local link is rejected"), Invalid.Error.IsSet());
    TestEqual(TEXT("Rejected patch restores graph"), Edit::ComputeGraphRevision(F.First), After);
    TestFalse(TEXT("Rejected patch preserves clean package"), F.BP->GetOutermost()->IsDirty());
    TestEqual(TEXT("Rejected patch preserves status"), F.BP->Status, BeforeRejectedStatus);
    TestEqual(TEXT("Rejected patch preserves local declaration GUID"), FFixture::Entry(F.First)->LocalVariables.Last().VarGuid, LocalGuid);
    return Compile(*this, F.BP) && !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPLocalVariableRollbackTest, "UnrealMCP.Blueprint.Authoring.LocalVariableNodes.CompileRollback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPLocalVariableRollbackTest::RunTest(const FString& Parameters)
{
    FFixture F; if (!Compile(*this, F.BP)) return false;
    const auto SetterResponse = Run(FAddBlueprintVariableSetNodeTool(), F.BP, F.First, [](FJsonObject& P)
    { P.SetStringField(TEXT("variableName"), TEXT("Shared")); P.SetBoolField(TEXT("compileAfterEdit"), false); });
    if (!Good(*this, SetterResponse, TEXT("Rollback fixture local setter"))) return false;
    auto* Setter = Cast<UK2Node_VariableSet>(Find(F.First, SetterResponse.Result->GetStringField(TEXT("nodeGuid"))));
    if (!TestNotNull(TEXT("Rollback fixture setter node"), Setter)) return false;
    auto* Broken = NewObject<UK2Node_VariableGet>(F.First);
    Broken->VariableReference.SetSelfMember(TEXT("RemovedVariable"));
    Edit::PlaceNewNode(F.First, Broken, {});
    // A stale variable node retains its old pin after its declaration disappears in a real graph.
    UEdGraphPin* BrokenValue = Broken->CreatePin(EGPD_Output, F.Int, TEXT("RemovedVariable"));
    const auto* Schema = GetDefault<UEdGraphSchema_K2>();
    if (!TestTrue(TEXT("Broken getter reaches local setter"), Schema->TryCreateConnection(BrokenValue, Setter->FindPin(TEXT("Shared"))))
        || !TestTrue(TEXT("Invalid local assignment is executable"), Schema->TryCreateConnection(FFixture::Entry(F.First)->FindPin(TEXT("then")), Setter->GetExecPin()))) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(F.BP);
    FCompilerResultsLog InvalidLog; InvalidLog.bSilentMode = true;
    FKismetEditorUtilities::CompileBlueprint(F.BP, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &InvalidLog);
    if (!TestTrue(TEXT("Fixture has a genuine reachable compiler error"), InvalidLog.NumErrors > 0)) return false;
    F.BP->GetOutermost()->SetDirtyFlag(false);
    const auto BeforeStatus = F.BP->Status; const auto BeforeRevision = Edit::ComputeGraphRevision(F.First);
    const int32 BeforeCount = F.First->Nodes.Num();
    const auto BeforeLocals = FFixture::Entry(F.First)->LocalVariables;
    const auto Failed = Run(FApplyBlueprintGraphPatchTool(), F.BP, F.First, [](FJsonObject& P)
    {
        P.SetStringField(TEXT("patchId"), TEXT("local-compile-rollback")); P.SetBoolField(TEXT("compileAfterEdit"), true);
        P.SetArrayField(TEXT("nodes"), {MakeShared<FJsonValueObject>(Node(TEXT("local"), TEXT("variableGet"), TEXT("PredictionActorsToIgnore"))),
            MakeShared<FJsonValueObject>(Node(TEXT("set"), TEXT("variableSet"), TEXT("PredictionActorsToIgnore")))});
    });
    TestTrue(TEXT("Compile failure returns error"), Failed.Error.IsSet());
    if (Failed.Error.IsSet()) TestTrue(TEXT("Compile failure confirms rollback"), Failed.Error->Message.Contains(TEXT("rolled back=true")));
    TestEqual(TEXT("Compile rollback removes patch-added nodes"), F.First->Nodes.Num(), BeforeCount);
    TestEqual(TEXT("Compile rollback restores exact graph revision"), Edit::ComputeGraphRevision(F.First), BeforeRevision);
    TestEqual(TEXT("Compile rollback restores Blueprint status"), F.BP->Status, BeforeStatus);
    TestFalse(TEXT("Compile rollback restores clean package"), F.BP->GetOutermost()->IsDirty());
    const auto& AfterLocals = FFixture::Entry(F.First)->LocalVariables;
    TestEqual(TEXT("Compile rollback preserves local declaration count"), AfterLocals.Num(), BeforeLocals.Num());
    for (int32 I = 0; I < FMath::Min(AfterLocals.Num(), BeforeLocals.Num()); ++I)
    {
        TestEqual(TEXT("Compile rollback preserves local declaration GUID"), AfterLocals[I].VarGuid, BeforeLocals[I].VarGuid);
        TestEqual(TEXT("Compile rollback preserves local declaration name"), AfterLocals[I].VarName, BeforeLocals[I].VarName);
        TestTrue(TEXT("Compile rollback preserves local declaration type"), AfterLocals[I].VarType == BeforeLocals[I].VarType);
    }
    const auto StandaloneFailure = Run(FAddBlueprintVariableGetNodeTool(), F.BP, F.First, [](FJsonObject& P)
    {
        P.SetStringField(TEXT("variableName"), TEXT("PredictionActorsToIgnore")); P.SetBoolField(TEXT("compileAfterEdit"), true);
    });
    TestTrue(TEXT("Standalone compile failure is reported"), StandaloneFailure.Error.IsSet());
    if (StandaloneFailure.Error.IsSet() && TestTrue(TEXT("Standalone failure has rollback evidence"), StandaloneFailure.Error->Data.IsValid()))
        TestTrue(TEXT("Standalone addition rolled back"), StandaloneFailure.Error->Data->GetBoolField(TEXT("rolledBack")));
    TestEqual(TEXT("Standalone rollback restores node count"), F.First->Nodes.Num(), BeforeCount);
    TestEqual(TEXT("Standalone rollback restores graph revision"), Edit::ComputeGraphRevision(F.First), BeforeRevision);
    TestEqual(TEXT("Standalone rollback restores compile status"), F.BP->Status, BeforeStatus);
    TestFalse(TEXT("Standalone rollback restores clean package"), F.BP->GetOutermost()->IsDirty());
    return !HasAnyErrors();
}
#endif
