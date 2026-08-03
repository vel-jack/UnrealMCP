#include "Index/UnrealMCPProjectIndexInternal.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Variable.h"

namespace
{
    FString NormalizeBlueprintClassPath(FString ClassPath)
    {
        if (ClassPath.IsEmpty())
        {
            return ClassPath;
        }

        FString PackagePath;
        FString ObjectName;
        if (!ClassPath.Split(TEXT("."), &PackagePath, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
        {
            return ClassPath;
        }

        static const TCHAR* PrefixesToStrip[] =
        {
            TEXT("SKEL_"),
            TEXT("REINST_"),
            TEXT("TRASHCLASS_")
        };

        for (const TCHAR* Prefix : PrefixesToStrip)
        {
            if (ObjectName.RemoveFromStart(Prefix))
            {
                break;
            }
        }

        int32 ClassSuffixIndex = INDEX_NONE;
        if (ObjectName.FindLastChar(TEXT('_'), ClassSuffixIndex) && ClassSuffixIndex > 2)
        {
            const FString Suffix = ObjectName.Mid(ClassSuffixIndex + 1);
            if (Suffix.IsNumeric() && ObjectName.Left(ClassSuffixIndex).EndsWith(TEXT("_C")))
            {
                ObjectName = ObjectName.Left(ClassSuffixIndex);
            }
        }

        return FString::Printf(TEXT("%s.%s"), *PackagePath, *ObjectName);
    }

    FString GetMemberParentPath(const FMemberReference& MemberReference, const UBlueprint* Blueprint)
    {
        if (Blueprint == nullptr)
        {
            return FString();
        }

        UClass* SelfScope = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass;
        if (UClass* MemberParentClass = MemberReference.GetMemberParentClass(SelfScope))
        {
            return NormalizeBlueprintClassPath(MemberParentClass->GetPathName());
        }

        if (MemberReference.IsSelfContext() && SelfScope != nullptr)
        {
            return NormalizeBlueprintClassPath(SelfScope->GetPathName());
        }

        return FString();
    }

    void PopulateNodeMemberMetadata(const UBlueprint* Blueprint, const UEdGraphNode* Node, FIndexedBlueprintNodeRow& NodeRow)
    {
        if (const UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(Node))
        {
            NodeRow.MemberName = CallFunctionNode->GetFunctionName().ToString();

            if (UFunction* TargetFunction = CallFunctionNode->GetTargetFunction())
            {
                if (UClass* OwnerClass = TargetFunction->GetOwnerClass())
                {
                    NodeRow.MemberParentPath = NormalizeBlueprintClassPath(OwnerClass->GetPathName());
                }
            }

            if (NodeRow.MemberParentPath.IsEmpty())
            {
                NodeRow.MemberParentPath = GetMemberParentPath(CallFunctionNode->FunctionReference, Blueprint);
            }
            return;
        }

        if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
        {
            NodeRow.MemberName = VariableNode->GetVarName().ToString();
            NodeRow.MemberParentPath = GetMemberParentPath(VariableNode->VariableReference, Blueprint);
            return;
        }

        if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
        {
            NodeRow.MemberName = CustomEventNode->GetFunctionName().ToString();
            NodeRow.MemberParentPath = GetMemberParentPath(CustomEventNode->EventReference, Blueprint);
            if (NodeRow.MemberParentPath.IsEmpty() && Blueprint != nullptr && Blueprint->GeneratedClass != nullptr)
            {
                NodeRow.MemberParentPath = NormalizeBlueprintClassPath(Blueprint->GeneratedClass->GetPathName());
            }
            return;
        }

        if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
        {
            NodeRow.MemberName = EventNode->GetFunctionName().ToString();
            NodeRow.MemberParentPath = GetMemberParentPath(EventNode->EventReference, Blueprint);
            return;
        }

        if (const UK2Node_FunctionEntry* FunctionEntryNode = Cast<UK2Node_FunctionEntry>(Node))
        {
            NodeRow.MemberName = FunctionEntryNode->CustomGeneratedFunctionName.IsNone()
                ? FunctionEntryNode->GetGraph()->GetName()
                : FunctionEntryNode->CustomGeneratedFunctionName.ToString();
            if (Blueprint != nullptr && Blueprint->GeneratedClass != nullptr)
            {
                NodeRow.MemberParentPath = NormalizeBlueprintClassPath(Blueprint->GeneratedClass->GetPathName());
            }
            return;
        }
    }

    FString GetStableGraphName(const UEdGraph* Graph)
    {
        return Graph ? Graph->GetName() : FString();
    }

    FString GetStableNodeGuid(const UEdGraphNode* Node)
    {
        if (Node == nullptr)
        {
            return FString();
        }

        if (Node->NodeGuid.IsValid())
        {
            return Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
        }

        return Node->GetName();
    }

    FString GetStablePinId(const UEdGraphPin* Pin)
    {
        if (Pin == nullptr)
        {
            return FString();
        }

        if (Pin->PinId.IsValid())
        {
            return Pin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower);
        }

        return FString::Printf(TEXT("%s:%s"),
            Pin->Direction == EGPD_Output ? TEXT("out") : TEXT("in"),
            *Pin->PinName.ToString());
    }

    FString GetNodeTypeString(const UEdGraphNode* Node)
    {
        if (Node == nullptr || Node->GetClass() == nullptr)
        {
            return TEXT("unknown");
        }

        const FString ClassName = Node->GetClass()->GetName();
        if (ClassName.Contains(TEXT("K2Node_CustomEvent")))
        {
            return TEXT("custom_event");
        }
        if (ClassName.Contains(TEXT("K2Node_Event")))
        {
            return TEXT("event");
        }
        if (ClassName.Contains(TEXT("K2Node_FunctionEntry")))
        {
            return TEXT("function_entry");
        }
        if (ClassName.Contains(TEXT("K2Node_FunctionResult")))
        {
            return TEXT("function_result");
        }
        if (ClassName.Contains(TEXT("K2Node_CallFunction")))
        {
            return TEXT("call_function");
        }
        if (ClassName.Contains(TEXT("K2Node_VariableGet")))
        {
            return TEXT("variable_get");
        }
        if (ClassName.Contains(TEXT("K2Node_VariableSet")))
        {
            return TEXT("variable_set");
        }
        if (ClassName.Contains(TEXT("K2Node_MacroInstance")))
        {
            return TEXT("macro_instance");
        }
        if (ClassName.Contains(TEXT("K2Node_ExecutionSequence")))
        {
            return TEXT("sequence");
        }
        if (ClassName.Contains(TEXT("K2Node_IfThenElse")))
        {
            return TEXT("branch");
        }
        if (ClassName.Contains(TEXT("K2Node_Timeline")))
        {
            return TEXT("timeline");
        }
        if (ClassName.Contains(TEXT("EdGraphNode_Comment")))
        {
            return TEXT("comment");
        }

        return TEXT("unknown");
    }

    FString GetGraphTypeString(const UBlueprint* Blueprint, const UEdGraph* Graph)
    {
        if (Blueprint == nullptr || Graph == nullptr)
        {
            return TEXT("unknown");
        }

        if (Blueprint->UbergraphPages.Contains(const_cast<UEdGraph*>(Graph)))
        {
            return TEXT("event_graph");
        }

        if (Blueprint->FunctionGraphs.Contains(const_cast<UEdGraph*>(Graph)))
        {
            return TEXT("function_graph");
        }

        if (Blueprint->MacroGraphs.Contains(const_cast<UEdGraph*>(Graph)))
        {
            return TEXT("macro_graph");
        }

        for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
        {
            if (InterfaceDescription.Graphs.Contains(const_cast<UEdGraph*>(Graph)))
            {
                return TEXT("interface_graph");
            }
        }

        return TEXT("graph");
    }

    bool IsExecPin(const UEdGraphPin* Pin)
    {
        return Pin != nullptr && Pin->PinType.PinCategory == TEXT("exec");
    }
}

bool ExtractBlueprintGraphRows(
    UBlueprint* Blueprint,
    TArray<FIndexedBlueprintGraphRow>& OutGraphs,
    TArray<FIndexedBlueprintNodeRow>& OutNodes,
    TArray<FIndexedBlueprintPinRow>& OutPins,
    TArray<FIndexedBlueprintEdgeRow>& OutEdges)
{
    if (Blueprint == nullptr)
    {
        return false;
    }

    TArray<UEdGraph*> AllGraphs;
    TSet<const UEdGraph*> SeenGraphs;

    auto AddGraphs = [&AllGraphs, &SeenGraphs](const TArray<UEdGraph*>& Graphs)
    {
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph != nullptr && !SeenGraphs.Contains(Graph))
            {
                SeenGraphs.Add(Graph);
                AllGraphs.Add(Graph);
            }
        }
    };

    AddGraphs(Blueprint->UbergraphPages);
    AddGraphs(Blueprint->FunctionGraphs);
    AddGraphs(Blueprint->MacroGraphs);
    for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
    {
        AddGraphs(InterfaceDescription.Graphs);
    }

    AllGraphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
    {
        return Left.GetName() < Right.GetName();
    });

    for (const UEdGraph* Graph : AllGraphs)
    {
        if (Graph == nullptr)
        {
            continue;
        }

        FIndexedBlueprintGraphRow GraphRow;
        GraphRow.GraphName = GetStableGraphName(Graph);
        GraphRow.GraphGuid = Graph->GraphGuid.IsValid()
            ? Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
            : FString();
        GraphRow.GraphType = GetGraphTypeString(Blueprint, Graph);
        GraphRow.NodeCount = Graph->Nodes.Num();

        TArray<const UEdGraphNode*> SortedNodes;
        SortedNodes.Reserve(Graph->Nodes.Num());
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node != nullptr)
            {
                SortedNodes.Add(Node);
            }
        }

        SortedNodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
        {
            const FString LeftGuid = GetStableNodeGuid(&Left);
            const FString RightGuid = GetStableNodeGuid(&Right);
            return LeftGuid == RightGuid ? Left.GetName() < Right.GetName() : LeftGuid < RightGuid;
        });

        for (const UEdGraphNode* Node : SortedNodes)
        {
            if (GraphRow.EntryNodeGuid.IsEmpty())
            {
                const FString NodeType = GetNodeTypeString(Node);
                if (NodeType == TEXT("event") || NodeType == TEXT("custom_event") || NodeType == TEXT("function_entry"))
                {
                    GraphRow.EntryNodeGuid = GetStableNodeGuid(Node);
                }
            }

            FIndexedBlueprintNodeRow NodeRow;
            NodeRow.GraphName = GraphRow.GraphName;
            NodeRow.NodeGuid = GetStableNodeGuid(Node);
            NodeRow.NodeName = Node->GetName();
            NodeRow.NodeClassPath = Node->GetClass() ? Node->GetClass()->GetPathName() : FString();
            NodeRow.NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
            NodeRow.NodeType = GetNodeTypeString(Node);
            PopulateNodeMemberMetadata(Blueprint, Node, NodeRow);
            NodeRow.NodePosX = Node->NodePosX;
            NodeRow.NodePosY = Node->NodePosY;

            bool bHasExecPin = false;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin == nullptr)
                {
                    continue;
                }

                if (Pin->Direction == EGPD_Input)
                {
                    ++NodeRow.InputPinCount;
                }
                else if (Pin->Direction == EGPD_Output)
                {
                    ++NodeRow.OutputPinCount;
                }

                bHasExecPin |= IsExecPin(Pin);

                FIndexedBlueprintPinRow PinRow;
                PinRow.GraphName = GraphRow.GraphName;
                PinRow.NodeGuid = NodeRow.NodeGuid;
                PinRow.PinId = GetStablePinId(Pin);
                PinRow.PinName = Pin->PinName.ToString();
                PinRow.Direction = Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input");
                PinRow.Category = Pin->PinType.PinCategory.ToString();
                PinRow.Subcategory = Pin->PinType.PinSubCategory.ToString();
                PinRow.SubcategoryObjectPath = Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : FString();
                PinRow.ContainerType = StaticEnum<EPinContainerType>()->GetNameStringByValue(static_cast<int64>(Pin->PinType.ContainerType));
                PinRow.bIsReference = Pin->PinType.bIsReference;
                PinRow.bIsConst = Pin->PinType.bIsConst;
                PinRow.LinkedPinCount = Pin->LinkedTo.Num();
                PinRow.DefaultValue = Pin->DefaultValue;
                OutPins.Add(MoveTemp(PinRow));

                if (Pin->Direction != EGPD_Output)
                {
                    continue;
                }

                for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (LinkedPin == nullptr || LinkedPin->GetOwningNodeUnchecked() == nullptr || LinkedPin->GetOwningNodeUnchecked()->GetGraph() == nullptr)
                    {
                        continue;
                    }

                    FIndexedBlueprintEdgeRow EdgeRow;
                    EdgeRow.SourceGraphName = GraphRow.GraphName;
                    EdgeRow.SourceNodeGuid = NodeRow.NodeGuid;
                    EdgeRow.SourcePinId = PinRow.PinId;
                    EdgeRow.TargetGraphName = GetStableGraphName(LinkedPin->GetOwningNodeUnchecked()->GetGraph());
                    EdgeRow.TargetNodeGuid = GetStableNodeGuid(LinkedPin->GetOwningNodeUnchecked());
                    EdgeRow.TargetPinId = GetStablePinId(LinkedPin);
                    EdgeRow.EdgeKind = IsExecPin(Pin) || IsExecPin(LinkedPin) ? TEXT("exec") : TEXT("data");
                    OutEdges.Add(MoveTemp(EdgeRow));
                }
            }

            NodeRow.bIsPure = !bHasExecPin;
            OutNodes.Add(MoveTemp(NodeRow));
        }

        OutGraphs.Add(MoveTemp(GraphRow));
    }

    return true;
}
