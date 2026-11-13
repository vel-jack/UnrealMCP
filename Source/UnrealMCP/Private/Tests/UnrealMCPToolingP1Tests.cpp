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
#include "Tools/BlueprintClassDefaultsTools.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPClassDefaultsTest, "UnrealMCP.Blueprint.Authoring.ClassDefaults.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPClassDefaultsTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Parent = Fixture.Create(TEXT("BP_DefaultsParent"));
    FEdGraphPinType ScaleType;
    ScaleType.PinCategory = UEdGraphSchema_K2::PC_Real;
    ScaleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    TestTrue(TEXT("Parent scale added"), FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("ModelScale"), ScaleType, TEXT("1.0")));
    TestTrue(TEXT("Parent flag added"), FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("ShouldSample"), BoolType, TEXT("True")));
    FEdGraphPinType MapType;
    MapType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    MapType.PinSubCategoryObject = LoadObject<UEnum>(nullptr, TEXT("/Script/Engine.ECollisionChannel"));
    MapType.ContainerType = EPinContainerType::Map;
    MapType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Class;
    MapType.PinValueType.TerminalSubCategoryObject = AActor::StaticClass();
    TestTrue(TEXT("Parent map added"), FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("TagMap"), MapType));
    Compile(Parent);
    UBlueprint* Child = Fixture.Create(TEXT("BP_DefaultsChild"), Parent->GeneratedClass);
    Compile(Child);
    UBlueprint* Shadow = Fixture.Create(TEXT("BP_DefaultsShadow"));
    TestTrue(TEXT("Shadow scale added"), FBlueprintEditorUtils::AddMemberVariable(Shadow, TEXT("ModelScale"), ScaleType, TEXT("0.0")));
    Compile(Shadow);

    auto GetOne = [&](UBlueprint* Blueprint, const TCHAR* Name)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
        Params->SetArrayField(TEXT("propertyNames"), {MakeShared<FJsonValueString>(Name)});
        const UnrealMCP::FMCPResponse Response = Run(FGetBlueprintClassDefaultsTool(), Params);
        if (Response.Error.IsSet()) AddError(Response.Error->Message);
        return Response.Result.IsValid() && Response.Result->GetArrayField(TEXT("properties")).Num() == 1
            ? Response.Result->GetArrayField(TEXT("properties"))[0]->AsObject() : TSharedPtr<FJsonObject>();
    };
    const TSharedPtr<FJsonObject> ParentScale = GetOne(Parent, TEXT("ModelScale"));
    const TSharedPtr<FJsonObject> ShadowScale = GetOne(Shadow, TEXT("ModelScale"));
    const TSharedPtr<FJsonObject> ChildFlag = GetOne(Child, TEXT("ShouldSample"));
    TestTrue(TEXT("Parent scale is one"), ParentScale.IsValid() && FCString::Atod(*ParentScale->GetStringField(TEXT("value"))) == 1.0);
    TestTrue(TEXT("Shadow scale is zero"), ShadowScale.IsValid() && FCString::Atod(*ShadowScale->GetStringField(TEXT("value"))) == 0.0);
    TestTrue(TEXT("Child initially inherits true"), ChildFlag.IsValid() && ChildFlag->GetStringField(TEXT("value")).Equals(TEXT("True"), ESearchCase::IgnoreCase));
    TestTrue(TEXT("Read leaves parent dirty state unchanged"), Parent->GetPackage()->IsDirty());
    TSharedRef<FJsonObject> CompareParams = MakeShared<FJsonObject>();
    CompareParams->SetStringField(TEXT("objectPathA"), PathOf(Parent));
    CompareParams->SetStringField(TEXT("objectPathB"), PathOf(Shadow));
    const UnrealMCP::FMCPResponse Compared = Run(FCompareBlueprintClassDefaultsTool(), CompareParams);
    if (Compared.Error.IsSet()) AddError(Compared.Error->Message);
    TestTrue(TEXT("Compare finds ModelScale regression"), Compared.Result.IsValid()
        && Compared.Result->GetArrayField(TEXT("properties")).ContainsByPredicate([](const TSharedPtr<FJsonValue>& Item)
            { return Item->AsObject()->GetStringField(TEXT("name")) == TEXT("ModelScale")
                && Item->AsObject()->GetStringField(TEXT("valueA")) != Item->AsObject()->GetStringField(TEXT("valueB")); }));

    auto SetOne = [&](UBlueprint* Blueprint, const TCHAR* Name, const TCHAR* Value, bool bAllowDirty)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("objectPath"), PathOf(Blueprint));
        TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
        Values->SetStringField(Name, Value);
        Params->SetObjectField(TEXT("values"), Values);
        Params->SetBoolField(TEXT("confirm"), true);
        Params->SetBoolField(TEXT("allowDirty"), bAllowDirty);
        return Run(FSetBlueprintClassDefaultsTool(), Params);
    };
    TestTrue(TEXT("Dirty guard rejects edit"), SetOne(Shadow, TEXT("ModelScale"), TEXT("1.0"), false).Error.IsSet());
    const UnrealMCP::FMCPResponse ChangedShadow = SetOne(Shadow, TEXT("ModelScale"), TEXT("1.0"), true);
    if (ChangedShadow.Error.IsSet()) AddError(ChangedShadow.Error->Message);
    TestTrue(TEXT("Own default changed"), ChangedShadow.Result.IsValid() && ChangedShadow.Result->GetBoolField(TEXT("compiled")));
    const UnrealMCP::FMCPResponse ChangedChild = SetOne(Child, TEXT("ShouldSample"), TEXT("False"), true);
    if (ChangedChild.Error.IsSet()) AddError(ChangedChild.Error->Message);
    TestTrue(TEXT("Inherited default changed"), ChangedChild.Result.IsValid() && ChangedChild.Result->GetBoolField(TEXT("compiled")));
    const TSharedPtr<FJsonObject> NewChildFlag = GetOne(Child, TEXT("ShouldSample"));
    if (NewChildFlag.IsValid()) AddInfo(FString::Printf(TEXT("Child default=%s parent=%s overridden=%s"),
        *NewChildFlag->GetStringField(TEXT("value")), *NewChildFlag->GetStringField(TEXT("parentValue")),
        NewChildFlag->GetBoolField(TEXT("isOverridden")) ? TEXT("true") : TEXT("false")));
    TestTrue(TEXT("Child override readback"), NewChildFlag.IsValid() && NewChildFlag->GetBoolField(TEXT("isOverridden"))
        && NewChildFlag->GetStringField(TEXT("value")).Equals(TEXT("False"), ESearchCase::IgnoreCase)
        && NewChildFlag->GetStringField(TEXT("parentValue")).Equals(TEXT("True"), ESearchCase::IgnoreCase));
    const TSharedPtr<FJsonObject> NewShadowScale = GetOne(Shadow, TEXT("ModelScale"));
    TestTrue(TEXT("Shadow scale readback"), NewShadowScale.IsValid() && FCString::Atod(*NewShadowScale->GetStringField(TEXT("value"))) == 1.0);

    TSharedRef<FJsonObject> Reset = MakeShared<FJsonObject>();
    Reset->SetStringField(TEXT("objectPath"), PathOf(Child));
    Reset->SetArrayField(TEXT("propertyNames"), {MakeShared<FJsonValueString>(TEXT("ShouldSample"))});
    Reset->SetBoolField(TEXT("confirm"), true);
    Reset->SetBoolField(TEXT("allowDirty"), true);
    const UnrealMCP::FMCPResponse ResetResult = Run(FResetBlueprintClassDefaultsTool(), Reset);
    if (ResetResult.Error.IsSet()) AddError(ResetResult.Error->Message);
    const TSharedPtr<FJsonObject> ResetFlag = GetOne(Child, TEXT("ShouldSample"));
    TestTrue(TEXT("Inherited default reset"), ResetFlag.IsValid() && !ResetFlag->GetBoolField(TEXT("isOverridden"))
        && ResetFlag->GetStringField(TEXT("value")).Equals(TEXT("True"), ESearchCase::IgnoreCase));

    const UnrealMCP::FMCPResponse Invalid = SetOne(Shadow, TEXT("ModelScale"), TEXT("not-a-double"), true);
    TestTrue(TEXT("Invalid import rejected"), Invalid.Error.IsSet());
    const TSharedPtr<FJsonObject> StillShadowScale = GetOne(Shadow, TEXT("ModelScale"));
    TestTrue(TEXT("Invalid import left CDO intact"), StillShadowScale.IsValid()
        && FCString::Atod(*StillShadowScale->GetStringField(TEXT("value"))) == 1.0);
    const FString MapExport = TEXT("((ECC_WorldStatic,Class'/Script/Engine.Actor'),(ECC_WorldDynamic,Class'/Script/Engine.Actor'),(ECC_Pawn,Class'/Script/Engine.Actor'),(ECC_Visibility,Class'/Script/Engine.Actor'))");
    const UnrealMCP::FMCPResponse MapSet = SetOne(Parent, TEXT("TagMap"), *MapExport, true);
    if (MapSet.Error.IsSet()) AddError(MapSet.Error->Message);
    const TSharedPtr<FJsonObject> MapRead = GetOne(Parent, TEXT("TagMap"));
    TestTrue(TEXT("Map default set and read"), MapSet.Result.IsValid() && MapSet.Result->GetBoolField(TEXT("compiled"))
        && MapRead.IsValid() && MapRead->GetStringField(TEXT("value")).Contains(TEXT("ECC_Visibility")));
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

    TSharedRef<FJsonObject> SetDefault = Params();
    SetDefault->SetStringField(TEXT("variableName"), TEXT("Count"));
    SetDefault->SetStringField(TEXT("defaultValue"), TEXT("7"));
    SetDefault->SetBoolField(TEXT("compileAfterEdit"), true);
    const UnrealMCP::FMCPResponse Updated = Run(FSetBlueprintLocalVariableDefaultTool(), SetDefault);
    if (Updated.Error.IsSet()) AddError(Updated.Error->Message);
    TestTrue(TEXT("Local default updated and compiled"), Updated.Result.IsValid() && Updated.Result->GetBoolField(TEXT("compileSucceeded")));
    const FBPVariableDescription* CountLocal = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Function, TEXT("Count"));
    TestTrue(TEXT("New local default stored"), CountLocal && CountLocal->DefaultValue == TEXT("7"));

    TSharedRef<FJsonObject> Map = Params();
    Map->SetStringField(TEXT("variableName"), TEXT("ClassByChannel"));
    Map->SetStringField(TEXT("type"), TEXT("enum"));
    Map->SetStringField(TEXT("typeObjectPath"), TEXT("/Script/Engine.ECollisionChannel"));
    Map->SetStringField(TEXT("containerType"), TEXT("map"));
    Map->SetStringField(TEXT("valueType"), TEXT("class"));
    Map->SetStringField(TEXT("valueTypeObjectPath"), AActor::StaticClass()->GetPathName());
    Map->SetBoolField(TEXT("compileAfterEdit"), true);
    const UnrealMCP::FMCPResponse MapAdded = Run(FAddBlueprintLocalVariableTool(), Map);
    if (MapAdded.Error.IsSet()) AddError(MapAdded.Error->Message);
    TestTrue(TEXT("Enum to class map local compiles"), MapAdded.Result.IsValid() && MapAdded.Result->GetBoolField(TEXT("compileSucceeded")));
    const FBPVariableDescription* MapLocal = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Function, TEXT("ClassByChannel"));
    TestTrue(TEXT("Map key and value retain types"), MapLocal
        && MapLocal->VarType.ContainerType == EPinContainerType::Map
        && MapLocal->VarType.PinCategory == UEdGraphSchema_K2::PC_Byte
        && MapLocal->VarType.PinSubCategoryObject == LoadObject<UEnum>(nullptr, TEXT("/Script/Engine.ECollisionChannel"))
        && MapLocal->VarType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Class);

    const FString MapDefault = TEXT("((ECC_WorldStatic,Class'/Script/Engine.Actor'),(ECC_WorldDynamic,Class'/Script/Engine.Actor'),(ECC_Pawn,Class'/Script/Engine.Actor'),(ECC_Visibility,Class'/Script/Engine.Actor'))");
    TSharedRef<FJsonObject> SetMap = Params();
    SetMap->SetStringField(TEXT("variableName"), TEXT("ClassByChannel"));
    SetMap->SetStringField(TEXT("defaultValue"), MapDefault);
    SetMap->SetBoolField(TEXT("compileAfterEdit"), true);
    const UnrealMCP::FMCPResponse MapUpdated = Run(FSetBlueprintLocalVariableDefaultTool(), SetMap);
    if (MapUpdated.Error.IsSet()) AddError(MapUpdated.Error->Message);
    TestTrue(TEXT("Populated map local compiles"), MapUpdated.Result.IsValid() && MapUpdated.Result->GetBoolField(TEXT("compileSucceeded")));
    MapLocal = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Function, TEXT("ClassByChannel"));
    TestTrue(TEXT("Four map entries stored"), MapLocal && MapLocal->DefaultValue == MapDefault);

    TSharedRef<FJsonObject> Remove = Params();
    Remove->SetStringField(TEXT("variableName"), TEXT("Count"));
    TestTrue(TEXT("Remove without confirm is refused"), Run(FRemoveBlueprintLocalVariableTool(), Remove).Error.IsSet());
    Remove->SetBoolField(TEXT("confirm"), true);
    const UnrealMCP::FMCPResponse Removed = Run(FRemoveBlueprintLocalVariableTool(), Remove);
    if (Removed.Error.IsSet()) AddError(Removed.Error->Message);
    TestEqual(TEXT("Only map local after remove"), Count(), 1);
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
