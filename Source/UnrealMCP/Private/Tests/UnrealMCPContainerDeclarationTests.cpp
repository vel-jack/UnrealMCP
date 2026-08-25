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
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tools/AddBlueprintFunctionParameterTool.h"
#include "Tools/AddBlueprintVariableTool.h"

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

    const FBPVariableDescription* FindVariable(const UBlueprint* Blueprint, const FName Name)
    {
        return Blueprint != nullptr ? Blueprint->NewVariables.FindByPredicate([Name](const FBPVariableDescription& Variable)
        {
            return Variable.VarName == Name;
        }) : nullptr;
    }

    UEdGraphPin* FindPin(const UEdGraphNode* Node, const FName Name)
    {
        if (Node == nullptr) return nullptr;
        UEdGraphPin* const* Match = Node->Pins.FindByPredicate([Name](const UEdGraphPin* Pin)
        {
            return Pin != nullptr && Pin->PinName == Name;
        });
        return Match != nullptr ? *Match : nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPContainerDeclarationsLiveTest,
    "UnrealMCP.Blueprint.Authoring.ContainerDeclarations.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealMCPContainerDeclarationsLiveTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintName = FString::Printf(TEXT("BP_Declarations_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), TestRoot, *BlueprintName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);
    UBlueprint* Blueprint = CreateActorBlueprintFixture(PackagePath);
    ON_SCOPE_EXIT
    {
        if (Blueprint != nullptr) ObjectTools::DeleteObjectsUnchecked({Blueprint});
        if (UPackage* Package = FindPackage(nullptr, *PackagePath)) Package->SetDirtyFlag(false);
    };
    if (!TestNotNull(TEXT("Temporary Actor Blueprint created"), Blueprint)) return false;

    const FAddBlueprintVariableTool VariableTool;
    UnrealMCP::FMCPRequest SetVariableRequest;
    SetVariableRequest.Id = TEXT("set-variable");
    SetVariableRequest.Params = MakeShared<FJsonObject>();
    SetVariableRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    SetVariableRequest.Params->SetStringField(TEXT("variableName"), TEXT("SelectedActors"));
    SetVariableRequest.Params->SetStringField(TEXT("type"), TEXT("object"));
    SetVariableRequest.Params->SetStringField(TEXT("typeObjectPath"), AActor::StaticClass()->GetPathName());
    SetVariableRequest.Params->SetStringField(TEXT("containerType"), TEXT("set"));
    const UnrealMCP::FMCPResponse SetVariableResponse = VariableTool.Execute(SetVariableRequest);
    TestFalse(TEXT("Set variable has no MCP error"), SetVariableResponse.Error.IsSet());

    UnrealMCP::FMCPRequest MapVariableRequest;
    MapVariableRequest.Id = TEXT("map-variable");
    MapVariableRequest.Params = MakeShared<FJsonObject>();
    MapVariableRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    MapVariableRequest.Params->SetStringField(TEXT("variableName"), TEXT("ActorsByName"));
    MapVariableRequest.Params->SetStringField(TEXT("type"), TEXT("name"));
    MapVariableRequest.Params->SetStringField(TEXT("containerType"), TEXT("map"));
    MapVariableRequest.Params->SetStringField(TEXT("valueType"), TEXT("object"));
    MapVariableRequest.Params->SetStringField(TEXT("valueTypeObjectPath"), AActor::StaticClass()->GetPathName());
    const UnrealMCP::FMCPResponse MapVariableResponse = VariableTool.Execute(MapVariableRequest);
    TestFalse(TEXT("Map variable has no MCP error"), MapVariableResponse.Error.IsSet());

    const FBPVariableDescription* SetVariable = FindVariable(Blueprint, TEXT("SelectedActors"));
    const FBPVariableDescription* MapVariable = FindVariable(Blueprint, TEXT("ActorsByName"));
    TestNotNull(TEXT("Set variable exists"), SetVariable);
    TestNotNull(TEXT("Map variable exists"), MapVariable);
    if (SetVariable != nullptr)
    {
        TestTrue(TEXT("Set variable has Set container"), SetVariable->VarType.ContainerType == EPinContainerType::Set);
        TestTrue(TEXT("Set variable element is Actor"), SetVariable->VarType.PinSubCategoryObject == AActor::StaticClass());
    }
    if (MapVariable != nullptr)
    {
        TestTrue(TEXT("Map variable has Map container"), MapVariable->VarType.ContainerType == EPinContainerType::Map);
        TestEqual(TEXT("Map key is Name"), MapVariable->VarType.PinCategory, UEdGraphSchema_K2::PC_Name);
        TestEqual(TEXT("Map value is Object"),
            MapVariable->VarType.PinValueType.TerminalCategory, UEdGraphSchema_K2::PC_Object);
        TestTrue(TEXT("Map value is Actor"),
            MapVariable->VarType.PinValueType.TerminalSubCategoryObject == AActor::StaticClass());
    }

    UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, TEXT("ContainerFunction"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FunctionGraph, true, nullptr);
    const FString GraphGuid = FunctionGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
    const FAddBlueprintFunctionParameterTool ParameterTool;

    UnrealMCP::FMCPRequest SetParameterRequest;
    SetParameterRequest.Id = TEXT("set-parameter");
    SetParameterRequest.Params = MakeShared<FJsonObject>();
    SetParameterRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    SetParameterRequest.Params->SetStringField(TEXT("graphGuid"), GraphGuid);
    SetParameterRequest.Params->SetStringField(TEXT("parameterName"), TEXT("InputActors"));
    SetParameterRequest.Params->SetStringField(TEXT("direction"), TEXT("input"));
    SetParameterRequest.Params->SetStringField(TEXT("type"), TEXT("object"));
    SetParameterRequest.Params->SetStringField(TEXT("typeObjectPath"), AActor::StaticClass()->GetPathName());
    SetParameterRequest.Params->SetStringField(TEXT("containerType"), TEXT("set"));
    const UnrealMCP::FMCPResponse SetParameterResponse = ParameterTool.Execute(SetParameterRequest);
    TestFalse(TEXT("Set parameter has no MCP error"), SetParameterResponse.Error.IsSet());

    UnrealMCP::FMCPRequest MapParameterRequest;
    MapParameterRequest.Id = TEXT("map-parameter");
    MapParameterRequest.Params = MakeShared<FJsonObject>();
    MapParameterRequest.Params->SetStringField(TEXT("objectPath"), ObjectPath);
    MapParameterRequest.Params->SetStringField(TEXT("graphGuid"), GraphGuid);
    MapParameterRequest.Params->SetStringField(TEXT("parameterName"), TEXT("OutputActors"));
    MapParameterRequest.Params->SetStringField(TEXT("direction"), TEXT("output"));
    MapParameterRequest.Params->SetStringField(TEXT("type"), TEXT("name"));
    MapParameterRequest.Params->SetStringField(TEXT("containerType"), TEXT("map"));
    MapParameterRequest.Params->SetStringField(TEXT("valueType"), TEXT("object"));
    MapParameterRequest.Params->SetStringField(TEXT("valueTypeObjectPath"), AActor::StaticClass()->GetPathName());
    const UnrealMCP::FMCPResponse MapParameterResponse = ParameterTool.Execute(MapParameterRequest);
    TestFalse(TEXT("Map parameter has no MCP error"), MapParameterResponse.Error.IsSet());

    TArray<UK2Node_FunctionEntry*> Entries;
    TArray<UK2Node_FunctionResult*> Results;
    FunctionGraph->GetNodesOfClass(Entries);
    FunctionGraph->GetNodesOfClass(Results);
    UEdGraphPin* SetParameter = Entries.Num() == 1 ? FindPin(Entries[0], TEXT("InputActors")) : nullptr;
    UEdGraphPin* MapParameter = Results.Num() == 1 ? FindPin(Results[0], TEXT("OutputActors")) : nullptr;
    TestNotNull(TEXT("Set function parameter exists"), SetParameter);
    TestNotNull(TEXT("Map function parameter exists"), MapParameter);
    if (SetParameter != nullptr) TestTrue(TEXT("Function input is Set"), SetParameter->PinType.ContainerType == EPinContainerType::Set);
    if (MapParameter != nullptr) TestTrue(TEXT("Function output is Map"), MapParameter->PinType.ContainerType == EPinContainerType::Map);

    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    TestTrue(TEXT("Blueprint compiles with Set/Map variables and parameters"),
        Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
