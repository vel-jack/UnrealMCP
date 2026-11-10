#include "Tools/ConsolidateAndDeleteAssetTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    TArray<TSharedPtr<FJsonValue>> NamesToJson(const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FName& Name : Names)
        {
            Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
        }
        return Values;
    }

    // Referencing packages other than the asset's own package.
    TArray<FName> GetExternalReferencers(IAssetRegistry& Registry, const FName Package)
    {
        TArray<FName> Referencers;
        Registry.GetReferencers(Package, Referencers);
        Referencers.Remove(Package);
        Referencers.Sort(FNameLexicalLess());
        return Referencers;
    }
}

FConsolidateAndDeleteAssetTool::FConsolidateAndDeleteAssetTool()
    : FMCPToolBase(TEXT("ConsolidateAndDeleteAsset"), TEXT("Editor 'Replace References' equivalent: repoints every referencer of oldAssetPath to replacementAssetPath, then deletes the old asset (leaving a redirector as the editor does). Requires confirm=true unless dryRun=true; dry-run lists the referencers that would be touched. Does not save: dirtied packages are returned so they can be saved explicitly."))
{
}

UnrealMCP::FMCPResponse FConsolidateAndDeleteAssetTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString OldPath;
    FString NewPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("oldAssetPath"), OldPath) || OldPath.IsEmpty()
        || !Request.Params->TryGetStringField(TEXT("replacementAssetPath"), NewPath) || NewPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ConsolidateAndDeleteAsset requires oldAssetPath and replacementAssetPath."));
    }
    if (OldPath == NewPath)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("oldAssetPath and replacementAssetPath must differ."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bConfirm = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false);
    if (!bDryRun && !bConfirm)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ConsolidateAndDeleteAsset deletes an asset and requires confirm=true unless dryRun=true."));
    }

    TArray<FName> ReferencersBefore;
    TArray<FName> ReferencersAfter;
    TArray<FName> Dirtied;
    TArray<FString> Failed;
    bool bOldStillExists = false;
    FString ExecutionError;

    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            UObject* OldObject = LoadObject<UObject>(nullptr, *OldPath);
            UObject* NewObject = LoadObject<UObject>(nullptr, *NewPath);
            if (!OldObject || !NewObject)
            {
                OutError = FString::Printf(TEXT("Could not load %s asset."), OldObject ? TEXT("replacement") : TEXT("old"));
                return false;
            }

            FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            const FName OldPackage = OldObject->GetOutermost()->GetFName();
            ReferencersBefore = GetExternalReferencers(Module.Get(), OldPackage);
            if (bDryRun)
            {
                return true;
            }

            TArray<UObject*> ToConsolidate{OldObject};
            ObjectTools::FConsolidationResults Results;
            {
                const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "ConsolidateAndDelete", "UnrealMCP Consolidate And Delete Asset"));
                Results = ObjectTools::ConsolidateObjects(NewObject, ToConsolidate, /*bShowDeleteConfirmation=*/false);
            }

            for (const UObject* Bad : Results.InvalidConsolidationObjs)
            {
                Failed.Add(FString::Printf(TEXT("invalid_for_consolidation: %s"), *GetNameSafe(Bad)));
            }
            for (const UObject* Bad : Results.FailedConsolidationObjs)
            {
                Failed.Add(FString::Printf(TEXT("partially_consolidated: %s"), *GetNameSafe(Bad)));
            }
            for (const UPackage* Package : Results.DirtiedPackages)
            {
                Dirtied.Add(Package->GetFName());
            }

            bOldStillExists = FindObject<UObject>(nullptr, *OldPath) != nullptr
                && !FindObject<UObject>(nullptr, *OldPath)->IsA<UObjectRedirector>();
            ReferencersAfter = GetExternalReferencers(Module.Get(), OldPackage);
            return true;
        },
        ExecutionError);

    if (!bOk)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    const bool bSuccess = bDryRun || (Failed.IsEmpty() && !bOldStillExists);

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(bSuccess);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetStringField(TEXT("oldAssetPath"), OldPath);
    Result->SetStringField(TEXT("replacementAssetPath"), NewPath);
    Result->SetArrayField(TEXT("referencersBefore"), NamesToJson(ReferencersBefore));
    if (!bDryRun)
    {
        Result->SetBoolField(TEXT("oldAssetStillExists"), bOldStillExists);
        Result->SetArrayField(TEXT("dirtiedPackages"), NamesToJson(Dirtied));
        // Registry data can lag a delete; a redirector package legitimately remains until it is fixed up and saved.
        Result->SetArrayField(TEXT("referencersAfter"), NamesToJson(ReferencersAfter));
        TArray<TSharedPtr<FJsonValue>> FailedValues;
        for (const FString& Entry : Failed)
        {
            FailedValues.Add(MakeShared<FJsonValueString>(Entry));
        }
        Result->SetArrayField(TEXT("failures"), FailedValues);
        Result->SetBoolField(TEXT("saved"), false);
        Result->SetStringField(TEXT("nextStepHint"), TEXT("Save dirtied packages explicitly (SaveValidatedBlueprints / SaveAllDirtyPackages), then recompile referencers."));
    }
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FConsolidateAndDeleteAssetTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    auto AddProp = [&Properties](const TCHAR* PropName, const TCHAR* PropType, const TCHAR* PropDesc)
    {
        TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
        Prop->SetStringField(TEXT("type"), PropType);
        Prop->SetStringField(TEXT("description"), PropDesc);
        Properties->SetObjectField(PropName, Prop);
    };
    AddProp(TEXT("oldAssetPath"), TEXT("string"), TEXT("Asset to retire, e.g. /Game/BP_Vehicle.BP_Vehicle"));
    AddProp(TEXT("replacementAssetPath"), TEXT("string"), TEXT("Asset all references are repointed to; must be a compatible type."));
    AddProp(TEXT("dryRun"), TEXT("boolean"), TEXT("List referencers without changing anything."));
    AddProp(TEXT("confirm"), TEXT("boolean"), TEXT("Required for a real run: the old asset is deleted."));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("oldAssetPath")));
    Required.Add(MakeShared<FJsonValueString>(TEXT("replacementAssetPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
