#include "Tools/Editor/EditorVisualTools.h"

#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"
#include "BlueprintEditorModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "EdGraphSchema_K2.h"
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
        FString ObjectPath;
        FString GraphGuid;
        FString GraphName;
        TOptional<FSlateRect> TileBounds;
        TOptional<FSlateRect> RegionBounds;
        TWeakPtr<SWidget> GraphPanel;
        float MinimumZoom = 0;
        int32 TileIndex = 0;
        int32 TileCount = 1;
    };
    TMap<FString, FWidgetReference> WidgetReferences;
    uint64 ReferenceSequence = 0;

    FString RememberWidget(const TSharedRef<SWidget>& Widget, bool bNewFrame = false)
    {
        const auto Window = FSlateApplication::Get().FindWidgetWindow(Widget);
        for (auto It = WidgetReferences.CreateIterator(); It; ++It)
        {
            if (!It.Value().Widget.IsValid() || !It.Value().Window.IsValid()) It.RemoveCurrent();
            else if (It.Value().Widget.Pin() == Widget && It.Value().Window.Pin() == Window)
            {
                if (!bNewFrame) return It.Key();
                It.RemoveCurrent();
            }
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

    TSharedPtr<SWidget> FindVisibleWidget(const TSharedRef<SWidget>& Root,
        TFunctionRef<bool(const TSharedRef<SWidget>&)> Matches, int32& Remaining)
    {
        if (Remaining-- <= 0 || !Root->GetVisibility().IsVisible()) return nullptr;
        if (Matches(Root)) return Root;
        if (FChildren* Children = Root->GetChildren())
            for (int32 Index = 0; Index < Children->Num() && Remaining > 0; ++Index)
                if (auto Match = FindVisibleWidget(Children->GetChildAt(Index), Matches, Remaining)) return Match;
        return nullptr;
    }

    TSharedPtr<SGraphEditor> FindBackgroundGraph(const UEdGraph* Target)
    {
        int32 Remaining = 50000;
        for (const auto& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
        {
            if (Window->IsWindowMinimized()) continue;
            auto Match = FindVisibleWidget(Window, [Target](const TSharedRef<SWidget>& Widget)
            {
                return Widget->GetType() == FName(TEXT("SGraphEditor"))
                    && StaticCastSharedRef<SGraphEditor>(Widget)->GetCurrentGraph() == Target;
            }, Remaining);
            if (Match) return StaticCastSharedPtr<SGraphEditor>(Match);
        }
        return nullptr;
    }

    TSharedRef<FJsonObject> BoundsJson(const FSlateRect& Rect)
    {
        auto Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("left"), Rect.Left);
        Object->SetNumberField(TEXT("top"), Rect.Top);
        Object->SetNumberField(TEXT("right"), Rect.Right);
        Object->SetNumberField(TEXT("bottom"), Rect.Bottom);
        return Object;
    }

    bool Covers(const FSlateRect& View, const FSlateRect& Target)
    {
        constexpr float Tolerance = 0.5f;
        return View.Left <= Target.Left + Tolerance && View.Top <= Target.Top + Tolerance
            && View.Right >= Target.Right - Tolerance && View.Bottom >= Target.Bottom - Tolerance;
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
    : FMCPToolBase(TEXT("CaptureEditorWidget"), TEXT("Capture an explicit live editor widgetRef to PNG under this project's Saved/UnrealMCP/Captures. Waits for pending graph framing on adapter requests. Returns exact widget and framing evidence.")) {}

UnrealMCP::FMCPResponse FCaptureEditorWidgetTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = BuildBooleanResult(true);
    FString Error;
    int32 WaitMs = 1500;
    if (Request.Params && Request.Params->HasField(TEXT("waitForFrameMs"))
        && (!Request.Params->TryGetNumberField(TEXT("waitForFrameMs"), WaitMs) || WaitMs < 0 || WaitMs > 5000))
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("waitForFrameMs must be between 0 and 5000."));
    const double StartedAt = FPlatformTime::Seconds();
    const auto TryCapture = [&](FString& OutError)
    {
        if (!SlateAvailable(OutError)) return false;
        FString Ref;
        if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("widgetRef"), Ref) || Ref.IsEmpty())
        { OutError = TEXT("widgetRef is required; obtain an explicit reference from GetEditorWidgetSnapshot or FrameBlueprintGraph."); return false; }
        auto Widget = ResolveWidget(Ref, OutError);
        if (!Widget.IsValid()) return false;
        auto Window = FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef());
        if (!Window) { OutError = TEXT("widget_not_visible: No owning window."); return false; }
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
            { OutError = TEXT("frame_pending: Graph layout or zoom has not settled."); return false; }
            Response.Result->SetBoolField(TEXT("framingComplete"), true);
            Response.Result->SetStringField(TEXT("objectPath"), Entry->ObjectPath);
            Response.Result->SetStringField(TEXT("graphGuid"), Entry->GraphGuid);
            Response.Result->SetStringField(TEXT("graphName"), Entry->GraphName);
            Response.Result->SetNumberField(TEXT("zoom"), Zoom);
            if (Entry->TileBounds.IsSet())
            {
                auto Panel = Entry->GraphPanel.Pin();
                if (!Panel || !FMath::IsFinite(Zoom) || Zoom <= 0)
                { OutError = TEXT("invalid_capture_geometry: Graph panel or its zoom is unavailable."); return false; }
                const FVector2D Extent = FVector2D(Panel->GetCachedGeometry().GetLocalSize()) / Zoom;
                if (Extent.X <= 0 || Extent.Y <= 0)
                { OutError = TEXT("invalid_capture_geometry: Graph panel has empty geometry."); return false; }
                const FSlateRect CapturedBounds(View.X, View.Y, View.X + Extent.X, View.Y + Extent.Y);
                Response.Result->SetObjectField(TEXT("framedBounds"), BoundsJson(CapturedBounds));
                Response.Result->SetObjectField(TEXT("tileBounds"), BoundsJson(Entry->TileBounds.GetValue()));
                Response.Result->SetObjectField(TEXT("requestedBounds"), BoundsJson(Entry->RegionBounds.GetValue()));
                Response.Result->SetBoolField(TEXT("clipped"), !Covers(CapturedBounds, Entry->TileBounds.GetValue()));
                Response.Result->SetBoolField(TEXT("regionClipped"), !Covers(CapturedBounds, Entry->RegionBounds.GetValue()));
                Response.Result->SetBoolField(TEXT("minimumZoomMet"), Zoom + 0.001f >= Entry->MinimumZoom);
                Response.Result->SetNumberField(TEXT("tileIndex"), Entry->TileIndex);
                Response.Result->SetNumberField(TEXT("tileCount"), Entry->TileCount);
            }
        }
        else Response.Result->SetBoolField(TEXT("framingComplete"), false);
        TArray<FColor> Pixels;
        FIntVector Size(0, 0, 0);
        if (!FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Size) || Size.X <= 0 || Size.Y <= 0
            || static_cast<int64>(Size.X) * Size.Y > 16000000 || Pixels.Num() != static_cast<int64>(Size.X) * Size.Y)
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
        Response.Result->SetStringField(TEXT("windowRef"), RememberWidget(Window.ToSharedRef()));
        Response.Result->SetStringField(TEXT("projectPath"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
        Response.Result->SetNumberField(TEXT("waitedMs"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
        return true;
    };
    do
    {
        Error.Reset();
        if (UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(TryCapture, Error)) return Response;
        if (IsInGameThread() || !Error.StartsWith(TEXT("frame_pending")) || FPlatformTime::Seconds() >= StartedAt + WaitMs / 1000.0)
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Error);
        FPlatformProcess::Sleep(0.05f);
    } while (true);
}

TSharedPtr<FJsonObject> FCaptureEditorWidgetTool::BuildInputSchema() const
{
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("widgetRef"), Property(TEXT("string"), TEXT("Required session-local widget/window reference from GetEditorWidgetSnapshot or FrameBlueprintGraph.")));
    Properties->SetObjectField(TEXT("waitForFrameMs"), Property(TEXT("integer"), TEXT("Maximum wait on adapter requests, 0..5000 ms, default 1500. 0 is a single observation.")));
    return Schema(Properties, {MakeShared<FJsonValueString>(TEXT("widgetRef"))});
}

FFrameBlueprintGraphTool::FFrameBlueprintGraphTool()
    : FMCPToolBase(TEXT("FrameBlueprintGraph"), TEXT("Frame an exact Blueprint graph, node set or graph-space region for visual review. Supports upstream data nodes, measured tiles and no-focus framing of an already visible graph.")) {}

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
        bool bBackgroundOnly = false, bIncludeData = false;
        double Padding = 32, MinimumZoom = 0.8;
        int32 TileIndex = 0;
        const auto& Params = Request.Params;
        if ((Params->HasField(TEXT("backgroundOnly")) && !Params->TryGetBoolField(TEXT("backgroundOnly"), bBackgroundOnly))
            || (Params->HasField(TEXT("includeConnectedData")) && !Params->TryGetBoolField(TEXT("includeConnectedData"), bIncludeData))
            || (Params->HasField(TEXT("padding")) && !Params->TryGetNumberField(TEXT("padding"), Padding))
            || (Params->HasField(TEXT("minimumZoom")) && !Params->TryGetNumberField(TEXT("minimumZoom"), MinimumZoom))
            || (Params->HasField(TEXT("tileIndex")) && !Params->TryGetNumberField(TEXT("tileIndex"), TileIndex))
            || !FMath::IsFinite(Padding) || Padding < 0 || Padding > 1024
            || !FMath::IsFinite(MinimumZoom) || MinimumZoom < 0.1 || MinimumZoom > 1.8 || TileIndex < 0 || TileIndex >= 32)
        { OutError = TEXT("Invalid background, padding, minimumZoom or tileIndex settings."); return false; }
        TOptional<FSlateRect> Region;
        if (Params->HasField(TEXT("region")))
        {
            const TSharedPtr<FJsonObject>* Rect = nullptr;
            double Left, Top, Right, Bottom;
            if (!Nodes.IsEmpty() || bIncludeData || !Params->TryGetObjectField(TEXT("region"), Rect)
                || !(*Rect)->TryGetNumberField(TEXT("left"), Left) || !(*Rect)->TryGetNumberField(TEXT("top"), Top)
                || !(*Rect)->TryGetNumberField(TEXT("right"), Right) || !(*Rect)->TryGetNumberField(TEXT("bottom"), Bottom)
                || !FMath::IsFinite(Left) || !FMath::IsFinite(Top) || !FMath::IsFinite(Right) || !FMath::IsFinite(Bottom)
                || FMath::Abs(Left) > 10000000 || FMath::Abs(Top) > 10000000 || FMath::Abs(Right) > 10000000 || FMath::Abs(Bottom) > 10000000
                || Right - Left < 1 || Bottom - Top < 1)
            { OutError = TEXT("region requires ordered graph-space left/top/right/bottom bounds; omit nodeGuids and includeConnectedData."); return false; }
            Region.Emplace(Left, Top, Right, Bottom);
        }
        const bool bMeasuredFrame = Region.IsSet() || bIncludeData || Params->HasField(TEXT("padding"))
            || Params->HasField(TEXT("minimumZoom")) || Params->HasField(TEXT("tileIndex"));
        if (bIncludeData)
        {
            if (Nodes.IsEmpty()) { OutError = TEXT("includeConnectedData requires explicit nodeGuids."); return false; }
            TSet<UEdGraphNode*> Included;
            for (UEdGraphNode* Node : Nodes) Included.Add(Node);
            for (int32 Index = 0; Index < Nodes.Num(); ++Index)
                for (const UEdGraphPin* Pin : Nodes[Index]->Pins)
                {
                    if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
                    for (const UEdGraphPin* Link : Pin->LinkedTo)
                    {
                        UEdGraphNode* Source = Link ? Link->GetOwningNodeUnchecked() : nullptr;
                        if (!Source || Source->GetGraph() != Target || Included.Contains(Source)
                            || Source->Pins.ContainsByPredicate([](const UEdGraphPin* SourcePin)
                                { return SourcePin && SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec; })) continue;
                        if (Nodes.Num() >= 128) { OutError = TEXT("Connected data expansion exceeds 128 nodes."); return false; }
                        Included.Add(Source);
                        Nodes.Add(Source);
                    }
                }
        }
        Response.Result->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
        Response.Result->SetStringField(TEXT("graphName"), Target->GetName());
        Response.Result->SetStringField(TEXT("graphGuid"), Target->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Response.Result->SetNumberField(TEXT("selectedNodeCount"), Nodes.Num());
        TArray<TSharedPtr<FJsonValue>> NodeGuids;
        for (const UEdGraphNode* Node : Nodes) NodeGuids.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
        Response.Result->SetArrayField(TEXT("nodeGuids"), NodeGuids);
        Response.Result->SetBoolField(TEXT("wholeGraph"), Nodes.IsEmpty() && !Region.IsSet());
        Response.Result->SetBoolField(TEXT("backgroundOnly"), bBackgroundOnly);
        if (Region.IsSet()) Response.Result->SetObjectField(TEXT("requestedRegion"), BoundsJson(Region.GetValue()));
        Response.Result->SetBoolField(TEXT("dryRun"), bDryRun);
        Response.Result->SetBoolField(TEXT("framingPending"), !bDryRun);
        if (bDryRun) return true;
        if (!SlateAvailable(OutError)) return false;
        TSharedPtr<SGraphEditor> GraphWidget;
        if (bBackgroundOnly)
        {
            GraphWidget = FindBackgroundGraph(Target);
            if (!GraphWidget)
            { OutError = TEXT("graph_not_visible: Background framing requires an already visible graph. Open its tab explicitly once."); return false; }
        }
        else
        {
            const auto Editor = FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, true);
            if (Editor) GraphWidget = Editor->OpenGraphAndBringToFront(Target, true);
        }
        if (!GraphWidget || GraphWidget->GetCurrentGraph() != Target)
        { OutError = TEXT("graph_editor_unavailable: Could not open the exact graph for visual review."); return false; }

        TOptional<FSlateRect> TileBounds;
        TSharedPtr<SWidget> Panel;
        int32 TileCount = 1;
        if (bMeasuredFrame)
        {
            if (!Region.IsSet())
            {
                auto AddBounds = [&](UEdGraphNode* Node)
                {
                    if (!Node) return true;
                    FSlateRect Bounds;
                    if (!GraphWidget->GetBoundsForNode(Node, Bounds, 0) || Bounds.Right <= Bounds.Left || Bounds.Bottom <= Bounds.Top)
                        return false;
                    if (!Region.IsSet()) Region = Bounds;
                    else
                    {
                        auto& Existing = Region.GetValue();
                        Existing.Left = FMath::Min(Existing.Left, Bounds.Left);
                        Existing.Top = FMath::Min(Existing.Top, Bounds.Top);
                        Existing.Right = FMath::Max(Existing.Right, Bounds.Right);
                        Existing.Bottom = FMath::Max(Existing.Bottom, Bounds.Bottom);
                    }
                    return true;
                };
                if (Nodes.IsEmpty())
                {
                    for (UEdGraphNode* Node : Target->Nodes)
                        if (!AddBounds(Node)) { OutError = TEXT("graph_layout_unavailable: A node is not laid out yet. Retry after opening the graph."); return false; }
                }
                else for (UEdGraphNode* Node : Nodes)
                    if (!AddBounds(Node)) { OutError = TEXT("graph_layout_unavailable: A requested node is not laid out yet."); return false; }
            }
            int32 Budget = 10000;
            Panel = FindVisibleWidget(GraphWidget.ToSharedRef(), [](const auto& Widget)
                { return Widget->GetType() == FName(TEXT("SGraphPanel")); }, Budget);
            if (!Region.IsSet() || !Panel)
            { OutError = TEXT("graph_layout_unavailable: Region or graph panel is unavailable."); return false; }
            const FVector2D Pixels(Panel->GetCachedGeometry().GetLocalSize());
            if (Pixels.X < 32 || Pixels.Y < 32 || !FMath::IsFinite(Pixels.X) || !FMath::IsFinite(Pixels.Y))
            { OutError = TEXT("graph_layout_unavailable: Graph panel has no usable layout yet."); return false; }
            auto& Bounds = Region.GetValue();
            Bounds.Left -= Padding;
            Bounds.Right += Padding;
            Bounds.Top -= Padding;
            Bounds.Bottom += Padding;
            const double Width = Bounds.Right - Bounds.Left;
            const double Height = Bounds.Bottom - Bounds.Top;
            FGraphViewPosition Original;
            float OriginalZoom = 0;
            GraphWidget->GetViewLocation(Original, OriginalZoom);
            const float Fit = FMath::Min(Pixels.X / Width, Pixels.Y / Height);
            float Desired = FMath::Clamp(Fit, static_cast<float>(MinimumZoom), FMath::Max(1.f, static_cast<float>(MinimumZoom)));
            float Actual = 0;
            FGraphViewPosition Current;
            for (int32 Attempt = 0; Attempt < 12; ++Attempt)
            {
                GraphWidget->SetViewLocation(Original, Desired);
                GraphWidget->GetViewLocation(Current, Actual);
                if (Actual + 0.001f >= MinimumZoom) break;
                Desired += 0.05f;
            }
            if (!FMath::IsFinite(Actual) || Actual + 0.001f < MinimumZoom)
            { GraphWidget->SetViewLocation(Original, OriginalZoom); OutError = TEXT("minimum_zoom_unavailable: Graph zoom could not meet the requested minimum."); return false; }
            const FVector2D Extent = Pixels / Actual;
            constexpr double StrideRatio = 0.8;
            auto AxisTiles = [&](double RegionSize, double ViewSize)
            {
                return 1 + FMath::CeilToInt(FMath::Max(0.0, RegionSize - ViewSize - 0.5) / (ViewSize * StrideRatio));
            };
            const int32 Columns = AxisTiles(Width, Extent.X);
            const int32 Rows = AxisTiles(Height, Extent.Y);
            if (Columns > 32 || Rows > 32 || Columns * Rows > 32 || TileIndex >= Columns * Rows)
            {
                GraphWidget->SetViewLocation(Original, OriginalZoom);
                OutError = TEXT("tile_budget_exceeded: Select a smaller region, lower minimumZoom or use a valid tile index. Maximum 32 tiles.");
                return false;
            }
            TileCount = Columns * Rows;
            auto Origin = [&](int32 Index, double Min, double Max, double Size)
            {
                return Max - Min < Size ? (Min + Max - Size) / 2
                    : FMath::Min(Min + Index * Size * StrideRatio, Max - Size);
            };
            const double X = Origin(TileIndex % Columns, Bounds.Left, Bounds.Right, Extent.X);
            const double Y = Origin(TileIndex / Columns, Bounds.Top, Bounds.Bottom, Extent.Y);
            GraphWidget->SetViewLocation(FGraphViewPosition(X, Y), Actual);
            TileBounds.Emplace(FMath::Max<double>(X, Bounds.Left), FMath::Max<double>(Y, Bounds.Top),
                FMath::Min<double>(X + Extent.X, Bounds.Right), FMath::Min<double>(Y + Extent.Y, Bounds.Bottom));
            Response.Result->SetObjectField(TEXT("requestedBounds"), BoundsJson(Bounds));
            Response.Result->SetObjectField(TEXT("tileBounds"), BoundsJson(TileBounds.GetValue()));
            Response.Result->SetNumberField(TEXT("tileIndex"), TileIndex);
            Response.Result->SetNumberField(TEXT("tileCount"), TileCount);
            if (TileIndex + 1 < TileCount) Response.Result->SetNumberField(TEXT("nextTileIndex"), TileIndex + 1);
        }
        else
        {
            GraphWidget->ClearSelectionSet();
            for (UEdGraphNode* Node : Nodes) GraphWidget->SetNodeSelection(Node, true);
            GraphWidget->ZoomToFit(!Nodes.IsEmpty());
        }

        const FString Ref = RememberWidget(GraphWidget.ToSharedRef(), true);
        auto& Entry = WidgetReferences.FindChecked(Ref);
        Entry.FramedGraph = GraphWidget;
        Entry.ObjectPath = Blueprint->GetPathName();
        Entry.GraphName = Target->GetName();
        Entry.GraphGuid = Target->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
        Entry.GraphPanel = Panel;
        Entry.TileBounds = TileBounds;
        Entry.RegionBounds = Region;
        Entry.MinimumZoom = MinimumZoom;
        Entry.TileIndex = TileIndex;
        Entry.TileCount = TileCount;
        Entry.FrameRequestedAt = Entry.StableSince = FPlatformTime::Seconds();
        GraphWidget->GetViewLocation(Entry.LastView, Entry.LastZoom);
        Response.Result->SetStringField(TEXT("widgetRef"), Ref);
        Response.Result->SetStringField(TEXT("captureHint"), TEXT("Call CaptureEditorWidget with this widgetRef to wait for the frame and capture it."));
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
    Properties->SetObjectField(TEXT("backgroundOnly"), Property(TEXT("boolean"), TEXT("Use an already visible exact graph without activating its tab or window.")));
    Properties->SetObjectField(TEXT("includeConnectedData"), Property(TEXT("boolean"), TEXT("Expand selected nodes upstream through pure data sources, up to 128 nodes.")));
    Properties->SetObjectField(TEXT("padding"), Property(TEXT("number"), TEXT("Graph-space padding for measured framing, default 32.")));
    Properties->SetObjectField(TEXT("minimumZoom"), Property(TEXT("number"), TEXT("Minimum graph zoom 0.1..1.8, default 0.8 for measured framing.")));
    Properties->SetObjectField(TEXT("tileIndex"), Property(TEXT("integer"), TEXT("Zero-based index for overlapping tiles, default 0. Capture this tile before framing the next.")));
    auto Rect = Property(TEXT("object"), TEXT("Explicit graph-coordinate rectangle. Alternative to nodeGuids."));
    auto Edges = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> RequiredEdges;
    for (const TCHAR* Edge : {TEXT("left"), TEXT("top"), TEXT("right"), TEXT("bottom")})
    {
        Edges->SetObjectField(Edge, Property(TEXT("number"), TEXT("Graph-space coordinate.")));
        RequiredEdges.Add(MakeShared<FJsonValueString>(Edge));
    }
    Rect->SetObjectField(TEXT("properties"), Edges);
    Rect->SetArrayField(TEXT("required"), RequiredEdges);
    Properties->SetObjectField(TEXT("region"), Rect);
    return Schema(Properties, {MakeShared<FJsonValueString>(TEXT("objectPath"))});
}
