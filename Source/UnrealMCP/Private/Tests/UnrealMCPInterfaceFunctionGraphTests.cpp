#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/AddBlueprintInterfaceFunctionGraphTool.h"
#include "Tools/AddBlueprintInterfaceTool.h"

namespace
{
    constexpr TCHAR TestRoot[] = TEXT("/Game/UnrealMCP_Automation");
    constexpr TCHAR FunctionName[] = TEXT("TransformValue");
    constexpr TCHAR InputPinName[] = TEXT("InputValue");
    constexpr TCHAR OutputPinName[] = TEXT("ReturnValue");

    UBlueprint* CreateBlueprintFixture(
        const FString& PackagePath,
        UClass* ParentClass,
        const EBlueprintType BlueprintType)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        const FName AssetName(*FPackageName::GetLongPackageAssetName(PackagePath));
        return FKismetEditorUtilities::CreateBlueprint(
            ParentClass,
            Package,
            AssetName,
            BlueprintType,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("UnrealMCPAutomation")));
    }

    UK2Node_FunctionEntry* AddOutputBearingInterfaceFunction(UBlueprint* InterfaceBlueprint)
    {
        UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
            InterfaceBlueprint,
            FName(FunctionName),
            UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(
            InterfaceBlueprint, Graph, true, nullptr);

        TArray<UK2Node_FunctionEntry*> Entries;
        Graph->GetNodesOfClass(Entries);
        if (Entries.Num() != 1)
        {
            return nullptr;
        }

        FEdGraphPinType IntegerType;
        IntegerType.PinCategory = UEdGraphSchema_K2::PC_Int;
        Entries[0]->CreateUserDefinedPin(
            FName(InputPinName), IntegerType, EGPD_Output, false);

        UK2Node_FunctionResult* Result =
            FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entries[0]);
        if (Result == nullptr)
        {
            return nullptr;
        }

        FEdGraphPinType BooleanType;
        BooleanType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        Result->CreateUserDefinedPin(
            FName(OutputPinName), BooleanType, EGPD_Input, false);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(InterfaceBlueprint);
        return Entries[0];
    }

    bool JsonPinsContain(
        const TArray<TSharedPtr<FJsonValue>>& Pins,
        const FString& PinName,
        const FString& Direction)
    {
        return Pins.ContainsByPredicate(
            [&PinName, &Direction](const TSharedPtr<FJsonValue>& Value)
            {
                const TSharedPtr<FJsonObject> Pin = Value.IsValid()
                    ? Value->AsObject()
                    : nullptr;
                return Pin.IsValid()
                    && Pin->GetStringField(TEXT("pinName")) == PinName
                    && Pin->GetStringField(TEXT("direction")) == Direction;
            });
    }

    bool NodeContainsPin(
        const UEdGraphNode* Node,
        const FName PinName,
        const EEdGraphPinDirection Direction)
    {
        return Node != nullptr && Node->Pins.ContainsByPredicate(
            [PinName, Direction](const UEdGraphPin* Pin)
            {
                return Pin != nullptr
                    && Pin->PinName == PinName
                    && Pin->Direction == Direction;
            });
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPAddBlueprintInterfaceFunctionGraphLiveTest,
    "UnrealMCP.Blueprint.Authoring.AddInterfaceFunctionGraph.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPAddBlueprintInterfaceFunctionGraphLiveTest::RunTest(
    const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString InterfaceName = FString::Printf(TEXT("BPI_Output_%s"), *Suffix);
    const FString ActorName = FString::Printf(TEXT("BP_Impl_%s"), *Suffix);
    const FString InterfacePackagePath =
        FString::Printf(TEXT("%s/%s"), TestRoot, *InterfaceName);
    const FString ActorPackagePath =
        FString::Printf(TEXT("%s/%s"), TestRoot, *ActorName);
    const FString InterfaceObjectPath =
        FString::Printf(TEXT("%s.%s"), *InterfacePackagePath, *InterfaceName);
    const FString ActorObjectPath =
        FString::Printf(TEXT("%s.%s"), *ActorPackagePath, *ActorName);

    UBlueprint* InterfaceBlueprint = nullptr;
    UBlueprint* ActorBlueprint = nullptr;
    ON_SCOPE_EXIT
    {
        TArray<UObject*> AssetsToDelete;
        if (ActorBlueprint != nullptr)
        {
            AssetsToDelete.Add(ActorBlueprint);
        }
        if (InterfaceBlueprint != nullptr)
        {
            AssetsToDelete.Add(InterfaceBlueprint);
        }
        if (!AssetsToDelete.IsEmpty())
        {
            ObjectTools::DeleteObjectsUnchecked(AssetsToDelete);
        }
        if (UPackage* Package = FindPackage(nullptr, *ActorPackagePath))
        {
            Package->SetDirtyFlag(false);
        }
        if (UPackage* Package = FindPackage(nullptr, *InterfacePackagePath))
        {
            Package->SetDirtyFlag(false);
        }
    };

    InterfaceBlueprint = CreateBlueprintFixture(
        InterfacePackagePath, UInterface::StaticClass(), BPTYPE_Interface);
    if (!TestNotNull(TEXT("Temporary Blueprint Interface created"), InterfaceBlueprint))
    {
        return false;
    }
    if (!TestNotNull(
            TEXT("Output-bearing interface function entry created"),
            AddOutputBearingInterfaceFunction(InterfaceBlueprint)))
    {
        return false;
    }

    FKismetEditorUtilities::CompileBlueprint(
        InterfaceBlueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    TestTrue(
        TEXT("Interface compiled"),
        InterfaceBlueprint->Status == BS_UpToDate
            || InterfaceBlueprint->Status == BS_UpToDateWithWarnings);

    UClass* InterfaceClass = InterfaceBlueprint->GeneratedClass;
    if (!TestNotNull(TEXT("Interface generated class exists"), InterfaceClass))
    {
        return false;
    }
    TestTrue(
        TEXT("Generated class is an interface"),
        InterfaceClass->HasAnyClassFlags(CLASS_Interface));
    const UFunction* InterfaceFunction =
        InterfaceClass->FindFunctionByName(FName(FunctionName));
    if (!TestNotNull(TEXT("Compiled interface function exists"), InterfaceFunction))
    {
        return false;
    }
    TestFalse(
        TEXT("Output-bearing interface function is not event-compatible"),
        UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(InterfaceFunction));

    ActorBlueprint = CreateBlueprintFixture(
        ActorPackagePath, AActor::StaticClass(), BPTYPE_Normal);
    if (!TestNotNull(TEXT("Temporary Actor Blueprint created"), ActorBlueprint))
    {
        return false;
    }

    FBPInterfaceDescription InterfaceDescription;
    InterfaceDescription.Interface = InterfaceClass;
    ActorBlueprint->ImplementedInterfaces.Add(InterfaceDescription);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ActorBlueprint);
    TestEqual(
        TEXT("Implementation starts without an interface graph"),
        ActorBlueprint->ImplementedInterfaces.Last().Graphs.Num(),
        0);

    UnrealMCP::FMCPRequest Request;
    Request.Id = TEXT("interface-function-live-create");
    Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), ActorObjectPath);
    Request.Params->SetStringField(
        TEXT("interfaceClassPath"), InterfaceClass->GetPathName());
    Request.Params->SetStringField(TEXT("functionName"), FunctionName);
    Request.Params->SetBoolField(TEXT("saveAfterEdit"), false);

    const FAddBlueprintInterfaceFunctionGraphTool Tool;
    const UnrealMCP::FMCPResponse CreateResponse = Tool.Execute(Request);
    TestFalse(TEXT("Create response has no error"), CreateResponse.Error.IsSet());
    if (!TestTrue(TEXT("Create response has a result"), CreateResponse.Result.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("Create succeeded"), CreateResponse.Result->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Graph was created"), CreateResponse.Result->GetBoolField(TEXT("created")));
    TestFalse(
        TEXT("First invocation is not already existing"),
        CreateResponse.Result->GetBoolField(TEXT("alreadyExists")));

    const TSharedPtr<FJsonObject> EntryJson =
        CreateResponse.Result->GetObjectField(TEXT("entryNode"));
    TestTrue(
        TEXT("Response entry exposes interface input"),
        JsonPinsContain(
            EntryJson->GetArrayField(TEXT("pins")), InputPinName, TEXT("output")));
    const TArray<TSharedPtr<FJsonValue>>& ResultJsonNodes =
        CreateResponse.Result->GetArrayField(TEXT("resultNodes"));
    TestEqual(TEXT("Response contains one result node"), ResultJsonNodes.Num(), 1);
    if (ResultJsonNodes.Num() == 1)
    {
        TestTrue(
            TEXT("Response result exposes interface output"),
            JsonPinsContain(
                ResultJsonNodes[0]->AsObject()->GetArrayField(TEXT("pins")),
                OutputPinName,
                TEXT("input")));
    }

    const FBPInterfaceDescription& ImplementedInterface =
        ActorBlueprint->ImplementedInterfaces.Last();
    TestEqual(
        TEXT("Tool added exactly one interface graph"),
        ImplementedInterface.Graphs.Num(),
        1);
    UEdGraph* ImplementationGraph = ImplementedInterface.Graphs.Num() == 1
        ? ImplementedInterface.Graphs[0]
        : nullptr;
    if (!TestNotNull(TEXT("Implementation graph exists"), ImplementationGraph))
    {
        return false;
    }

    TArray<UK2Node_FunctionEntry*> Entries;
    TArray<UK2Node_FunctionResult*> Results;
    ImplementationGraph->GetNodesOfClass(Entries);
    ImplementationGraph->GetNodesOfClass(Results);
    TestEqual(TEXT("Implementation has one entry"), Entries.Num(), 1);
    TestEqual(TEXT("Implementation has one result"), Results.Num(), 1);
    if (Entries.Num() == 1)
    {
        TestTrue(
            TEXT("Implementation entry has the input signature pin"),
            NodeContainsPin(Entries[0], FName(InputPinName), EGPD_Output));
    }
    if (Results.Num() == 1)
    {
        TestTrue(
            TEXT("Implementation result has the return signature pin"),
            NodeContainsPin(Results[0], FName(OutputPinName), EGPD_Input));
    }

    Request.Id = TEXT("interface-function-live-idempotent");
    const UnrealMCP::FMCPResponse ExistingResponse = Tool.Execute(Request);
    TestFalse(TEXT("Idempotent response has no error"), ExistingResponse.Error.IsSet());
    if (TestTrue(
            TEXT("Idempotent response has a result"),
            ExistingResponse.Result.IsValid()))
    {
        TestTrue(
            TEXT("Second invocation reports alreadyExists"),
            ExistingResponse.Result->GetBoolField(TEXT("alreadyExists")));
        TestFalse(
            TEXT("Second invocation creates nothing"),
            ExistingResponse.Result->GetBoolField(TEXT("created")));
    }
    TestEqual(
        TEXT("Idempotent invocation did not duplicate the graph"),
        ActorBlueprint->ImplementedInterfaces.Last().Graphs.Num(),
        1);

    FKismetEditorUtilities::CompileBlueprint(
        ActorBlueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    TestTrue(
        TEXT("Implementing Actor Blueprint validates after tool execution"),
        ActorBlueprint->Status == BS_UpToDate
            || ActorBlueprint->Status == BS_UpToDateWithWarnings);

    return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPAttachInterfaceTest, "UnrealMCP.Blueprint.Authoring.Interface.Attach", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPAttachInterfaceTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UBlueprint* Interface = CreateBlueprintFixture(TEXT("/Game/UnrealMCP_Automation/BPI_Attach_") + Suffix, UInterface::StaticClass(), BPTYPE_Interface);
    UBlueprint* Target = CreateBlueprintFixture(TEXT("/Game/UnrealMCP_Automation/BP_Attach_") + Suffix, AActor::StaticClass(), BPTYPE_Normal);
    ON_SCOPE_EXIT { ObjectTools::DeleteObjectsUnchecked({Target, Interface}); };
    if (!TestNotNull(TEXT("Interface signature"), AddOutputBearingInterfaceFunction(Interface))) return false;
    FKismetEditorUtilities::CompileBlueprint(Interface);
    FKismetEditorUtilities::CompileBlueprint(Target);
    UnrealMCP::FMCPRequest Request; Request.Params = MakeShared<FJsonObject>();
    Request.Params->SetStringField(TEXT("objectPath"), Target->GetPathName());
    Request.Params->SetStringField(TEXT("interfaceClassPath"), Interface->GeneratedClass->GetPathName());
    Request.Params->SetBoolField(TEXT("dryRun"), true);
    const int32 Before = Target->ImplementedInterfaces.Num();
    auto Preview = FAddBlueprintInterfaceTool().Execute(Request);
    TestTrue(TEXT("Preflight succeeds"), Preview.Result->GetBoolField(TEXT("success")));
    TestEqual(TEXT("Dry-run preserves interface inventory"), Target->ImplementedInterfaces.Num(), Before);
    Request.Params->SetBoolField(TEXT("dryRun"), false);
    Target->GetOutermost()->SetDirtyFlag(true);
    Request.Params->SetBoolField(TEXT("saveAfterEdit"), true);
    auto Dirty = FAddBlueprintInterfaceTool().Execute(Request);
    TestFalse(TEXT("Saving pre-existing unsaved work is refused"), Dirty.Result->GetBoolField(TEXT("success")));
    TestEqual(TEXT("Save refusal preserves interface inventory"), Target->ImplementedInterfaces.Num(), Before);
    Request.Params->SetBoolField(TEXT("saveAfterEdit"), false);
    auto Apply = FAddBlueprintInterfaceTool().Execute(Request);
    TestTrue(TEXT("Attachment succeeds"), Apply.Result->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Attachment compiles"), Apply.Result->GetBoolField(TEXT("compileSucceeded")));
    TestEqual(TEXT("One interface attached"), Target->ImplementedInterfaces.Num(), Before + 1);
    TestTrue(TEXT("Signature-correct function exists"), Target->GeneratedClass->FindFunctionByName(FName(FunctionName)) != nullptr);
    auto Retry = FAddBlueprintInterfaceTool().Execute(Request);
    TestTrue(TEXT("Retry reports existing interface"), Retry.Result->GetBoolField(TEXT("alreadyExists")));
    TestFalse(TEXT("Retry does not attach again"), Retry.Result->GetBoolField(TEXT("added")));
    TestEqual(TEXT("No duplicate interface"), Target->ImplementedInterfaces.Num(), Before + 1);
    UBlueprint* Child = CreateBlueprintFixture(TEXT("/Game/UnrealMCP_Automation/BP_AttachChild_") + Suffix, Target->GeneratedClass, BPTYPE_Normal);
    Request.Params->SetStringField(TEXT("objectPath"), Child->GetPathName());
    auto InheritedResult = FAddBlueprintInterfaceTool().Execute(Request);
    TestTrue(TEXT("Inherited implementation detected"), InheritedResult.Result->GetBoolField(TEXT("inherited")));
    TestEqual(TEXT("Inherited interface not duplicated directly"), Child->ImplementedInterfaces.Num(), 0);
    ObjectTools::DeleteObjectsUnchecked({Child});
    UBlueprint* Collision = CreateBlueprintFixture(TEXT("/Game/UnrealMCP_Automation/BP_AttachCollision_") + Suffix, AActor::StaticClass(), BPTYPE_Normal);
    AddOutputBearingInterfaceFunction(Collision);
    Request.Params->SetStringField(TEXT("objectPath"), Collision->GetPathName());
    auto Conflict = FAddBlueprintInterfaceTool().Execute(Request);
    TestFalse(TEXT("Existing function is not replaced"), Conflict.Result->GetBoolField(TEXT("success")));
    TestEqual(TEXT("Collision leaves interface inventory unchanged"), Collision->ImplementedInterfaces.Num(), 0);
    ObjectTools::DeleteObjectsUnchecked({Collision});
    Request.Params->SetStringField(TEXT("objectPath"), Target->GetPathName());
    Request.Params->SetStringField(TEXT("interfaceClassPath"), TEXT("/Script/Engine.Actor"));
    auto Invalid = FAddBlueprintInterfaceTool().Execute(Request);
    TestFalse(TEXT("Non-interface class rejected"), Invalid.Result->GetBoolField(TEXT("success")));
    return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
