#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/UnrealMCPTypedContainerFunctionNode.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/AddBlueprintMapOperationNodeTool.h"
#include "Tools/AddBlueprintSetOperationNodeTool.h"

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

    UEdGraphPin* FindPin(const UEdGraphNode* Node, const FName PinName)
    {
        if (Node == nullptr) return nullptr;
        UEdGraphPin* const* Match = Node->Pins.FindByPredicate([PinName](const UEdGraphPin* Pin)
        {
            return Pin != nullptr && Pin->PinName == PinName;
        });
        return Match != nullptr ? *Match : nullptr;
    }

    bool IsActorType(const FEdGraphPinType& Type)
    {
        return Type.PinCategory == UEdGraphSchema_K2::PC_Object
            && Type.PinSubCategoryObject == AActor::StaticClass();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPTypedContainerOperationsLiveTest,
    "UnrealMCP.Blueprint.Authoring.TypedContainerOperations.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPTypedContainerOperationsLiveTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_Containers_%s"), *Suffix);
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
    const FString GraphName = Graph->GetName();
    const FAddBlueprintSetOperationNodeTool SetTool;
    const TArray<FString> SetOperations = {TEXT("Contains"), TEXT("Add"), TEXT("Remove"), TEXT("Clear")};

    for (int32 Index = 0; Index < SetOperations.Num(); ++Index)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = FString::Printf(TEXT("set-%d"), Index);
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->SetStringField(TEXT("graphName"), GraphName);
        Request.Params->SetStringField(TEXT("operation"), SetOperations[Index]);
        Request.Params->SetStringField(TEXT("elementType"), TEXT("object"));
        Request.Params->SetStringField(TEXT("typeObjectPath"), AActor::StaticClass()->GetPathName());
        Request.Params->SetNumberField(TEXT("positionX"), Index * 350);
        Request.Params->SetNumberField(TEXT("positionY"), 300);

        const UnrealMCP::FMCPResponse Response = SetTool.Execute(Request);
        TestFalse(*FString::Printf(TEXT("Set %s has no MCP error"), *SetOperations[Index]), Response.Error.IsSet());
        if (!TestTrue(TEXT("Set operation returns a result"), Response.Result.IsValid())) continue;
        TestEqual(TEXT("Set operation has no wildcard pins"),
            static_cast<int32>(Response.Result->GetNumberField(TEXT("remainingWildcardPinCount"))), 0);

        UUnrealMCPTypedContainerFunctionNode* Node = Cast<UUnrealMCPTypedContainerFunctionNode>(
            FindNodeByGuid(Graph, Response.Result->GetStringField(TEXT("nodeGuid"))));
        if (!TestNotNull(TEXT("Persistent typed Set node exists"), Node)) continue;
        Node->ReconstructNode();
        UEdGraphPin* TargetSet = FindPin(Node, TEXT("TargetSet"));
        TestNotNull(TEXT("Set target pin exists after reconstruction"), TargetSet);
        if (TargetSet != nullptr)
        {
            TestTrue(TEXT("Set target remains a Set"), TargetSet->PinType.ContainerType == EPinContainerType::Set);
            TestTrue(TEXT("Set element remains Actor after reconstruction"), IsActorType(TargetSet->PinType));
        }
    }

    const FAddBlueprintMapOperationNodeTool MapTool;
    const TArray<FString> MapOperations = {
        TEXT("Add"), TEXT("Find"), TEXT("Contains"), TEXT("Remove"), TEXT("Clear"), TEXT("Keys"), TEXT("Values")};
    for (int32 Index = 0; Index < MapOperations.Num(); ++Index)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = FString::Printf(TEXT("map-%d"), Index);
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->SetStringField(TEXT("graphName"), GraphName);
        Request.Params->SetStringField(TEXT("operation"), MapOperations[Index]);
        Request.Params->SetStringField(TEXT("keyType"), TEXT("name"));
        Request.Params->SetStringField(TEXT("valueType"), TEXT("object"));
        Request.Params->SetStringField(TEXT("valueTypeObjectPath"), AActor::StaticClass()->GetPathName());
        Request.Params->SetNumberField(TEXT("positionX"), Index * 350);
        Request.Params->SetNumberField(TEXT("positionY"), 700);

        const UnrealMCP::FMCPResponse Response = MapTool.Execute(Request);
        TestFalse(*FString::Printf(TEXT("Map %s has no MCP error"), *MapOperations[Index]), Response.Error.IsSet());
        if (!TestTrue(TEXT("Map operation returns a result"), Response.Result.IsValid())) continue;
        TestEqual(TEXT("Map operation has no unresolved pins"),
            static_cast<int32>(Response.Result->GetNumberField(TEXT("remainingUnresolvedPinCount"))), 0);

        UUnrealMCPTypedContainerFunctionNode* Node = Cast<UUnrealMCPTypedContainerFunctionNode>(
            FindNodeByGuid(Graph, Response.Result->GetStringField(TEXT("nodeGuid"))));
        if (!TestNotNull(TEXT("Persistent typed Map node exists"), Node)) continue;
        Node->ReconstructNode();
        UEdGraphPin* TargetMap = FindPin(Node, TEXT("TargetMap"));
        TestNotNull(TEXT("Map target pin exists after reconstruction"), TargetMap);
        if (TargetMap != nullptr)
        {
            TestTrue(TEXT("Map target remains a Map"), TargetMap->PinType.ContainerType == EPinContainerType::Map);
            TestEqual(TEXT("Map key remains Name after reconstruction"),
                TargetMap->PinType.PinCategory, UEdGraphSchema_K2::PC_Name);
            TestEqual(TEXT("Map value remains Object after reconstruction"),
                TargetMap->PinType.PinValueType.TerminalCategory, UEdGraphSchema_K2::PC_Object);
            TestTrue(TEXT("Map value remains Actor after reconstruction"),
                TargetMap->PinType.PinValueType.TerminalSubCategoryObject == AActor::StaticClass());
        }
    }

    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    TestTrue(TEXT("Blueprint compiles after typed Set/Map node reconstruction"),
        Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
