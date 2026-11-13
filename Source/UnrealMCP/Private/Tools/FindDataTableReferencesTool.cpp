#include "Tools/FindDataTableReferencesTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Tools/BlueprintToolUtils.h"

FFindDataTableReferencesTool::FFindDataTableReferencesTool()
    : FMCPToolBase(TEXT("FindDataTableReferences"), TEXT("Finds Blueprint nodes with a direct DataTable pin default, scanning registry referencers or one specified Blueprint."))
{
}

UnrealMCP::FMCPResponse FFindDataTableReferencesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, BlueprintPath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("FindDataTableReferences requires objectPath."));
    Request.Params->TryGetStringField(TEXT("blueprintObjectPath"), BlueprintPath);
    double OffsetNumber = 0, MaxAssetsNumber = 100, LimitNumber = 100;
    Request.Params->TryGetNumberField(TEXT("offset"), OffsetNumber);
    Request.Params->TryGetNumberField(TEXT("maxAssets"), MaxAssetsNumber);
    Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
    if (!FMath::IsFinite(OffsetNumber) || OffsetNumber < 0 || OffsetNumber > MAX_int32 - 500
        || !FMath::IsFinite(MaxAssetsNumber) || MaxAssetsNumber < 1 || MaxAssetsNumber > 500
        || !FMath::IsFinite(LimitNumber) || LimitNumber < 1 || LimitNumber > 500)
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("offset must be nonnegative; maxAssets and limit must be 1 to 500."));
    const int32 Offset = FMath::RoundToInt(OffsetNumber), MaxAssets = FMath::RoundToInt(MaxAssetsNumber), Limit = FMath::RoundToInt(LimitNumber);
    int32 CandidateCount = 0, ScannedCount = 0, TotalHits = 0;
    TArray<TSharedPtr<FJsonValue>> Hits;
    FString Error;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UDataTable* Table = LoadObject<UDataTable>(nullptr, *ObjectPath);
        if (!Table) { OutError = TEXT("DataTable not found."); return false; }
        TArray<UBlueprint*> Blueprints;
        if (!BlueprintPath.IsEmpty())
        {
            UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
            if (!Blueprint) { OutError = TEXT("Blueprint not found."); return false; }
            Blueprints.Add(Blueprint);
            CandidateCount = 1;
        }
        else
        {
            FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            TArray<FName> Packages;
            Module.Get().GetReferencers(Table->GetOutermost()->GetFName(), Packages);
            Packages.Sort(FNameLexicalLess());
            CandidateCount = Packages.Num();
            for (int32 Index = Offset; Index < FMath::Min(Offset + MaxAssets, CandidateCount); ++Index)
            {
                TArray<FAssetData> Assets;
                Module.Get().GetAssetsByPackageName(Packages[Index], Assets);
                for (const FAssetData& Asset : Assets)
                    if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset())) Blueprints.Add(Blueprint);
            }
        }
        ScannedCount = Blueprints.Num();
        for (const UBlueprint* Blueprint : Blueprints)
        {
            TArray<UEdGraph*> Graphs;
            Blueprint->GetAllGraphs(Graphs);
            for (const UEdGraph* Graph : Graphs)
            {
                for (const UEdGraphNode* Node : Graph->Nodes)
                {
                    if (!Node) continue;
                    for (const UEdGraphPin* Pin : Node->Pins)
                    {
                        if (!Pin || Pin->DefaultObject != Table) continue;
                        ++TotalHits;
                        if (Hits.Num() >= Limit) continue;
                        TSharedRef<FJsonObject> Hit = MakeShared<FJsonObject>();
                        Hit->SetStringField(TEXT("blueprintObjectPath"), Blueprint->GetPathName());
                        Hit->SetStringField(TEXT("graphPath"), Graph->GetPathName(Blueprint));
                        Hit->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
                        Hit->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetPathName());
                        Hit->SetStringField(TEXT("pin"), Pin->PinName.ToString());
                        Hits.Add(MakeShared<FJsonValueObject>(Hit));
                    }
                }
            }
        }
        return true;
    }, Error);
    if (!bOk) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("scope"), BlueprintPath.IsEmpty() ? TEXT("registry_referencer_packages") : TEXT("one_blueprint"));
    Result->SetNumberField(TEXT("candidatePackageCount"), CandidateCount);
    Result->SetNumberField(TEXT("scannedBlueprintCount"), ScannedCount);
    Result->SetNumberField(TEXT("offset"), Offset);
    Result->SetBoolField(TEXT("hasMoreCandidates"), BlueprintPath.IsEmpty() && Offset + MaxAssets < CandidateCount);
    Result->SetNumberField(TEXT("totalHitsInScannedBlueprints"), TotalHits);
    Result->SetBoolField(TEXT("hitsTruncated"), TotalHits > Hits.Num());
    Result->SetArrayField(TEXT("references"), Hits);
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FFindDataTableReferencesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("objectPath"), TEXT("blueprintObjectPath"), TEXT("offset"), TEXT("maxAssets"), TEXT("limit")})
    {
        TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), FCString::Strcmp(FieldName, TEXT("objectPath")) == 0 || FCString::Strcmp(FieldName, TEXT("blueprintObjectPath")) == 0 ? TEXT("string") : TEXT("integer"));
        Properties->SetObjectField(FieldName, Property);
    }
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath"))});
    return Schema;
}
