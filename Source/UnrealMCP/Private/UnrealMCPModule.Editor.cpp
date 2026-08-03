#include "UnrealMCPModule.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Framework/Application/SlateApplication.h"
#include "Index/UnrealMCPProjectIndex.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "ToolMenus.h"
#include "UnrealMCPLog.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    FString GetPluginResourcePath(const FString& RelativePath)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
        return Plugin.IsValid()
            ? FPaths::Combine(Plugin->GetBaseDir(), RelativePath)
            : FString();
    }

    FString LoadPluginTextFile(const FString& RelativePath, const FString& FallbackText)
    {
        const FString FilePath = GetPluginResourcePath(RelativePath);
        FString Body;
        if (!FilePath.IsEmpty() && FFileHelper::LoadFileToString(Body, *FilePath))
        {
            return Body;
        }

        return FallbackText;
    }

    FString GetUnrealMCPHelpText()
    {
        return LoadPluginTextFile(
            TEXT("Resources/Help/HowToUse.txt"),
            TEXT("UnrealMCP help file is missing.\n\nExpected file:\nResources/Help/HowToUse.txt\n\nReinstall or update the plugin resources."));
    }

    FString GetUnrealMCPPromptExamplesText()
    {
        return LoadPluginTextFile(
            TEXT("Resources/Help/PromptExamples.txt"),
            TEXT("UnrealMCP prompt examples file is missing.\n\nExpected file:\nResources/Help/PromptExamples.txt\n\nReinstall or update the plugin resources."));
    }

    void OpenReadOnlyTextWindow(const FString& Title, const FString& Header, const FString& Body)
    {
        TSharedRef<SWindow> Window = SNew(SWindow)
            .Title(FText::FromString(Title))
            .ClientSize(FVector2D(900.0f, 720.0f))
            .SupportsMaximize(true)
            .SupportsMinimize(true);

        Window->SetContent(
            SNew(SBox)
            .Padding(12.0f)
            [
                SNew(SScrollBox)
                + SScrollBox::Slot()
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Header))
                        .AutoWrapText(true)
                    ]
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                    [
                        SNew(SSeparator)
                    ]
                    + SVerticalBox::Slot()
                    .FillHeight(1.0f)
                    [
                        SNew(SMultiLineEditableTextBox)
                        .IsReadOnly(true)
                        .AutoWrapText(true)
                        .Text(FText::FromString(Body))
                    ]
                ]
            ]);

        FSlateApplication::Get().AddWindow(Window);
    }
}

void FUnrealMCPModule::RegisterEditorControls()
{
    if (bEditorControlsRegistered)
    {
        return;
    }

    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
    {
        if (bEditorControlsRegistered)
        {
            return;
        }

        FToolMenuOwnerScoped OwnerScoped(this);

        if (UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
        {
            FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("UnrealMCP"));
            Section.AddSubMenu(
                TEXT("UnrealMCP.SubMenu"),
                FText::FromString(TEXT("UnrealMCP")),
                FText::FromString(TEXT("Manual UnrealMCP controls.")),
                FNewToolMenuDelegate::CreateLambda([this](UToolMenu* SubMenu)
                {
                    FToolMenuSection& SubSection = SubMenu->AddSection(TEXT("UnrealMCPActions"), FText::FromString(TEXT("UnrealMCP")));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.InitializeIndex"), FText::FromString(TEXT("Initialize Index")), FText::FromString(TEXT("Opens the UnrealMCP project index on demand without rebuilding it.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleInitializeProjectIndex)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.RebuildIndex"), FText::FromString(TEXT("Rebuild Index")), FText::FromString(TEXT("Runs a full UnrealMCP project index rebuild.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleRebuildProjectIndex)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.LogIndexStatus"), FText::FromString(TEXT("Log Index Status")), FText::FromString(TEXT("Logs the current UnrealMCP index status snapshot.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleLogIndexStatus)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.OpenIndexFolder"), FText::FromString(TEXT("Open Index Folder")), FText::FromString(TEXT("Opens the Saved/UnrealMCP folder in Explorer.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenIndexFolder)));
                    SubSection.AddSeparator(TEXT("UnrealMCP.HelpSeparator"));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.OpenHelp"), FText::FromString(TEXT("How To Use")), FText::FromString(TEXT("Opens a user guide for UnrealMCP inside the editor.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenHelp)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.OpenPromptExamples"), FText::FromString(TEXT("Prompt Examples")), FText::FromString(TEXT("Opens ready-to-use prompt examples for coding agents.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenPromptExamples)));
                    SubSection.AddSeparator(TEXT("UnrealMCP.TransportSeparator"));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.StartTransport"), FText::FromString(TEXT("Start Transport")), FText::FromString(TEXT("Starts the configured UnrealMCP transport if it is not already running.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleStartTransport)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.StopTransport"), FText::FromString(TEXT("Stop Transport")), FText::FromString(TEXT("Stops the configured UnrealMCP transport.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleStopTransportCommand)));
                }));
        }

        if (UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User")))
        {
            FToolMenuSection& Section = ToolbarMenu->FindOrAddSection(TEXT("UnrealMCP"));
            Section.AddEntry(FToolMenuEntry::InitComboButton(
                TEXT("UnrealMCP.Combo"),
                FUIAction(),
                FNewToolMenuDelegate::CreateLambda([this](UToolMenu* SubMenu)
                {
                    FToolMenuSection& SubSection = SubMenu->AddSection(TEXT("UnrealMCPToolbarActions"), FText::FromString(TEXT("UnrealMCP")));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.InitializeIndex"), FText::FromString(TEXT("Initialize Index")), FText::FromString(TEXT("Opens the UnrealMCP project index on demand without rebuilding it.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleInitializeProjectIndex)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.RebuildIndex"), FText::FromString(TEXT("Rebuild Index")), FText::FromString(TEXT("Runs a full UnrealMCP project index rebuild.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleRebuildProjectIndex)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.LogIndexStatus"), FText::FromString(TEXT("Log Index Status")), FText::FromString(TEXT("Logs the current UnrealMCP index status snapshot.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleLogIndexStatus)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.OpenIndexFolder"), FText::FromString(TEXT("Open Index Folder")), FText::FromString(TEXT("Opens the Saved/UnrealMCP folder in Explorer.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenIndexFolder)));
                    SubSection.AddSeparator(TEXT("UnrealMCP.Toolbar.HelpSeparator"));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.OpenHelp"), FText::FromString(TEXT("How To Use")), FText::FromString(TEXT("Opens a user guide for UnrealMCP inside the editor.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenHelp)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.OpenPromptExamples"), FText::FromString(TEXT("Prompt Examples")), FText::FromString(TEXT("Opens ready-to-use prompt examples for coding agents.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenPromptExamples)));
                    SubSection.AddSeparator(TEXT("UnrealMCP.Toolbar.TransportSeparator"));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.StartTransport"), FText::FromString(TEXT("Start Transport")), FText::FromString(TEXT("Starts the configured UnrealMCP transport if it is not already running.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleStartTransport)));
                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.StopTransport"), FText::FromString(TEXT("Stop Transport")), FText::FromString(TEXT("Stops the configured UnrealMCP transport.")), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleStopTransportCommand)));
                }),
                FText::FromString(TEXT("UnrealMCP")),
                FText::FromString(TEXT("Manual UnrealMCP controls.")),
                FSlateIcon()));
        }

        bEditorControlsRegistered = true;
        UE_LOG(LogUnrealMCP, Log, TEXT("Registered UnrealMCP editor controls."));
    }));
}

void FUnrealMCPModule::UnregisterEditorControls()
{
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
    bEditorControlsRegistered = false;
}

void FUnrealMCPModule::HandleInitializeProjectIndex() const
{
    const bool bInitialized = EnsureProjectIndexInitialized();
    FMessageDialog::Open(
        EAppMsgType::Ok,
        bInitialized
            ? FText::FromString(TEXT("UnrealMCP project index initialized."))
            : FText::FromString(TEXT("UnrealMCP project index failed to initialize. Check LogUnrealMCP for details.")));
}

void FUnrealMCPModule::HandleRebuildProjectIndex() const
{
    if (!EnsureProjectIndexInitialized())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("UnrealMCP could not initialize the project index. Check LogUnrealMCP for details.")));
        return;
    }

    FString Error;
    FScopedSlowTask SlowTask(100.0f, FText::FromString(TEXT("Rebuilding UnrealMCP project index...")));
    SlowTask.MakeDialog(true);

    const bool bSucceeded = ProjectIndex->BuildFullIndex(
        Error,
        [&SlowTask](int32 CurrentIndex, int32 TotalAssets, const FString& StatusText)
        {
            const float Total = FMath::Max(1, TotalAssets);
            const float Current = FMath::Clamp(static_cast<float>(CurrentIndex), 0.0f, Total);
            const float TargetProgress = (Current / Total) * 100.0f;
            const float Remaining = FMath::Max(0.0f, TargetProgress - SlowTask.CompletedWork);
            if (Remaining > 0.0f)
            {
                SlowTask.EnterProgressFrame(Remaining, FText::FromString(StatusText));
            }
            else
            {
                SlowTask.DefaultMessage = FText::FromString(StatusText);
            }
        });
    if (!bSucceeded)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Manual index rebuild failed: %s"), *Error);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("UnrealMCP index rebuild failed:\n%s"), *Error)));
        return;
    }

    const FUnrealMCPProjectIndex::FStatusSnapshot Snapshot = ProjectIndex->GetStatusSnapshot();
    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::FromString(FString::Printf(TEXT("UnrealMCP index rebuilt.\nAssets: %lld\nBlueprints: %lld"), Snapshot.IndexedAssetCount, Snapshot.IndexedBlueprintCount)));
}

void FUnrealMCPModule::HandleLogIndexStatus() const
{
    if (!EnsureProjectIndexInitialized())
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("Cannot log index status because project index initialization failed."));
        return;
    }

    const FUnrealMCPProjectIndex::FStatusSnapshot Snapshot = ProjectIndex->GetStatusSnapshot();
    UE_LOG(LogUnrealMCP, Log, TEXT("Index status: DatabaseOpen=%s SchemaReady=%s HasUsableIndex=%s IsDirty=%s DirtyAssets=%d Assets=%lld Blueprints=%lld LastFullBuildUtc=%s LastUpdateUtc=%s RebuildRecommended=%s LastError=%s"),
        Snapshot.bDatabaseOpen ? TEXT("true") : TEXT("false"),
        Snapshot.bSchemaReady ? TEXT("true") : TEXT("false"),
        Snapshot.bHasUsableIndex ? TEXT("true") : TEXT("false"),
        Snapshot.bIndexDirty ? TEXT("true") : TEXT("false"),
        Snapshot.DirtyAssetCount,
        Snapshot.IndexedAssetCount,
        Snapshot.IndexedBlueprintCount,
        *Snapshot.LastFullBuildUtc,
        *Snapshot.LastUpdateUtc,
        Snapshot.bRebuildRecommended ? TEXT("true") : TEXT("false"),
        *Snapshot.LastError);

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::FromString(FString::Printf(TEXT("DatabaseOpen=%s\nHasUsableIndex=%s\nIsDirty=%s\nAssets=%lld\nBlueprints=%lld"),
            Snapshot.bDatabaseOpen ? TEXT("true") : TEXT("false"),
            Snapshot.bHasUsableIndex ? TEXT("true") : TEXT("false"),
            Snapshot.bIndexDirty ? TEXT("true") : TEXT("false"),
            Snapshot.IndexedAssetCount,
            Snapshot.IndexedBlueprintCount)));
}

void FUnrealMCPModule::HandleOpenIndexFolder() const
{
    const FString FolderPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMCP")));
    IFileManager::Get().MakeDirectory(*FolderPath, true);
    FPlatformProcess::LaunchFileInDefaultExternalApplication(*FolderPath);
}

void FUnrealMCPModule::HandleOpenHelp() const
{
    OpenReadOnlyTextWindow(
        TEXT("UnrealMCP Help"),
        TEXT("This window explains what UnrealMCP does, how users normally work with it, and what capability groups are available."),
        GetUnrealMCPHelpText());
}

void FUnrealMCPModule::HandleOpenPromptExamples() const
{
    OpenReadOnlyTextWindow(
        TEXT("UnrealMCP Prompt Examples"),
        TEXT("These examples are written for Codex or another MCP-compatible coding agent. Copy the ones that match your task and adapt the asset paths or feature names."),
        GetUnrealMCPPromptExamplesText());
}

void FUnrealMCPModule::HandleStartTransport()
{
    if (Transport.IsValid())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("UnrealMCP transport is already running.")));
        return;
    }

    StartConfiguredTransport();
    FMessageDialog::Open(EAppMsgType::Ok,
        Transport.IsValid()
            ? FText::FromString(TEXT("UnrealMCP transport started."))
            : FText::FromString(TEXT("UnrealMCP transport did not start. Check LogUnrealMCP for details.")));
}

void FUnrealMCPModule::HandleStopTransportCommand()
{
    if (!Transport.IsValid())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("UnrealMCP transport is not running.")));
        return;
    }

    StopTransport();
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("UnrealMCP transport stopped.")));
}
