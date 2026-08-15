#include "Tools/ValidateBlueprintTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FValidateBlueprintTool::FValidateBlueprintTool()
    : FMCPToolBase(TEXT("ValidateBlueprint"), TEXT("Runs a no-save compile validation for one Blueprint and returns structured diagnostics and dirty state.")) {}

UnrealMCP::FMCPResponse FValidateBlueprintTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    if(!Request.Params.IsValid()||!Request.Params->TryGetStringField(TEXT("objectPath"),ObjectPath)||ObjectPath.IsEmpty())
        return BuildError(Request,UnrealMCP::EMCPErrorCode::InvalidParams,TEXT("ValidateBlueprint requires objectPath."));
    bool bWasDirty=false,bIsDirty=false,bValid=false;int32 ErrorCount=0,WarningCount=0;FString StatusBefore,StatusAfter,ExecutionError;TArray<TSharedPtr<FJsonValue>>Messages;
    const bool bSucceeded=UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint=nullptr;if(!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath,Blueprint,OutError))return false;
        bWasDirty=Blueprint->GetPackage()->IsDirty();StatusBefore=UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status);
        FCompilerResultsLog Log;Log.bSilentMode=true;FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipSave|EBlueprintCompileOptions::SkipGarbageCollection,&Log);
        ErrorCount=Log.NumErrors;WarningCount=Log.NumWarnings;StatusAfter=UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status);bValid=ErrorCount==0&&(Blueprint->Status==BS_UpToDate||Blueprint->Status==BS_UpToDateWithWarnings);bIsDirty=Blueprint->GetPackage()->IsDirty();
        for(const TSharedRef<FTokenizedMessage>& Message:Log.Messages){TSharedRef<FJsonObject>Item=MakeShared<FJsonObject>();const EMessageSeverity::Type Severity=Message->GetSeverity();Item->SetStringField(TEXT("severity"),Severity==EMessageSeverity::Error?TEXT("error"):(Severity==EMessageSeverity::Warning||Severity==EMessageSeverity::PerformanceWarning?TEXT("warning"):TEXT("info")));Item->SetStringField(TEXT("text"),Message->ToText().ToString());Messages.Add(MakeShared<FJsonValueObject>(Item));}return true;
    },ExecutionError);
    if(!bSucceeded)return BuildError(Request,UnrealMCP::EMCPErrorCode::InternalError,ExecutionError);
    UnrealMCP::FMCPResponse Response;Response.Id=Request.Id;TSharedRef<FJsonObject>Result=BuildBooleanResult(bValid);Result->SetStringField(TEXT("objectPath"),ObjectPath);Result->SetBoolField(TEXT("valid"),bValid);Result->SetBoolField(TEXT("saved"),false);Result->SetBoolField(TEXT("wasDirty"),bWasDirty);Result->SetBoolField(TEXT("isDirty"),bIsDirty);Result->SetStringField(TEXT("statusBefore"),StatusBefore);Result->SetStringField(TEXT("statusAfter"),StatusAfter);Result->SetNumberField(TEXT("errorCount"),ErrorCount);Result->SetNumberField(TEXT("warningCount"),WarningCount);Result->SetArrayField(TEXT("messages"),Messages);Response.Result=Result;return Response;
}

TSharedPtr<FJsonObject> FValidateBlueprintTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject>S=MakeShared<FJsonObject>();S->SetStringField(TEXT("type"),TEXT("object"));TSharedRef<FJsonObject>P=MakeShared<FJsonObject>();P->SetObjectField(TEXT("objectPath"),UnrealMCP::BlueprintEditToolUtils::BuildStringProperty(TEXT("Blueprint object path.")));S->SetObjectField(TEXT("properties"),P);TArray<TSharedPtr<FJsonValue>>R{MakeShared<FJsonValueString>(TEXT("objectPath"))};S->SetArrayField(TEXT("required"),R);return S;
}
