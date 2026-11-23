#include "Tools/Blueprint/Shared/BlueprintMemberToolUtils.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallParentFunction.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "UObject/UnrealType.h"

namespace UnrealMCP::BlueprintMemberToolUtils
{
    bool IsSelfOrAncestor(UBlueprint* Blueprint, UClass* Owner)
    {
        if (!Blueprint || !Owner) return false;
        Owner = Owner->GetAuthoritativeClass();
        UClass* Self = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
        return Self && (Self->IsChildOf(Owner) || Self->GetAuthoritativeClass()->IsChildOf(Owner));
    }
    bool ResolveVariable(UBlueprint* Blueprint, UEdGraph* Graph, const FString& VariableName, const FString& OwnerPath,
        bool bWrite, FResolvedVariable& Out, FString& Error)
    {
        Out = FResolvedVariable();
        UEdGraph* Scope = Graph ? FBlueprintEditorUtils::GetTopLevelGraph(Graph) : nullptr;
        if (OwnerPath.IsEmpty() && Scope && Blueprint->FunctionGraphs.Contains(Scope))
        {
            TArray<UK2Node_FunctionEntry*> Entries;
            Scope->GetNodesOfClass(Entries);
            if (Entries.Num() != 1) { Error = TEXT("Function scope requires exactly one entry node."); return false; }
            const FBPVariableDescription* Local = Entries[0]->LocalVariables.FindByPredicate([&](const FBPVariableDescription& V)
                { return V.VarName == FName(*VariableName); });
            if (Local)
            {
                if (!Local->VarGuid.IsValid()) { Error = TEXT("Local variable declaration has no stable GUID."); return false; }
                Out.Name = Local->VarName; Out.Guid = Local->VarGuid;
                Out.LocalScope = Scope->GetName(); Out.DeclarationPath = Scope->GetPathName();
                return true;
            }
        }
        UClass* Lookup = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
        UClass* Owner = nullptr;
        if (!OwnerPath.IsEmpty())
        {
            if (!BlueprintEditToolUtils::ResolveClass(OwnerPath, Owner, Error)) return false;
            if (!IsSelfOrAncestor(Blueprint, Owner)) Lookup = Owner;
        }
        FProperty* Property = Lookup ? FindFProperty<FProperty>(Lookup, *VariableName) : nullptr;
        if (!Property || !Property->HasAnyPropertyFlags(CPF_BlueprintVisible)
            || (bWrite && Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly)))
        { Error = FString::Printf(TEXT("Variable '%s' is missing, inaccessible or read-only for this operation."), *VariableName); return false; }
        Out.Name = Property->GetFName(); Out.Property = Property;
        Out.DeclaringClass = Property->GetOwnerClass()->GetAuthoritativeClass();
        Out.DeclarationPath = Out.DeclaringClass->GetPathName();
        Out.ExternalOwner = Owner && !IsSelfOrAncestor(Blueprint, Owner) ? Owner->GetAuthoritativeClass() : nullptr;
        // Component variables and native properties may have no Blueprint declaration GUID; name resolution still applies.
        UBlueprint::GetGuidFromClassByFieldName<FProperty>(Lookup, Out.Name, Out.Guid);
        return true;
    }
    UK2Node_Variable* CreateVariableNode(UEdGraph* Graph, const FResolvedVariable& Variable, bool bSet, bool bDetached)
    {
        UK2Node_Variable* Node = bSet ? static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableSet>(Graph))
            : static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableGet>(Graph));
        if (Variable.IsLocalContext()) Node->VariableReference.SetLocalMember(Variable.Name, Variable.LocalScope, Variable.Guid);
        else if (Variable.ExternalOwner) Node->VariableReference.SetExternalMember(Variable.Name, Variable.ExternalOwner, Variable.Guid);
        else Node->VariableReference.SetSelfMember(Variable.Name, Variable.Guid);
        if (bDetached) { Node->AllocateDefaultPins(); EnsureLocalVariablePins(Node); }
        return Node;
    }
    void EnsureLocalVariablePins(UK2Node_Variable* Node)
    {
        if (!Node || !Node->VariableReference.IsLocalScope()) return;
        UEdGraph* Scope = FBlueprintEditorUtils::GetTopLevelGraph(Node->GetGraph());
        UBlueprint* BP = Scope ? Scope->GetTypedOuter<UBlueprint>() : nullptr;
        if (!BP || Node->VariableReference.GetMemberScopeName() != Scope->GetName()) return;
        const FBPVariableDescription* Local = FBlueprintEditorUtils::FindLocalVariable(BP, Scope, Node->VariableReference.GetMemberName());
        if (!Local || Local->VarGuid != Node->VariableReference.GetMemberGuid()) return;
        // Newly declared locals may not exist in reflection until the next compile.
        auto EnsurePin = [&](FName PinName, EEdGraphPinDirection Direction)
        {
            UEdGraphPin* Pin = Node->FindPin(PinName, Direction);
            if (!Pin)
            {
                Pin = Node->CreatePin(Direction, Local->VarType, PinName);
                GetDefault<UEdGraphSchema_K2>()->SetPinAutogeneratedDefaultValueBasedOnType(Pin);
            }
            else Pin->PinType = Local->VarType;
        };
        const bool bSet = Node->IsA<UK2Node_VariableSet>();
        EnsurePin(Local->VarName, bSet ? EGPD_Input : EGPD_Output);
        if (bSet) EnsurePin(TEXT("Output_Get"), EGPD_Output);
    }
    bool ValidateStringCases(const TArray<FString>& Cases, FString& Error)
    {
        if (Cases.Num() > 128) { Error = TEXT("At most 128 string switch cases are supported."); return false; }
        TSet<FName> Seen;
        for (const FString& Case : Cases)
        {
            const FName Key(*Case);
            if (Case.TrimStartAndEnd().IsEmpty() || Case.Len() > 256 || Seen.Contains(Key)
                || Key == TEXT("Default") || Key == TEXT("Selection") || Key == UEdGraphSchema_K2::PN_Execute
                || Key == UEdGraphSchema_K2::PN_Self || Key == TEXT("NotEqual_StrStr") || Key == TEXT("NotEqual_StriStri"))
            { Error = TEXT("Switch cases must be unique non-empty labels and cannot use reserved pin names."); return false; }
            Seen.Add(Key);
        }
        return true;
    }
    bool ResolveParentFunction(UBlueprint* Blueprint, const FString& FunctionName, UFunction*& Out, FString& Error)
    {
        Out = Blueprint->ParentClass ? Blueprint->ParentClass->FindFunctionByName(*FunctionName) : nullptr;
        if (!Out || !Out->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent) || Out->HasAnyFunctionFlags(FUNC_Static | FUNC_Private))
        { Error = TEXT("No callable parent implementation for function '") + FunctionName + TEXT("'."); return false; }
        return true;
    }
    UK2Node_CallParentFunction* CreateParentCall(UEdGraph* Graph, UFunction* Function, bool bDetached)
    {
        auto* Node = NewObject<UK2Node_CallParentFunction>(Graph);
        Node->SetFromFunction(Function);
        if (bDetached) Node->AllocateDefaultPins();
        return Node;
    }
    bool WireParentCall(UEdGraphNode* Source, UK2Node_CallParentFunction* Parent, FString& Error)
    {
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        for (UEdGraphPin* Input : Parent->Pins)
        {
            if (Input->Direction != EGPD_Input || Input->bHidden || Input->PinName == UEdGraphSchema_K2::PN_Self) continue;
            const FName SourceName = Input->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? UEdGraphSchema_K2::PN_Then : Input->PinName;
            UEdGraphPin* Output = Source->FindPin(SourceName, EGPD_Output);
            if (!Output || Schema->CanCreateConnection(Output, Input).Response != CONNECT_RESPONSE_MAKE || !Schema->TryCreateConnection(Output, Input))
            { Error = TEXT("Could not wire parent-call input '") + Input->PinName.ToString() + TEXT("'."); return false; }
        }
        return true;
    }
}
