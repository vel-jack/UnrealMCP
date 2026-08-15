#include "Tools/AddBlueprintVariableTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintVariableTool::FAddBlueprintVariableTool()
    : FMCPToolBase(TEXT("AddBlueprintVariable"), TEXT("Transactionally adds a typed Blueprint member variable with optional default, instance-editable metadata, compile, and save.")) {}

UnrealMCP::FMCPResponse FAddBlueprintVariableTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, VariableName, TypeName, TypeObjectPath, DefaultValue, Category;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("variableName"), VariableName) || !Request.Params->TryGetStringField(TEXT("type"), TypeName))
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintVariable requires objectPath, variableName, and type."));
    Request.Params->TryGetStringField(TEXT("typeObjectPath"), TypeObjectPath); Request.Params->TryGetStringField(TEXT("defaultValue"), DefaultValue); Request.Params->TryGetStringField(TEXT("category"), Category);
    const bool bArray=UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params,TEXT("isArray"),false), bInstanceEditable=UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params,TEXT("instanceEditable"),false);
    const bool bDryRun=UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params,TEXT("dryRun"),false), bCompile=UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params,TEXT("compileAfterEdit"),false), bSave=UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params,TEXT("saveAfterEdit"),false);
    bool bAlreadyExists=false,bCompileSucceeded=false,bIndexRefreshed=false; FString SavedFilename,ExecutionError,IndexRefreshError;
    const bool bSucceeded=UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint=nullptr; if(!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath,Blueprint,OutError))return false;
        FEdGraphPinType PinType; if(!UnrealMCP::BlueprintEditToolUtils::BuildPinType(TypeName,TypeObjectPath,bArray,PinType,OutError))return false;
        for(const FBPVariableDescription& Variable:Blueprint->NewVariables) if(Variable.VarName.ToString().Equals(VariableName,ESearchCase::IgnoreCase)){bAlreadyExists=true; return true;}
        if(bDryRun)return true;
        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP","AddBlueprintVariable","UnrealMCP Add Blueprint Variable")); Blueprint->Modify();
        if(!FBlueprintEditorUtils::AddMemberVariable(Blueprint,*VariableName,PinType,DefaultValue)){OutError=TEXT("Unreal failed to add the Blueprint member variable.");return false;}
        FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint,*VariableName,!bInstanceEditable);
        if(!Category.IsEmpty())FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint,*VariableName,nullptr,FText::FromString(Category),true);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        if(bCompile){FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipSave|EBlueprintCompileOptions::SkipGarbageCollection); bCompileSucceeded=Blueprint->Status==BS_UpToDate||Blueprint->Status==BS_UpToDateWithWarnings; if(!bCompileSucceeded){OutError=TEXT("Variable was added, but compilation failed; use CompileBlueprint for diagnostics.");return false;}}
        if(bSave&&!UnrealMCP::BlueprintEditToolUtils::SaveAsset(Blueprint,SavedFilename,OutError))return false;
        if(bSave)bIndexRefreshed=UnrealMCP::BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath,IndexRefreshError);
        return true;
    },ExecutionError);
    if(!bSucceeded)return BuildError(Request,UnrealMCP::EMCPErrorCode::InternalError,ExecutionError);
    UnrealMCP::FMCPResponse Response;Response.Id=Request.Id;TSharedRef<FJsonObject>Result=BuildBooleanResult(true);Result->SetStringField(TEXT("objectPath"),ObjectPath);Result->SetStringField(TEXT("variableName"),VariableName);Result->SetStringField(TEXT("type"),TypeName);Result->SetStringField(TEXT("typeObjectPath"),TypeObjectPath);Result->SetBoolField(TEXT("isArray"),bArray);Result->SetBoolField(TEXT("instanceEditable"),bInstanceEditable);Result->SetBoolField(TEXT("dryRun"),bDryRun);Result->SetBoolField(TEXT("alreadyExists"),bAlreadyExists);Result->SetBoolField(TEXT("added"),!bDryRun&&!bAlreadyExists);Result->SetBoolField(TEXT("compiled"),bCompile&&!bDryRun&&!bAlreadyExists);Result->SetBoolField(TEXT("compileSucceeded"),bCompileSucceeded);Result->SetBoolField(TEXT("saved"),bSave&&!bDryRun&&!bAlreadyExists);Result->SetStringField(TEXT("savedFilename"),SavedFilename);Result->SetBoolField(TEXT("indexRefreshed"),bIndexRefreshed);Result->SetStringField(TEXT("indexRefreshError"),IndexRefreshError);Response.Result=Result;return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintVariableTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;TSharedRef<FJsonObject>S=MakeShared<FJsonObject>();S->SetStringField(TEXT("type"),TEXT("object"));TSharedRef<FJsonObject>P=MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"),BuildStringProperty(TEXT("Target Blueprint object path.")));P->SetObjectField(TEXT("variableName"),BuildStringProperty(TEXT("Unique member variable name.")));P->SetObjectField(TEXT("type"),BuildStringProperty(TEXT("bool, float, int, string, name, text, object, or class.")));P->SetObjectField(TEXT("typeObjectPath"),BuildStringProperty(TEXT("Required class path for object/class types.")));P->SetObjectField(TEXT("defaultValue"),BuildStringProperty(TEXT("Optional Unreal-exported default literal.")));P->SetObjectField(TEXT("category"),BuildStringProperty(TEXT("Optional variable category.")));P->SetObjectField(TEXT("isArray"),BuildBoolProperty(TEXT("Create an array variable.")));P->SetObjectField(TEXT("instanceEditable"),BuildBoolProperty(TEXT("Allow editing on Blueprint instances.")));P->SetObjectField(TEXT("dryRun"),BuildBoolProperty(TEXT("Validate without mutation.")));P->SetObjectField(TEXT("compileAfterEdit"),BuildBoolProperty(TEXT("Compile only this Blueprint.")));P->SetObjectField(TEXT("saveAfterEdit"),BuildBoolProperty(TEXT("Save after mutation.")));S->SetObjectField(TEXT("properties"),P);TArray<TSharedPtr<FJsonValue>>R{MakeShared<FJsonValueString>(TEXT("objectPath")),MakeShared<FJsonValueString>(TEXT("variableName")),MakeShared<FJsonValueString>(TEXT("type"))};S->SetArrayField(TEXT("required"),R);return S;
}
