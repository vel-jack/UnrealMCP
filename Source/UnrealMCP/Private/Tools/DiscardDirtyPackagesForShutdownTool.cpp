#include "Tools/DiscardDirtyPackagesForShutdownTool.h"

#include "Dom/JsonObject.h"
#include "FileHelpers.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/Package.h"

FDiscardDirtyPackagesForShutdownTool::FDiscardDirtyPackagesForShutdownTool()
    : FMCPToolBase(TEXT("DiscardDirtyPackagesForShutdown"), TEXT("Clears dirty flags on explicitly discarded editor packages before shutdown, without saving them.")) {}

UnrealMCP::FMCPResponse FDiscardDirtyPackagesForShutdownTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bRestoreDirty = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("restoreDirty"), false);
    if (!bDryRun && !UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("DiscardDirtyPackagesForShutdown requires confirm=true unless dryRun=true."));
    TSet<FString> Allowed;
    const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
    if (Request.Params.IsValid() && Request.Params->TryGetArrayField(TEXT("packageNames"), Names))
    {
        for (const TSharedPtr<FJsonValue>& Item : *Names)
        {
            if (!Item.IsValid() || Item->Type != EJson::String)
                return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("packageNames must contain exact package-name strings."));
            Allowed.Add(Item->AsString());
        }
    }
    if (bRestoreDirty && (!Names || Names->IsEmpty()))
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("restoreDirty requires exact packageNames."));
    TArray<TSharedPtr<FJsonValue>> Discarded;
    FString Error;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        TArray<UPackage*> Packages;
        if (bRestoreDirty)
        {
            for (const FString& PackageName : Allowed)
            {
                UPackage* Package = FindPackage(nullptr, *PackageName);
                if (!Package) { OutError = FString::Printf(TEXT("Package '%s' is no longer loaded."), *PackageName); return false; }
                Packages.Add(Package);
            }
        }
        else FEditorFileUtils::GetDirtyPackages(Packages);
        for (UPackage* Package : Packages)
        {
            if (!Package || (Names && !Allowed.Contains(Package->GetName()))) continue;
            Discarded.Add(MakeShared<FJsonValueString>(Package->GetName()));
            if (!bDryRun) Package->SetDirtyFlag(bRestoreDirty);
        }
        return true;
    }, Error);
    if (!bOk) return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, Error);
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("restored"), bRestoreDirty);
    Result->SetArrayField(bRestoreDirty ? TEXT("restoredPackages") : TEXT("discardedPackages"), Discarded);
    Result->SetNumberField(TEXT("count"), Discarded.Num());
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FDiscardDirtyPackagesForShutdownTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("confirm"), UnrealMCP::BlueprintEditToolUtils::BuildBoolProperty(TEXT("Required to discard unsaved package changes.")));
    Properties->SetObjectField(TEXT("dryRun"), UnrealMCP::BlueprintEditToolUtils::BuildBoolProperty(TEXT("List dirty packages without clearing flags.")));
    Properties->SetObjectField(TEXT("restoreDirty"), UnrealMCP::BlueprintEditToolUtils::BuildBoolProperty(TEXT("Restore dirty flags for exact packageNames if shutdown did not finish.")));
    TSharedRef<FJsonObject> Names = MakeShared<FJsonObject>();
    Names->SetStringField(TEXT("type"), TEXT("array"));
    Names->SetStringField(TEXT("description"), TEXT("Optional exact package names to discard; omitted means all dirty packages."));
    TSharedRef<FJsonObject> StringType = MakeShared<FJsonObject>();
    StringType->SetStringField(TEXT("type"), TEXT("string"));
    Names->SetObjectField(TEXT("items"), StringType);
    Properties->SetObjectField(TEXT("packageNames"), Names);
    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
