#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/AddBlueprintMacroNodeTool.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/ConnectBlueprintPinsTool.h"

namespace
{
    constexpr TCHAR TestRoot[] = TEXT("/Game/UnrealMCP_Automation");

    UBlueprint* CreateActorBlueprintFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        const FName AssetName(*FPackageName::GetLongPackageAssetName(PackagePath));
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package, AssetName, BPTYPE_Normal,
            UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("UnrealMCPAutomation")));
    }

    UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString)
    {
        FGuid Guid;
        if (Graph == nullptr || !FGuid::Parse(GuidString, Guid)) return nullptr;
        const TObjectPtr<UEdGraphNode>* Match = Graph->Nodes.FindByPredicate([Guid](const UEdGraphNode* Node)
        {
            return Node != nullptr && Node->NodeGuid == Guid;
        });
        return Match != nullptr ? Match->Get() : nullptr;
    }

    bool HasPin(const UEdGraphNode* Node, const FName Name, const EEdGraphPinDirection Direction)
    {
        return Node != nullptr && Node->Pins.ContainsByPredicate([Name, Direction](const UEdGraphPin* Pin)
        {
            return Pin != nullptr && Pin->PinName == Name && Pin->Direction == Direction;
        });
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPForEachLoopWithBreakLiveTest,
    "UnrealMCP.Blueprint.Authoring.ForEachLoopWithBreak.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPForEachLoopWithBreakLiveTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_LoopBreak_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), TestRoot, *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);

    UBlueprint* Blueprint = CreateActorBlueprintFixture(PackagePath);
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
    };

    if (!TestNotNull(TEXT("Temporary Actor Blueprint created"), Blueprint)
        || !TestTrue(TEXT("Blueprint has an event graph"), Blueprint->UbergraphPages.Num() > 0))
    {
        return false;
    }

    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    FEdGraphPinType ActorArrayType;
    ActorArrayType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ActorArrayType.PinSubCategoryObject = AActor::StaticClass();
    ActorArrayType.ContainerType = EPinContainerType::Array;
    const FName VariableName(TEXT("Actors"));
    if (!TestTrue(TEXT("Typed Actor array variable added"),
        FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, ActorArrayType)))
    {
        return false;
    }

    UnrealMCP::FMCPRequest Request;
    Request.Id = TEXT("foreach-loop-with-break");
    Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    Request.Params->SetStringField(TEXT("graphName"), Graph->GetName());
    Request.Params->SetStringField(TEXT("macroName"), TEXT("ForEachLoopWithBreak"));
    Request.Params->SetNumberField(TEXT("positionX"), 500);
    Request.Params->SetNumberField(TEXT("positionY"), 300);

    const FAddBlueprintMacroNodeTool Tool;
    const UnrealMCP::FMCPResponse Response = Tool.Execute(Request);
    TestFalse(TEXT("Macro tool has no MCP error"), Response.Error.IsSet());
    if (!TestTrue(TEXT("Macro tool returns a result"), Response.Result.IsValid())) return false;

    UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(
        FindNodeByGuid(Graph, Response.Result->GetStringField(TEXT("nodeGuid"))));
    if (!TestNotNull(TEXT("ForEachLoopWithBreak macro node exists"), MacroNode)) return false;

    TestTrue(TEXT("Macro has Exec input"), HasPin(MacroNode, TEXT("Exec"), EGPD_Input));
    TestTrue(TEXT("Macro has Array input"), HasPin(MacroNode, TEXT("Array"), EGPD_Input));
    TestTrue(TEXT("Macro has Break input"), HasPin(MacroNode, TEXT("Break"), EGPD_Input));
    TestTrue(TEXT("Macro has Loop Body output"), HasPin(MacroNode, TEXT("LoopBody"), EGPD_Output));
    TestTrue(TEXT("Macro has Array Element output"), HasPin(MacroNode, TEXT("Array Element"), EGPD_Output));
    TestTrue(TEXT("Macro has Array Index output"), HasPin(MacroNode, TEXT("Array Index"), EGPD_Output));
    TestTrue(TEXT("Macro has Completed output"), HasPin(MacroNode, TEXT("Completed"), EGPD_Output));

    const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName);
    if (!TestTrue(TEXT("Array variable is resolvable"), VariableIndex != INDEX_NONE)) return false;
    UK2Node_VariableGet* VariableGet = NewObject<UK2Node_VariableGet>(Graph);
    VariableGet->VariableReference.SetSelfMember(VariableName, Blueprint->NewVariables[VariableIndex].VarGuid);
    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;
    Placement.X = 100;
    Placement.Y = 300;
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, VariableGet, Placement);

    UnrealMCP::FMCPRequest ConnectRequest;
    ConnectRequest.Id = TEXT("foreach-loop-with-break-connect");
    ConnectRequest.Params = MakeShared<FJsonObject>();
    ConnectRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    ConnectRequest.Params->SetStringField(TEXT("graphName"), Graph->GetName());
    ConnectRequest.Params->SetStringField(TEXT("sourceNodeGuid"),
        UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(VariableGet));
    ConnectRequest.Params->SetStringField(TEXT("sourcePinName"), VariableGet->GetValuePin()->PinName.ToString());
    ConnectRequest.Params->SetStringField(TEXT("targetNodeGuid"),
        UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(MacroNode));
    ConnectRequest.Params->SetStringField(TEXT("targetPinName"), TEXT("Array"));
    const FConnectBlueprintPinsTool ConnectTool;
    const UnrealMCP::FMCPResponse ConnectResponse = ConnectTool.Execute(ConnectRequest);
    TestFalse(TEXT("Array connection has no MCP error"), ConnectResponse.Error.IsSet());
    if (!TestTrue(TEXT("Array connection returns a result"), ConnectResponse.Result.IsValid())) return false;
    TestTrue(TEXT("Typed Actor array connects to ForEachLoopWithBreak"),
        ConnectResponse.Result->GetBoolField(TEXT("connected")));
    TestTrue(TEXT("Connection reports wildcard specialization"),
        ConnectResponse.Result->GetBoolField(TEXT("wildcardResolved")));
    TestFalse(TEXT("Connection returns resolved target node pins"),
        ConnectResponse.Result->GetArrayField(TEXT("targetNodePinsAfter")).IsEmpty());

    MacroNode->ReconstructNode();
    UEdGraphPin* MacroArray = MacroNode->FindPin(TEXT("Array"), EGPD_Input);
    TestNotNull(TEXT("Macro array input survives reconstruction"), MacroArray);
    if (MacroArray != nullptr)
    {
        TestEqual(TEXT("Macro array resolves to Object"),
            MacroArray->PinType.PinCategory, UEdGraphSchema_K2::PC_Object);
        TestTrue(TEXT("Macro array resolves to Actor"),
            MacroArray->PinType.PinSubCategoryObject == AActor::StaticClass());
    }
    TestTrue(TEXT("Break pin remains stable after reconstruction"),
        HasPin(MacroNode, TEXT("Break"), EGPD_Input));

    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    TestTrue(TEXT("Blueprint compiles with typed ForEachLoopWithBreak"),
        Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
