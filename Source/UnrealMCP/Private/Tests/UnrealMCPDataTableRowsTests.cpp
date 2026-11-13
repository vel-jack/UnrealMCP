#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_GetDataTableRow.h"
#include "Misc/AutomationTest.h"
#include "Tests/UnrealMCPDataTableTestRow.h"
#include "Tools/DataTableRowMutationTool.h"
#include "Tools/FindDataTableReferencesTool.h"
#include "Tools/InspectDataTableRowsTool.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPDataTableRowsTest,
    "UnrealMCP.DataTable.Rows.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealMCPDataTableRowsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDataTable> Table(NewObject<UDataTable>(GetTransientPackage(), NAME_None, RF_Transient));
    FUnrealMCPDataTableTestRow Alpha, Zulu;
    Alpha.Label = TEXT("Before");
    Alpha.SpawnClass = TSoftClassPtr<UObject>(FSoftObjectPath(TEXT("/Game/UnrealMCP_Missing/BP_Absent.BP_Absent_C")));
    Zulu.SpawnClass = TSoftClassPtr<UObject>(FSoftObjectPath(TEXT("/Script/Engine.UnrealMCPAbsentClass")));
    TMap<FName, const uint8*> Data;
    Data.Add(TEXT("Zulu"), reinterpret_cast<const uint8*>(&Zulu));
    Data.Add(TEXT("Alpha"), reinterpret_cast<const uint8*>(&Alpha));
    if (!TestTrue(TEXT("Transient rows created"), Table->CreateTableFromRawData(Data, FUnrealMCPDataTableTestRow::StaticStruct()).IsEmpty())) return false;

    UnrealMCP::FMCPRequest Request;
    Request.Id = TEXT("table-test");
    Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), Table->GetPathName());
    Request.Params->SetNumberField(TEXT("limit"), 1);
    const FInspectDataTableRowsTool Tool;
    const auto First = Tool.Execute(Request);
    if (!TestFalse(TEXT("First page succeeds"), First.Error.IsSet())) return false;
    TestEqual(TEXT("Total count"), First.Result->GetIntegerField(TEXT("totalRows")), 2);
    TestEqual(TEXT("Sorted first row"), First.Result->GetArrayField(TEXT("rows"))[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Alpha")));
    TestTrue(TEXT("Has second page"), First.Result->GetBoolField(TEXT("hasMore")));
    TestEqual(TEXT("Missing class package flagged"), First.Result->GetArrayField(TEXT("rows"))[0]->AsObject()->GetArrayField(TEXT("unresolvedReferences")).Num(), 1);
    Request.Params->SetNumberField(TEXT("offset"), 1);
    const auto Second = Tool.Execute(Request);
    TestEqual(TEXT("Sorted second row"), Second.Result->GetArrayField(TEXT("rows"))[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Zulu")));
    TestEqual(TEXT("Missing object in existing script package flagged"), Second.Result->GetArrayField(TEXT("rows"))[0]->AsObject()->GetArrayField(TEXT("unresolvedReferences"))[0]->AsObject()->GetStringField(TEXT("reason")), FString(TEXT("object_unresolved")));
    TestFalse(TEXT("Last page"), Second.Result->GetBoolField(TEXT("hasMore")));
    Request.Params->SetNumberField(TEXT("limit"), 101);
    TestTrue(TEXT("Oversized page rejected"), Tool.Execute(Request).Error.IsSet());
    TestFalse(TEXT("Inspection leaves package clean"), Table->GetOutermost()->IsDirty());

    Request.Params->SetStringField(TEXT("rowName"), TEXT("Alpha"));
    TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
    Fields->SetStringField(TEXT("Label"), TEXT("After"));
    Request.Params->SetObjectField(TEXT("fields"), Fields);
    const FDataTableRowMutationTool Set(FDataTableRowMutationTool::EAction::Set);
    Request.Params->SetBoolField(TEXT("dryRun"), true);
    TestTrue(TEXT("Set preflight predicts a change"), Set.Execute(Request).Result->GetBoolField(TEXT("wouldChange")));
    TestEqual(TEXT("Dry run preserves row"), Table->FindRow<FUnrealMCPDataTableTestRow>(TEXT("Alpha"), TEXT("test"))->Label, FString(TEXT("Before")));
    Request.Params->SetBoolField(TEXT("dryRun"), false);
    if (!TestFalse(TEXT("Set succeeds"), Set.Execute(Request).Error.IsSet())) return false;
    TestEqual(TEXT("Set changes only requested field"), Table->FindRow<FUnrealMCPDataTableTestRow>(TEXT("Alpha"), TEXT("test"))->Label, FString(TEXT("After")));
    TestFalse(TEXT("Set does not clear untouched reference"), Table->FindRow<FUnrealMCPDataTableTestRow>(TEXT("Alpha"), TEXT("test"))->SpawnClass.IsNull());

    Request.Params->SetStringField(TEXT("rowName"), TEXT("New"));
    const FDataTableRowMutationTool Add(FDataTableRowMutationTool::EAction::Add);
    if (!TestFalse(TEXT("Add succeeds"), Add.Execute(Request).Error.IsSet())) return false;
    TestTrue(TEXT("Added row exists"), Table->GetRowNames().Contains(TEXT("New")));
    const FDataTableRowMutationTool Remove(FDataTableRowMutationTool::EAction::Remove);
    TestTrue(TEXT("Remove requires confirmation"), Remove.Execute(Request).Error.IsSet());
    Request.Params->SetBoolField(TEXT("confirm"), true);
    if (!TestFalse(TEXT("Remove succeeds"), Remove.Execute(Request).Error.IsSet())) return false;
    TestFalse(TEXT("Removed row absent"), Table->GetRowNames().Contains(TEXT("New")));
    Request.Params->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
    if (!TestFalse(TEXT("Add default row succeeds"), Add.Execute(Request).Error.IsSet())) return false;
    TestTrue(TEXT("Default row exists"), Table->GetRowNames().Contains(TEXT("New")));
    if (!TestFalse(TEXT("Remove default row succeeds"), Remove.Execute(Request).Error.IsSet())) return false;

    TStrongObjectPtr<UBlueprint> Blueprint(NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient));
    UEdGraph* Graph = NewObject<UEdGraph>(Blueprint.Get(), TEXT("EventGraph"), RF_Transient);
    Blueprint->UbergraphPages.Add(Graph);
    UK2Node_GetDataTableRow* Node = NewObject<UK2Node_GetDataTableRow>(Graph);
    Node->NodeGuid = FGuid::NewGuid();
    Node->AllocateDefaultPins();
    Node->GetDataTablePin()->DefaultObject = Table.Get();
    Graph->Nodes.Add(Node);
    Request.Params->SetStringField(TEXT("blueprintObjectPath"), Blueprint->GetPathName());
    const FFindDataTableReferencesTool Find;
    const auto References = Find.Execute(Request);
    if (!TestFalse(TEXT("Direct reference search succeeds"), References.Error.IsSet())) return false;
    TestEqual(TEXT("Node reference found"), References.Result->GetIntegerField(TEXT("totalHitsInScannedBlueprints")), 1);
    TestEqual(TEXT("Exact node GUID returned"), References.Result->GetArrayField(TEXT("references"))[0]->AsObject()->GetStringField(TEXT("nodeGuid")), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
    return true;
}
#endif
