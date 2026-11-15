#pragma once
#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UK2Node_Variable;
class UK2Node_CallParentFunction;
class UEdGraphNode;
class FProperty;

namespace UnrealMCP::BlueprintMemberToolUtils
{
    struct FResolvedVariable
    {
        FName Name;
        FGuid Guid;
        FProperty* Property = nullptr;
        UClass* DeclaringClass = nullptr;
        UClass* ExternalOwner = nullptr;
        bool IsSelfContext() const { return ExternalOwner == nullptr; }
    };
    bool IsSelfOrAncestor(UBlueprint* Blueprint, UClass* Owner);
    bool ResolveVariable(UBlueprint* Blueprint, const FString& VariableName, const FString& OwnerPath,
        bool bWrite, FResolvedVariable& Out, FString& Error);
    UK2Node_Variable* CreateVariableNode(UEdGraph* Graph, const FResolvedVariable& Variable, bool bSet, bool bDetached);
    bool ValidateStringCases(const TArray<FString>& Cases, FString& Error);
    bool ResolveParentFunction(UBlueprint* Blueprint, const FString& FunctionName, UFunction*& Out, FString& Error);
    UK2Node_CallParentFunction* CreateParentCall(UEdGraph* Graph, UFunction* Function, bool bDetached);
    bool WireParentCall(UEdGraphNode* Source, UK2Node_CallParentFunction* Parent, FString& Error);
}
