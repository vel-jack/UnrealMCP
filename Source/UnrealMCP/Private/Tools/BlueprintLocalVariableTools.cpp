#include "Tools/BlueprintLocalVariableTools.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    namespace Edit = UnrealMCP::BlueprintEditToolUtils;

    struct FLocalArgs
    {
        FString ObjectPath;
        FString GraphName;
        FString GraphGuid;
        FString VariableName;
    };

    bool ReadCommonArgs(const TSharedPtr<FJsonObject>& Params, bool bNeedVariable, FLocalArgs& Out)
    {
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("objectPath"), Out.ObjectPath) || Out.ObjectPath.IsEmpty())
        {
            return false;
        }
        Params->TryGetStringField(TEXT("graphName"), Out.GraphName);
        Params->TryGetStringField(TEXT("graphGuid"), Out.GraphGuid);
        Params->TryGetStringField(TEXT("variableName"), Out.VariableName);
        return (!Out.GraphName.IsEmpty() || !Out.GraphGuid.IsEmpty()) && (!bNeedVariable || !Out.VariableName.IsEmpty());
    }

    // Resolves the Blueprint, a graph that supports locals (function graphs) and the function entry node that stores them.
    bool ResolveFunctionGraph(const FLocalArgs& Args, UBlueprint*& Blueprint, UEdGraph*& Graph, UK2Node_FunctionEntry*& Entry, FString& OutError)
    {
        if (!Edit::ResolveBlueprint(Args.ObjectPath, Blueprint, OutError)
            || !UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, Args.GraphName, Args.GraphGuid, Graph, OutError))
        {
            return false;
        }
        if (!FBlueprintEditorUtils::DoesSupportLocalVariables(Graph))
        {
            OutError = TEXT("The graph does not support local variables; target a function graph.");
            return false;
        }
        TArray<UK2Node_FunctionEntry*> Entries;
        Graph->GetNodesOfClass(Entries);
        if (Entries.Num() != 1)
        {
            OutError = TEXT("Could not find exactly one function entry node in the graph.");
            return false;
        }
        Entry = Entries[0];
        return true;
    }

    TSharedRef<FJsonObject> SerializeLocal(const FBPVariableDescription& Variable)
    {
        TSharedRef<FJsonObject> Object = UnrealMCP::BlueprintToolUtils::SerializeVariable(Variable);
        Object->SetStringField(TEXT("defaultValue"), Variable.DefaultValue);
        return Object;
    }

    void AddProp(const TSharedRef<FJsonObject>& Properties, const TCHAR* Name, const TCHAR* Type, const TCHAR* Description)
    {
        TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
        Prop->SetStringField(TEXT("type"), Type);
        Prop->SetStringField(TEXT("description"), Description);
        Properties->SetObjectField(Name, Prop);
    }

    TSharedRef<FJsonObject> BuildSchema(TFunctionRef<void(const TSharedRef<FJsonObject>&)> Extra, std::initializer_list<const TCHAR*> RequiredNames)
    {
        TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("object"));
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        AddProp(Properties, TEXT("objectPath"), TEXT("string"), TEXT("Blueprint asset object path."));
        AddProp(Properties, TEXT("graphName"), TEXT("string"), TEXT("Function graph name (or name::guid) from ListBlueprintGraphs."));
        AddProp(Properties, TEXT("graphGuid"), TEXT("string"), TEXT("Function graph GUID; alternative to graphName."));
        Extra(Properties);
        Schema->SetObjectField(TEXT("properties"), Properties);
        TArray<TSharedPtr<FJsonValue>> Required;
        for (const TCHAR* Name : RequiredNames)
        {
            Required.Add(MakeShared<FJsonValueString>(Name));
        }
        Schema->SetArrayField(TEXT("required"), Required);
        return Schema;
    }
}

// ---------------------------------------------------------------------------------------------------------------
FListBlueprintLocalVariablesTool::FListBlueprintLocalVariablesTool()
    : FMCPToolBase(TEXT("ListBlueprintLocalVariables"), TEXT("Lists the local variables (name, GUID, type, default) of one Blueprint function graph."))
{
}

UnrealMCP::FMCPResponse FListBlueprintLocalVariablesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FLocalArgs Args;
    if (!ReadCommonArgs(Request.Params, false, Args))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListBlueprintLocalVariables requires objectPath and graphName or graphGuid."));
    }

    TArray<TSharedPtr<FJsonValue>> Locals;
    FString ExecutionError;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        UK2Node_FunctionEntry* Entry = nullptr;
        if (!ResolveFunctionGraph(Args, Blueprint, Graph, Entry, OutError))
        {
            return false;
        }
        for (const FBPVariableDescription& Variable : Entry->LocalVariables)
        {
            Locals.Add(MakeShared<FJsonValueObject>(SerializeLocal(Variable)));
        }
        return true;
    }, ExecutionError);

    if (!bOk)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), Args.ObjectPath);
    Result->SetNumberField(TEXT("count"), Locals.Num());
    Result->SetArrayField(TEXT("localVariables"), Locals);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FListBlueprintLocalVariablesTool::BuildInputSchema() const
{
    return BuildSchema([](const TSharedRef<FJsonObject>&) {}, {TEXT("objectPath")});
}

// ---------------------------------------------------------------------------------------------------------------
FAddBlueprintLocalVariableTool::FAddBlueprintLocalVariableTool()
    : FMCPToolBase(TEXT("AddBlueprintLocalVariable"), TEXT("Transactionally adds a typed local variable to one Blueprint function graph (type/containerType/valueType as in AddBlueprintVariable). Idempotent by name. Use AddBlueprintVariableGetNode/SetNode-style graph tools or ApplyBlueprintGraphPatch afterwards to read/write it."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintLocalVariableTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FLocalArgs Args;
    FString TypeName, TypeObjectPath, DefaultValue, ContainerType, ValueType, ValueTypeObjectPath;
    if (!ReadCommonArgs(Request.Params, true, Args) || !Request.Params->TryGetStringField(TEXT("type"), TypeName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintLocalVariable requires objectPath, graphName or graphGuid, variableName and type."));
    }
    Request.Params->TryGetStringField(TEXT("typeObjectPath"), TypeObjectPath);
    Request.Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);
    Request.Params->TryGetStringField(TEXT("containerType"), ContainerType);
    Request.Params->TryGetStringField(TEXT("valueType"), ValueType);
    Request.Params->TryGetStringField(TEXT("valueTypeObjectPath"), ValueTypeObjectPath);
    if (ContainerType.IsEmpty())
    {
        ContainerType = TEXT("none");
    }

    const bool bDryRun = Edit::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = Edit::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), false);
    const bool bSave = Edit::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    bool bAlreadyExists = false;
    bool bCompileSucceeded = false;
    FString SavedFilename, ExecutionError;
    TSharedPtr<FJsonObject> Added;

    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        UK2Node_FunctionEntry* Entry = nullptr;
        if (!ResolveFunctionGraph(Args, Blueprint, Graph, Entry, OutError))
        {
            return false;
        }

        FEdGraphPinType PinType;
        if (!Edit::BuildContainerPinType(TypeName, TypeObjectPath, ContainerType, ValueType, ValueTypeObjectPath, PinType, OutError))
        {
            return false;
        }

        if (FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, *Args.VariableName))
        {
            bAlreadyExists = true;
            return true;
        }
        if (bDryRun)
        {
            return true;
        }

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AddBlueprintLocalVariable", "UnrealMCP Add Blueprint Local Variable"));
        Blueprint->Modify();
        if (!FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph, *Args.VariableName, PinType, DefaultValue))
        {
            OutError = TEXT("Unreal failed to add the local variable (invalid or conflicting name).");
            return false;
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

        if (const FBPVariableDescription* Variable = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, *Args.VariableName))
        {
            Added = SerializeLocal(*Variable);
        }

        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
            bCompileSucceeded = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
            if (!bCompileSucceeded)
            {
                OutError = TEXT("Local variable was added, but compilation failed; use CompileBlueprint for diagnostics.");
                return false;
            }
        }
        if (bSave && !Edit::SaveAsset(Blueprint, SavedFilename, OutError))
        {
            return false;
        }
        return true;
    }, ExecutionError);

    if (!bOk)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), Args.ObjectPath);
    Result->SetStringField(TEXT("variableName"), Args.VariableName);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyExists"), bAlreadyExists);
    Result->SetBoolField(TEXT("added"), !bDryRun && !bAlreadyExists);
    Result->SetBoolField(TEXT("compiled"), bCompile && !bDryRun && !bAlreadyExists);
    Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bAlreadyExists);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    if (Added.IsValid())
    {
        Result->SetObjectField(TEXT("localVariable"), Added);
    }
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintLocalVariableTool::BuildInputSchema() const
{
    return BuildSchema([](const TSharedRef<FJsonObject>& P)
    {
        AddProp(P, TEXT("variableName"), TEXT("string"), TEXT("New local variable name."));
        AddProp(P, TEXT("type"), TEXT("string"), TEXT("Type name as in AddBlueprintVariable (bool, int, float, string, object, struct, ...)."));
        AddProp(P, TEXT("typeObjectPath"), TEXT("string"), TEXT("Class/struct/enum path for object, class, struct or enum types."));
        AddProp(P, TEXT("containerType"), TEXT("string"), TEXT("none, array, set or map."));
        AddProp(P, TEXT("valueType"), TEXT("string"), TEXT("Map value type."));
        AddProp(P, TEXT("valueTypeObjectPath"), TEXT("string"), TEXT("Map value object/struct path."));
        AddProp(P, TEXT("defaultValue"), TEXT("string"), TEXT("Optional default as export text."));
        AddProp(P, TEXT("dryRun"), TEXT("boolean"), TEXT("Validate without changing."));
        AddProp(P, TEXT("compileAfterEdit"), TEXT("boolean"), TEXT("Compile after the edit (default false)."));
        AddProp(P, TEXT("saveAfterEdit"), TEXT("boolean"), TEXT("Save after the edit (default false)."));
    }, {TEXT("objectPath"), TEXT("variableName"), TEXT("type")});
}

// ---------------------------------------------------------------------------------------------------------------
FRemoveBlueprintLocalVariableTool::FRemoveBlueprintLocalVariableTool()
    : FMCPToolBase(TEXT("RemoveBlueprintLocalVariable"), TEXT("Removes one local variable from a Blueprint function graph. Requires confirm=true unless dryRun=true; reports how many Get/Set nodes still reference the name."))
{
}

UnrealMCP::FMCPResponse FRemoveBlueprintLocalVariableTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FLocalArgs Args;
    if (!ReadCommonArgs(Request.Params, true, Args))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("RemoveBlueprintLocalVariable requires objectPath, graphName or graphGuid, and variableName."));
    }
    const bool bDryRun = Edit::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bConfirm = Edit::GetOptionalBool(Request.Params, TEXT("confirm"), false);
    const bool bSave = Edit::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    if (!bDryRun && !bConfirm)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("RemoveBlueprintLocalVariable requires confirm=true unless dryRun=true."));
    }

    int32 AccessNodes = 0;
    bool bExisted = false;
    FString SavedFilename, ExecutionError;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        UK2Node_FunctionEntry* Entry = nullptr;
        if (!ResolveFunctionGraph(Args, Blueprint, Graph, Entry, OutError))
        {
            return false;
        }
        bExisted = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, *Args.VariableName) != nullptr;
        if (!bExisted)
        {
            OutError = TEXT("The function graph has no local variable with that name.");
            return false;
        }
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
            {
                AccessNodes += Var->GetVarName() == FName(*Args.VariableName) ? 1 : 0;
            }
        }
        if (bDryRun)
        {
            return true;
        }
        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "RemoveBlueprintLocalVariable", "UnrealMCP Remove Blueprint Local Variable"));
        Blueprint->Modify();
        const UStruct* Scope = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass->FindFunctionByName(Graph->GetFName()) : nullptr;
        if (!Scope)
        {
            OutError = TEXT("Could not resolve the function scope; compile the Blueprint and retry.");
            return false;
        }
        FBlueprintEditorUtils::RemoveLocalVariable(Blueprint, Scope, *Args.VariableName);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        if (bSave && !Edit::SaveAsset(Blueprint, SavedFilename, OutError))
        {
            return false;
        }
        return true;
    }, ExecutionError);

    if (!bOk)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), Args.ObjectPath);
    Result->SetStringField(TEXT("variableName"), Args.VariableName);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("removed"), !bDryRun);
    Result->SetNumberField(TEXT("accessNodesReferencingVariable"), AccessNodes);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FRemoveBlueprintLocalVariableTool::BuildInputSchema() const
{
    return BuildSchema([](const TSharedRef<FJsonObject>& P)
    {
        AddProp(P, TEXT("variableName"), TEXT("string"), TEXT("Local variable to remove."));
        AddProp(P, TEXT("dryRun"), TEXT("boolean"), TEXT("Report access nodes without removing."));
        AddProp(P, TEXT("confirm"), TEXT("boolean"), TEXT("Required for a real removal."));
        AddProp(P, TEXT("saveAfterEdit"), TEXT("boolean"), TEXT("Save after the edit (default false)."));
    }, {TEXT("objectPath"), TEXT("variableName")});
}
