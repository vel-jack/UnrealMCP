#include "Tools/LayoutBlueprintNodesTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    struct FNodeLayout
    {
        UEdGraphNode* Node = nullptr;
        int32 Layer = 0;
        int32 NewX = 0;
        int32 NewY = 0;
    };

    bool NodeSort(const UEdGraphNode& A, const UEdGraphNode& B)
    {
        return A.NodePosY == B.NodePosY ? A.NodePosX < B.NodePosX : A.NodePosY < B.NodePosY;
    }
}

FLayoutBlueprintNodesTool::FLayoutBlueprintNodesTool()
    : FMCPToolBase(
        TEXT("LayoutBlueprintNodes"),
        TEXT("Deterministically lays out an explicit Blueprint node set by execution-flow depth with configurable spacing, collision avoidance, dry-run, and optional save."))
{
}

UnrealMCP::FMCPResponse FLayoutBlueprintNodesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
    const TArray<TSharedPtr<FJsonValue>>* NodeGuidValues = nullptr;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetArrayField(TEXT("nodeGuids"), NodeGuidValues)
        || NodeGuidValues == nullptr
        || NodeGuidValues->IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("LayoutBlueprintNodes requires objectPath, graph selector, and a non-empty nodeGuids array."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));
    }

    TArray<FString> NodeGuids;
    TSet<FString> UniqueGuids;
    for (const TSharedPtr<FJsonValue>& Value : *NodeGuidValues)
    {
        FString Guid;
        if (!Value.IsValid() || !Value->TryGetString(Guid) || Guid.IsEmpty())
        {
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Every nodeGuids item must be a non-empty GUID string."));
        }
        if (!UniqueGuids.Contains(Guid))
        {
            UniqueGuids.Add(Guid);
            NodeGuids.Add(Guid);
        }
    }

    double HorizontalValue = 360.0;
    double VerticalValue = 220.0;
    double StartXValue = 0.0;
    double StartYValue = 0.0;
    Request.Params->TryGetNumberField(TEXT("horizontalSpacing"), HorizontalValue);
    Request.Params->TryGetNumberField(TEXT("verticalSpacing"), VerticalValue);
    const bool bHasStartX = Request.Params->TryGetNumberField(TEXT("startX"), StartXValue);
    const bool bHasStartY = Request.Params->TryGetNumberField(TEXT("startY"), StartYValue);
    if (bHasStartX != bHasStartY)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("startX and startY must be provided together."));
    }
    const int32 HorizontalSpacing = FMath::Clamp(FMath::RoundToInt(HorizontalValue), 260, 1200);
    const int32 VerticalSpacing = FMath::Clamp(FMath::RoundToInt(VerticalValue), 160, 800);
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool bChanged = false;
    bool bCollisionAdjusted = false;
    bool bIndexRefreshed = false;
    FString SavedFilename;
    FString IndexRefreshError;
    FString ExecutionError;
    TArray<TSharedPtr<FJsonValue>> LayoutResults;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
        {
            return false;
        }
        UEdGraph* Graph = nullptr;
        if (!BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError))
        {
            return false;
        }

        TArray<UEdGraphNode*> SelectedNodes;
        TSet<UEdGraphNode*> SelectedSet;
        for (const FString& Guid : NodeGuids)
        {
            UEdGraphNode* Node = nullptr;
            if (!BlueprintGraphEditToolUtils::ResolveNode(Graph, Guid, Node, OutError))
            {
                return false;
            }
            SelectedNodes.Add(Node);
            SelectedSet.Add(Node);
        }

        TMap<UEdGraphNode*, int32> InDegree;
        TMap<UEdGraphNode*, TArray<UEdGraphNode*>> Successors;
        TMap<UEdGraphNode*, int32> Layers;
        for (UEdGraphNode* Node : SelectedNodes)
        {
            InDegree.Add(Node, 0);
            Layers.Add(Node, 0);
        }
        for (UEdGraphNode* Node : SelectedNodes)
        {
            TSet<UEdGraphNode*> UniqueSuccessors;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin == nullptr || Pin->Direction != EGPD_Output || !UEdGraphSchema_K2::IsExecPin(*Pin))
                {
                    continue;
                }
                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    UEdGraphNode* Target = LinkedPin != nullptr ? LinkedPin->GetOwningNode() : nullptr;
                    if (Target != nullptr && SelectedSet.Contains(Target) && Target != Node)
                    {
                        UniqueSuccessors.Add(Target);
                    }
                }
            }
            for (UEdGraphNode* Target : UniqueSuccessors)
            {
                Successors.FindOrAdd(Node).Add(Target);
                ++InDegree.FindChecked(Target);
            }
        }

        TArray<UEdGraphNode*> Queue;
        for (UEdGraphNode* Node : SelectedNodes)
        {
            if (InDegree.FindChecked(Node) == 0)
            {
                Queue.Add(Node);
            }
        }
        Queue.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return NodeSort(A, B); });
        for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
        {
            UEdGraphNode* Node = Queue[QueueIndex];
            for (UEdGraphNode* Target : Successors.FindRef(Node))
            {
                Layers.FindChecked(Target) = FMath::Max(Layers.FindChecked(Target), Layers.FindChecked(Node) + 1);
                int32& TargetDegree = InDegree.FindChecked(Target);
                if (--TargetDegree == 0)
                {
                    Queue.Add(Target);
                }
            }
        }

        int32 StartX = bHasStartX ? FMath::RoundToInt(StartXValue) : MAX_int32;
        int32 StartY = bHasStartY ? FMath::RoundToInt(StartYValue) : MAX_int32;
        if (!bHasStartX)
        {
            for (UEdGraphNode* Node : SelectedNodes)
            {
                StartX = FMath::Min(StartX, Node->NodePosX);
                StartY = FMath::Min(StartY, Node->NodePosY);
            }
        }

        TMap<int32, TArray<UEdGraphNode*>> NodesByLayer;
        for (UEdGraphNode* Node : SelectedNodes)
        {
            NodesByLayer.FindOrAdd(Layers.FindChecked(Node)).Add(Node);
        }
        TArray<FNodeLayout> Layouts;
        for (TPair<int32, TArray<UEdGraphNode*>>& Pair : NodesByLayer)
        {
            Pair.Value.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return NodeSort(A, B); });
            for (int32 Row = 0; Row < Pair.Value.Num(); ++Row)
            {
                FNodeLayout Layout;
                Layout.Node = Pair.Value[Row];
                Layout.Layer = Pair.Key;
                Layout.NewX = StartX + Pair.Key * HorizontalSpacing;
                Layout.NewY = StartY + Row * VerticalSpacing;
                Layouts.Add(Layout);
            }
        }

        int32 GlobalYOffset = 0;
        bool bHasCollision = true;
        for (int32 Attempt = 0; Attempt < 50 && bHasCollision; ++Attempt)
        {
            bHasCollision = false;
            for (const FNodeLayout& Layout : Layouts)
            {
                for (UEdGraphNode* Other : Graph->Nodes)
                {
                    if (Other != nullptr && !SelectedSet.Contains(Other)
                        && FMath::Abs(Other->NodePosX - Layout.NewX) < 260
                        && FMath::Abs(Other->NodePosY - (Layout.NewY + GlobalYOffset)) < 140)
                    {
                        GlobalYOffset += VerticalSpacing;
                        bCollisionAdjusted = true;
                        bHasCollision = true;
                        break;
                    }
                }
                if (bHasCollision)
                {
                    break;
                }
            }
        }

        for (FNodeLayout& Layout : Layouts)
        {
            Layout.NewY += GlobalYOffset;
            bChanged |= Layout.Node->NodePosX != Layout.NewX || Layout.Node->NodePosY != Layout.NewY;
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("nodeGuid"), BlueprintGraphEditToolUtils::GetNodeGuid(Layout.Node));
            Item->SetStringField(TEXT("nodeTitle"), Layout.Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Item->SetNumberField(TEXT("layer"), Layout.Layer);
            Item->SetNumberField(TEXT("previousX"), Layout.Node->NodePosX);
            Item->SetNumberField(TEXT("previousY"), Layout.Node->NodePosY);
            Item->SetNumberField(TEXT("positionX"), Layout.NewX);
            Item->SetNumberField(TEXT("positionY"), Layout.NewY);
            LayoutResults.Add(MakeShared<FJsonValueObject>(Item));
        }
        if (bDryRun || !bChanged)
        {
            return true;
        }

        const FScopedTransaction Transaction(
            NSLOCTEXT("UnrealMCP", "LayoutBlueprintNodes", "UnrealMCP Layout Blueprint Nodes"));
        Blueprint->Modify();
        Graph->Modify();
        for (const FNodeLayout& Layout : Layouts)
        {
            Layout.Node->Modify();
            Layout.Node->NodePosX = Layout.NewX;
            Layout.Node->NodePosY = Layout.NewY;
        }
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        return BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
            Blueprint,
            ObjectPath,
            bSave,
            SavedFilename,
            bIndexRefreshed,
            IndexRefreshError,
            OutError);
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);
    }

    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetNumberField(TEXT("nodeCount"), NodeGuids.Num());
    Result->SetNumberField(TEXT("horizontalSpacing"), HorizontalSpacing);
    Result->SetNumberField(TEXT("verticalSpacing"), VerticalSpacing);
    Result->SetBoolField(TEXT("collisionAdjusted"), bCollisionAdjusted);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("wouldChange"), bChanged);
    Result->SetBoolField(TEXT("changed"), bChanged && !bDryRun);
    Result->SetArrayField(TEXT("nodes"), LayoutResults);
    Result->SetBoolField(TEXT("saved"), bSave && bChanged && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FLayoutBlueprintNodesTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact graph GUID.")));
    TSharedRef<FJsonObject> NodeGuids = MakeShared<FJsonObject>();
    NodeGuids->SetStringField(TEXT("type"), TEXT("array"));
    NodeGuids->SetStringField(TEXT("description"), TEXT("Explicit stable node GUIDs to arrange."));
    TSharedRef<FJsonObject> GuidItem = MakeShared<FJsonObject>();
    GuidItem->SetStringField(TEXT("type"), TEXT("string"));
    NodeGuids->SetObjectField(TEXT("items"), GuidItem);
    NodeGuids->SetNumberField(TEXT("minItems"), 1);
    Properties->SetObjectField(TEXT("nodeGuids"), NodeGuids);
    TSharedRef<FJsonObject> IntegerProperty = MakeShared<FJsonObject>();
    IntegerProperty->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("startX"), IntegerProperty);
    Properties->SetObjectField(TEXT("startY"), IntegerProperty);
    Properties->SetObjectField(TEXT("horizontalSpacing"), IntegerProperty);
    Properties->SetObjectField(TEXT("verticalSpacing"), IntegerProperty);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Return proposed positions without moving nodes.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("nodeGuids"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
