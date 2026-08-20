#include "Tools/SetBlueprintComponentDefaultsTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintComponentEditUtils.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FSetBlueprintComponentDefaultsTool::FSetBlueprintComponentDefaultsTool()
    : FMCPToolBase(TEXT("SetBlueprintComponentDefaults"), TEXT("Transactionally validates and updates safely editable defaults on one existing Blueprint component template."))
{
}

UnrealMCP::FMCPResponse FSetBlueprintComponentDefaultsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, ComponentName;
    const TSharedPtr<FJsonObject>* Defaults = nullptr;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("componentName"), ComponentName)
        || !Request.Params->TryGetObjectField(TEXT("propertyDefaults"), Defaults) || !Defaults->IsValid() || (*Defaults)->Values.IsEmpty())
    { return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("SetBlueprintComponentDefaults requires objectPath, componentName, and non-empty propertyDefaults.")); }
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), true);
    bool bChanged = false, bWouldChange = false, bCompileSucceeded = false, bIndexRefreshed = false;
    int32 CompileErrors = 0, CompileWarnings = 0;
    FString ComponentClassPath, SavedFilename, IndexError, Error;
    TArray<BlueprintComponentEditUtils::FPropertyValue> Values;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        USCS_Node* Node = BlueprintComponentEditUtils::FindComponentNode(Blueprint, ComponentName);
        if (Node == nullptr || Node->ComponentTemplate == nullptr) { OutError = TEXT("The requested Blueprint component template was not found."); return false; }
        ComponentName = Node->GetVariableName().ToString(); ComponentClassPath = Node->ComponentTemplate->GetClass()->GetPathName();
        if (!BlueprintComponentEditUtils::ValidatePropertyDefaults(Node->ComponentTemplate, *Defaults, OutError)) return false;
        if (bDryRun)
        {
            UObject* Preview = DuplicateObject<UObject>(Node->ComponentTemplate.Get(), GetTransientPackage());
            if (!BlueprintComponentEditUtils::ApplyPropertyDefaults(Preview, *Defaults, Values, OutError)) return false;
            for (const BlueprintComponentEditUtils::FPropertyValue& Value : Values) bWouldChange |= Value.bChanged;
            return true;
        }
        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetBlueprintComponentDefaults", "UnrealMCP Set Blueprint Component Defaults"));
        Blueprint->Modify(); Node->Modify(); Node->ComponentTemplate->Modify();
        if (!BlueprintComponentEditUtils::ApplyPropertyDefaults(Node->ComponentTemplate, *Defaults, Values, OutError)) return false;
        for (const BlueprintComponentEditUtils::FPropertyValue& Value : Values) bChanged |= Value.bChanged;
        bWouldChange = bChanged;
        if (!bChanged) return true;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        if (bCompile)
        {
            FCompilerResultsLog CompilerLog; CompilerLog.bSilentMode = true;
            FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &CompilerLog);
            CompileErrors = CompilerLog.NumErrors; CompileWarnings = CompilerLog.NumWarnings;
            bCompileSucceeded = CompileErrors == 0 && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
            if (!bCompileSucceeded) { OutError = TEXT("Component defaults changed, but Blueprint compilation failed. Use ValidateBlueprint for diagnostics."); return false; }
        }
        if (bSave && !BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, OutError)) return false;
        if (bSave) bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
        return true;
    }, Error);
    if (!bSucceeded) return BuildError(Request, EMCPErrorCode::InternalError, Error);

    TArray<TSharedPtr<FJsonValue>> PropertyResults;
    for (const BlueprintComponentEditUtils::FPropertyValue& Value : Values)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("name"), Value.Name); Item->SetStringField(TEXT("type"), Value.Type);
        Item->SetStringField(TEXT("previousValue"), Value.PreviousValue); Item->SetStringField(TEXT("value"), Value.Value); Item->SetBoolField(TEXT("changed"), Value.bChanged);
        PropertyResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    FMCPResponse Response; Response.Id = Request.Id; TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("componentName"), ComponentName); Result->SetStringField(TEXT("componentClassPath"), ComponentClassPath);
    Result->SetBoolField(TEXT("dryRun"), bDryRun); Result->SetBoolField(TEXT("wouldChange"), bWouldChange); Result->SetBoolField(TEXT("changed"), bChanged); Result->SetArrayField(TEXT("properties"), PropertyResults);
    Result->SetBoolField(TEXT("compiled"), bCompile && bChanged && !bDryRun); Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetNumberField(TEXT("compileErrorCount"), CompileErrors); Result->SetNumberField(TEXT("compileWarningCount"), CompileWarnings);
    Result->SetBoolField(TEXT("saved"), bSave && bChanged && !bDryRun); Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed); Result->SetStringField(TEXT("indexRefreshError"), IndexError); Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FSetBlueprintComponentDefaultsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object")); TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path."))); Properties->SetObjectField(TEXT("componentName"), BuildStringProperty(TEXT("Exact existing SCS component variable name.")));
    TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>(); Defaults->SetStringField(TEXT("type"), TEXT("object")); Defaults->SetBoolField(TEXT("additionalProperties"), true); Defaults->SetStringField(TEXT("description"), TEXT("Properties expressed as JSON primitives or Unreal export-text strings.")); Properties->SetObjectField(TEXT("propertyDefaults"), Defaults);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate and return normalized values without mutation."))); Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile after a real change; defaults true."))); Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh the index after a real change; defaults true.")));
    Schema->SetObjectField(TEXT("properties"), Properties); Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("componentName")), MakeShared<FJsonValueString>(TEXT("propertyDefaults"))}); return Schema;
}
