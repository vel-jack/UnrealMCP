#include "Tools/EditorVisualTools.h"

#include "Tools/BlueprintToolUtils.h"
#include "BlueprintEditorModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Layout/WidgetPath.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 8
    using FGraphViewPosition = FVector2f;
#else
    using FGraphViewPosition = FVector2D;
#endif
    // Accessed only on the game thread. References never resolve to a replacement widget.
    struct FWidgetReference
    {
        TWeakPtr<SWidget> Widget;
        TWeakPtr<SWindow> Window;
        uint64 Sequence = 0;
        TWeakPtr<SGraphEditor> FramedGraph;
        double FrameRequestedAt = 0;
        double StableSince = 0;
        FGraphViewPosition LastView = FGraphViewPosition::ZeroVector;
        float LastZoom = 0;
    };
    TMap<FString, FWidgetReference> WidgetReferences;
    uint64 ReferenceSequence = 0;

    FString RememberWidget(const TSharedRef<SWidget>& Widget)
    {
        const auto Window = FSlateApplication::Get().FindWidgetWindow(Widget);
        for (auto It = WidgetReferences.CreateIterator(); It; ++It)
        {
            if (!It.Value().Widget.IsValid() || !It.Value().Window.IsValid()) It.RemoveCurrent();
            else if (It.Value().Widget.Pin() == Widget && It.Value().Window.Pin() == Window) return It.Key();
        }
        while (WidgetReferences.Num() >= 4096)
        {
            FString Oldest;
            uint64 Sequence = MAX_uint64;
            for (const auto& Pair : WidgetReferences)
                if (Pair.Value.Sequence < Sequence) { Sequence = Pair.Value.Sequence; Oldest = Pair.Key; }
            WidgetReferences.Remove(Oldest);
        }
        const FString Ref = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
        FWidgetReference Entry;
        Entry.Widget = Widget;
        Entry.Window = Window;
        Entry.Sequence = ++ReferenceSequence;
        WidgetReferences.Add(Ref, Entry);
        return Ref;
    }

    TSharedPtr<SWidget> ResolveWidget(const FString& Ref, FString& Error)
    {
        FWidgetReference* Entry = WidgetReferences.Find(Ref);
        TSharedPtr<SWidget> Widget = Entry ? Entry->Widget.Pin() : nullptr;
        if (!Widget.IsValid() || !Entry->Window.IsValid()
            || FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) != Entry->Window.Pin())
        {
            Error = TEXT("stale_widget_ref: Take a new window/widget snapshot or frame the graph again.");
            return nullptr;
        }
        return Widget;
    }

    bool SlateAvailable(FString& Error)
    {
        if (FSlateApplication::IsInitialized()) return true;
        Error = TEXT("slate_unavailable: An interactive Unreal Editor Slate session is required.");
        return false;
    }

    TSharedRef<FJsonObject> Property(const TCHAR* Type, const TCHAR* Description)
    {
        auto Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("type"), Type);
        Value->SetStringField(TEXT("description"), Description);
        return Value;
    }

    TSharedRef<FJsonObject> Schema(const TSharedRef<FJsonObject>& Properties, const TArray<TSharedPtr<FJsonValue>>& Required = {})
    {
        auto Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("type"), TEXT("object"));
        Value->SetObjectField(TEXT("properties"), Properties);
        Value->SetBoolField(TEXT("additionalProperties"), false);
        if (!Required.IsEmpty()) Value->SetArrayField(TEXT("required"), Required);
        return Value;
    }

    TSharedRef<FJsonObject> WidgetDescription(const TSharedRef<SWidget>& Widget, bool bIncludeLabel)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("widgetRef"), RememberWidget(Widget));
        Item->SetStringField(TEXT("type"), Widget->GetTypeAsString());
        Item->SetBoolField(TEXT("visible"), Widget->GetVisibility().IsVisible());
        const FGeometry& Geometry = Widget->GetCachedGeometry();
        const FVector2D Position = Geometry.GetAbsolutePosition();
        const FVector2D Size = Geometry.GetLocalSize();
        Item->SetNumberField(TEXT("x"), Position.X);
        Item->SetNumberField(TEXT("y"), Position.Y);
        Item->SetNumberField(TEXT("width"), Size.X);
        Item->SetNumberField(TEXT("height"), Size.Y);
        if (bIncludeLabel)
        {
            FString Label;
            if (Widget->GetType() == FName(TEXT("SWindow"))) Label = StaticCastSharedRef<SWindow>(Widget)->GetTitle().ToString();
            else if (Widget->GetType() == FName(TEXT("STextBlock"))) Label = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
#if WITH_ACCESSIBILITY
            else Label = Widget->GetAccessibleText(EAccessibleType::Main).ToString();
#endif
            if (!Label.IsEmpty()) Item->SetStringField(TEXT("label"), Label.Left(256));
        }
        return Item;
    }

    TSharedRef<FJsonObject> WidgetTree(const TSharedRef<SWidget>& Widget, int32 Depth, int32 MaxDepth, int32 MaxWidgets,
        bool bIncludeLabels, int32& Count, bool& bTruncated)
    {
        ++Count;
        auto Item = WidgetDescription(Widget, bIncludeLabels);
        TArray<TSharedPtr<FJsonValue>> Children;
        FChildren* WidgetChildren = Widget->GetChildren();
        if (WidgetChildren)
        {
            for (int32 Index = 0; Index < WidgetChildren->Num(); ++Index)
            {
                if (Depth >= MaxDepth || Count >= MaxWidgets) { bTruncated = true; break; }
                Children.Add(MakeShared<FJsonValueObject>(WidgetTree(WidgetChildren->GetChildAt(Index), Depth + 1,
                    MaxDepth, MaxWidgets, bIncludeLabels, Count, bTruncated)));
            }
        }
        Item->SetArrayField(TEXT("children"), Children);
        return Item;
    }
}

FGetEditorWidgetSnapshotTool::FGetEditorWidgetSnapshotTool()
    : FMCPToolBase(TEXT("GetEditorWidgetSnapshot"), TEXT("Read-only native Slate inspection. Without windowRef lists visible editor windows; with an explicit windowRef returns a bounded widget tree. Session-local weak references are used by CaptureEditorWidget.")) {}

UnrealMCP::FMCPResponse FGetEditorWidgetSnapshotTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = BuildBooleanResult(true);
    FString Error;
    if (!UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        if (!SlateAvailable(OutError)) return false;
        FString WindowRef;
        if (Request.Params.IsValid() && Request.Params->HasField(TEXT("windowRef"))
            && (!Request.Params->TryGetStringField(TEXT("windowRef"), WindowRef) || WindowRef.IsEmpty()))
        { OutError = TEXT("windowRef must be a nonempty explicit window reference, or omitted to list windows."); return false; }
        if (WindowRef.IsEmpty())
        {
            TArray<TSharedRef<SWindow>> Windows;
            FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const auto& Window : Windows)
            {
                if (Values.Num() >= 128) break;
                auto Item = WidgetDescription(Window, false);
                Item->SetStringField(TEXT("windowRef"), Item->GetStringField(TEXT("widgetRef")));
                Item->SetStringField(TEXT("title"), Window->GetTitle().ToString().Left(512));
                Values.Add(MakeShared<FJsonValueObject>(Item));
            }
            Response.Result->SetArrayField(TEXT("windows"), Values);
            Response.Result->SetBoolField(TEXT("truncated"), Windows.Num() > Values.Num());
            Response.Result->SetNumberField(TEXT("returnedCount"), Values.Num());
            return true;
        }
        auto Root = ResolveWidget(WindowRef, OutError);
        if (!Root.IsValid()) return false;
        const auto Window = FSlateApplication::Get().FindWidgetWindow(Root.ToSharedRef());
        if (Root != Window) { OutError = TEXT("windowRef must reference an explicit window from GetEditorWidgetSnapshot."); return false; }
        double DepthValue = 4, WidgetsValue = 500;
        bool bIncludeLabels = true;
        if (Request.Params.IsValid())
        {
            if ((Request.Params->HasField(TEXT("maxDepth")) && !Request.Params->TryGetNumberField(TEXT("maxDepth"), DepthValue))
                || (Request.Params->HasField(TEXT("maxWidgets")) && !Request.Params->TryGetNumberField(TEXT("maxWidgets"), WidgetsValue))
                || (Request.Params->HasField(TEXT("includeLabels")) && (Request.Params->TryGetField(TEXT("includeLabels"))->Type != EJson::Boolean || !Request.Params->TryGetBoolField(TEXT("includeLabels"), bIncludeLabels))))
            { OutError = TEXT("maxDepth/maxWidgets must be integers and includeLabels must be boolean."); return false; }
        }
        if (!FMath::IsFinite(DepthValue) || !FMath::IsFinite(WidgetsValue) || DepthValue < 0 || DepthValue > 8 || WidgetsValue < 1 || WidgetsValue > 1000
            || FMath::FloorToDouble(DepthValue) != DepthValue || FMath::FloorToDouble(WidgetsValue) != WidgetsValue)
        { OutError = TEXT("maxDepth must be 0..8 and maxWidgets must be 1..1000."); return false; }
        const int32 MaxDepth = static_cast<int32>(DepthValue), MaxWidgets = static_cast<int32>(WidgetsValue);
        int32 Count = 0;
        bool bTruncated = false;
        Response.Result->SetObjectField(TEXT("root"), WidgetTree(Root.ToSharedRef(), 0, MaxDepth, MaxWidgets, bIncludeLabels, Count, bTruncated));
        Response.Result->SetNumberField(TEXT("returnedCount"), Count);
        Response.Result->SetBoolField(TEXT("truncated"), bTruncated);
        Response.Result->SetNumberField(TEXT("maxDepth"), MaxDepth);
        Response.Result->SetNumberField(TEXT("maxWidgets"), MaxWidgets);
        return true;
    }, Error)) return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Error);
    return Response;
}

TSharedPtr<FJsonObject> FGetEditorWidgetSnapshotTool::BuildInputSchema() const
{
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("windowRef"), Property(TEXT("string"), TEXT("Explicit window reference from a previous window listing. Omit to list windows only.")));
    auto Depth = Property(TEXT("integer"), TEXT("Tree depth 0..8, default 4."));
    Depth->SetNumberField(TEXT("minimum"), 0); Depth->SetNumberField(TEXT("maximum"), 8);
    Properties->SetObjectField(TEXT("maxDepth"), Depth);
    auto Count = Property(TEXT("integer"), TEXT("Widget count 1..1000, default 500."));
    Count->SetNumberField(TEXT("minimum"), 1); Count->SetNumberField(TEXT("maximum"), 1000);
    Properties->SetObjectField(TEXT("maxWidgets"), Count);
    Properties->SetObjectField(TEXT("includeLabels"), Property(TEXT("boolean"), TEXT("Include bounded widget text labels, default true.")));
    return Schema(Properties);
}

FCaptureEditorWidgetTool::FCaptureEditorWidgetTool()
    : FMCPToolBase(TEXT("CaptureEditorWidget"), TEXT("Capture an explicit live editor widgetRef to PNG under this project's Saved/UnrealMCP/Captures. No implicit active window or OS screenshot. Retry frame_pending after graph framing settles.")) {}

UnrealMCP::FMCPResponse FCaptureEditorWidgetTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = BuildBooleanResult(true);
    FString Error;
    if (!UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        if (!SlateAvailable(OutError)) return false;
        FString Ref;
        if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("widgetRef"), Ref) || Ref.IsEmpty())
        { OutError = TEXT("widgetRef is required; obtain an explicit reference from GetEditorWidgetSnapshot or FrameBlueprintGraph."); return false; }
        auto Widget = ResolveWidget(Ref, OutError);
        if (!Widget.IsValid()) return false;
        auto Window = FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef());
        for (auto Ancestor = Widget; Ancestor.IsValid(); Ancestor = Ancestor->GetParentWidget())
            if (!Ancestor->GetVisibility().IsVisible()) { OutError = TEXT("widget_not_visible: Widget or ancestor is hidden."); return false; }
        if (Window->IsWindowMinimized()) { OutError = TEXT("widget_not_visible: Window is minimized."); return false; }
        FWidgetPath Path;
        if (!FSlateApplication::Get().GeneratePathToWidgetUnchecked(Widget.ToSharedRef(), Path))
        { OutError = TEXT("widget_not_visible: Widget is no longer in a visible window hierarchy."); return false; }
        const auto Arranged = Path.FindArrangedWidget(Widget.ToSharedRef());
        if (!Arranged.IsSet()) { OutError = TEXT("invalid_capture_geometry: Widget is not arranged in its window."); return false; }
        // Match Slate's capture geometry instead of trusting potentially stale cached size.
        const FVector2D PixelSize = Arranged.GetValue().Geometry.GetDrawSize();
        if (!FMath::IsFinite(PixelSize.X) || !FMath::IsFinite(PixelSize.Y) || PixelSize.X <= 0 || PixelSize.Y <= 0
            || PixelSize.X > 16000000 || PixelSize.Y > 16000000 || FMath::CeilToDouble(PixelSize.X) * FMath::CeilToDouble(PixelSize.Y) > 16000000)
        { OutError = TEXT("invalid_capture_geometry: Widget must have nonzero laid-out geometry within 16 million pixels."); return false; }
        auto* Entry = WidgetReferences.Find(Ref);
        if (auto Graph = Entry->FramedGraph.Pin())
        {
            FGraphViewPosition View;
            float Zoom = 0;
            Graph->GetViewLocation(View, Zoom);
            const double Now = FPlatformTime::Seconds();
            const bool bSameView = View.Equals(Entry->LastView, 0.01f) && FMath::IsNearlyEqual(Zoom, Entry->LastZoom, 0.0001f);
            if (!bSameView) { Entry->LastView = View; Entry->LastZoom = Zoom; Entry->StableSince = Now; }
            if (!bSameView || Now - Entry->FrameRequestedAt < 0.5 || Now - Entry->StableSince < 0.2)
            { OutError = TEXT("frame_pending: Graph layout/zoom has not settled. Retry CaptureEditorWidget with the same reference after at least 0.2 seconds."); return false; }
        }
        TArray<FColor> Pixels;
        FIntVector Size(0, 0, 0);
        if (!FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Size) || Size.X <= 0 || Size.Y <= 0
            || static_cast<int64>(Size.X) * Size.Y > 16000000)
        { OutError = TEXT("capture_failed: Slate could not capture the attached widget within the pixel limit."); return false; }
        TArray64<uint8> Png;
        FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), static_cast<int64>(Size.X) * Size.Y), Png);
        const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMCP/Captures")));
        const FString Filename = FPaths::Combine(Directory, FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower) + TEXT(".png"));
        if (Png.IsEmpty() || !IFileManager::Get().MakeDirectory(*Directory, true) || !FFileHelper::SaveArrayToFile(Png, *Filename))
        { OutError = TEXT("capture_write_failed: Could not write PNG under this project's Saved/UnrealMCP/Captures."); return false; }
        Response.Result->SetStringField(TEXT("filename"), Filename);
        Response.Result->SetNumberField(TEXT("width"), Size.X);
        Response.Result->SetNumberField(TEXT("height"), Size.Y);
        Response.Result->SetNumberField(TEXT("filebytes"), static_cast<double>(Png.Num()));
        Response.Result->SetStringField(TEXT("widgetRef"), Ref);
        return true;
    }, Error)) return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Error);
    return Response;
}

TSharedPtr<FJsonObject> FCaptureEditorWidgetTool::BuildInputSchema() const
{
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("widgetRef"), Property(TEXT("string"), TEXT("Required session-local widget/window reference from GetEditorWidgetSnapshot or FrameBlueprintGraph.")));
    return Schema(Properties, {MakeShared<FJsonValueString>(TEXT("widgetRef"))});
}

FFrameBlueprintGraphTool::FFrameBlueprintGraphTool()
    : FMCPToolBase(TEXT("FrameBlueprintGraph"), TEXT("Open an exact Blueprint graph and frame all nodes or explicit node GUIDs for visual review. Changes editor view/selection only; no compile/save/asset mutation. dryRun validates without changing UI. Capture returned widgetRef after framing settles.")) {}

UnrealMCP::FMCPResponse FFrameBlueprintGraphTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = BuildBooleanResult(true);
    FString Error;
    if (!UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        FString ObjectPath, GraphName, GraphGuidString;
        bool bDryRun = false;
        if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
        { OutError = TEXT("objectPath is required."); return false; }
        if ((Request.Params->HasField(TEXT("graphName")) && !Request.Params->TryGetStringField(TEXT("graphName"), GraphName))
            || (Request.Params->HasField(TEXT("graphGuid")) && !Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuidString))
            || (Request.Params->HasField(TEXT("dryRun")) && (Request.Params->TryGetField(TEXT("dryRun"))->Type != EJson::Boolean || !Request.Params->TryGetBoolField(TEXT("dryRun"), bDryRun))))
        { OutError = TEXT("graphName/graphGuid must be strings and dryRun must be boolean."); return false; }
        if (GraphName.IsEmpty() && GraphGuidString.IsEmpty()) { OutError = TEXT("graphName or graphGuid is required."); return false; }
        FGuid GraphGuid;
        if (!GraphGuidString.IsEmpty() && !FGuid::Parse(GraphGuidString, GraphGuid)) { OutError = TEXT("graphGuid must be a GUID."); return false; }
        UBlueprint* Blueprint = UnrealMCP::BlueprintToolUtils::LoadBlueprintFromObjectPath(ObjectPath);
        if (!Blueprint) { OutError = TEXT("Blueprint objectPath could not be loaded."); return false; }
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        UEdGraph* Target = nullptr;
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph && (GraphName.IsEmpty() || Graph->GetName() == GraphName) && (GraphGuidString.IsEmpty() || Graph->GraphGuid == GraphGuid))
            {
                if (Target) { OutError = TEXT("Graph selector is ambiguous; specify graphGuid."); return false; }
                Target = Graph;
            }
        }
        if (!Target) { OutError = TEXT("Exact Blueprint graph was not found; graphName and graphGuid, when both supplied, must match the same graph."); return false; }
        TArray<UEdGraphNode*> Nodes;
        const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
        if (Request.Params->HasField(TEXT("nodeGuids")))
        {
            if (!Request.Params->TryGetArrayField(TEXT("nodeGuids"), NodeValues) || NodeValues->Num() > 128)
            { OutError = TEXT("nodeGuids must be an array of at most 128 GUID strings."); return false; }
            for (const auto& Value : *NodeValues)
            {
                FString Text;
                FGuid Guid;
                if (!Value.IsValid() || !Value->TryGetString(Text) || !FGuid::Parse(Text, Guid))
                { OutError = TEXT("Each nodeGuids entry must be a GUID string."); return false; }
                UEdGraphNode* Match = nullptr;
                for (UEdGraphNode* Node : Target->Nodes) if (Node && Node->NodeGuid == Guid)
                {
                    if (Match) { OutError = TEXT("Duplicate node identity in graph; framing is ambiguous."); return false; }
                    Match = Node;
                }
                if (!Match) { OutError = FString::Printf(TEXT("Node GUID %s is not in the selected graph."), *Text); return false; }
                Nodes.AddUnique(Match);
            }
        }
        Response.Result->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
        Response.Result->SetStringField(TEXT("graphName"), Target->GetName());
        Response.Result->SetStringField(TEXT("graphGuid"), Target->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Response.Result->SetNumberField(TEXT("selectedNodeCount"), Nodes.Num());
        TArray<TSharedPtr<FJsonValue>> NodeGuids;
        for (const UEdGraphNode* Node : Nodes) NodeGuids.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
        Response.Result->SetArrayField(TEXT("nodeGuids"), NodeGuids);
        Response.Result->SetBoolField(TEXT("wholeGraph"), Nodes.IsEmpty());
        Response.Result->SetBoolField(TEXT("dryRun"), bDryRun);
        Response.Result->SetBoolField(TEXT("framingPending"), !bDryRun);
        if (bDryRun) return true;
        if (!SlateAvailable(OutError)) return false;
        const auto Editor = FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, true);
        const auto GraphWidget = Editor.IsValid() ? Editor->OpenGraphAndBringToFront(Target, true) : nullptr;
        if (!GraphWidget.IsValid() || GraphWidget->GetCurrentGraph() != Target)
        { OutError = TEXT("graph_editor_unavailable: Could not open the exact graph for visual review."); return false; }
        GraphWidget->ClearSelectionSet();
        for (UEdGraphNode* Node : Nodes) GraphWidget->SetNodeSelection(Node, true);
        GraphWidget->ZoomToFit(!Nodes.IsEmpty());
        const FString Ref = RememberWidget(GraphWidget.ToSharedRef());
        auto& Entry = WidgetReferences.FindChecked(Ref);
        Entry.FramedGraph = GraphWidget;
        Entry.FrameRequestedAt = Entry.StableSince = FPlatformTime::Seconds();
        GraphWidget->GetViewLocation(Entry.LastView, Entry.LastZoom);
        Response.Result->SetStringField(TEXT("widgetRef"), Ref);
        Response.Result->SetStringField(TEXT("captureHint"), TEXT("Retry CaptureEditorWidget after at least 0.5 seconds; frame_pending requires another observation after 0.2 seconds."));
        return true;
    }, Error)) return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Error);
    return Response;
}

TSharedPtr<FJsonObject> FFrameBlueprintGraphTool::BuildInputSchema() const
{
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), Property(TEXT("string"), TEXT("Exact Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), Property(TEXT("string"), TEXT("Exact graph name; ambiguity is rejected. graphName or graphGuid is required.")));
    Properties->SetObjectField(TEXT("graphGuid"), Property(TEXT("string"), TEXT("Exact graph GUID. If graphName is also supplied both must match.")));
    auto Nodes = Property(TEXT("array"), TEXT("Up to 128 exact node GUIDs belonging to the graph. Omit/empty frames the whole graph."));
    Nodes->SetObjectField(TEXT("items"), Property(TEXT("string"), TEXT("Node GUID.")));
    Nodes->SetNumberField(TEXT("maxItems"), 128);
    Properties->SetObjectField(TEXT("nodeGuids"), Nodes);
    Properties->SetObjectField(TEXT("dryRun"), Property(TEXT("boolean"), TEXT("Validate graph and node identities without opening/focusing/selecting, default false.")));
    return Schema(Properties, {MakeShared<FJsonValueString>(TEXT("objectPath"))});
}
