#include "Tools/BlueprintMemberToolUtils.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallParentFunction.h"
#include "EdGraphSchema_K2.h"
#include "Tools/BlueprintEditToolUtils.h"
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
    bool ResolveVariable(UBlueprint* Blueprint, const FString& VariableName, const FString& OwnerPath,
        bool bWrite, FResolvedVariable& Out, FString& Error)
    {
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
        Out.ExternalOwner = Owner && !IsSelfOrAncestor(Blueprint, Owner) ? Owner->GetAuthoritativeClass() : nullptr;
        // Component variables and native properties may have no Blueprint declaration GUID; name resolution still applies.
        UBlueprint::GetGuidFromClassByFieldName<FProperty>(Lookup, Out.Name, Out.Guid);
        return true;
    }
    UK2Node_Variable* CreateVariableNode(UEdGraph* Graph, const FResolvedVariable& Variable, bool bSet, bool bDetached)
    {
        UK2Node_Variable* Node = bSet ? static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableSet>(Graph))
            : static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableGet>(Graph));
        if (Variable.ExternalOwner) Node->VariableReference.SetExternalMember(Variable.Name, Variable.ExternalOwner, Variable.Guid);
        else Node->VariableReference.SetSelfMember(Variable.Name, Variable.Guid);
        if (bDetached) Node->AllocateDefaultPins();
        return Node;
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
