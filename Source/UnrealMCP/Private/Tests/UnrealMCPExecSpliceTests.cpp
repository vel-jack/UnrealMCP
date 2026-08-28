#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/SpliceBlueprintExecFlowTool.h"

namespace
{
    UBlueprint* CreateSpliceFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package, *FPackageName::GetLongPackageAssetName(PackagePath),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("UnrealMCPAutomation")));
    }

    UEdGraphPin* Pin(UEdGraphNode* Node, const TCHAR* Name, EEdGraphPinDirection Direction)
    {
        for (UEdGraphPin* Candidate : Node->Pins)
            if (Candidate != nullptr && Candidate->Direction == Direction && Candidate->PinName == Name) return Candidate;
        return nullptr;
    }

    UnrealMCP::FMCPRequest BuildSpliceRequest(
        const FString& ObjectPath, UEdGraph* Graph, UEdGraphNode* Source,
        UEdGraphNode* Target, const TArray<UEdGraphNode*>& Inserted, bool bDryRun)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = bDryRun ? TEXT("exec-splice-dry-run") : TEXT("exec-splice-apply");
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Request.Params->SetStringField(TEXT("spliceId"), TEXT("exec-splice-automation-v1"));
        Request.Params->SetStringField(TEXT("sourceNodeGuid"), UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Source));
        Request.Params->SetStringField(TEXT("sourcePinName"), TEXT("then"));
        Request.Params->SetStringField(TEXT("targetNodeGuid"), UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Target));
        Request.Params->SetStringField(TEXT("targetPinName"), TEXT("execute"));
        Request.Params->SetBoolField(TEXT("dryRun"), bDryRun);
        Request.Params->SetBoolField(TEXT("compileAfterEdit"), true);
        Request.Params->SetBoolField(TEXT("saveAfterEdit"), false);
        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (UEdGraphNode* Node : Inserted)
        {
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("nodeGuid"), UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node));
            Item->SetStringField(TEXT("inputPinName"), TEXT("execute"));
            Item->SetStringField(TEXT("outputPinName"), TEXT("then"));
            Nodes.Add(MakeShared<FJsonValueObject>(Item));
        }
        Request.Params->SetArrayField(TEXT("insertedNodes"), Nodes);
        return Request;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPExecSpliceLiveTest,
    "UnrealMCP.Blueprint.Authoring.ExecSplice.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPExecSpliceLiveTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_ExecSplice_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("/Game/UnrealMCP_Automation/%s"), *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);
    UBlueprint* Blueprint = CreateSpliceFixture(PackagePath);
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
    };
    if (!TestNotNull(TEXT("Splice fixture created"), Blueprint)
        || !TestTrue(TEXT("Splice fixture has EventGraph"), Blueprint->UbergraphPages.Num() > 0)) return false;

    FEdGraphPinType FlagType; FlagType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    TestTrue(TEXT("SpliceFlag variable created"),
        FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("SpliceFlag"), FlagType));
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("SpliceFlag"));
    TestTrue(TEXT("SpliceFlag variable resolves"), VariableIndex != INDEX_NONE);

    UK2Node_CustomEvent* Source = NewObject<UK2Node_CustomEvent>(Graph);
    Source->CustomFunctionName = TEXT("ExecSpliceSource");
    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;
    Placement.X = 0; Placement.Y = 600;
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Source, Placement);
    UK2Node_VariableSet* First = UnrealMCP::BlueprintGraphEditToolUtils::CreateVariableSetNode(
        Graph, TEXT("SpliceFlag"), Blueprint->NewVariables[VariableIndex].VarGuid, false);
    Placement.X = 350;
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, First, Placement);
    UK2Node_VariableSet* Second = UnrealMCP::BlueprintGraphEditToolUtils::CreateVariableSetNode(
        Graph, TEXT("SpliceFlag"), Blueprint->NewVariables[VariableIndex].VarGuid, false);
    Placement.X = 700;
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Second, Placement);
    UK2Node_IfThenElse* Target = UnrealMCP::BlueprintGraphEditToolUtils::CreateBranchNode(Graph, false);
    Placement.X = 1050;
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Target, Placement);
    const UEdGraphSchema_K2* Schema = CastChecked<UEdGraphSchema_K2>(Graph->GetSchema());
    UEdGraphPin* SourceOut = Pin(Source, TEXT("then"), EGPD_Output);
    UEdGraphPin* TargetIn = Pin(Target, TEXT("execute"), EGPD_Input);
    TestNotNull(TEXT("Source execution output resolves"), SourceOut);
    TestNotNull(TEXT("Target execution input resolves"), TargetIn);
    TestTrue(TEXT("Initial exact route created"), Schema->TryCreateConnection(SourceOut, TargetIn));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);

    const FSpliceBlueprintExecFlowTool Tool;
    const TArray<UEdGraphNode*> Inserted = {First, Second};
    UnrealMCP::FMCPRequest DryRun = BuildSpliceRequest(ObjectPath, Graph, Source, Target, Inserted, true);
    const UnrealMCP::FMCPResponse DryResponse = Tool.Execute(DryRun);
    TestFalse(TEXT("Exec splice dry-run has no error"), DryResponse.Error.IsSet());
    if (TestTrue(TEXT("Exec splice dry-run returns result"), DryResponse.Result.IsValid()))
    {
        TestEqual(TEXT("Exec splice dry-run returns both nodes"), DryResponse.Result->GetArrayField(TEXT("insertedNodes")).Num(), 2);
        TestFalse(TEXT("Exec splice dry-run does not change graph"), DryResponse.Result->GetBoolField(TEXT("changed")));
    }
    TestTrue(TEXT("Dry-run preserves original route"), SourceOut->LinkedTo.Contains(TargetIn));

    UnrealMCP::FMCPRequest Apply = BuildSpliceRequest(ObjectPath, Graph, Source, Target, Inserted, false);
    const FString OriginalRevision = UnrealMCP::BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
    Apply.Params->SetStringField(TEXT("expectedGraphRevision"), OriginalRevision);
    const UnrealMCP::FMCPResponse ApplyResponse = Tool.Execute(Apply);
    TestFalse(TEXT("Exec splice apply has no error"), ApplyResponse.Error.IsSet());
    if (TestTrue(TEXT("Exec splice apply returns result"), ApplyResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Exec splice changed graph"), ApplyResponse.Result->GetBoolField(TEXT("changed")));
        TestTrue(TEXT("Exec splice compiled"), ApplyResponse.Result->GetBoolField(TEXT("compiled")));
        TestTrue(TEXT("Exec splice compilation succeeded"), ApplyResponse.Result->GetBoolField(TEXT("compileSucceeded")));
        TestFalse(TEXT("Exec splice remained unsaved"), ApplyResponse.Result->GetBoolField(TEXT("saved")));
        TestEqual(TEXT("Exec splice removed one link"), ApplyResponse.Result->GetArrayField(TEXT("removedLinks")).Num(), 1);
        TestEqual(TEXT("Exec splice added three links"), ApplyResponse.Result->GetArrayField(TEXT("addedLinks")).Num(), 3);
    }

    Apply.Params->RemoveField(TEXT("expectedGraphRevision"));
    const UnrealMCP::FMCPResponse RetryResponse = Tool.Execute(Apply);
    TestFalse(TEXT("Exec splice retry has no error"), RetryResponse.Error.IsSet());
    if (TestTrue(TEXT("Exec splice retry returns result"), RetryResponse.Result.IsValid()))
    {
        TestTrue(TEXT("Exec splice retry is complete"), RetryResponse.Result->GetBoolField(TEXT("alreadyComplete")));
        TestFalse(TEXT("Exec splice retry makes no change"), RetryResponse.Result->GetBoolField(TEXT("changed")));
    }

    Apply.Params->SetStringField(TEXT("expectedGraphRevision"), OriginalRevision);
    const UnrealMCP::FMCPResponse StaleResponse = Tool.Execute(Apply);
    TestTrue(TEXT("Exec splice stale revision is rejected"), StaleResponse.Error.IsSet());
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
