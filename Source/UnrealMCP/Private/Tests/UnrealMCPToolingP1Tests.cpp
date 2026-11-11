#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintLocalVariableTools.h"
#include "Tools/CompileBlueprintTool.h"
#include "Tools/ConsolidateAndDeleteAssetTool.h"
#include "Tools/FindClassUsagesTool.h"
#include "Tools/RetargetBlueprintNodeTool.h"

// Tests for the tooling-requirements P1 tools: R2 retarget, R3 locals, R7 class usages, R11 compile, R12 consolidate.
namespace UnrealMCPToolingP1
{
    struct FFixture
    {
        TArray<TWeakObjectPtr<UBlueprint>> Blueprints;

        UBlueprint* Create(const TCHAR* Prefix, UClass* Parent = AActor::StaticClass())
        {
            const FString Name = FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
                Parent, CreatePackage(*FString::Printf(TEXT("/Game/UnrealMCP_Automation/%s"), *Name)), FName(*Name), BPTYPE_Normal,
                UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("UnrealMCPAutomation")));
            FAssetRegistryModule::AssetCreated(Blueprint);
            Blueprints.Add(Blueprint);
            return Blueprint;
        }

        ~FFixture()
        {
            for (const TWeakObjectPtr<UBlueprint>& Weak : Blueprints)
            {
                UBlueprint* Blueprint = Weak.Get();
                if (Blueprint == nullptr) continue;
                UPackage* Package = Blueprint->GetPackage();
                FAssetRegistryModule::AssetDeleted(Blueprint);
                ObjectTools::DeleteObjectsUnchecked({Blueprint});
                if (Package) Package->SetDirtyFlag(false);
            }
        }
    };

    FString PathOf(const UObject* Object) { return Object->GetPathName(); }

    UnrealMCP::FMCPResponse Run(const FMCPToolBase& Tool, const TSharedRef<FJsonObject>& Params)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = TEXT("tooling-p1");
        Request.Params = Params;
        return Tool.Execute(Request);
    }

    template <typename TNode>
    TNode* Place(UEdGraph* Graph)
    {
        TNode* Node = NewObject<TNode>(Graph);
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, {});
        return Node;
    }

    FEdGraphPinType ObjectType(UClass* Class)
    {
        FEdGraphPinType Type;
        Type.PinCategory = UEdGraphSchema_K2::PC_Object;
        Type.PinSubCategoryObject = Class;
        return Type;
    }

    TSet<FString> Kinds(const UnrealMCP::FMCPResponse& Response)
    {
        TSet<FString> Result;
        if (!Response.Result.IsValid()) return Result;
        for (const TSharedPtr<FJsonValue>& Hit : Response.Result->GetArrayField(TEXT("usages")))
        {
            Result.Add(Hit->AsObject()->GetStringField(TEXT("kind")));
        }
        return Result;
    }

    void Compile(UBlueprint* Blueprint)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    }
}

using namespace UnrealMCPToolingP1;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPRetargetNodeTest, "UnrealMCP.Blueprint.Authoring.RetargetNode.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPRetargetNodeTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Blueprint = Fixture.Create(TEXT("BP_Retarget"));
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
    CastNode->TargetType = APawn::StaticClass();
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, CastNode, {});
    UK2Node_CallFunction* Destroy = Place<UK2Node_CallFunction>(Graph);
    Destroy->FunctionReference.SetExternalMember(TEXT("K2_DestroyActor"), AActor::StaticClass());
    Destroy->ReconstructNode();
    TestTrue(TEXT("Cast result linked"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(CastNode->GetCastResultPin(), Destroy->FindPin(UEdGraphSchema_K2::PN_Self)));

    // Simulate the target class being deleted: this is the "Bad cast node" state.
    CastNode->TargetType = nullptr;
    CastNode->ReconstructNode();

    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
    Params->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString());
    Params->SetStringField(TEXT("nodeGuid"), CastNode->NodeGuid.ToString());
    Params->SetStringField(TEXT("newClassPath"), PathOf(ACharacter::StaticClass()));
    TestTrue(TEXT("Retarget without confirm is refused"), Run(FRetargetBlueprintNodeTool(), Params).Error.IsSet());

    Params->SetBoolField(TEXT("confirm"), true);
    const UnrealMCP::FMCPResponse Response = Run(FRetargetBlueprintNodeTool(), Params);
    if (Response.Error.IsSet()) AddError(Response.Error->Message);
    if (!TestTrue(TEXT("Retarget succeeds"), Response.Result.IsValid())) return false;
    TestTrue(TEXT("Reported as broken before"), Response.Result->GetBoolField(TEXT("wasBroken")));
    TestTrue(TEXT("Cast now targets Character"), CastNode->TargetType == ACharacter::StaticClass());
    UEdGraphPin* Result = CastNode->GetCastResultPin();
    TestTrue(TEXT("Result link survives"), Result && Result->LinkedTo.Num() == 1 && Result->LinkedTo[0]->GetOwningNode() == Destroy);
    TestFalse(TEXT("No orphan pins left"), CastNode->Pins.ContainsByPredicate([](const UEdGraphPin* Pin) { return Pin->bOrphanedPin; }));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPLocalVariablesTest, "UnrealMCP.Blueprint.Authoring.LocalVariables.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPLocalVariablesTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Blueprint = Fixture.Create(TEXT("BP_Locals"));
    UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("DoWork"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);

    auto Params = [&]()
    {
        TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
        P->SetStringField(TEXT("graphGuid"), Function->GraphGuid.ToString());
        return P;
    };
    auto Count = [&]()
    {
        const UnrealMCP::FMCPResponse R = Run(FListBlueprintLocalVariablesTool(), Params());
        return R.Result.IsValid() ? static_cast<int32>(R.Result->GetNumberField(TEXT("count"))) : -1;
    };

    TestEqual(TEXT("No locals at start"), Count(), 0);
    TSharedRef<FJsonObject> Add = Params();
    Add->SetStringField(TEXT("variableName"), TEXT("Count"));
    Add->SetStringField(TEXT("type"), TEXT("int"));
    Add->SetStringField(TEXT("defaultValue"), TEXT("3"));
    Add->SetBoolField(TEXT("compileAfterEdit"), true);
    const UnrealMCP::FMCPResponse Added = Run(FAddBlueprintLocalVariableTool(), Add);
    if (Added.Error.IsSet()) AddError(Added.Error->Message);
    TestTrue(TEXT("Local added"), Added.Result.IsValid() && Added.Result->GetBoolField(TEXT("added")));
    TestTrue(TEXT("Compiles with the local"), Added.Result.IsValid() && Added.Result->GetBoolField(TEXT("compileSucceeded")));
    TestEqual(TEXT("One local listed"), Count(), 1);
    const UnrealMCP::FMCPResponse Again = Run(FAddBlueprintLocalVariableTool(), Add);
    TestTrue(TEXT("Second add is idempotent"), Again.Result.IsValid() && Again.Result->GetBoolField(TEXT("alreadyExists")));
    TestEqual(TEXT("Still one local"), Count(), 1);

    TSharedRef<FJsonObject> Remove = Params();
    Remove->SetStringField(TEXT("variableName"), TEXT("Count"));
    TestTrue(TEXT("Remove without confirm is refused"), Run(FRemoveBlueprintLocalVariableTool(), Remove).Error.IsSet());
    Remove->SetBoolField(TEXT("confirm"), true);
    const UnrealMCP::FMCPResponse Removed = Run(FRemoveBlueprintLocalVariableTool(), Remove);
    if (Removed.Error.IsSet()) AddError(Removed.Error->Message);
    TestEqual(TEXT("No locals after remove"), Count(), 0);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPFindClassUsagesTest, "UnrealMCP.Blueprint.Query.FindClassUsages.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPFindClassUsagesTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Target = Fixture.Create(TEXT("BP_UsageTarget"));
    Compile(Target);
    UBlueprint* Blueprint = Fixture.Create(TEXT("BP_UsageUser"));
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("MyPawn"), ObjectType(APawn::StaticClass()));
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("MyTarget"), ObjectType(Target->GeneratedClass));
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
    CastNode->TargetType = APawn::StaticClass();
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, CastNode, {});
    UK2Node_CallFunction* Destroy = Place<UK2Node_CallFunction>(Graph);
    Destroy->FunctionReference.SetExternalMember(TEXT("K2_DestroyActor"), AActor::StaticClass());
    Destroy->ReconstructNode();

    auto Find = [&](const FString& Path)
    {
        TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
        P->SetStringField(TEXT("classOrAssetPath"), Path);
        return Run(FFindClassUsagesTool(), P);
    };

    const UnrealMCP::FMCPResponse Pawn = Find(PathOf(APawn::StaticClass()));
    const TSet<FString> PawnKinds = Kinds(Pawn);
    TestTrue(TEXT("Pawn: member variable found"), PawnKinds.Contains(TEXT("member_variable")));
    TestTrue(TEXT("Pawn: cast target found"), PawnKinds.Contains(TEXT("cast_target")));
    TestFalse(TEXT("Pawn: Actor parent not matched"), PawnKinds.Contains(TEXT("parent_class")));
    TestFalse(TEXT("Pawn: Actor function call not matched"), PawnKinds.Contains(TEXT("function_call")));
    TestFalse(TEXT("Pawn: no false stale import"), PawnKinds.Contains(TEXT("stale_import_only")));

    TestTrue(TEXT("Actor: parent class found"), Kinds(Find(PathOf(AActor::StaticClass()))).Contains(TEXT("parent_class")));
    TestTrue(TEXT("Blueprint target: member variable found by asset path"), Kinds(Find(PathOf(Target))).Contains(TEXT("member_variable")));
    TestTrue(TEXT("Bare /Script package rejected"), Find(TEXT("/Script/Engine")).Error.IsSet());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPCompileResultTest, "UnrealMCP.Blueprint.Authoring.CompileResult.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPCompileResultTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Blueprint = Fixture.Create(TEXT("BP_Compile"));
    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("objectPath"), PathOf(Blueprint));

    const UnrealMCP::FMCPResponse Clean = Run(FCompileBlueprintTool(), Params);
    if (!TestTrue(TEXT("Clean compile returns a result"), Clean.Result.IsValid())) return false;
    TestEqual(TEXT("Clean compile has no errors"), static_cast<int32>(Clean.Result->GetNumberField(TEXT("errorCount"))), 0);
    TestTrue(TEXT("Clean compile has an errors array"), Clean.Result->HasTypedField<EJson::Array>(TEXT("errors")));
    TestTrue(TEXT("Clean compile reports a status"), !Clean.Result->GetStringField(TEXT("status")).IsEmpty());

    // An executed cast with no target class is a compile error.
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
    Event->CustomFunctionName = TEXT("Go");
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Event, {});
    UK2Node_DynamicCast* BadCast = NewObject<UK2Node_DynamicCast>(Graph);
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, BadCast, {});
    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), BadCast->GetExecPin());

    AddExpectedError(TEXT("cast"), EAutomationExpectedErrorFlags::Contains, 0);
    const UnrealMCP::FMCPResponse Broken = Run(FCompileBlueprintTool(), Params);
    if (!TestTrue(TEXT("Failed compile still returns a structured result"), Broken.Result.IsValid())) return false;
    const TArray<TSharedPtr<FJsonValue>> Errors = Broken.Result->GetArrayField(TEXT("errors"));
    TestTrue(TEXT("Errors reported"), Errors.Num() > 0);
    const FString BadGuid = BadCast->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
    TestTrue(TEXT("Error points at the bad cast node"), Errors.ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Error)
    {
        return Error->AsObject()->GetStringField(TEXT("nodeGuid")).Equals(BadGuid, ESearchCase::IgnoreCase)
            || Error->AsObject()->GetStringField(TEXT("nodeGuid")).Equals(BadCast->NodeGuid.ToString(), ESearchCase::IgnoreCase);
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPConsolidateTest, "UnrealMCP.Asset.ConsolidateAndDelete.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPConsolidateTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Old = Fixture.Create(TEXT("BP_Old"));
    UBlueprint* Replacement = Fixture.Create(TEXT("BP_New"));
    Compile(Old);
    Compile(Replacement);
    UBlueprint* User = Fixture.Create(TEXT("BP_OldUser"));
    FBlueprintEditorUtils::AddMemberVariable(User, TEXT("Ref"), ObjectType(Old->GeneratedClass));
    Compile(User);
    const FString OldPath = PathOf(Old);

    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("oldAssetPath"), OldPath);
    Params->SetStringField(TEXT("replacementAssetPath"), PathOf(Replacement));
    TestTrue(TEXT("Consolidate without confirm is refused"), Run(FConsolidateAndDeleteAssetTool(), Params).Error.IsSet());
    Params->SetBoolField(TEXT("dryRun"), true);
    TestTrue(TEXT("Dry-run succeeds"), Run(FConsolidateAndDeleteAssetTool(), Params).Result.IsValid());
    TestNotNull(TEXT("Dry-run keeps the old asset"), FindObject<UBlueprint>(nullptr, *OldPath));

    Params->SetBoolField(TEXT("dryRun"), false);
    Params->SetBoolField(TEXT("confirm"), true);
    const UnrealMCP::FMCPResponse Response = Run(FConsolidateAndDeleteAssetTool(), Params);
    if (Response.Error.IsSet()) AddError(Response.Error->Message);
    if (!TestTrue(TEXT("Consolidate returns a result"), Response.Result.IsValid())) return false;
    TestTrue(TEXT("Consolidate reports success"), Response.Result->GetBoolField(TEXT("success")));
    TestFalse(TEXT("Old asset is gone"), Response.Result->GetBoolField(TEXT("oldAssetStillExists")));

    const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(User, TEXT("Ref"));
    if (Index != INDEX_NONE) AddInfo(FString::Printf(TEXT("Ref type after consolidate: %s (replacement %s)"),
        *GetPathNameSafe(User->NewVariables[Index].VarType.PinSubCategoryObject.Get()), *GetPathNameSafe(Replacement->GeneratedClass)));
    TestTrue(TEXT("User variable now points at the replacement class"), Index != INDEX_NONE
        && User->NewVariables[Index].VarType.PinSubCategoryObject.Get() == Replacement->GeneratedClass.Get());
    Compile(User);
    TestTrue(TEXT("Referencer still compiles"), User->Status == BS_UpToDate || User->Status == BS_UpToDateWithWarnings);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
