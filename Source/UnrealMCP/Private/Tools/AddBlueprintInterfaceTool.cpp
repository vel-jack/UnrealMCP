#include "Tools/AddBlueprintInterfaceTool.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

FAddBlueprintInterfaceTool::FAddBlueprintInterfaceTool()
    : FMCPToolBase(TEXT("AddBlueprintInterface"), TEXT("Attaches an exact Blueprint interface transactionally, with dry-run, duplicate/inherited detection, collision refusal, compile rollback and optional save. Refuses saving pre-existing unsaved work.")) {}
UnrealMCP::FMCPResponse FAddBlueprintInterfaceTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, InterfacePath, Error, Filename, IndexError;
    if (!Request.Params || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("interfaceClassPath"), InterfacePath))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("objectPath and interfaceClassPath are required."));
    const bool Dry = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool Save = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    const bool Compile = Save || BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    bool Already = false, Inherited = false, Added = false, Compiled = false, CompileOk = false, Saved = false, RolledBack = false, Index = false;
    int32 Errors = 0, Warnings = 0;
    TArray<TSharedPtr<FJsonValue>> Graphs;
    bool Ok = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& E)
    {
        UBlueprint* BP = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, BP, E)) return false;
        UClass* Interface = LoadObject<UClass>(nullptr, *InterfacePath);
        if (!Interface || !Interface->HasAnyClassFlags(CLASS_Interface)) { E = TEXT("Exact path must resolve to an interface class."); return false; }
        InterfacePath = Interface->GetPathName();
        TArray<UClass*> Existing; FBlueprintEditorUtils::FindImplementedInterfaces(BP, true, Existing);
        Already = Existing.ContainsByPredicate([&](const UClass* C) { return C && (C->GetAuthoritativeClass() == Interface->GetAuthoritativeClass() || C->IsChildOf(Interface)); });
        if (Already)
        {
            Inherited = !BP->ImplementedInterfaces.ContainsByPredicate([&](const FBPInterfaceDescription& D) { return D.Interface && (D.Interface->GetAuthoritativeClass() == Interface->GetAuthoritativeClass() || D.Interface->IsChildOf(Interface)); });
            return true;
        }
        // Never replace a pre-existing function/event implementation as a side effect.
        TArray<UEdGraph*> All; BP->GetAllGraphs(All);
        for (TFieldIterator<UFunction> It(Interface); It; ++It)
        {
            const FName Name = It->GetFName();
            if (All.ContainsByPredicate([&](const UEdGraph* G) { return G && G->GetFName() == Name; })
                || (BP->SkeletonGeneratedClass && BP->SkeletonGeneratedClass->FindFunctionByName(Name, EIncludeSuperFlag::ExcludeSuper)))
            { E = FString::Printf(TEXT("Existing function '%s' needs explicit reconciliation before interface attachment."), *Name.ToString()); return false; }
        }
        const bool WasDirty = BP->GetOutermost()->IsDirty();
        if (Save && WasDirty) { E = TEXT("Asset has pre-existing unsaved work. Save/review it first, or attach without saving."); return false; }
        if (Dry) return true;
        if (!GEditor) { E = TEXT("Editor transactions are unavailable."); return false; }
        const int32 BeforeInterfaces = BP->ImplementedInterfaces.Num();
        const int32 BeforeGraphs = BP->FunctionGraphs.Num();
        bool Attached = false;
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AttachInterface", "UnrealMCP Add Blueprint Interface"));
            BP->Modify();
            Attached = FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath(Interface));
            if (Attached) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        }
        auto Rollback = [&]()
        {
            RolledBack = GEditor->UndoTransaction();
            if (RolledBack)
            {
                FCompilerResultsLog Log; Log.bSilentMode = true;
                FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
                BP->GetOutermost()->SetDirtyFlag(WasDirty);
            }
        };
        if (!Attached)
        {
            if (BP->ImplementedInterfaces.Num() != BeforeInterfaces || BP->FunctionGraphs.Num() != BeforeGraphs) Rollback();
            E = TEXT("Engine refused interface attachment; nothing was saved."); return false;
        }
        Added = true;
        if (Compile)
        {
            FCompilerResultsLog Log; Log.bSilentMode = true;
            FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
            Compiled = true; Errors = Log.NumErrors; Warnings = Log.NumWarnings;
            CompileOk = Errors == 0 && (BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);
            if (!CompileOk) { Rollback(); E = TEXT("Compilation failed; interface attachment rolled back."); return false; }
        }
        for (const auto& D : BP->ImplementedInterfaces) if (D.Interface == Interface)
            for (const UEdGraph* G : D.Graphs) if (G) Graphs.Add(MakeShared<FJsonValueString>(G->GetName()));
        if (Save)
        {
            if (!BlueprintEditToolUtils::SaveAsset(BP, Filename, E)) { Rollback(); return false; }
            Saved = true; Index = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
        }
        return true;
    }, Error);
    auto Result = BuildBooleanResult(Ok);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("interfaceClassPath"), InterfacePath);
    Result->SetBoolField(TEXT("dryRun"), Dry); Result->SetBoolField(TEXT("alreadyExists"), Already); Result->SetBoolField(TEXT("inherited"), Inherited);
    Result->SetBoolField(TEXT("added"), Added && !RolledBack); Result->SetBoolField(TEXT("wouldAdd"), Dry && !Already && Ok);
    Result->SetBoolField(TEXT("compiled"), Compiled); Result->SetBoolField(TEXT("compileSucceeded"), CompileOk);
    Result->SetNumberField(TEXT("compileErrorCount"), Errors); Result->SetNumberField(TEXT("compileWarningCount"), Warnings);
    Result->SetBoolField(TEXT("rolledBack"), RolledBack); Result->SetBoolField(TEXT("saved"), Saved);
    Result->SetStringField(TEXT("savedFilename"), Filename); Result->SetBoolField(TEXT("indexRefreshed"), Index);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError); Result->SetArrayField(TEXT("createdGraphs"), Graphs);
    if (!Ok) Result->SetStringField(TEXT("error"), Error);
    FMCPResponse Response; Response.Id = Request.Id; Response.Result = Result; return Response;
}
TSharedPtr<FJsonObject> FAddBlueprintInterfaceTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>(); S->SetStringField(TEXT("type"), TEXT("object"));
    auto P = MakeShared<FJsonObject>();
    for (const TCHAR* Field : {TEXT("objectPath"), TEXT("interfaceClassPath")}) P->SetObjectField(Field, BuildStringProperty(TEXT("Exact Blueprint asset or interface class path.")));
    for (const TCHAR* Field : {TEXT("dryRun"), TEXT("compileAfterEdit"), TEXT("saveAfterEdit")}) P->SetObjectField(Field, BuildBoolProperty(TEXT("Dry-run false, compile true, save false by default. Saving forces compilation.")));
    S->SetObjectField(TEXT("properties"), P); S->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("interfaceClassPath"))});
    return S;
}
