#include "Tools/BlueprintGraphEditToolUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Tools/BlueprintEditToolUtils.h"

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
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("pinId"), GetPinId(Pin)); Item->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
            Item->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            Item->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
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
}
