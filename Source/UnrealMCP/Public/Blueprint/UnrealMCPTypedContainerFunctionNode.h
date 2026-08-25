#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "UnrealMCPTypedContainerFunctionNode.generated.h"

UCLASS()
class UNREALMCP_API UUnrealMCPTypedContainerFunctionNode : public UK2Node_CallFunction
{
    GENERATED_BODY()

public:
    virtual void AllocateDefaultPins() override;
    virtual void PostReconstructNode() override;
    virtual void NotifyPinConnectionListChanged(UEdGraphPin* Pin) override;

    void ConfigureSet(const FEdGraphPinType& ElementType);
    void ConfigureMap(const FEdGraphPinType& KeyType, const FEdGraphPinType& ValueType);

private:
    void ApplyConfiguredTypes();
    static void ApplyTerminalType(FEdGraphPinType& TargetType, const FEdGraphPinType& SourceType);
    static void ApplyMapValueType(FEdGraphPinType& TargetType, const FEdGraphPinType& SourceType);

    UPROPERTY()
    bool bIsMap = false;

    UPROPERTY()
    FEdGraphPinType ElementOrKeyType;

    UPROPERTY()
    FEdGraphPinType MapValueType;
};
