#include "Tools/PlanProjectRefactorTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tools/AnalyzeBlueprintCouplingTool.h"
#include "Tools/AnalyzeFeatureBoundaryTool.h"
#include "Tools/FindBrokenBlueprintReferencesTool.h"
#include "Tools/FindCircularDependenciesTool.h"
#include "Tools/FindUnusedBlueprintAssetsTool.h"

namespace
{
    constexpr int32 DefaultMaxActions = 25;
    constexpr int32 MaximumMaxActions = 100;
    constexpr int32 MaximumFeatureBoundaries = 20;

    struct FPlanAction
    {
        int32 CategoryOrder = 0;
        FString Severity;
        FString Category;
        FString Title;
        FString Rationale;
        TArray<FString> EvidenceRefs;
        TArray<FString> ObjectPaths;
        TArray<FString> PackagePaths;
        TArray<FString> RecommendedSteps;
        FString VerificationTool;
        TSharedPtr<FJsonObject> VerificationParams;
        TSharedPtr<FJsonObject> Evidence;
        bool bRequiresUserConfirmation = false;
        FString Confidence;
    };

    FString NormalizePackagePath(FString Path)
    {
        Path.TrimStartAndEndInline();
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
        {
            Path.LeftChopInline(1);
        }
        return Path;
    }

    TArray<TSharedPtr<FJsonValue>> StringValues(TArray<FString> Values)
    {
        Values.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
        Values.Sort();
        TArray<TSharedPtr<FJsonValue>> Result;
        Result.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            if (Result.IsEmpty() || Result.Last()->AsString() != Value)
            {
                Result.Add(MakeShared<FJsonValueString>(Value));
            }
        }
        return Result;
    }

    TArray<TSharedPtr<FJsonValue>> OrderedStringValues(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FString& Value : Values)
        {
            if (!Value.IsEmpty())
            {
                Result.Add(MakeShared<FJsonValueString>(Value));
            }
        }
        return Result;
    }

    FString ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        FString Value;
        if (Object.IsValid())
        {
            Object->TryGetStringField(Field, Value);
        }
        return Value;
    }

    int32 SeverityRank(const FString& Severity)
    {
        if (Severity == TEXT("critical")) return 0;
        if (Severity == TEXT("error") || Severity == TEXT("high")) return 1;
        if (Severity == TEXT("warning") || Severity == TEXT("medium")) return 2;
        return 3;
    }

    TSharedRef<FJsonObject> MakeScopeParams(const FString& RootPath, const int32 MaxResults)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        if (!RootPath.IsEmpty())
        {
            Params->SetStringField(TEXT("rootPath"), RootPath);
        }
        Params->SetNumberField(TEXT("maxResults"), MaxResults);
        return Params;
    }

    UnrealMCP::FMCPResponse RunAnalysis(const IMCPTool& Tool, const TSharedPtr<FJsonObject>& Params)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = TEXT("PlanProjectRefactor.analysis");
        Request.Params = Params;
        return Tool.Execute(Request);
    }

    void AddWarning(
        TArray<TSharedPtr<FJsonValue>>& Warnings,
        const FString& Section,
        const UnrealMCP::FMCPResponse& Response)
    {
        TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
        Warning->SetStringField(TEXT("section"), Section);
        Warning->SetStringField(
            TEXT("message"),
            Response.Error.IsSet() ? Response.Error.GetValue().Message : TEXT("Analysis returned no result."));
        Warning->SetBoolField(TEXT("continued"), true);
        Warnings.Add(MakeShared<FJsonValueObject>(Warning));
    }

    TSharedRef<FJsonObject> ScopeVerificationParams(const FString& RootPath)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        if (!RootPath.IsEmpty())
        {
            Params->SetStringField(TEXT("rootPath"), RootPath);
        }
        return Params;
    }

    FString ActionKey(const FPlanAction& Action)
    {
        TArray<FString> Objects = Action.ObjectPaths;
        TArray<FString> Packages = Action.PackagePaths;
        Objects.Sort();
        Packages.Sort();
        return Action.Category + TEXT("|") + FString::Join(Objects, TEXT(",")) + TEXT("|") + FString::Join(Packages, TEXT(","));
    }

    TSharedRef<FJsonObject> SerializeAction(const FPlanAction& Action, const int32 Priority, const bool bIncludeEvidence)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("priority"), Priority);
        Json->SetStringField(TEXT("severity"), Action.Severity);
        Json->SetStringField(TEXT("category"), Action.Category);
        Json->SetStringField(TEXT("title"), Action.Title);
        Json->SetStringField(TEXT("rationale"), Action.Rationale);
        Json->SetArrayField(TEXT("evidenceRefs"), StringValues(Action.EvidenceRefs));
        Json->SetArrayField(TEXT("objectPaths"), StringValues(Action.ObjectPaths));
        Json->SetArrayField(TEXT("packagePaths"), StringValues(Action.PackagePaths));
        Json->SetArrayField(TEXT("recommendedSteps"), OrderedStringValues(Action.RecommendedSteps));
        Json->SetStringField(TEXT("verificationTool"), Action.VerificationTool);
        Json->SetObjectField(TEXT("verificationParams"), Action.VerificationParams.IsValid() ? Action.VerificationParams.ToSharedRef() : MakeShared<FJsonObject>());
        Json->SetBoolField(TEXT("destructive"), false);
        Json->SetBoolField(TEXT("requiresUserConfirmation"), Action.bRequiresUserConfirmation);
        Json->SetStringField(TEXT("confidence"), Action.Confidence);
        if (bIncludeEvidence && Action.Evidence.IsValid())
        {
            Json->SetObjectField(TEXT("evidence"), Action.Evidence.ToSharedRef());
        }
        return Json;
    }
}

FPlanProjectRefactorTool::FPlanProjectRefactorTool()
    : FMCPToolBase(
        TEXT("PlanProjectRefactor"),
        TEXT("Builds a deterministic, evidence-backed, non-mutating Blueprint refactor plan from indexed architecture analyses. It never changes assets and never treats unused candidates as deletion instructions."))
{
}

UnrealMCP::FMCPResponse FPlanProjectRefactorTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString RootPath;
    FString PackagePath;
    int32 MaxActions = DefaultMaxActions;
    bool bIncludeEvidence = false;
    TArray<FString> FeatureBoundaries;

    if (Request.Params.IsValid())
    {
        Request.Params->TryGetStringField(TEXT("rootPath"), RootPath);
        Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath);
        if (Request.Params->HasField(TEXT("includeEvidence"))
            && !Request.Params->TryGetBoolField(TEXT("includeEvidence"), bIncludeEvidence))
        {
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("includeEvidence must be a boolean."));
        }

        if (Request.Params->HasField(TEXT("maxActions")))
        {
            double Value = 0.0;
            if (!Request.Params->TryGetNumberField(TEXT("maxActions"), Value)
                || !FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value))
                || Value < 1.0 || Value > MaximumMaxActions)
            {
                return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("maxActions must be an integer from 1 to 100."));
            }
            MaxActions = FMath::RoundToInt(Value);
        }

        const TArray<TSharedPtr<FJsonValue>>* BoundaryValues = nullptr;
        if (Request.Params->TryGetArrayField(TEXT("featureBoundaries"), BoundaryValues))
        {
            if (BoundaryValues->Num() > MaximumFeatureBoundaries)
            {
                return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("featureBoundaries accepts at most 20 paths."));
            }
            for (const TSharedPtr<FJsonValue>& Value : *BoundaryValues)
            {
                FString Boundary;
                if (!Value.IsValid() || !Value->TryGetString(Boundary))
                {
                    return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("featureBoundaries must contain only strings."));
                }
                Boundary = NormalizePackagePath(Boundary);
                if (!Boundary.StartsWith(TEXT("/Game"), ESearchCase::IgnoreCase))
                {
                    return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Every feature boundary must begin with /Game."));
                }
                FeatureBoundaries.AddUnique(Boundary);
            }
        }
    }

    RootPath = NormalizePackagePath(RootPath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!RootPath.IsEmpty() && !PackagePath.IsEmpty() && !RootPath.Equals(PackagePath, ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("rootPath and packagePath must match when both are supplied."));
    }
    const FString ScopePath = !PackagePath.IsEmpty() ? PackagePath : RootPath;
    if (!ScopePath.IsEmpty() && !ScopePath.StartsWith(TEXT("/Game"), ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("rootPath/packagePath must begin with /Game."));
    }
    FeatureBoundaries.Sort();

    const int32 AnalysisLimit = MaximumMaxActions;
    TArray<FPlanAction> Actions;
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TArray<TSharedPtr<FJsonValue>> AnalysisSummary;

    auto RecordSummary = [&](const FString& Section, const UnrealMCP::FMCPResponse& Response)
    {
        TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetStringField(TEXT("section"), Section);
        Summary->SetBoolField(TEXT("success"), Response.Result.IsValid() && !Response.Error.IsSet());
        AnalysisSummary.Add(MakeShared<FJsonValueObject>(Summary));
        if (!Response.Result.IsValid() || Response.Error.IsSet())
        {
            AddWarning(Warnings, Section, Response);
            return false;
        }
        return true;
    };

    const FFindBrokenBlueprintReferencesTool BrokenTool;
    const UnrealMCP::FMCPResponse BrokenResponse = RunAnalysis(BrokenTool, MakeScopeParams(ScopePath, AnalysisLimit));
    if (RecordSummary(TEXT("brokenReferences"), BrokenResponse))
    {
        const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
        if (BrokenResponse.Result->TryGetArrayField(TEXT("findings"), Findings))
        {
            for (int32 Index = 0; Index < Findings->Num(); ++Index)
            {
                const TSharedPtr<FJsonObject> Finding = (*Findings)[Index]->AsObject();
                FPlanAction Action;
                Action.CategoryOrder = 0;
                Action.Severity = ReadString(Finding, TEXT("severity"));
                Action.Category = ReadString(Finding, TEXT("category")).Contains(TEXT("edge_")) ? TEXT("repair_index_integrity") : TEXT("repair_broken_reference");
                Action.Title = FString::Printf(TEXT("Repair %s in %s"), *ReadString(Finding, TEXT("category")), *ReadString(Finding, TEXT("blueprintObjectPath")));
                Action.Rationale = ReadString(Finding, TEXT("evidence"));
                Action.EvidenceRefs = {FString::Printf(TEXT("FindBrokenBlueprintReferences.findings[%d]"), Index)};
                Action.ObjectPaths = {ReadString(Finding, TEXT("blueprintObjectPath"))};
                Action.PackagePaths = {ReadString(Finding, TEXT("referencedPath"))};
                Action.RecommendedSteps = {ReadString(Finding, TEXT("recommendedAction")), TEXT("Refresh the affected Blueprint index entry and validate the Blueprint after repair.")};
                Action.VerificationTool = TEXT("FindBrokenBlueprintReferences");
                Action.VerificationParams = ScopeVerificationParams(ScopePath);
                Action.VerificationParams->SetStringField(TEXT("objectPath"), ReadString(Finding, TEXT("blueprintObjectPath")));
                Action.Confidence = TEXT("high");
                Action.Evidence = Finding;
                Actions.Add(MoveTemp(Action));
            }
        }
    }

    const FFindCircularDependenciesTool CircularTool;
    const UnrealMCP::FMCPResponse CircularResponse = RunAnalysis(CircularTool, MakeScopeParams(ScopePath, AnalysisLimit));
    if (RecordSummary(TEXT("circularDependencies"), CircularResponse))
    {
        const TArray<TSharedPtr<FJsonValue>>* Cycles = nullptr;
        if (CircularResponse.Result->TryGetArrayField(TEXT("cycles"), Cycles))
        {
            for (int32 Index = 0; Index < Cycles->Num(); ++Index)
            {
                const TSharedPtr<FJsonObject> Cycle = (*Cycles)[Index]->AsObject();
                FPlanAction Action;
                Action.CategoryOrder = 1;
                Action.Severity = TEXT("high");
                Action.Category = TEXT("break_circular_dependency");
                Action.Title = FString::Printf(TEXT("Break circular dependency group %d"), Index + 1);
                Action.Rationale = ReadString(Cycle, TEXT("description"));
                Action.EvidenceRefs = {FString::Printf(TEXT("FindCircularDependencies.cycles[%d]"), Index)};
                const TArray<TSharedPtr<FJsonValue>>* Blueprints = nullptr;
                if (Cycle->TryGetArrayField(TEXT("blueprints"), Blueprints))
                {
                    for (const TSharedPtr<FJsonValue>& Value : *Blueprints)
                    {
                        const TSharedPtr<FJsonObject> Blueprint = Value->AsObject();
                        Action.ObjectPaths.Add(ReadString(Blueprint, TEXT("objectPath")));
                        Action.PackagePaths.Add(ReadString(Blueprint, TEXT("packageName")));
                    }
                }
                Action.RecommendedSteps = {ReadString(Cycle, TEXT("recommendedAction")), TEXT("Choose one stable dependency seam and verify the cycle disappears after the refactor.")};
                Action.VerificationTool = TEXT("FindCircularDependencies");
                Action.VerificationParams = ScopeVerificationParams(ScopePath);
                Action.Confidence = TEXT("high");
                Action.Evidence = Cycle;
                Actions.Add(MoveTemp(Action));
            }
        }
    }

    const FAnalyzeBlueprintCouplingTool CouplingTool;
    const UnrealMCP::FMCPResponse CouplingResponse = RunAnalysis(CouplingTool, MakeScopeParams(ScopePath, AnalysisLimit));
    if (RecordSummary(TEXT("blueprintCoupling"), CouplingResponse))
    {
        const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
        if (CouplingResponse.Result->TryGetArrayField(TEXT("assets"), Assets))
        {
            for (int32 Index = 0; Index < Assets->Num(); ++Index)
            {
                const TSharedPtr<FJsonObject> Asset = (*Assets)[Index]->AsObject();
                if (ReadString(Asset, TEXT("riskLevel")) != TEXT("high")) continue;
                FPlanAction Action;
                Action.CategoryOrder = 2;
                Action.Severity = TEXT("high");
                Action.Category = TEXT("reduce_blueprint_coupling");
                Action.Title = FString::Printf(TEXT("Reduce coupling around %s"), *ReadString(Asset, TEXT("assetName")));
                Action.Rationale = TEXT("This Blueprint exceeds an explicit indexed package-dependency coupling threshold.");
                Action.EvidenceRefs = {FString::Printf(TEXT("AnalyzeBlueprintCoupling.assets[%d]"), Index)};
                Action.ObjectPaths = {ReadString(Asset, TEXT("objectPath"))};
                Action.PackagePaths = {ReadString(Asset, TEXT("packageName"))};
                Action.RecommendedSteps = {TEXT("Inspect the highest incoming and outgoing dependency seams."), TEXT("Prefer a stable interface, event, or ownership boundary before moving behavior.")};
                Action.VerificationTool = TEXT("AnalyzeBlueprintCoupling");
                Action.VerificationParams = ScopeVerificationParams(ScopePath);
                Action.VerificationParams->SetStringField(TEXT("objectPath"), ReadString(Asset, TEXT("objectPath")));
                Action.Confidence = TEXT("high");
                Action.Evidence = Asset;
                Actions.Add(MoveTemp(Action));
            }
        }
    }

    const FAnalyzeFeatureBoundaryTool BoundaryTool;
    for (const FString& Boundary : FeatureBoundaries)
    {
        const UnrealMCP::FMCPResponse BoundaryResponse = RunAnalysis(BoundaryTool, MakeScopeParams(Boundary, AnalysisLimit));
        if (!RecordSummary(FString::Printf(TEXT("featureBoundary:%s"), *Boundary), BoundaryResponse)) continue;
        const FString Risk = ReadString(BoundaryResponse.Result, TEXT("riskLevel"));
        if (Risk != TEXT("high") && Risk != TEXT("medium")) continue;
        FPlanAction Action;
        Action.CategoryOrder = 3;
        Action.Severity = Risk;
        Action.Category = TEXT("reduce_feature_boundary_crossings");
        Action.Title = FString::Printf(TEXT("Reduce %s-risk crossings at %s"), *Risk, *Boundary);
        Action.Rationale = TEXT("The indexed feature boundary has package-dependency crossings above the configured risk threshold.");
        Action.EvidenceRefs = {FString::Printf(TEXT("AnalyzeFeatureBoundary[%s]"), *Boundary)};
        Action.PackagePaths = {Boundary};
        const TArray<TSharedPtr<FJsonValue>>* BoundaryBlueprints = nullptr;
        if (BoundaryResponse.Result->TryGetArrayField(TEXT("boundaryBlueprints"), BoundaryBlueprints))
        {
            for (const TSharedPtr<FJsonValue>& Value : *BoundaryBlueprints)
            {
                Action.ObjectPaths.Add(ReadString(Value->AsObject(), TEXT("objectPath")));
            }
        }
        Action.RecommendedSteps = {TEXT("Review the highest-ranked boundary Blueprints and crossing edges."), TEXT("Consolidate contracts at an interface or event boundary, then re-run boundary analysis.")};
        Action.VerificationTool = TEXT("AnalyzeFeatureBoundary");
        Action.VerificationParams = MakeShared<FJsonObject>();
        Action.VerificationParams->SetStringField(TEXT("rootPath"), Boundary);
        Action.Confidence = TEXT("high");
        Action.Evidence = BoundaryResponse.Result;
        Actions.Add(MoveTemp(Action));
    }

    const FFindUnusedBlueprintAssetsTool UnusedTool;
    const UnrealMCP::FMCPResponse UnusedResponse = RunAnalysis(UnusedTool, MakeScopeParams(ScopePath, AnalysisLimit));
    if (RecordSummary(TEXT("unusedBlueprintCandidates"), UnusedResponse))
    {
        const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
        if (UnusedResponse.Result->TryGetArrayField(TEXT("candidates"), Candidates))
        {
            for (int32 Index = 0; Index < Candidates->Num(); ++Index)
            {
                const TSharedPtr<FJsonObject> Candidate = (*Candidates)[Index]->AsObject();
                FPlanAction Action;
                Action.CategoryOrder = 4;
                Action.Severity = TEXT("low");
                Action.Category = TEXT("review_unused_candidate");
                Action.Title = FString::Printf(TEXT("Manually review possible unused Blueprint %s"), *ReadString(Candidate, TEXT("assetName")));
                Action.Rationale = TEXT("The asset has zero indexed incoming project dependencies, but indirect and runtime entry points may be invisible.");
                Action.EvidenceRefs = {FString::Printf(TEXT("FindUnusedBlueprintAssets.candidates[%d]"), Index)};
                Action.ObjectPaths = {ReadString(Candidate, TEXT("objectPath"))};
                Action.PackagePaths = {ReadString(Candidate, TEXT("packageName"))};
                Action.RecommendedSteps = {TEXT("Review maps, project settings, soft references, Asset Manager roots, C++ usage, and runtime loading."), TEXT("Keep the asset unless a user confirms removal after independent validation.")};
                Action.VerificationTool = TEXT("FindUnusedBlueprintAssets");
                Action.VerificationParams = ScopeVerificationParams(ScopePath);
                Action.bRequiresUserConfirmation = true;
                Action.Confidence = ReadString(Candidate, TEXT("candidateConfidence"));
                Action.Evidence = Candidate;
                Actions.Add(MoveTemp(Action));
            }
        }
    }

    Actions.Sort([](const FPlanAction& A, const FPlanAction& B)
    {
        if (A.CategoryOrder != B.CategoryOrder) return A.CategoryOrder < B.CategoryOrder;
        if (SeverityRank(A.Severity) != SeverityRank(B.Severity)) return SeverityRank(A.Severity) < SeverityRank(B.Severity);
        if (A.Title != B.Title) return A.Title < B.Title;
        return ActionKey(A) < ActionKey(B);
    });
    TSet<FString> Seen;
    TArray<FPlanAction> UniqueActions;
    for (FPlanAction& Action : Actions)
    {
        const FString Key = ActionKey(Action);
        if (!Seen.Contains(Key))
        {
            Seen.Add(Key);
            UniqueActions.Add(MoveTemp(Action));
        }
    }

    const int32 TotalActionCount = UniqueActions.Num();
    const int32 ReturnedActionCount = FMath::Min(TotalActionCount, MaxActions);
    TArray<TSharedPtr<FJsonValue>> ActionValues;
    for (int32 Index = 0; Index < ReturnedActionCount; ++Index)
    {
        ActionValues.Add(MakeShared<FJsonValueObject>(SerializeAction(UniqueActions[Index], Index + 1, bIncludeEvidence)));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("analysisType"), TEXT("indexed_non_mutating_project_refactor_plan"));
    Result->SetStringField(TEXT("rootPath"), ScopePath);
    Result->SetArrayField(TEXT("featureBoundaries"), StringValues(FeatureBoundaries));
    Result->SetNumberField(TEXT("maxActions"), MaxActions);
    Result->SetBoolField(TEXT("includeEvidence"), bIncludeEvidence);
    Result->SetNumberField(TEXT("totalActionCount"), TotalActionCount);
    Result->SetNumberField(TEXT("returnedActionCount"), ReturnedActionCount);
    Result->SetBoolField(TEXT("truncated"), ReturnedActionCount < TotalActionCount);
    Result->SetNumberField(TEXT("warningCount"), Warnings.Num());
    Result->SetArrayField(TEXT("warnings"), Warnings);
    Result->SetArrayField(TEXT("analysisSections"), AnalysisSummary);
    Result->SetArrayField(TEXT("actions"), ActionValues);
    Result->SetBoolField(TEXT("assetsChanged"), false);
    Result->SetStringField(TEXT("safetyStatement"), TEXT("No assets were changed. Unused Blueprint candidates are manual review leads only and are never deletion instructions."));
    Result->SetStringField(TEXT("scopeLimitation"), TEXT("This plan uses indexed static Blueprint and package evidence. It does not include C++ migration actions or prove runtime behavior."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FPlanProjectRefactorTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    for (const FString Field : {FString(TEXT("rootPath")), FString(TEXT("packagePath"))})
    {
        TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), TEXT("string"));
        Property->SetStringField(TEXT("description"), TEXT("Optional /Game package-path scope. rootPath and packagePath are aliases."));
        Properties->SetObjectField(Field, Property);
    }

    TSharedRef<FJsonObject> Boundaries = MakeShared<FJsonObject>();
    Boundaries->SetStringField(TEXT("type"), TEXT("array"));
    Boundaries->SetNumberField(TEXT("maxItems"), MaximumFeatureBoundaries);
    Boundaries->SetStringField(TEXT("description"), TEXT("Optional /Game feature boundaries to analyze independently, up to 20."));
    TSharedRef<FJsonObject> BoundaryItem = MakeShared<FJsonObject>();
    BoundaryItem->SetStringField(TEXT("type"), TEXT("string"));
    Boundaries->SetObjectField(TEXT("items"), BoundaryItem);
    Properties->SetObjectField(TEXT("featureBoundaries"), Boundaries);

    TSharedRef<FJsonObject> MaxActions = MakeShared<FJsonObject>();
    MaxActions->SetStringField(TEXT("type"), TEXT("integer"));
    MaxActions->SetNumberField(TEXT("default"), DefaultMaxActions);
    MaxActions->SetNumberField(TEXT("minimum"), 1);
    MaxActions->SetNumberField(TEXT("maximum"), MaximumMaxActions);
    MaxActions->SetStringField(TEXT("description"), TEXT("Maximum prioritized actions to return."));
    Properties->SetObjectField(TEXT("maxActions"), MaxActions);

    TSharedRef<FJsonObject> IncludeEvidence = MakeShared<FJsonObject>();
    IncludeEvidence->SetStringField(TEXT("type"), TEXT("boolean"));
    IncludeEvidence->SetBoolField(TEXT("default"), false);
    IncludeEvidence->SetStringField(TEXT("description"), TEXT("Include detailed source analysis objects in addition to compact evidence references."));
    Properties->SetObjectField(TEXT("includeEvidence"), IncludeEvidence);

    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    return Schema;
}
