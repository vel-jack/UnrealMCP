#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Misc/AutomationTest.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/GetBlueprintOverviewTool.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPBlueprintOverviewTest,
    "UnrealMCP.Blueprint.Overview.ReadOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealMCPBlueprintOverviewTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Temp/UnrealMCPOverview_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    Package->SetFlags(RF_Transient);
    UBlueprint* Blueprint = NewObject<UBlueprint>(Package.Get(), TEXT("OverviewFixture"), RF_Transient);
    UEdGraph* Graph = NewObject<UEdGraph>(Blueprint, TEXT("EventGraph"), RF_Transient);
    Graph->Schema = UEdGraphSchema_K2::StaticClass();
    Graph->GraphGuid = FGuid::NewGuid();
    Blueprint->UbergraphPages.Add(Graph);
    for (const TCHAR* Name : { TEXT("Zulu"), TEXT("Alpha") })
    {
        FBPVariableDescription Variable;
        Variable.VarName = FName(Name);
        Variable.VarGuid = FGuid::NewGuid();
        Variable.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
        Blueprint->NewVariables.Add(Variable);
    }
    const auto StatusBefore = Blueprint->Status;
    const FString RevisionBefore = UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
    UnrealMCP::FMCPRequest Request;
    Request.Id = TEXT("overview-test");
    Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
    Request.Params->SetBoolField(TEXT("allowLoad"), false);
    const FGetBlueprintOverviewTool Tool;
    for (bool bDirty : { false, true })
    {
        Package->SetDirtyFlag(bDirty);
        auto Response = Tool.Execute(Request);
        if (!TestFalse(TEXT("Resident unregistered Blueprint can be inspected"), Response.Error.IsSet())) return false;
        TestEqual(TEXT("All owned members counted"), Response.Result->GetIntegerField(TEXT("matchedCount")), 3);
        TestTrue(TEXT("Complete bounded inventory"), Response.Result->GetBoolField(TEXT("coverageComplete")));
        TestFalse(TEXT("Does not load resident asset"), Response.Result->GetBoolField(TEXT("loadedForInspection")));
        TestEqual(TEXT("Live provenance"), Response.Result->GetStringField(TEXT("source")), FString(TEXT("live_editor")));
        TestFalse(TEXT("No index required"), Response.Result->GetBoolField(TEXT("indexUsed")));
        TestEqual(TEXT("Dirty state preserved"), Package->IsDirty(), bDirty);
        TestEqual(TEXT("Compile state preserved"), Blueprint->Status, StatusBefore);
        TestEqual(TEXT("Graph revision preserved"), UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph), RevisionBefore);
    }
    Request.Params->SetNumberField(TEXT("limit"), 1);
    auto Page = Tool.Execute(Request);
    TestTrue(TEXT("Has continuation"), Page.Result->GetBoolField(TEXT("hasMore")));
    TestFalse(TEXT("Partial inventory is not complete"), Page.Result->GetBoolField(TEXT("coverageComplete")));
    TestEqual(TEXT("One member emitted"), Page.Result->GetArrayField(TEXT("members")).Num(), 1);
    TestTrue(TEXT("Graph identity is available without pins"), Page.Result->GetArrayField(TEXT("members"))[0]->AsObject()->HasField(TEXT("graphGuid")));
    Request.Params->SetNumberField(TEXT("offset"), 1);
    auto Second = Tool.Execute(Request);
    TestEqual(TEXT("Stable alphabetical member order"), Second.Result->GetArrayField(TEXT("members"))[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Alpha")));
    Request.Params->SetNumberField(TEXT("offset"), 0);
    Request.Params->SetStringField(TEXT("kind"), TEXT("variable"));
    Request.Params->SetStringField(TEXT("query"), TEXT("alpha"));
    auto Filtered = Tool.Execute(Request);
    TestEqual(TEXT("Case insensitive server filtering"), Filtered.Result->GetIntegerField(TEXT("matchedCount")), 1);
    TestTrue(TEXT("Filtered scope fully covered"), Filtered.Result->GetBoolField(TEXT("coverageComplete")));
    Request.Params->SetStringField(TEXT("query"), TEXT("Absent"));
    auto Empty = Tool.Execute(Request);
    TestEqual(TEXT("Zero results explicit"), Empty.Result->GetIntegerField(TEXT("matchedCount")), 0);
    TestEqual(TEXT("Zero result scan coverage preserved"), Empty.Result->GetIntegerField(TEXT("scannedCount")), 3);
    Request.Params->SetNumberField(TEXT("limit"), 101);
    TestTrue(TEXT("Oversized page rejected"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetNumberField(TEXT("limit"), 1.5);
    TestTrue(TEXT("Fractional page rejected"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetNumberField(TEXT("limit"), 30);
    Request.Params->SetStringField(TEXT("allowLoad"), TEXT("false"));
    TestTrue(TEXT("Malformed load policy rejected before any load"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetBoolField(TEXT("allowLoad"), false);
    auto* LevelScript = NewObject<ULevelScriptBlueprint>(Package.Get(), TEXT("ExcludedLevel"), RF_Transient);
    Request.Params->SetStringField(TEXT("objectPath"), LevelScript->GetPathName());
    TestTrue(TEXT("No level script scope"), Tool.Execute(Request).Error.IsSet());
    Request.Params->SetStringField(TEXT("objectPath"), TEXT("/Game/UnrealMCP_NoSuchBlueprint.UnrealMCP_NoSuchBlueprint"));
    TestTrue(TEXT("Resident-only miss is an error"), Tool.Execute(Request).Error.IsSet());
    Package->SetDirtyFlag(false);
    return true;
}
#endif
