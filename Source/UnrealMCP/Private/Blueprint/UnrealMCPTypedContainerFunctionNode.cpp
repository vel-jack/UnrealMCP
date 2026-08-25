#include "Blueprint/UnrealMCPTypedContainerFunctionNode.h"

#include "EdGraph/EdGraphPin.h"

void UUnrealMCPTypedContainerFunctionNode::AllocateDefaultPins()
{
    Super::AllocateDefaultPins();
    ApplyConfiguredTypes();
}

void UUnrealMCPTypedContainerFunctionNode::PostReconstructNode()
{
    Super::PostReconstructNode();
    ApplyConfiguredTypes();
}

void UUnrealMCPTypedContainerFunctionNode::NotifyPinConnectionListChanged(UEdGraphPin* Pin)
{
    Super::NotifyPinConnectionListChanged(Pin);
    ApplyConfiguredTypes();
}

void UUnrealMCPTypedContainerFunctionNode::ConfigureSet(const FEdGraphPinType& ElementType)
{
    bIsMap = false;
    ElementOrKeyType = ElementType;
    MapValueType = FEdGraphPinType();
    ApplyConfiguredTypes();
}

void UUnrealMCPTypedContainerFunctionNode::ConfigureMap(
    const FEdGraphPinType& KeyType,
    const FEdGraphPinType& ValueType)
{
    bIsMap = true;
    ElementOrKeyType = KeyType;
    MapValueType = ValueType;
    ApplyConfiguredTypes();
}

void UUnrealMCPTypedContainerFunctionNode::ApplyTerminalType(
    FEdGraphPinType& TargetType,
    const FEdGraphPinType& SourceType)
{
    TargetType.PinCategory = SourceType.PinCategory;
    TargetType.PinSubCategory = SourceType.PinSubCategory;
    TargetType.PinSubCategoryObject = SourceType.PinSubCategoryObject;
}

void UUnrealMCPTypedContainerFunctionNode::ApplyMapValueType(
    FEdGraphPinType& TargetType,
    const FEdGraphPinType& SourceType)
{
    TargetType.PinValueType.TerminalCategory = SourceType.PinCategory;
    TargetType.PinValueType.TerminalSubCategory = SourceType.PinSubCategory;
    TargetType.PinValueType.TerminalSubCategoryObject = SourceType.PinSubCategoryObject;
}

void UUnrealMCPTypedContainerFunctionNode::ApplyConfiguredTypes()
{
    if (ElementOrKeyType.PinCategory.IsNone())
    {
        return;
    }

    for (UEdGraphPin* Pin : Pins)
    {
        if (Pin == nullptr)
        {
            continue;
        }

        if (!bIsMap)
        {
            if (Pin->PinName == TEXT("TargetSet")
                || Pin->PinName == TEXT("NewItem")
                || Pin->PinName == TEXT("Item")
                || Pin->PinName == TEXT("ItemToFind"))
            {
                ApplyTerminalType(Pin->PinType, ElementOrKeyType);
            }
            continue;
        }

        if (Pin->PinName == TEXT("TargetMap"))
        {
            ApplyTerminalType(Pin->PinType, ElementOrKeyType);
            ApplyMapValueType(Pin->PinType, MapValueType);
        }
        else if (Pin->PinName == TEXT("Key") || Pin->PinName == TEXT("Keys"))
        {
            ApplyTerminalType(Pin->PinType, ElementOrKeyType);
        }
        else if (Pin->PinName == TEXT("Value") || Pin->PinName == TEXT("Values"))
        {
            ApplyTerminalType(Pin->PinType, MapValueType);
        }
    }
}
