#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_GetArrayItem.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/AddBlueprintArrayOperationNodeTool.h"

namespace
{
    constexpr TCHAR TestRoot[] = TEXT("/Game/UnrealMCP_Automation");

    UBlueprint* CreateActorBlueprintFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        const FName AssetName(*FPackageName::GetLongPackageAssetName(PackagePath));
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            Package,
            AssetName,
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("UnrealMCPAutomation")));
    }

    UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString)
    {
        FGuid Guid;
        if (Graph == nullptr || !FGuid::Parse(GuidString, Guid))
        {
            return nullptr;
        }
        const TObjectPtr<UEdGraphNode>* Match = Graph->Nodes.FindByPredicate([Guid](const UEdGraphNode* Node)
        {
            return Node != nullptr && Node->NodeGuid == Guid;
        });
        return Match != nullptr ? Match->Get() : nullptr;
    }

    bool HasResolvedActorArrayPin(const UEdGraphNode* Node)
    {
        return Node != nullptr && Node->Pins.ContainsByPredicate([](const UEdGraphPin* Pin)
        {
            return Pin != nullptr
                && Pin->PinType.ContainerType == EPinContainerType::Array
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
                && Pin->PinType.PinSubCategoryObject == AActor::StaticClass();
        });
    }

    bool HasWildcardPin(const UEdGraphNode* Node)
    {
        return Node != nullptr && Node->Pins.ContainsByPredicate([](const UEdGraphPin* Pin)
        {
            return Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;
        });
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPArrayOperationLiveTest,
    "UnrealMCP.Blueprint.Authoring.ArrayOperations.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPArrayOperationLiveTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_ArrayOps_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), TestRoot, *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);

    UBlueprint* Blueprint = CreateActorBlueprintFixture(PackagePath);
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr)
        {
            ObjectTools::DeleteObjectsUnchecked({Blueprint});
        }
        if (UPackage* Package = FindPackage(nullptr, *PackagePath))
        {
            Package->SetDirtyFlag(false);
        }
    };

    if (!TestNotNull(TEXT("Temporary Actor Blueprint created"), Blueprint)
        || !TestTrue(TEXT("Blueprint has an event graph"), Blueprint->UbergraphPages.Num() > 0))
    {
        return false;
    }

    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    const FString GraphName = Graph->GetName();
    const TArray<FString> Operations = {
        TEXT("Contains"), TEXT("Add"), TEXT("AddUnique"), TEXT("RemoveItem"),
        TEXT("Clear"), TEXT("Length"), TEXT("Get")};

    const FAddBlueprintArrayOperationNodeTool Tool;
    for (int32 Index = 0; Index < Operations.Num(); ++Index)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = FString::Printf(TEXT("array-operation-%d"), Index);
        Request.Params = MakeShared<FJsonObject>();
        Request.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Request.Params->SetStringField(TEXT("graphName"), GraphName);
        Request.Params->SetStringField(TEXT("operation"), Operations[Index]);
        Request.Params->SetStringField(TEXT("elementType"), TEXT("object"));
        Request.Params->SetStringField(TEXT("typeObjectPath"), AActor::StaticClass()->GetPathName());
        Request.Params->SetNumberField(TEXT("positionX"), Index * 350);
        Request.Params->SetNumberField(TEXT("positionY"), 300);
        Request.Params->SetBoolField(TEXT("saveAfterEdit"), false);

        const UnrealMCP::FMCPResponse Response = Tool.Execute(Request);
        TestFalse(*FString::Printf(TEXT("%s has no MCP error"), *Operations[Index]), Response.Error.IsSet());
        if (!TestTrue(*FString::Printf(TEXT("%s returns a result"), *Operations[Index]), Response.Result.IsValid()))
        {
            continue;
        }

        TestTrue(*FString::Printf(TEXT("%s succeeds"), *Operations[Index]),
            Response.Result->GetBoolField(TEXT("success")));
        TestEqual(*FString::Printf(TEXT("%s has no wildcard pins"), *Operations[Index]),
            static_cast<int32>(Response.Result->GetNumberField(TEXT("remainingWildcardPinCount"))), 0);

        UEdGraphNode* AddedNode = FindNodeByGuid(Graph, Response.Result->GetStringField(TEXT("nodeGuid")));
        if (!TestNotNull(*FString::Printf(TEXT("%s node exists"), *Operations[Index]), AddedNode))
        {
            continue;
        }
        TestTrue(*FString::Printf(TEXT("%s has a typed Actor array pin"), *Operations[Index]),
            HasResolvedActorArrayPin(AddedNode));
        TestFalse(*FString::Printf(TEXT("%s has no live wildcard pin"), *Operations[Index]),
            HasWildcardPin(AddedNode));

        if (Operations[Index] == TEXT("Get"))
        {
            TestTrue(TEXT("Get uses the native Get Array Item node"), AddedNode->IsA<UK2Node_GetArrayItem>());
        }
        else
        {
            TestTrue(*FString::Printf(TEXT("%s uses an array function node"), *Operations[Index]),
                AddedNode->IsA<UK2Node_CallArrayFunction>());
        }
    }

    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    TestTrue(TEXT("Blueprint compiles after all typed array nodes are added"),
        Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
