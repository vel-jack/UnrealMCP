#include "UnrealMCPModule.h"

#include "Index/UnrealMCPProjectIndex.h"
#include "MCP/MCPServer.h"
#include "Tools/AssetExistsTool.h"
#include "Tools/BuildProjectIndexTool.h"
#include "Tools/CompileAllBlueprintsTool.h"
#include "Tools/CompileBlueprintTool.h"
#include "Tools/ExplainBlueprintRoleTool.h"
#include "Tools/ExplainFeatureWorkflowTool.h"
#include "Tools/FindAssetsByClassTool.h"
#include "Tools/FindAssetsByPathTool.h"
#include "Tools/FindActorsUsingBlueprintTool.h"
#include "Tools/FindFeatureEntryPointsTool.h"
#include "Tools/GetAssetReferencesTool.h"
#include "Tools/GetAssetReferencersTool.h"
#include "Tools/GetActorInfoTool.h"
#include "Tools/GetBlueprintInfoTool.h"
#include "Tools/GetBlueprintDependenciesTool.h"
#include "Tools/GetBlueprintComponentHierarchyTool.h"
#include "Tools/GetBlueprintInterfacesTool.h"
#include "Tools/GetIndexStatusTool.h"
#include "Tools/GetLevelActorDependenciesTool.h"
#include "Tools/GetParentBlueprintTool.h"
#include "Tools/GetAssetInfoTool.h"
#include "Tools/GetDependenciesTool.h"
#include "Tools/GetReferencersTool.h"
#include "Tools/GetServerInfoTool.h"
#include "Tools/HealthCheckTool.h"
#include "Tools/ListBlueprintComponentsTool.h"
#include "Tools/ListBlueprintFunctionsTool.h"
#include "Tools/ListBlueprintVariablesTool.h"
#include "Tools/ListChildBlueprintsTool.h"
#include "Tools/ListActorsTool.h"
#include "Tools/ListAssetsTool.h"
#include "Tools/ListFoldersTool.h"
#include "Tools/ListSelectedActorsTool.h"
#include "Tools/ListToolsTool.h"
#include "Tools/SearchAssetsTool.h"
#include "Tools/SummarizeBlueprintClusterTool.h"
#include "Tools/TraceFeatureFlowTool.h"
#include "Transport/NamedPipeMCPTransport.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/Paths.h"
#include "ToolMenus.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY(LogUnrealMCP);

IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP)

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

FUnrealMCPModule& FUnrealMCPModule::Get()
{
    return FModuleManager::LoadModuleChecked<FUnrealMCPModule>(TEXT("UnrealMCP"));
}

bool FUnrealMCPModule::IsAvailable()
{
    return FModuleManager::Get().IsModuleLoaded(TEXT("UnrealMCP"));
}

void FUnrealMCPModule::StartupModule()
{
    Server = MakeUnique<FMCPServer>();
    ProjectIndex = MakeUnique<FUnrealMCPProjectIndex>();
    RegisterCoreTools();
    RegisterTestCommands();
    RegisterEditorControls();

    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();

    UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP started. Enabled=%s ServerName=%s Version=%s Engine=%s IndexInitOnStartup=%s LiveIndexTracking=%s"),
        Settings->bEnableServer ? TEXT("true") : TEXT("false"),
        *Settings->ServerName,
        *Settings->ServerVersion,
        *FEngineVersion::Current().ToString(),
        Settings->bInitializeProjectIndexOnStartup ? TEXT("true") : TEXT("false"),
        Settings->bEnableLiveIndexTracking ? TEXT("true") : TEXT("false"));

    if (Settings->bInitializeProjectIndexOnStartup)
    {
        EnsureProjectIndexInitialized();
    }
    else
    {
        UE_LOG(LogUnrealMCP, Log, TEXT("Project index initialization deferred until first use."));
    }

    StartConfiguredTransport();
}

void FUnrealMCPModule::ShutdownModule()
{
    StopTransport();
    UnregisterEditorControls();
    UnregisterTestCommands();
    ProjectIndex.Reset();
    Server.Reset();
    UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP shut down."));
}

FMCPServer& FUnrealMCPModule::GetServer()
{
    check(Server.IsValid());
    return *Server;
}

const FMCPServer& FUnrealMCPModule::GetServer() const
{
    check(Server.IsValid());
    return *Server;
}

FUnrealMCPProjectIndex& FUnrealMCPModule::GetProjectIndex()
{
    check(ProjectIndex.IsValid());
    EnsureProjectIndexInitialized();
    return *ProjectIndex;
}

const FUnrealMCPProjectIndex& FUnrealMCPModule::GetProjectIndex() const
{
    check(ProjectIndex.IsValid());
    EnsureProjectIndexInitialized();
    return *ProjectIndex;
}

bool FUnrealMCPModule::EnsureProjectIndexInitialized() const
{
    check(ProjectIndex.IsValid());
    if (ProjectIndex->GetStatusSnapshot().bDatabaseOpen)
    {
        return true;
    }

    const bool bInitialized = ProjectIndex->Initialize();
    if (!bInitialized)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Deferred project index initialization failed."));
    }
    return bInitialized;
}

FString FUnrealMCPModule::GetEffectiveNamedPipeName() const
{
    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
    const FString ExplicitPipeName = SanitizeNamedPipeToken(Settings->NamedPipeName);
    if (!ExplicitPipeName.IsEmpty())
    {
        return ExplicitPipeName;
    }

    const FString BasePipeName = TEXT("UnrealMCP");
    if (!Settings->bUseProjectSpecificNamedPipe)
    {
        return BasePipeName;
    }

    const FString ProjectToken = SanitizeNamedPipeToken(FApp::GetProjectName());
    return ProjectToken.IsEmpty()
        ? BasePipeName
        : FString::Printf(TEXT("%s_%s"), *BasePipeName, *ProjectToken);
}

FString FUnrealMCPModule::SanitizeNamedPipeToken(const FString& Value)
{
    FString Result;
    Result.Reserve(Value.Len());

    for (const TCHAR Character : Value)
    {
        if (FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-'))
        {
            Result.AppendChar(Character);
        }
    }

    return Result;
}

void FUnrealMCPModule::RegisterCoreTools()
{
    FMCPToolRegistry& Registry = GetServer().GetToolRegistry();
    Registry.RegisterTool(MakeShared<FHealthCheckTool>());
    Registry.RegisterTool(MakeShared<FGetServerInfoTool>());
    Registry.RegisterTool(MakeShared<FGetIndexStatusTool>());
    Registry.RegisterTool(MakeShared<FBuildProjectIndexTool>());
    Registry.RegisterTool(MakeShared<FGetAssetReferencesTool>());
    Registry.RegisterTool(MakeShared<FGetAssetReferencersTool>());
    Registry.RegisterTool(MakeShared<FExplainBlueprintRoleTool>());
    Registry.RegisterTool(MakeShared<FExplainFeatureWorkflowTool>());
    Registry.RegisterTool(MakeShared<FFindFeatureEntryPointsTool>());
    Registry.RegisterTool(MakeShared<FSummarizeBlueprintClusterTool>());
    Registry.RegisterTool(MakeShared<FTraceFeatureFlowTool>());
    Registry.RegisterTool(MakeShared<FSearchAssetsTool>());
    Registry.RegisterTool(MakeShared<FAssetExistsTool>());
    Registry.RegisterTool(MakeShared<FGetAssetInfoTool>());
    Registry.RegisterTool(MakeShared<FListAssetsTool>());
    Registry.RegisterTool(MakeShared<FListFoldersTool>());
    Registry.RegisterTool(MakeShared<FGetDependenciesTool>());
    Registry.RegisterTool(MakeShared<FGetReferencersTool>());
    Registry.RegisterTool(MakeShared<FFindAssetsByClassTool>());
    Registry.RegisterTool(MakeShared<FFindAssetsByPathTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintInfoTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintDependenciesTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintComponentHierarchyTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintVariablesTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintFunctionsTool>());
    Registry.RegisterTool(MakeShared<FListBlueprintComponentsTool>());
    Registry.RegisterTool(MakeShared<FGetParentBlueprintTool>());
    Registry.RegisterTool(MakeShared<FGetBlueprintInterfacesTool>());
    Registry.RegisterTool(MakeShared<FListChildBlueprintsTool>());
    Registry.RegisterTool(MakeShared<FListActorsTool>());
    Registry.RegisterTool(MakeShared<FGetActorInfoTool>());
    Registry.RegisterTool(MakeShared<FFindActorsUsingBlueprintTool>());
    Registry.RegisterTool(MakeShared<FGetLevelActorDependenciesTool>());
    Registry.RegisterTool(MakeShared<FListSelectedActorsTool>());
    Registry.RegisterTool(MakeShared<FCompileBlueprintTool>());
    Registry.RegisterTool(MakeShared<FCompileAllBlueprintsTool>());
    Registry.RegisterTool(MakeShared<FListToolsTool>(Registry));
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

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.InitializeIndex"),
                        FText::FromString(TEXT("Initialize Index")),
                        FText::FromString(TEXT("Opens the UnrealMCP project index on demand without rebuilding it.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleInitializeProjectIndex)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.RebuildIndex"),
                        FText::FromString(TEXT("Rebuild Index")),
                        FText::FromString(TEXT("Runs a full UnrealMCP project index rebuild.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleRebuildProjectIndex)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.LogIndexStatus"),
                        FText::FromString(TEXT("Log Index Status")),
                        FText::FromString(TEXT("Logs the current UnrealMCP index status snapshot.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleLogIndexStatus)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.OpenIndexFolder"),
                        FText::FromString(TEXT("Open Index Folder")),
                        FText::FromString(TEXT("Opens the Saved/UnrealMCP folder in Explorer.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenIndexFolder)));

                    SubSection.AddSeparator(TEXT("UnrealMCP.HelpSeparator"));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.OpenHelp"),
                        FText::FromString(TEXT("How To Use")),
                        FText::FromString(TEXT("Opens a user guide for UnrealMCP inside the editor.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenHelp)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.OpenPromptExamples"),
                        FText::FromString(TEXT("Prompt Examples")),
                        FText::FromString(TEXT("Opens ready-to-use prompt examples for coding agents.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenPromptExamples)));

                    SubSection.AddSeparator(TEXT("UnrealMCP.TransportSeparator"));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.StartTransport"),
                        FText::FromString(TEXT("Start Transport")),
                        FText::FromString(TEXT("Starts the configured UnrealMCP transport if it is not already running.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleStartTransport)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.StopTransport"),
                        FText::FromString(TEXT("Stop Transport")),
                        FText::FromString(TEXT("Stops the configured UnrealMCP transport.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleStopTransportCommand)));
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

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.InitializeIndex"),
                        FText::FromString(TEXT("Initialize Index")),
                        FText::FromString(TEXT("Opens the UnrealMCP project index on demand without rebuilding it.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleInitializeProjectIndex)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.RebuildIndex"),
                        FText::FromString(TEXT("Rebuild Index")),
                        FText::FromString(TEXT("Runs a full UnrealMCP project index rebuild.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleRebuildProjectIndex)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.LogIndexStatus"),
                        FText::FromString(TEXT("Log Index Status")),
                        FText::FromString(TEXT("Logs the current UnrealMCP index status snapshot.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleLogIndexStatus)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.OpenIndexFolder"),
                        FText::FromString(TEXT("Open Index Folder")),
                        FText::FromString(TEXT("Opens the Saved/UnrealMCP folder in Explorer.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenIndexFolder)));

                    SubSection.AddSeparator(TEXT("UnrealMCP.Toolbar.HelpSeparator"));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.OpenHelp"),
                        FText::FromString(TEXT("How To Use")),
                        FText::FromString(TEXT("Opens a user guide for UnrealMCP inside the editor.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenHelp)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.OpenPromptExamples"),
                        FText::FromString(TEXT("Prompt Examples")),
                        FText::FromString(TEXT("Opens ready-to-use prompt examples for coding agents.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleOpenPromptExamples)));

                    SubSection.AddSeparator(TEXT("UnrealMCP.Toolbar.TransportSeparator"));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.StartTransport"),
                        FText::FromString(TEXT("Start Transport")),
                        FText::FromString(TEXT("Starts the configured UnrealMCP transport if it is not already running.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleStartTransport)));

                    SubSection.AddMenuEntry(TEXT("UnrealMCP.Toolbar.StopTransport"),
                        FText::FromString(TEXT("Stop Transport")),
                        FText::FromString(TEXT("Stops the configured UnrealMCP transport.")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMCPModule::HandleStopTransportCommand)));
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

void FUnrealMCPModule::StartConfiguredTransport()
{
    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
    if (!Settings->bEnableServer)
    {
        UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP transport startup skipped because the server is disabled."));
        return;
    }

#if PLATFORM_WINDOWS
    if (Settings->bEnableNamedPipeTransport)
    {
        const FString EffectivePipeName = GetEffectiveNamedPipeName();
        Transport = MakeUnique<FNamedPipeMCPTransport>(EffectivePipeName);
        if (Transport->Start(GetServer()))
        {
            UE_LOG(LogUnrealMCP, Log, TEXT("Started %s transport. EffectivePipe=%s"), *Transport->GetTransportName(), *EffectivePipeName);
            return;
        }

        UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to start named pipe transport '%s'."), *EffectivePipeName);
        Transport.Reset();
    }
#endif

    if (Settings->bEnableStdIOTransport)
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("StdIO transport is enabled in settings but not implemented yet."));
    }
}

void FUnrealMCPModule::StopTransport()
{
    if (Transport.IsValid())
    {
        Transport->Stop();
        UE_LOG(LogUnrealMCP, Log, TEXT("Stopped %s transport."), *Transport->GetTransportName());
        Transport.Reset();
    }
}

void FUnrealMCPModule::RegisterTestCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();

    RegisteredConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("UnrealMCP.TestHealth"),
        TEXT("Runs the HealthCheck MCP tool and logs the JSON response."),
        FConsoleCommandDelegate::CreateLambda([this]()
        {
            RunTestCommand(TEXT("HealthCheck"));
        }),
        ECVF_Default));

    RegisteredConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("UnrealMCP.TestServerInfo"),
        TEXT("Runs the GetServerInfo MCP tool and logs the JSON response."),
        FConsoleCommandDelegate::CreateLambda([this]()
        {
            RunTestCommand(TEXT("GetServerInfo"));
        }),
        ECVF_Default));

    RegisteredConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("UnrealMCP.TestListTools"),
        TEXT("Runs the ListTools MCP tool and logs the JSON response."),
        FConsoleCommandDelegate::CreateLambda([this]()
        {
            RunTestCommand(TEXT("ListTools"));
        }),
        ECVF_Default));

    RegisteredConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("UnrealMCP.TestSearchAssets"),
        TEXT("Runs the SearchAssets MCP tool and logs the JSON response. Usage: UnrealMCP.TestSearchAssets Door"),
        FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
        {
            HandleSearchAssetsCommand(Args);
        }),
        ECVF_Default));
}

void FUnrealMCPModule::UnregisterTestCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();
    for (IConsoleObject* Command : RegisteredConsoleCommands)
    {
        if (Command != nullptr)
        {
            ConsoleManager.UnregisterConsoleObject(Command, false);
        }
    }

    RegisteredConsoleCommands.Reset();
}

void FUnrealMCPModule::RunTestCommand(const FString& Method, TSharedPtr<FJsonObject> Params) const
{
    if (!Server.IsValid())
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Cannot run MCP test command '%s' because the server is not initialized."), *Method);
        return;
    }

    UnrealMCP::FMCPRequest Request;
    Request.Id = FString::Printf(TEXT("test-%s"), *Method);
    Request.Method = Method;
    Request.Params = Params;

    const UnrealMCP::FMCPResponse Response = GetServer().HandleRequest(Request);
    const FString SerializedResponse = GetServer().SerializeResponse(Response);

    UE_LOG(LogUnrealMCP, Log, TEXT("MCP test response for %s: %s"), *Method, *SerializedResponse);
}

void FUnrealMCPModule::HandleSearchAssetsCommand(const TArray<FString>& Args) const
{
    if (Args.IsEmpty())
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("Usage: UnrealMCP.TestSearchAssets <Query>"));
        return;
    }

    TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("query"), FString::Join(Args, TEXT(" ")));
    RunTestCommand(TEXT("SearchAssets"), Params);
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
