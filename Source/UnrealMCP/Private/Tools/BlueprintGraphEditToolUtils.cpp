#include "Tools/BlueprintGraphEditToolUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace UnrealMCP::BlueprintGraphEditToolUtils
{
    bool ResolveGraph(UBlueprint* Blueprint, const FString& GraphName, const FString& GraphGuid, UEdGraph*& OutGraph, FString& OutError)
    {
        OutGraph = nullptr;
        if (Blueprint == nullptr) { OutError = TEXT("Blueprint is null."); return false; }
        FString EffectiveGuid = GraphGuid;
        if (EffectiveGuid.IsEmpty())
        {
            TArray<FString> Parts;
            GraphName.ParseIntoArray(Parts, TEXT("::"), false);
            if (Parts.Num() >= 2) EffectiveGuid = Parts[1];
        }
        FGuid ParsedGuid;
        const bool bHasGuid = FGuid::Parse(EffectiveGuid, ParsedGuid);
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph == nullptr) continue;
            if (bHasGuid && Graph->GraphGuid == ParsedGuid) { OutGraph = Graph; return true; }
            if (!bHasGuid && Graph->GetName().Equals(GraphName, ESearchCase::CaseSensitive))
            {
                if (OutGraph != nullptr) { OutError = TEXT("graphName is ambiguous; provide graphGuid or the stable graphName from ListBlueprintGraphs."); return false; }
                OutGraph = Graph;
            }
        }
        if (OutGraph != nullptr) return true;
        OutError = TEXT("Could not resolve the requested graph in the live Blueprint.");
        return false;
    }

    bool ResolveNode(UEdGraph* Graph, const FString& NodeGuid, UEdGraphNode*& OutNode, FString& OutError)
    {
        OutNode = nullptr;
        FGuid ParsedGuid;
        if (!FGuid::Parse(NodeGuid, ParsedGuid)) { OutError = TEXT("nodeGuid is not a valid GUID."); return false; }
        for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == ParsedGuid) { OutNode = Node; return true; }
        OutError = TEXT("Could not find nodeGuid in the requested graph."); return false;
    }

    bool ResolvePin(UEdGraphNode* Node, const FString& PinId, const FString& PinName, const FString& Direction, UEdGraphPin*& OutPin, FString& OutError)
    {
        OutPin = nullptr;
        FGuid ParsedPinId;
        const bool bHasPinId = FGuid::Parse(PinId, ParsedPinId);
        const TOptional<EEdGraphPinDirection> RequiredDirection = Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase)
            ? TOptional<EEdGraphPinDirection>(EGPD_Input)
            : (Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase) ? TOptional<EEdGraphPinDirection>(EGPD_Output) : TOptional<EEdGraphPinDirection>());
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            const bool bMatches = bHasPinId ? Pin->PinId == ParsedPinId : Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase);
            if (bMatches && (!RequiredDirection.IsSet() || Pin->Direction == RequiredDirection.GetValue()))
            {
                if (OutPin != nullptr && !bHasPinId) { OutError = TEXT("Pin name is ambiguous; provide pinId or direction."); return false; }
                OutPin = Pin;
            }
        }
        if (OutPin) return true;
        OutError = TEXT("Could not resolve the requested pin on the node."); return false;
    }

    FPlacement ResolvePlacement(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FPlacement Placement;
        double X = 0, Y = 0;
        const bool bHasX = Params.IsValid() && Params->TryGetNumberField(TEXT("positionX"), X);
        const bool bHasY = Params.IsValid() && Params->TryGetNumberField(TEXT("positionY"), Y);
        FString RelativeGuid;
        if (Params.IsValid()) Params->TryGetStringField(TEXT("relativeToNodeGuid"), RelativeGuid);
        if (bHasX && bHasY) { Placement.X = FMath::RoundToInt(X); Placement.Y = FMath::RoundToInt(Y); }
        else if (!RelativeGuid.IsEmpty())
        {
            UEdGraphNode* RelativeNode = nullptr;
            if (!ResolveNode(Graph, RelativeGuid, RelativeNode, OutError)) return Placement;
            double HorizontalSpacing = 320, VerticalOffset = 0;
            Params->TryGetNumberField(TEXT("horizontalSpacing"), HorizontalSpacing);
            Params->TryGetNumberField(TEXT("verticalOffset"), VerticalOffset);
            Placement.X = RelativeNode->NodePosX + FMath::RoundToInt(HorizontalSpacing);
            Placement.Y = RelativeNode->NodePosY + FMath::RoundToInt(VerticalOffset);
            Placement.bAutoPlaced = true;
        }
        else
        {
            int32 MaxX = 0;
            for (const UEdGraphNode* Node : Graph->Nodes) if (Node) MaxX = FMath::Max(MaxX, Node->NodePosX);
            Placement.X = MaxX + 320; Placement.Y = 0; Placement.bAutoPlaced = true;
        }

        bool bCollision = true;
        while (bCollision)
        {
            bCollision = false;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && FMath::Abs(Node->NodePosX - Placement.X) < 260 && FMath::Abs(Node->NodePosY - Placement.Y) < 140)
                { Placement.Y += 180; Placement.bCollisionAdjusted = true; bCollision = true; break; }
            }
        }
        return Placement;
    }

    FString GetNodeGuid(const UEdGraphNode* Node) { return Node && Node->NodeGuid.IsValid() ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString(); }
    FString GetPinId(const UEdGraphPin* Pin) { return Pin && Pin->PinId.IsValid() ? Pin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower) : FString(); }

    FString GetPinDefaultObjectPath(const UEdGraphPin* Pin)
    {
        return Pin != nullptr && Pin->DefaultObject != nullptr ? Pin->DefaultObject->GetPathName() : FString();
    }

    FString GetPinDefaultTextValue(const UEdGraphPin* Pin)
    {
        return Pin != nullptr && !Pin->DefaultTextValue.IsEmpty() ? Pin->DefaultTextValue.ToString() : FString();
    }

    FString GetEffectivePinDefaultValue(const UEdGraphPin* Pin)
    {
        if (Pin == nullptr)
        {
            return FString();
        }
        if (Pin->DefaultObject != nullptr)
        {
            return Pin->DefaultObject->GetPathName();
        }
        if (!Pin->DefaultValue.IsEmpty())
        {
            return Pin->DefaultValue;
        }
        return GetPinDefaultTextValue(Pin);
    }

    FString GetPinDefaultValueSource(const UEdGraphPin* Pin)
    {
        if (Pin == nullptr)
        {
            return TEXT("none");
        }
        if (Pin->DefaultObject != nullptr)
        {
            return TEXT("object");
        }
        if (!Pin->DefaultValue.IsEmpty())
        {
            return TEXT("literal");
        }
        return Pin->DefaultTextValue.IsEmpty() ? TEXT("none") : TEXT("text");
    }

    TArray<TSharedPtr<FJsonValue>> SerializePins(const UEdGraphNode* Node)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        if (!Node) return Result;
        const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
        const UFunction* TargetFunction = CallNode != nullptr ? CallNode->GetTargetFunction() : nullptr;
        const UBlueprint* OwningBlueprint = Node->GetGraph() != nullptr
            ? Node->GetGraph()->GetTypedOuter<UBlueprint>()
            : nullptr;
        const UClass* BlueprintClass = OwningBlueprint != nullptr ? OwningBlueprint->GeneratedClass : nullptr;
        const UClass* FunctionOwner = TargetFunction != nullptr ? TargetFunction->GetOwnerClass() : nullptr;
        const bool bExternalInstanceTarget = TargetFunction != nullptr
            && !TargetFunction->HasAnyFunctionFlags(FUNC_Static)
            && FunctionOwner != nullptr
            && (BlueprintClass == nullptr || !BlueprintClass->IsChildOf(FunctionOwner));
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            const bool bIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
            const bool bIsSelf = Pin->PinName == UEdGraphSchema_K2::PN_Self;
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("pinId"), GetPinId(Pin)); Item->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
            Item->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            Item->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
            Item->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
            Item->SetStringField(TEXT("subCategoryObjectPath"),
                Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : FString());
            Item->SetStringField(TEXT("containerType"),
                StaticEnum<EPinContainerType>()->GetNameStringByValue(static_cast<int64>(Pin->PinType.ContainerType)));
            Item->SetBoolField(TEXT("isReference"), Pin->PinType.bIsReference);
            Item->SetBoolField(TEXT("isExec"), bIsExec);
            Item->SetBoolField(TEXT("isData"), !bIsExec);
            Item->SetBoolField(TEXT("isSelfPin"), bIsSelf);
            Item->SetBoolField(TEXT("requiresExplicitTarget"), bIsSelf && bExternalInstanceTarget);
            Item->SetBoolField(TEXT("isHidden"), Pin->bHidden);
            Item->SetStringField(TEXT("valueCategory"), Pin->PinType.PinValueType.TerminalCategory.ToString());
            Item->SetStringField(TEXT("valueSubCategory"), Pin->PinType.PinValueType.TerminalSubCategory.ToString());
            Item->SetStringField(TEXT("valueSubCategoryObjectPath"),
                Pin->PinType.PinValueType.TerminalSubCategoryObject.IsValid()
                    ? Pin->PinType.PinValueType.TerminalSubCategoryObject->GetPathName()
                    : FString());
            Item->SetStringField(TEXT("defaultValue"), GetEffectivePinDefaultValue(Pin));
            Item->SetStringField(TEXT("literalDefaultValue"), Pin->DefaultValue);
            Item->SetStringField(TEXT("defaultObjectPath"), GetPinDefaultObjectPath(Pin));
            Item->SetStringField(TEXT("defaultTextValue"), GetPinDefaultTextValue(Pin));
            Item->SetStringField(TEXT("defaultValueSource"), GetPinDefaultValueSource(Pin));
            Item->SetStringField(TEXT("parentPinId"), GetPinId(Pin->ParentPin));
            Item->SetNumberField(TEXT("subPinCount"), Pin->SubPins.Num());
            Item->SetNumberField(TEXT("linkedPinCount"), Pin->LinkedTo.Num()); Result.Add(MakeShared<FJsonValueObject>(Item));
        }
        return Result;
    }

    bool ResolveTypedOperatorFunction(
        const FString& RequestedOperator,
        FString& OutCanonicalName,
        UFunction*& OutFunction,
        bool& bOutSupportsTolerance,
        FString& OutError)
    {
        FString Normalized = RequestedOperator;
        Normalized.TrimStartAndEndInline();
        Normalized.ReplaceInline(TEXT("_"), TEXT(""));
        Normalized.ReplaceInline(TEXT(" "), TEXT(""));
        Normalized = Normalized.ToLower();

        FName FunctionName;
        bOutSupportsTolerance = false;
        if (Normalized == TEXT("objectequal") || Normalized == TEXT("objectequals"))
        { OutCanonicalName = TEXT("ObjectEqual"); FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_ObjectObject); }
        else if (Normalized == TEXT("objectnotequal"))
        { OutCanonicalName = TEXT("ObjectNotEqual"); FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_ObjectObject); }
        else if (Normalized == TEXT("booleanand") || Normalized == TEXT("booland"))
        { OutCanonicalName = TEXT("BooleanAnd"); FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanAND); }
        else if (Normalized == TEXT("booleanor") || Normalized == TEXT("boolor"))
        { OutCanonicalName = TEXT("BooleanOr"); FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanOR); }
        else if (Normalized == TEXT("booleannot") || Normalized == TEXT("boolnot") || Normalized == TEXT("not"))
        { OutCanonicalName = TEXT("BooleanNot"); FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Not_PreBool); }
        else if (Normalized == TEXT("vectoradd"))
        { OutCanonicalName = TEXT("VectorAdd"); FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_VectorVector); }
        else if (Normalized == TEXT("vectorsubtract") || Normalized == TEXT("vectorsub"))
        { OutCanonicalName = TEXT("VectorSubtract"); FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Subtract_VectorVector); }
        else if (Normalized == TEXT("vectornearlyequal") || Normalized == TEXT("vectorequal"))
        {
            OutCanonicalName = TEXT("VectorNearlyEqual");
            FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_VectorVector);
            bOutSupportsTolerance = true;
        }
        else
        {
            OutError = FString::Printf(
                TEXT("Unsupported typed operator '%s'. Use ObjectEqual, ObjectNotEqual, BooleanAnd, BooleanOr, BooleanNot, VectorAdd, VectorSubtract, or VectorNearlyEqual."),
                *RequestedOperator);
            return false;
        }
        OutFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName);
        if (OutFunction == nullptr)
        {
            OutError = FString::Printf(TEXT("Could not resolve Kismet operator function '%s'."), *FunctionName.ToString());
            return false;
        }
        return true;
    }

    FString ComputeGraphRevision(const UEdGraph* Graph)
    {
        if (Graph == nullptr) return FString();
        TArray<const UEdGraphNode*> Nodes;
        for (const UEdGraphNode* Node : Graph->Nodes) if (Node != nullptr) Nodes.Add(Node);
        Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B){ return A.NodeGuid < B.NodeGuid; });
        FString Canonical;
        for (const UEdGraphNode* Node : Nodes)
        {
            Canonical += FString::Printf(TEXT("N|%s|%s|%d|%d|%s\n"),
                *GetNodeGuid(Node), *Node->GetClass()->GetPathName(), Node->NodePosX, Node->NodePosY, *Node->NodeComment);
            TArray<const UEdGraphPin*> Pins;
            for (const UEdGraphPin* Pin : Node->Pins) if (Pin != nullptr) Pins.Add(Pin);
            Pins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B){ return A.PinId < B.PinId; });
            for (const UEdGraphPin* Pin : Pins)
            {
                Canonical += FString::Printf(TEXT("P|%s|%s|%d|%s|%s|%s\n"),
                    *GetPinId(Pin), *Pin->PinName.ToString(), static_cast<int32>(Pin->Direction),
                    *Pin->PinType.PinCategory.ToString(), *GetEffectivePinDefaultValue(Pin), *GetPinDefaultValueSource(Pin));
                TArray<FString> Links;
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (Linked != nullptr && Linked->GetOwningNode() != nullptr)
                        Links.Add(GetNodeGuid(Linked->GetOwningNode()) + TEXT(":") + GetPinId(Linked));
                }
                Links.Sort();
                for (const FString& Link : Links) Canonical += TEXT("L|") + Link + TEXT("\n");
            }
        }
        return FMD5::HashAnsiString(*Canonical);
    }

    UK2Node_CallFunction* CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, bool bDetached)
    {
        if (Graph == nullptr || Function == nullptr)
        {
            return nullptr;
        }
        UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
        Node->SetFromFunction(Function);
        if (bDetached)
        {
            Node->AllocateDefaultPins();
        }
        return Node;
    }

    UK2Node_IfThenElse* CreateBranchNode(UEdGraph* Graph, bool bDetached)
    {
        UK2Node_IfThenElse* Node = Graph != nullptr ? NewObject<UK2Node_IfThenElse>(Graph) : nullptr;
        if (Node != nullptr && bDetached) Node->AllocateDefaultPins();
        return Node;
    }

    UK2Node_VariableGet* CreateVariableGetNode(
        UEdGraph* Graph, FName VariableName, const FGuid& VariableGuid, bool bDetached)
    {
        UK2Node_VariableGet* Node = Graph != nullptr ? NewObject<UK2Node_VariableGet>(Graph) : nullptr;
        if (Node != nullptr)
        {
            Node->VariableReference.SetSelfMember(VariableName, VariableGuid);
            if (bDetached) Node->AllocateDefaultPins();
        }
        return Node;
    }

    UK2Node_VariableSet* CreateVariableSetNode(
        UEdGraph* Graph, FName VariableName, const FGuid& VariableGuid, bool bDetached)
    {
        UK2Node_VariableSet* Node = Graph != nullptr ? NewObject<UK2Node_VariableSet>(Graph) : nullptr;
        if (Node != nullptr)
        {
            Node->VariableReference.SetSelfMember(VariableName, VariableGuid);
            if (bDetached) Node->AllocateDefaultPins();
        }
        return Node;
    }

    UK2Node_Knot* CreateRerouteNode(UEdGraph* Graph, bool bDetached)
    {
        UK2Node_Knot* Node = Graph != nullptr ? NewObject<UK2Node_Knot>(Graph) : nullptr;
        if (Node != nullptr && bDetached) Node->AllocateDefaultPins();
        return Node;
    }

    void PlaceNewNode(UEdGraph* Graph, UEdGraphNode* Node, const FPlacement& Placement)
    {
        Graph->AddNode(Node, true, false);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Node->NodePosX = Placement.X;
        Node->NodePosY = Placement.Y;
    }

    bool SaveAndRefreshIfRequested(UBlueprint* Blueprint, const FString& ObjectPath, bool bSave, FString& OutFilename, bool& bOutIndexRefreshed, FString& OutIndexError, FString& OutError)
    {
        if (!bSave) return true;
        if (!BlueprintEditToolUtils::SaveAsset(Blueprint, OutFilename, OutError)) return false;
        bOutIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, OutIndexError);
        return true;
    }

    FNodeAdditionResult AddSimpleGraphNode(
        const FString& ObjectPath,
        const FString& GraphName,
        const FString& GraphGuid,
        const TSharedPtr<FJsonObject>& Params,
        bool bDryRun,
        bool bSave,
        const FText& TransactionDescription,
        FGraphNodeFactory CreateNode)
    {
        FNodeAdditionResult Result;
        FString ExecutionError;
        const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
        {
            UBlueprint* Blueprint = nullptr;
            if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
            {
                return false;
            }

            UEdGraph* Graph = nullptr;
            if (!ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError))
            {
                return false;
            }

            Result.Placement = ResolvePlacement(Graph, Params, OutError);
            if (!OutError.IsEmpty())
            {
                return false;
            }

            if (bDryRun)
            {
                FString FactoryError;
                UEdGraphNode* Preview = CreateNode(Blueprint, Graph, true, FactoryError);
                if (!Preview)
                {
                    OutError = FactoryError;
                    return false;
                }

                Result.Pins = SerializePins(Preview);
                return true;
            }

            const FScopedTransaction Transaction(TransactionDescription);
            Blueprint->Modify();
            Graph->Modify();
            FString FactoryError;
            UEdGraphNode* Node = CreateNode(Blueprint, Graph, false, FactoryError);
            if (!Node)
            {
                OutError = FactoryError;
                return false;
            }

            PlaceNewNode(Graph, Node, Result.Placement);
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            Result.NodeGuid = GetNodeGuid(Node);
            Result.Pins = SerializePins(Node);
            Result.bAdded = true;
            return SaveAndRefreshIfRequested(Blueprint, ObjectPath, bSave, Result.SavedFilename, Result.bIndexRefreshed, Result.IndexRefreshError, OutError);
        }, ExecutionError);

        Result.bSucceeded = bSucceeded;
        Result.bSaved = bSave && !bDryRun;
        if (!bSucceeded)
        {
            Result.ErrorMessage = ExecutionError;
        }

        return Result;
    }
}
