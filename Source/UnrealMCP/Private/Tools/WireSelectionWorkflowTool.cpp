#include "Tools/WireSelectionWorkflowTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Tools/AddBlueprintArrayOperationNodeTool.h"
#include "Tools/AddBlueprintBranchNodeTool.h"
#include "Tools/AddBlueprintFunctionParameterTool.h"
#include "Tools/AddBlueprintSetOperationNodeTool.h"
#include "Tools/AddBlueprintVariableGetNodeTool.h"
#include "Tools/AddBlueprintVariableTool.h"
#include "EdGraph/EdGraph.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "Tools/ConnectBlueprintPinsTool.h"
#include "Tools/CreateBlueprintFunctionGraphTool.h"
#include "Tools/ListBlueprintNodePinsTool.h"

namespace
{
    using namespace UnrealMCP;

    // Composes already-shipped primitive tools by calling their Execute() directly, following the
    // precedent in PlanProjectRefactorTool.cpp, rather than re-implementing K2Node construction.
    struct FSubResult
    {
        bool bSuccess = false;
        TSharedPtr<FJsonObject> Json;
        FString Error;
    };

    FSubResult CallTool(const FMCPToolBase& Tool, const TSharedRef<FJsonObject>& Params)
    {
        FMCPRequest Request;
        Request.Id = TEXT("WireSelectionWorkflow.sub");
        Request.Params = Params;
        const FMCPResponse Response = Tool.Execute(Request);
        FSubResult Result;
        if (Response.Error.IsSet())
        {
            Result.Error = Response.Error->Message;
            return Result;
        }
        Result.bSuccess = true;
        Result.Json = Response.Result;
        return Result;
    }

    // CreateBlueprintFunctionGraph only reports that a graph with this name exists; it does not check
    // that the body matches. A previous run that failed partway leaves a graph with just its entry
    // (and result) node, so treating "exists" as "scaffolded" would silently report success for a
    // function that does nothing. Counting nodes against the minimum this workflow builds catches that.
    int32 CountGraphNodes(const FString& ObjectPath, const FString& GraphGuid, FString& OutError)
    {
        int32 NodeCount = INDEX_NONE;
        FString ExecutionError;
        BlueprintToolUtils::ExecuteOnGameThreadSync(
            [&](FString& InnerError)
            {
                UBlueprint* Blueprint = nullptr;
                if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, InnerError))
                {
                    return false;
                }
                UEdGraph* Graph = nullptr;
                if (!BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, FString(), GraphGuid, Graph, InnerError))
                {
                    return false;
                }
                NodeCount = Graph->Nodes.Num();
                return true;
            },
            ExecutionError);
        OutError = ExecutionError;
        return NodeCount;
    }

    TSharedRef<FJsonObject> MakeParams()
    {
        return MakeShared<FJsonObject>();
    }

    FString GetStr(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
    {
        FString Value;
        if (Json.IsValid())
        {
            Json->TryGetStringField(Field, Value);
        }
        return Value;
    }

    bool GetPinBool(const FJsonObject& Pin, const TCHAR* Field)
    {
        bool Value = false;
        Pin.TryGetBoolField(Field, Value);
        return Value;
    }

    FString GetPinStr(const FJsonObject& Pin, const TCHAR* Field)
    {
        FString Value;
        Pin.TryGetStringField(Field, Value);
        return Value;
    }

    FString FindPinName(const TArray<TSharedPtr<FJsonValue>>& Pins, TFunctionRef<bool(const FJsonObject&)> Predicate)
    {
        for (const TSharedPtr<FJsonValue>& Value : Pins)
        {
            const TSharedPtr<FJsonObject> Pin = Value.IsValid() ? Value->AsObject() : nullptr;
            if (Pin.IsValid() && Predicate(*Pin))
            {
                return GetPinStr(*Pin, TEXT("pinName"));
            }
        }
        return FString();
    }

    // Pin roles on a freshly created array/set operation node, discovered from its own response
    // rather than guessed from reflected UFunction parameter names.
    struct FOpPinNames
    {
        FString CollectionPin, ItemPin, ExecIn, ExecOut, ReturnValuePin;
    };

    FOpPinNames ExtractOpPins(const TArray<TSharedPtr<FJsonValue>>& Pins, const FString& ContainerKind)
    {
        FOpPinNames Names;
        Names.CollectionPin = FindPinName(Pins, [&](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == TEXT("input") && GetPinBool(P, TEXT("isData"))
                && GetPinStr(P, TEXT("containerType")).Equals(ContainerKind, ESearchCase::IgnoreCase);
        });
        Names.ItemPin = FindPinName(Pins, [&](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == TEXT("input") && GetPinBool(P, TEXT("isData"))
                && GetPinStr(P, TEXT("containerType")).Equals(TEXT("None"), ESearchCase::IgnoreCase);
        });
        Names.ExecIn = FindPinName(Pins, [](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == TEXT("input") && GetPinBool(P, TEXT("isExec"));
        });
        Names.ExecOut = FindPinName(Pins, [](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == TEXT("output") && GetPinBool(P, TEXT("isExec"));
        });
        Names.ReturnValuePin = FindPinName(Pins, [](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == TEXT("output") && GetPinBool(P, TEXT("isData"));
        });
        return Names;
    }

    bool Connect(const FString& ObjectPath, const FString& GraphGuid,
        const FString& SourceNodeGuid, const FString& SourcePinName,
        const FString& TargetNodeGuid, const FString& TargetPinName,
        FString& OutError)
    {
        const FConnectBlueprintPinsTool Tool;
        TSharedRef<FJsonObject> Params = MakeParams();
        Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Params->SetStringField(TEXT("graphGuid"), GraphGuid);
        Params->SetStringField(TEXT("sourceNodeGuid"), SourceNodeGuid);
        Params->SetStringField(TEXT("sourcePinName"), SourcePinName);
        Params->SetStringField(TEXT("targetNodeGuid"), TargetNodeGuid);
        Params->SetStringField(TEXT("targetPinName"), TargetPinName);
        const FSubResult Result = CallTool(Tool, Params);
        if (!Result.bSuccess)
        {
            OutError = FString::Printf(TEXT("Could not connect %s.%s to %s.%s: %s"),
                *SourceNodeGuid, *SourcePinName, *TargetNodeGuid, *TargetPinName, *Result.Error);
        }
        return Result.bSuccess;
    }

    TArray<TSharedPtr<FJsonValue>> GetPinsArray(const TSharedPtr<FJsonObject>& Json)
    {
        const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
        if (Json.IsValid() && Json->TryGetArrayField(TEXT("pins"), Pins) && Pins)
        {
            return *Pins;
        }
        return {};
    }

    FString FindExecOutputPinOnNode(const FString& ObjectPath, const FString& GraphGuid, const FString& NodeGuid, bool bWantInput, FString& OutError)
    {
        const FListBlueprintNodePinsTool Tool;
        TSharedRef<FJsonObject> Params = MakeParams();
        Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Params->SetStringField(TEXT("graphGuid"), GraphGuid);
        Params->SetStringField(TEXT("nodeGuid"), NodeGuid);
        const FSubResult Result = CallTool(Tool, Params);
        if (!Result.bSuccess)
        {
            OutError = Result.Error;
            return FString();
        }
        const TArray<TSharedPtr<FJsonValue>> Pins = GetPinsArray(Result.Json);
        const FString WantDirection = bWantInput ? TEXT("input") : TEXT("output");
        const FString Name = FindPinName(Pins, [&](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == WantDirection && GetPinStr(P, TEXT("category")) == TEXT("exec");
        });
        if (Name.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Could not find an exec %s pin on node %s."), *WantDirection, *NodeGuid);
        }
        return Name;
    }

    // Body builders. Each assumes the owning function graph and its Target/return parameters (if any)
    // already exist and is only responsible for the operation nodes and their wiring.

    bool BuildClearBody(const FString& ObjectPath, const FString& GraphGuid, const FString& EntryNodeGuid,
        const FString& EntryThenPin, const FString& CollectionVariableName, const FString& CollectionType,
        const FString& ElementTypeObjectPath, int32& OutNodeCount, int32& OutConnectionCount, FString& OutError)
    {
        const FAddBlueprintVariableGetNodeTool GetTool;
        TSharedRef<FJsonObject> GetParams = MakeParams();
        GetParams->SetStringField(TEXT("objectPath"), ObjectPath);
        GetParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        GetParams->SetStringField(TEXT("variableName"), CollectionVariableName);
        const FSubResult GetResult = CallTool(GetTool, GetParams);
        if (!GetResult.bSuccess) { OutError = GetResult.Error; return false; }
        const FString GetNodeGuid = GetStr(GetResult.Json, TEXT("nodeGuid"));
        ++OutNodeCount;

        TSharedRef<FJsonObject> ClearParams = MakeParams();
        ClearParams->SetStringField(TEXT("objectPath"), ObjectPath);
        ClearParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        ClearParams->SetStringField(TEXT("operation"), TEXT("Clear"));
        ClearParams->SetStringField(TEXT("elementType"), TEXT("object"));
        ClearParams->SetStringField(TEXT("typeObjectPath"), ElementTypeObjectPath);
        const FSubResult ClearResult = CollectionType == TEXT("set")
            ? CallTool(FAddBlueprintSetOperationNodeTool(), ClearParams)
            : CallTool(FAddBlueprintArrayOperationNodeTool(), ClearParams);
        if (!ClearResult.bSuccess) { OutError = ClearResult.Error; return false; }
        const FString ClearNodeGuid = GetStr(ClearResult.Json, TEXT("nodeGuid"));
        const FOpPinNames ClearPins = ExtractOpPins(GetPinsArray(ClearResult.Json), CollectionType == TEXT("set") ? TEXT("Set") : TEXT("Array"));
        ++OutNodeCount;

        if (!Connect(ObjectPath, GraphGuid, EntryNodeGuid, EntryThenPin, ClearNodeGuid, ClearPins.ExecIn, OutError)) return false;
        ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, GetNodeGuid, CollectionVariableName, ClearNodeGuid, ClearPins.CollectionPin, OutError)) return false;
        ++OutConnectionCount;
        return true;
    }

    bool BuildSelectOneBody(const FString& ObjectPath, const FString& GraphGuid, const FString& EntryNodeGuid,
        const FString& EntryThenPin, const FString& TargetPinId, const FString& CollectionVariableName,
        const FString& CollectionType, const FString& ElementTypeObjectPath, int32& OutNodeCount, int32& OutConnectionCount, FString& OutError)
    {
        const FString AddOperation = CollectionType == TEXT("set") ? TEXT("Add") : TEXT("AddUnique");
        const FString ContainerKind = CollectionType == TEXT("set") ? TEXT("Set") : TEXT("Array");

        TSharedRef<FJsonObject> GetParams = MakeParams();
        GetParams->SetStringField(TEXT("objectPath"), ObjectPath);
        GetParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        GetParams->SetStringField(TEXT("variableName"), CollectionVariableName);
        const FSubResult GetResult = CallTool(FAddBlueprintVariableGetNodeTool(), GetParams);
        if (!GetResult.bSuccess) { OutError = GetResult.Error; return false; }
        const FString GetNodeGuid = GetStr(GetResult.Json, TEXT("nodeGuid"));
        ++OutNodeCount;

        TSharedRef<FJsonObject> ClearParams = MakeParams();
        ClearParams->SetStringField(TEXT("objectPath"), ObjectPath);
        ClearParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        ClearParams->SetStringField(TEXT("operation"), TEXT("Clear"));
        ClearParams->SetStringField(TEXT("elementType"), TEXT("object"));
        ClearParams->SetStringField(TEXT("typeObjectPath"), ElementTypeObjectPath);
        const FSubResult ClearResult = CollectionType == TEXT("set")
            ? CallTool(FAddBlueprintSetOperationNodeTool(), ClearParams)
            : CallTool(FAddBlueprintArrayOperationNodeTool(), ClearParams);
        if (!ClearResult.bSuccess) { OutError = ClearResult.Error; return false; }
        const FString ClearNodeGuid = GetStr(ClearResult.Json, TEXT("nodeGuid"));
        const FOpPinNames ClearPins = ExtractOpPins(GetPinsArray(ClearResult.Json), ContainerKind);
        ++OutNodeCount;

        TSharedRef<FJsonObject> AddParams = MakeParams();
        AddParams->SetStringField(TEXT("objectPath"), ObjectPath);
        AddParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        AddParams->SetStringField(TEXT("operation"), AddOperation);
        AddParams->SetStringField(TEXT("elementType"), TEXT("object"));
        AddParams->SetStringField(TEXT("typeObjectPath"), ElementTypeObjectPath);
        const FSubResult AddResult = CollectionType == TEXT("set")
            ? CallTool(FAddBlueprintSetOperationNodeTool(), AddParams)
            : CallTool(FAddBlueprintArrayOperationNodeTool(), AddParams);
        if (!AddResult.bSuccess) { OutError = AddResult.Error; return false; }
        const FString AddNodeGuid = GetStr(AddResult.Json, TEXT("nodeGuid"));
        const FOpPinNames AddPins = ExtractOpPins(GetPinsArray(AddResult.Json), ContainerKind);
        ++OutNodeCount;

        if (!Connect(ObjectPath, GraphGuid, EntryNodeGuid, EntryThenPin, ClearNodeGuid, ClearPins.ExecIn, OutError)) return false;
        ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, ClearNodeGuid, ClearPins.ExecOut, AddNodeGuid, AddPins.ExecIn, OutError)) return false;
        ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, GetNodeGuid, CollectionVariableName, ClearNodeGuid, ClearPins.CollectionPin, OutError)) return false;
        ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, GetNodeGuid, CollectionVariableName, AddNodeGuid, AddPins.CollectionPin, OutError)) return false;
        ++OutConnectionCount;

        const FConnectBlueprintPinsTool ConnectTool;
        TSharedRef<FJsonObject> TargetConnectParams = MakeParams();
        TargetConnectParams->SetStringField(TEXT("objectPath"), ObjectPath);
        TargetConnectParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        TargetConnectParams->SetStringField(TEXT("sourceNodeGuid"), EntryNodeGuid);
        TargetConnectParams->SetStringField(TEXT("sourcePinId"), TargetPinId);
        TargetConnectParams->SetStringField(TEXT("targetNodeGuid"), AddNodeGuid);
        TargetConnectParams->SetStringField(TEXT("targetPinName"), AddPins.ItemPin);
        const FSubResult TargetConnectResult = CallTool(ConnectTool, TargetConnectParams);
        if (!TargetConnectResult.bSuccess) { OutError = TargetConnectResult.Error; return false; }
        ++OutConnectionCount;
        return true;
    }

    bool BuildToggleBody(const FString& ObjectPath, const FString& GraphGuid, const FString& EntryNodeGuid,
        const FString& EntryThenPin, const FString& TargetPinId, const FString& CollectionVariableName,
        const FString& CollectionType, const FString& ElementTypeObjectPath, int32& OutNodeCount, int32& OutConnectionCount, FString& OutError)
    {
        const FString AddOperation = CollectionType == TEXT("set") ? TEXT("Add") : TEXT("AddUnique");
        const FString RemoveOperation = CollectionType == TEXT("set") ? TEXT("Remove") : TEXT("RemoveItem");
        const FString ContainerKind = CollectionType == TEXT("set") ? TEXT("Set") : TEXT("Array");

        auto MakeOpNode = [&](const FString& Operation, FString& OutNodeGuid, FOpPinNames& OutPins) -> bool
        {
            TSharedRef<FJsonObject> Params = MakeParams();
            Params->SetStringField(TEXT("objectPath"), ObjectPath);
            Params->SetStringField(TEXT("graphGuid"), GraphGuid);
            Params->SetStringField(TEXT("operation"), Operation);
            Params->SetStringField(TEXT("elementType"), TEXT("object"));
            Params->SetStringField(TEXT("typeObjectPath"), ElementTypeObjectPath);
            const FSubResult Result = CollectionType == TEXT("set")
                ? CallTool(FAddBlueprintSetOperationNodeTool(), Params)
                : CallTool(FAddBlueprintArrayOperationNodeTool(), Params);
            if (!Result.bSuccess) { OutError = Result.Error; return false; }
            OutNodeGuid = GetStr(Result.Json, TEXT("nodeGuid"));
            OutPins = ExtractOpPins(GetPinsArray(Result.Json), ContainerKind);
            ++OutNodeCount;
            return true;
        };

        TSharedRef<FJsonObject> GetParams = MakeParams();
        GetParams->SetStringField(TEXT("objectPath"), ObjectPath);
        GetParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        GetParams->SetStringField(TEXT("variableName"), CollectionVariableName);
        const FSubResult GetResult = CallTool(FAddBlueprintVariableGetNodeTool(), GetParams);
        if (!GetResult.bSuccess) { OutError = GetResult.Error; return false; }
        const FString GetNodeGuid = GetStr(GetResult.Json, TEXT("nodeGuid"));
        ++OutNodeCount;

        FString ContainsNodeGuid; FOpPinNames ContainsPins;
        if (!MakeOpNode(TEXT("Contains"), ContainsNodeGuid, ContainsPins)) return false;
        FString RemoveNodeGuid; FOpPinNames RemovePins;
        if (!MakeOpNode(RemoveOperation, RemoveNodeGuid, RemovePins)) return false;
        FString AddNodeGuid; FOpPinNames AddPins;
        if (!MakeOpNode(AddOperation, AddNodeGuid, AddPins)) return false;

        const FSubResult BranchResult = CallTool(FAddBlueprintBranchNodeTool(), [&]
        {
            TSharedRef<FJsonObject> Params = MakeParams();
            Params->SetStringField(TEXT("objectPath"), ObjectPath);
            Params->SetStringField(TEXT("graphGuid"), GraphGuid);
            return Params;
        }());
        if (!BranchResult.bSuccess) { OutError = BranchResult.Error; return false; }
        const FString BranchNodeGuid = GetStr(BranchResult.Json, TEXT("nodeGuid"));
        const TArray<TSharedPtr<FJsonValue>> BranchPins = GetPinsArray(BranchResult.Json);
        const FString BranchExecIn = FindPinName(BranchPins, [](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == TEXT("input") && GetPinBool(P, TEXT("isExec"));
        });
        const FString BranchCondition = FindPinName(BranchPins, [](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == TEXT("input") && GetPinBool(P, TEXT("isData"));
        });
        const FString BranchThen = FindPinName(BranchPins, [](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == TEXT("output") && GetPinBool(P, TEXT("isExec")) && GetPinStr(P, TEXT("pinName")).Contains(TEXT("then"), ESearchCase::IgnoreCase);
        });
        const FString BranchElse = FindPinName(BranchPins, [](const FJsonObject& P)
        {
            return GetPinStr(P, TEXT("direction")) == TEXT("output") && GetPinBool(P, TEXT("isExec")) && GetPinStr(P, TEXT("pinName")).Contains(TEXT("else"), ESearchCase::IgnoreCase);
        });
        if (BranchExecIn.IsEmpty() || BranchCondition.IsEmpty() || BranchThen.IsEmpty() || BranchElse.IsEmpty())
        {
            OutError = TEXT("Could not resolve Branch node pins.");
            return false;
        }
        ++OutNodeCount;

        if (!Connect(ObjectPath, GraphGuid, EntryNodeGuid, EntryThenPin, BranchNodeGuid, BranchExecIn, OutError)) return false; ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, ContainsNodeGuid, ContainsPins.ReturnValuePin, BranchNodeGuid, BranchCondition, OutError)) return false; ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, BranchNodeGuid, BranchThen, RemoveNodeGuid, RemovePins.ExecIn, OutError)) return false; ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, BranchNodeGuid, BranchElse, AddNodeGuid, AddPins.ExecIn, OutError)) return false; ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, GetNodeGuid, CollectionVariableName, ContainsNodeGuid, ContainsPins.CollectionPin, OutError)) return false; ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, GetNodeGuid, CollectionVariableName, RemoveNodeGuid, RemovePins.CollectionPin, OutError)) return false; ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, GetNodeGuid, CollectionVariableName, AddNodeGuid, AddPins.CollectionPin, OutError)) return false; ++OutConnectionCount;

        const FConnectBlueprintPinsTool ConnectTool;
        auto ConnectTargetById = [&](const FString& TargetNodeGuid, const FString& TargetPinName) -> bool
        {
            TSharedRef<FJsonObject> Params = MakeParams();
            Params->SetStringField(TEXT("objectPath"), ObjectPath);
            Params->SetStringField(TEXT("graphGuid"), GraphGuid);
            Params->SetStringField(TEXT("sourceNodeGuid"), EntryNodeGuid);
            Params->SetStringField(TEXT("sourcePinId"), TargetPinId);
            Params->SetStringField(TEXT("targetNodeGuid"), TargetNodeGuid);
            Params->SetStringField(TEXT("targetPinName"), TargetPinName);
            const FSubResult Result = CallTool(ConnectTool, Params);
            if (!Result.bSuccess) { OutError = Result.Error; return false; }
            ++OutConnectionCount;
            return true;
        };
        if (!ConnectTargetById(ContainsNodeGuid, ContainsPins.ItemPin)) return false;
        if (!ConnectTargetById(RemoveNodeGuid, RemovePins.ItemPin)) return false;
        if (!ConnectTargetById(AddNodeGuid, AddPins.ItemPin)) return false;
        return true;
    }

    bool BuildContainsBody(const FString& ObjectPath, const FString& GraphGuid, const FString& EntryNodeGuid,
        const FString& EntryThenPin, const FString& TargetPinId, const FString& ResultNodeGuid, const FString& ResultPinId,
        const FString& CollectionVariableName, const FString& CollectionType, const FString& ElementTypeObjectPath,
        int32& OutNodeCount, int32& OutConnectionCount, FString& OutError)
    {
        const FString ContainerKind = CollectionType == TEXT("set") ? TEXT("Set") : TEXT("Array");

        TSharedRef<FJsonObject> GetParams = MakeParams();
        GetParams->SetStringField(TEXT("objectPath"), ObjectPath);
        GetParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        GetParams->SetStringField(TEXT("variableName"), CollectionVariableName);
        const FSubResult GetResult = CallTool(FAddBlueprintVariableGetNodeTool(), GetParams);
        if (!GetResult.bSuccess) { OutError = GetResult.Error; return false; }
        const FString GetNodeGuid = GetStr(GetResult.Json, TEXT("nodeGuid"));
        ++OutNodeCount;

        TSharedRef<FJsonObject> ContainsParams = MakeParams();
        ContainsParams->SetStringField(TEXT("objectPath"), ObjectPath);
        ContainsParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        ContainsParams->SetStringField(TEXT("operation"), TEXT("Contains"));
        ContainsParams->SetStringField(TEXT("elementType"), TEXT("object"));
        ContainsParams->SetStringField(TEXT("typeObjectPath"), ElementTypeObjectPath);
        const FSubResult ContainsResult = CollectionType == TEXT("set")
            ? CallTool(FAddBlueprintSetOperationNodeTool(), ContainsParams)
            : CallTool(FAddBlueprintArrayOperationNodeTool(), ContainsParams);
        if (!ContainsResult.bSuccess) { OutError = ContainsResult.Error; return false; }
        const FString ContainsNodeGuid = GetStr(ContainsResult.Json, TEXT("nodeGuid"));
        const FOpPinNames ContainsPins = ExtractOpPins(GetPinsArray(ContainsResult.Json), ContainerKind);
        ++OutNodeCount;

        // Entry.then connects directly to the Return node's exec pin, since Contains is pure and there is no impure work.
        FString ResultExecError;
        const FString ResultExecIn = FindExecOutputPinOnNode(ObjectPath, GraphGuid, ResultNodeGuid, true, ResultExecError);
        if (ResultExecIn.IsEmpty()) { OutError = ResultExecError; return false; }

        if (!Connect(ObjectPath, GraphGuid, EntryNodeGuid, EntryThenPin, ResultNodeGuid, ResultExecIn, OutError)) return false;
        ++OutConnectionCount;
        if (!Connect(ObjectPath, GraphGuid, GetNodeGuid, CollectionVariableName, ContainsNodeGuid, ContainsPins.CollectionPin, OutError)) return false;
        ++OutConnectionCount;

        const FConnectBlueprintPinsTool ConnectTool;
        TSharedRef<FJsonObject> ItemConnectParams = MakeParams();
        ItemConnectParams->SetStringField(TEXT("objectPath"), ObjectPath);
        ItemConnectParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        ItemConnectParams->SetStringField(TEXT("sourceNodeGuid"), EntryNodeGuid);
        ItemConnectParams->SetStringField(TEXT("sourcePinId"), TargetPinId);
        ItemConnectParams->SetStringField(TEXT("targetNodeGuid"), ContainsNodeGuid);
        ItemConnectParams->SetStringField(TEXT("targetPinName"), ContainsPins.ItemPin);
        const FSubResult ItemConnectResult = CallTool(ConnectTool, ItemConnectParams);
        if (!ItemConnectResult.bSuccess) { OutError = ItemConnectResult.Error; return false; }
        ++OutConnectionCount;

        TSharedRef<FJsonObject> ReturnConnectParams = MakeParams();
        ReturnConnectParams->SetStringField(TEXT("objectPath"), ObjectPath);
        ReturnConnectParams->SetStringField(TEXT("graphGuid"), GraphGuid);
        ReturnConnectParams->SetStringField(TEXT("sourceNodeGuid"), ContainsNodeGuid);
        ReturnConnectParams->SetStringField(TEXT("sourcePinName"), ContainsPins.ReturnValuePin);
        ReturnConnectParams->SetStringField(TEXT("targetNodeGuid"), ResultNodeGuid);
        ReturnConnectParams->SetStringField(TEXT("targetPinId"), ResultPinId);
        const FSubResult ReturnConnectResult = CallTool(ConnectTool, ReturnConnectParams);
        if (!ReturnConnectResult.bSuccess) { OutError = ReturnConnectResult.Error; return false; }
        ++OutConnectionCount;
        return true;
    }
}

FWireSelectionWorkflowTool::FWireSelectionWorkflowTool()
    : FMCPToolBase(
        TEXT("WireSelectionWorkflow"),
        TEXT("Idempotently scaffolds four collection-backed helper functions (SelectOne, ToggleSelection, ClearSelection, ContainsSelection) over a named array/set-of-object selection variable, composed from existing Blueprint authoring primitives."))
{
}

UnrealMCP::FMCPResponse FWireSelectionWorkflowTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString ObjectPath, ElementTypeObjectPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("elementTypeObjectPath"), ElementTypeObjectPath)
        || ObjectPath.IsEmpty() || ElementTypeObjectPath.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("WireSelectionWorkflow requires objectPath and elementTypeObjectPath."));
    }

    FString SelectionVariableName, CollectionType, SelectOneName, ToggleName, ClearName, ContainsName;
    Request.Params->TryGetStringField(TEXT("selectionVariableName"), SelectionVariableName);
    Request.Params->TryGetStringField(TEXT("collectionType"), CollectionType);
    Request.Params->TryGetStringField(TEXT("selectOneFunctionName"), SelectOneName);
    Request.Params->TryGetStringField(TEXT("toggleSelectionFunctionName"), ToggleName);
    Request.Params->TryGetStringField(TEXT("clearSelectionFunctionName"), ClearName);
    Request.Params->TryGetStringField(TEXT("containsSelectionFunctionName"), ContainsName);
    if (SelectionVariableName.IsEmpty()) SelectionVariableName = TEXT("SelectedActors");
    CollectionType = CollectionType.IsEmpty() ? TEXT("array") : CollectionType.ToLower();
    if (CollectionType != TEXT("array") && CollectionType != TEXT("set"))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("collectionType must be 'array' or 'set'."));
    }
    if (SelectOneName.IsEmpty()) SelectOneName = TEXT("SelectOne");
    if (ToggleName.IsEmpty()) ToggleName = TEXT("ToggleSelection");
    if (ClearName.IsEmpty()) ClearName = TEXT("ClearSelection");
    if (ContainsName.IsEmpty()) ContainsName = TEXT("ContainsSelection");

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompileAfterEdit = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool bSaveAfterEdit = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), true);

    if (bDryRun)
    {
        bool bVariableExists = false;
        TArray<TSharedPtr<FJsonValue>> FunctionPlan;
        FString ExecutionError;
        const bool bResolved = BlueprintToolUtils::ExecuteOnGameThreadSync(
            [&](FString& OutError)
            {
                UBlueprint* Blueprint = nullptr;
                if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
                {
                    return false;
                }
                for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
                {
                    if (Variable.VarName.ToString().Equals(SelectionVariableName, ESearchCase::IgnoreCase)) { bVariableExists = true; break; }
                }
                for (const TCHAR* FunctionName : { *SelectOneName, *ToggleName, *ClearName, *ContainsName })
                {
                    bool bExists = false;
                    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
                    {
                        if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase)) { bExists = true; break; }
                    }
                    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                    Item->SetStringField(TEXT("functionName"), FunctionName);
                    Item->SetBoolField(TEXT("alreadyScaffolded"), bExists);
                    Item->SetStringField(TEXT("plannedAction"), bExists ? TEXT("skip (already exists)") : TEXT("create function graph, parameters, and body"));
                    FunctionPlan.Add(MakeShared<FJsonValueObject>(Item));
                }
                return true;
            },
            ExecutionError);
        if (!bResolved)
        {
            return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);
        }

        FMCPResponse Response;
        Response.Id = Request.Id;
        TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
        Result->SetBoolField(TEXT("dryRun"), true);
        Result->SetStringField(TEXT("objectPath"), ObjectPath);
        Result->SetStringField(TEXT("selectionVariableName"), SelectionVariableName);
        Result->SetStringField(TEXT("collectionType"), CollectionType);
        Result->SetBoolField(TEXT("selectionVariableAlreadyExists"), bVariableExists);
        Result->SetArrayField(TEXT("functions"), FunctionPlan);
        Result->SetStringField(TEXT("note"), TEXT("This is a structural plan, not a byte-exact pin preview: chaining several primitive tools together cannot be dry-run at pin level. Nothing was mutated."));
        Response.Result = Result;
        return Response;
    }

    TSharedRef<FJsonObject> VariableParams = MakeParams();
    VariableParams->SetStringField(TEXT("objectPath"), ObjectPath);
    VariableParams->SetStringField(TEXT("variableName"), SelectionVariableName);
    VariableParams->SetStringField(TEXT("type"), TEXT("object"));
    VariableParams->SetStringField(TEXT("typeObjectPath"), ElementTypeObjectPath);
    VariableParams->SetStringField(TEXT("containerType"), CollectionType);
    const FSubResult VariableResult = CallTool(FAddBlueprintVariableTool(), VariableParams);
    if (!VariableResult.bSuccess)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, FString::Printf(TEXT("Could not ensure the selection variable: %s"), *VariableResult.Error));
    }

    struct FFunctionSpec { FString Name; bool bHasTarget; bool bHasBoolReturn; int32 Kind; };
    enum { Kind_Clear, Kind_SelectOne, Kind_Toggle, Kind_Contains };
    // Smallest node count each body below actually produces, entry/result nodes included.
    auto MinimumScaffoldedNodeCount = [](int32 Kind) -> int32
    {
        switch (Kind)
        {
        case Kind_Clear: return 3;      // entry + collection get + Clear
        case Kind_SelectOne: return 4;  // entry + get + Clear + Add
        case Kind_Toggle: return 6;     // entry + get + Contains + Branch + Add + Remove
        case Kind_Contains: return 4;   // entry + result + get + Contains
        default: return 2;
        }
    };
    const TArray<FFunctionSpec> Specs = {
        { ClearName, false, false, Kind_Clear },
        { SelectOneName, true, false, Kind_SelectOne },
        { ToggleName, true, false, Kind_Toggle },
        { ContainsName, true, true, Kind_Contains },
    };

    TArray<TSharedPtr<FJsonValue>> FunctionResults;
    bool bChanged = false;
    bool bAnyFailed = false;
    FString FirstError;

    for (const FFunctionSpec& Spec : Specs)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("functionName"), Spec.Name);

        if (bAnyFailed)
        {
            Item->SetBoolField(TEXT("attempted"), false);
            FunctionResults.Add(MakeShared<FJsonValueObject>(Item));
            continue;
        }

        TSharedRef<FJsonObject> GraphParams = MakeParams();
        GraphParams->SetStringField(TEXT("objectPath"), ObjectPath);
        GraphParams->SetStringField(TEXT("functionName"), Spec.Name);
        const FSubResult GraphResult = CallTool(FCreateBlueprintFunctionGraphTool(), GraphParams);
        if (!GraphResult.bSuccess)
        {
            Item->SetStringField(TEXT("error"), GraphResult.Error);
            FunctionResults.Add(MakeShared<FJsonValueObject>(Item));
            bAnyFailed = true; FirstError = GraphResult.Error;
            continue;
        }
        const FString GraphGuid = GetStr(GraphResult.Json, TEXT("graphGuid"));
        bool bAlreadyExists = false;
        GraphResult.Json->TryGetBoolField(TEXT("alreadyExists"), bAlreadyExists);
        Item->SetStringField(TEXT("graphGuid"), GraphGuid);
        Item->SetBoolField(TEXT("alreadyScaffolded"), bAlreadyExists);

        if (bAlreadyExists)
        {
            // Verify the existing graph structurally instead of trusting the name. An incomplete
            // leftover from an earlier failed run must not be reported as successful scaffolding.
            FString CountError;
            const int32 ExistingNodeCount = CountGraphNodes(ObjectPath, GraphGuid, CountError);
            const int32 MinimumNodeCount = MinimumScaffoldedNodeCount(Spec.Kind);
            const bool bStructurallyComplete = ExistingNodeCount >= MinimumNodeCount;
            Item->SetNumberField(TEXT("existingNodeCount"), ExistingNodeCount);
            Item->SetNumberField(TEXT("expectedMinimumNodeCount"), MinimumNodeCount);
            Item->SetBoolField(TEXT("structurallyComplete"), bStructurallyComplete);

            if (ExistingNodeCount == INDEX_NONE)
            {
                Item->SetStringField(TEXT("error"), CountError.IsEmpty()
                    ? TEXT("A function graph with this name exists but could not be inspected, so it cannot be confirmed as scaffolded.")
                    : CountError);
                FunctionResults.Add(MakeShared<FJsonValueObject>(Item));
                bAnyFailed = true; FirstError = Item->GetStringField(TEXT("error"));
                continue;
            }
            if (!bStructurallyComplete)
            {
                Item->SetStringField(TEXT("error"), FString::Printf(
                    TEXT("Function '%s' already exists but holds only %d node(s) where this workflow builds at least %d; it looks like an incomplete earlier attempt. Delete or finish it, then re-run. This tool will not overwrite an existing function body."),
                    *Spec.Name, ExistingNodeCount, MinimumNodeCount));
                FunctionResults.Add(MakeShared<FJsonValueObject>(Item));
                bAnyFailed = true; FirstError = Item->GetStringField(TEXT("error"));
                continue;
            }

            FunctionResults.Add(MakeShared<FJsonValueObject>(Item));
            continue;
        }

        const FString EntryNodeGuid = GetStr(GraphResult.Json, TEXT("entryNodeGuid"));
        FString TargetPinId, ResultNodeGuid, ResultPinId;

        if (Spec.bHasTarget)
        {
            TSharedRef<FJsonObject> ParamParams = MakeParams();
            ParamParams->SetStringField(TEXT("objectPath"), ObjectPath);
            ParamParams->SetStringField(TEXT("graphGuid"), GraphGuid);
            ParamParams->SetStringField(TEXT("parameterName"), TEXT("Target"));
            ParamParams->SetStringField(TEXT("direction"), TEXT("input"));
            ParamParams->SetStringField(TEXT("type"), TEXT("object"));
            ParamParams->SetStringField(TEXT("typeObjectPath"), ElementTypeObjectPath);
            const FSubResult ParamResult = CallTool(FAddBlueprintFunctionParameterTool(), ParamParams);
            if (!ParamResult.bSuccess)
            {
                Item->SetStringField(TEXT("error"), ParamResult.Error);
                FunctionResults.Add(MakeShared<FJsonValueObject>(Item));
                bAnyFailed = true; FirstError = ParamResult.Error;
                continue;
            }
            TargetPinId = GetStr(ParamResult.Json, TEXT("pinId"));
        }

        if (Spec.bHasBoolReturn)
        {
            TSharedRef<FJsonObject> ReturnParamParams = MakeParams();
            ReturnParamParams->SetStringField(TEXT("objectPath"), ObjectPath);
            ReturnParamParams->SetStringField(TEXT("graphGuid"), GraphGuid);
            ReturnParamParams->SetStringField(TEXT("parameterName"), TEXT("bContainsSelection"));
            ReturnParamParams->SetStringField(TEXT("direction"), TEXT("output"));
            ReturnParamParams->SetStringField(TEXT("type"), TEXT("bool"));
            const FSubResult ReturnParamResult = CallTool(FAddBlueprintFunctionParameterTool(), ReturnParamParams);
            if (!ReturnParamResult.bSuccess)
            {
                Item->SetStringField(TEXT("error"), ReturnParamResult.Error);
                FunctionResults.Add(MakeShared<FJsonValueObject>(Item));
                bAnyFailed = true; FirstError = ReturnParamResult.Error;
                continue;
            }
            ResultNodeGuid = GetStr(ReturnParamResult.Json, TEXT("resultNodeGuid"));
            ResultPinId = GetStr(ReturnParamResult.Json, TEXT("pinId"));
        }

        FString PinError;
        const FString EntryThenPin = FindExecOutputPinOnNode(ObjectPath, GraphGuid, EntryNodeGuid, false, PinError);
        if (EntryThenPin.IsEmpty())
        {
            Item->SetStringField(TEXT("error"), PinError);
            FunctionResults.Add(MakeShared<FJsonValueObject>(Item));
            bAnyFailed = true; FirstError = PinError;
            continue;
        }

        int32 NodeCount = 0, ConnectionCount = 0;
        FString BodyError;
        bool bBodyOk = false;
        switch (Spec.Kind)
        {
        case Kind_Clear:
            bBodyOk = BuildClearBody(ObjectPath, GraphGuid, EntryNodeGuid, EntryThenPin, SelectionVariableName, CollectionType, ElementTypeObjectPath, NodeCount, ConnectionCount, BodyError);
            break;
        case Kind_SelectOne:
            bBodyOk = BuildSelectOneBody(ObjectPath, GraphGuid, EntryNodeGuid, EntryThenPin, TargetPinId, SelectionVariableName, CollectionType, ElementTypeObjectPath, NodeCount, ConnectionCount, BodyError);
            break;
        case Kind_Toggle:
            bBodyOk = BuildToggleBody(ObjectPath, GraphGuid, EntryNodeGuid, EntryThenPin, TargetPinId, SelectionVariableName, CollectionType, ElementTypeObjectPath, NodeCount, ConnectionCount, BodyError);
            break;
        case Kind_Contains:
            bBodyOk = BuildContainsBody(ObjectPath, GraphGuid, EntryNodeGuid, EntryThenPin, TargetPinId, ResultNodeGuid, ResultPinId, SelectionVariableName, CollectionType, ElementTypeObjectPath, NodeCount, ConnectionCount, BodyError);
            break;
        default:
            break;
        }

        Item->SetBoolField(TEXT("created"), bBodyOk);
        Item->SetNumberField(TEXT("nodesAdded"), NodeCount);
        Item->SetNumberField(TEXT("connectionsAdded"), ConnectionCount);
        if (!bBodyOk)
        {
            Item->SetStringField(TEXT("error"), BodyError);
            bAnyFailed = true; FirstError = BodyError;
        }
        else
        {
            bChanged = true;
        }
        FunctionResults.Add(MakeShared<FJsonValueObject>(Item));
    }

    bool bCompiled = false, bCompileSucceeded = false, bSaved = false, bIndexRefreshed = false;
    int32 CompileErrors = 0, CompileWarnings = 0;
    FString SavedFilename, IndexError, CompileExecutionError;

    if (!bAnyFailed && bChanged)
    {
        BlueprintToolUtils::ExecuteOnGameThreadSync(
            [&](FString& OutError)
            {
                UBlueprint* Blueprint = nullptr;
                if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
                {
                    return false;
                }
                if (bCompileAfterEdit)
                {
                    FCompilerResultsLog Log;
                    Log.bSilentMode = true;
                    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
                    bCompiled = true;
                    CompileErrors = Log.NumErrors;
                    CompileWarnings = Log.NumWarnings;
                    bCompileSucceeded = CompileErrors == 0 && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
                    if (!bCompileSucceeded)
                    {
                        OutError = TEXT("The workflow scaffolded new functions, but Blueprint compilation failed. Use ValidateBlueprint for diagnostics.");
                        return false;
                    }
                }
                if (bSaveAfterEdit)
                {
                    if (!BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, OutError))
                    {
                        return false;
                    }
                    bSaved = true;
                    bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
                }
                return true;
            },
            CompileExecutionError);
        if (!CompileExecutionError.IsEmpty() && FirstError.IsEmpty())
        {
            FirstError = CompileExecutionError;
            bAnyFailed = true;
        }
    }

    if (bAnyFailed)
    {
        // This workflow composes several independently mutating sub-tools, so a mid-sequence failure
        // can leave earlier functions fully scaffolded and the failing one partly built. Returning a
        // bare error would hide that; the caller needs the per-function record to reconcile the asset.
        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("errorCode"), TEXT("selection_workflow_incomplete"));
        Data->SetStringField(TEXT("objectPath"), ObjectPath);
        Data->SetStringField(TEXT("selectionVariableName"), SelectionVariableName);
        Data->SetBoolField(TEXT("changed"), bChanged);
        Data->SetBoolField(TEXT("atomic"), false);
        Data->SetArrayField(TEXT("functions"), FunctionResults);
        Data->SetBoolField(TEXT("compiled"), bCompiled);
        Data->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
        Data->SetBoolField(TEXT("saved"), bSaved);
        Data->SetStringField(TEXT("guidance"), bChanged
            ? TEXT("This Blueprint was partially modified and was not compiled or saved. Inspect functions[]: entries with created=true are fully built, an entry carrying an error is incomplete, and entries with attempted=false were never started. Delete or finish the incomplete function before re-running; re-running treats any existing function name as already scaffolded.")
            : TEXT("Nothing was modified. Resolve the reported error and re-run."));
        return BuildError(Request, EMCPErrorCode::InvalidParams, FirstError, Data);
    }

    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("selectionVariableName"), SelectionVariableName);
    Result->SetStringField(TEXT("collectionType"), CollectionType);
    Result->SetBoolField(TEXT("dryRun"), false);
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetArrayField(TEXT("functions"), FunctionResults);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetNumberField(TEXT("compileErrorCount"), CompileErrors);
    Result->SetNumberField(TEXT("compileWarningCount"), CompileWarnings);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FWireSelectionWorkflowTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("elementTypeObjectPath"), BuildStringProperty(TEXT("Class path of the object type the selection collection holds, e.g. an Actor subclass.")));
    Properties->SetObjectField(TEXT("selectionVariableName"), BuildStringProperty(TEXT("Collection variable name to create or reuse. Defaults to SelectedActors.")));
    Properties->SetObjectField(TEXT("collectionType"), BuildStringProperty(TEXT("array or set. Defaults to array.")));
    Properties->SetObjectField(TEXT("selectOneFunctionName"), BuildStringProperty(TEXT("Defaults to SelectOne.")));
    Properties->SetObjectField(TEXT("toggleSelectionFunctionName"), BuildStringProperty(TEXT("Defaults to ToggleSelection.")));
    Properties->SetObjectField(TEXT("clearSelectionFunctionName"), BuildStringProperty(TEXT("Defaults to ClearSelection.")));
    Properties->SetObjectField(TEXT("containsSelectionFunctionName"), BuildStringProperty(TEXT("Defaults to ContainsSelection.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Report a structural plan without mutating anything.")));
    Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile once after scaffolding. Defaults to true.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index once after scaffolding. Defaults to true.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(
        TEXT("required"),
        {
            MakeShared<FJsonValueString>(TEXT("objectPath")),
            MakeShared<FJsonValueString>(TEXT("elementTypeObjectPath"))
        });
    return Schema;
}
