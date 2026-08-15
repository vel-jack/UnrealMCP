#include "Tools/AnalyzeProjectArchitectureTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Index/UnrealMCPProjectIndex.h"
#include "Tools/AnalyzeBlueprintCouplingTool.h"
#include "Tools/AnalyzeFeatureBoundaryTool.h"
#include "Tools/FindBrokenBlueprintReferencesTool.h"
#include "Tools/FindCircularDependenciesTool.h"
#include "Tools/FindUnusedBlueprintAssetsTool.h"
#include "UnrealMCPModule.h"

namespace
{
    constexpr int32 DefaultMaxResults = 20;
    constexpr int32 MaximumMaxResults = 100;
    constexpr int32 MaximumFeatureBoundaries = 20;
    constexpr int32 MaximumHotspots = 20;

    struct FHotspot
    {
        FString ObjectPath;
        FString PackageName;
        int32 Score = 0;
        TArray<FString> Reasons;
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

    bool IsGamePath(const FString& Path)
    {
        return Path.Equals(TEXT("/Game"), ESearchCase::IgnoreCase)
            || Path.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase);
    }

    double NumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
    {
        double Value = 0.0;
        if (Object.IsValid())
        {
            Object->TryGetNumberField(Name, Value);
        }
        return Value;
    }

    FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
    {
        FString Value;
        if (Object.IsValid())
        {
            Object->TryGetStringField(Name, Value);
        }
        return Value;
    }

    TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
    {
        const TSharedPtr<FJsonObject>* Value = nullptr;
        return Object.IsValid() && Object->TryGetObjectField(Name, Value) && Value != nullptr
            ? *Value
            : nullptr;
    }

    const TArray<TSharedPtr<FJsonValue>>* ArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
    {
        const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
        return Object.IsValid() && Object->TryGetArrayField(Name, Value) ? Value : nullptr;
    }

    TSharedRef<FJsonObject> MakeParams(const FString& RootPath, const int32 MaxResults)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        if (!RootPath.IsEmpty())
        {
            Params->SetStringField(TEXT("rootPath"), RootPath);
        }
        Params->SetNumberField(TEXT("maxResults"), MaxResults);
        return Params;
    }

    template <typename TTool>
    UnrealMCP::FMCPResponse RunTool(
        const UnrealMCP::FMCPRequest& ParentRequest,
        const TSharedPtr<FJsonObject>& Params)
    {
        UnrealMCP::FMCPRequest ChildRequest;
        ChildRequest.Id = ParentRequest.Id;
        ChildRequest.Method = TEXT("tools/call");
        ChildRequest.Params = Params;
        return TTool().Execute(ChildRequest);
    }

    TSharedRef<FJsonObject> MakeSection(
        const UnrealMCP::FMCPResponse& Response,
        const bool bIncludeEvidence)
    {
        TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
        if (Response.Error.IsSet())
        {
            const UnrealMCP::FMCPError& Error = Response.Error.GetValue();
            Section->SetStringField(TEXT("status"), TEXT("error"));
            Section->SetNumberField(TEXT("errorCode"), static_cast<int32>(Error.Code));
            Section->SetStringField(TEXT("message"), Error.Message);
            return Section;
        }
        if (!Response.Result.IsValid())
        {
            Section->SetStringField(TEXT("status"), TEXT("error"));
            Section->SetStringField(TEXT("message"), TEXT("The composed analysis returned no result."));
            return Section;
        }

        Section->SetStringField(TEXT("status"), TEXT("ok"));
        if (bIncludeEvidence)
        {
            Section->SetObjectField(TEXT("evidence"), Response.Result.ToSharedRef());
        }
        return Section;
    }

    void AddHotspot(
        TMap<FString, FHotspot>& Hotspots,
        const FString& ObjectPath,
        const FString& PackageName,
        const int32 Score,
        const FString& Reason)
    {
        const FString Key = !ObjectPath.IsEmpty() ? ObjectPath : PackageName;
        if (Key.IsEmpty())
        {
            return;
        }

        FHotspot& Hotspot = Hotspots.FindOrAdd(Key);
        if (Hotspot.ObjectPath.IsEmpty()) Hotspot.ObjectPath = ObjectPath;
        if (Hotspot.PackageName.IsEmpty()) Hotspot.PackageName = PackageName;
        Hotspot.Score += Score;
        Hotspot.Reasons.AddUnique(Reason);
    }

    void CollectCycleHotspots(const TSharedPtr<FJsonObject>& Result, TMap<FString, FHotspot>& Hotspots)
    {
        const TArray<TSharedPtr<FJsonValue>>* Cycles = ArrayField(Result, TEXT("cycles"));
        if (Cycles == nullptr) return;
        for (const TSharedPtr<FJsonValue>& CycleValue : *Cycles)
        {
            const TSharedPtr<FJsonObject> Cycle = CycleValue.IsValid() ? CycleValue->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Blueprints = ArrayField(Cycle, TEXT("blueprints"));
            if (Blueprints == nullptr) continue;
            for (const TSharedPtr<FJsonValue>& BlueprintValue : *Blueprints)
            {
                const TSharedPtr<FJsonObject> Blueprint = BlueprintValue.IsValid() ? BlueprintValue->AsObject() : nullptr;
                AddHotspot(Hotspots, StringField(Blueprint, TEXT("objectPath")), StringField(Blueprint, TEXT("packageName")), 100, TEXT("circular_dependency"));
            }
        }
    }

    void CollectCouplingHotspots(const TSharedPtr<FJsonObject>& Result, TMap<FString, FHotspot>& Hotspots)
    {
        const TArray<TSharedPtr<FJsonValue>>* Assets = ArrayField(Result, TEXT("assets"));
        if (Assets == nullptr) return;
        for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
        {
            const TSharedPtr<FJsonObject> Asset = AssetValue.IsValid() ? AssetValue->AsObject() : nullptr;
            const FString Risk = StringField(Asset, TEXT("riskLevel"));
            const int32 RiskScore = Risk == TEXT("high") ? 60 : Risk == TEXT("medium") ? 30 : 5;
            const int32 DegreeScore = FMath::Min(FMath::RoundToInt(NumberField(Asset, TEXT("totalDegree"))), 40);
            AddHotspot(Hotspots, StringField(Asset, TEXT("objectPath")), StringField(Asset, TEXT("packageName")), RiskScore + DegreeScore, TEXT("coupling_") + Risk);
        }
    }

    void CollectBrokenHotspots(const TSharedPtr<FJsonObject>& Result, TMap<FString, FHotspot>& Hotspots)
    {
        const TArray<TSharedPtr<FJsonValue>>* Findings = ArrayField(Result, TEXT("findings"));
        if (Findings == nullptr) return;
        for (const TSharedPtr<FJsonValue>& FindingValue : *Findings)
        {
            const TSharedPtr<FJsonObject> Finding = FindingValue.IsValid() ? FindingValue->AsObject() : nullptr;
            AddHotspot(Hotspots, StringField(Finding, TEXT("blueprintObjectPath")), FString(), 50, TEXT("broken_reference"));
        }
    }

    void CollectUnusedHotspots(const TSharedPtr<FJsonObject>& Result, TMap<FString, FHotspot>& Hotspots)
    {
        const TArray<TSharedPtr<FJsonValue>>* Candidates = ArrayField(Result, TEXT("candidates"));
        if (Candidates == nullptr) return;
        for (const TSharedPtr<FJsonValue>& CandidateValue : *Candidates)
        {
            const TSharedPtr<FJsonObject> Candidate = CandidateValue.IsValid() ? CandidateValue->AsObject() : nullptr;
            AddHotspot(Hotspots, StringField(Candidate, TEXT("objectPath")), StringField(Candidate, TEXT("packageName")), 10, TEXT("unused_candidate_review"));
        }
    }

    void CollectBoundaryHotspots(const TSharedPtr<FJsonObject>& Result, TMap<FString, FHotspot>& Hotspots)
    {
        const FString Risk = StringField(Result, TEXT("riskLevel"));
        const int32 RiskScore = Risk == TEXT("high") ? 50 : Risk == TEXT("medium") ? 25 : 5;
        const TArray<TSharedPtr<FJsonValue>>* Blueprints = ArrayField(Result, TEXT("boundaryBlueprints"));
        if (Blueprints == nullptr) return;
        for (const TSharedPtr<FJsonValue>& BlueprintValue : *Blueprints)
        {
            const TSharedPtr<FJsonObject> Blueprint = BlueprintValue.IsValid() ? BlueprintValue->AsObject() : nullptr;
            const int32 CrossingScore = FMath::Min(FMath::RoundToInt(NumberField(Blueprint, TEXT("totalCrossingDependencyCount"))), 30);
            AddHotspot(Hotspots, StringField(Blueprint, TEXT("objectPath")), StringField(Blueprint, TEXT("packageName")), RiskScore + CrossingScore, TEXT("feature_boundary_") + Risk);
        }
    }

    TArray<TSharedPtr<FJsonValue>> SerializeHotspots(TMap<FString, FHotspot>& Hotspots, const int32 MaxResults)
    {
        TArray<FHotspot> Ranked;
        Hotspots.GenerateValueArray(Ranked);
        Ranked.Sort([](const FHotspot& A, const FHotspot& B)
        {
            if (A.Score != B.Score) return A.Score > B.Score;
            const FString AKey = !A.ObjectPath.IsEmpty() ? A.ObjectPath : A.PackageName;
            const FString BKey = !B.ObjectPath.IsEmpty() ? B.ObjectPath : B.PackageName;
            return AKey < BKey;
        });

        TArray<TSharedPtr<FJsonValue>> Values;
        const int32 Count = FMath::Min3(Ranked.Num(), MaxResults, MaximumHotspots);
        Values.Reserve(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FHotspot& Hotspot = Ranked[Index];
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetNumberField(TEXT("rank"), Index + 1);
            if (!Hotspot.ObjectPath.IsEmpty()) Json->SetStringField(TEXT("objectPath"), Hotspot.ObjectPath);
            if (!Hotspot.PackageName.IsEmpty()) Json->SetStringField(TEXT("packageName"), Hotspot.PackageName);
            Json->SetNumberField(TEXT("priorityScore"), Hotspot.Score);
            TArray<TSharedPtr<FJsonValue>> Reasons;
            for (const FString& Reason : Hotspot.Reasons) Reasons.Add(MakeShared<FJsonValueString>(Reason));
            Json->SetArrayField(TEXT("reasons"), Reasons);
            Values.Add(MakeShared<FJsonValueObject>(Json));
        }
        return Values;
    }
}

FAnalyzeProjectArchitectureTool::FAnalyzeProjectArchitectureTool()
    : FMCPToolBase(
        TEXT("AnalyzeProjectArchitecture"),
        TEXT("Returns a compact, fault-tolerant static architecture overview composed from the indexed cycle, coupling, broken-reference, unused-candidate, and optional feature-boundary analyses."))
{
}

UnrealMCP::FMCPResponse FAnalyzeProjectArchitectureTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString RootPath;
    FString PackagePath;
    int32 MaxResults = DefaultMaxResults;
    bool bIncludeEvidence = false;
    TArray<FString> FeatureBoundaries;

    if (Request.Params.IsValid())
    {
        Request.Params->TryGetStringField(TEXT("rootPath"), RootPath);
        Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath);
        if (Request.Params->HasField(TEXT("includeEvidence"))
            && !Request.Params->TryGetBoolField(TEXT("includeEvidence"), bIncludeEvidence))
        {
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeProjectArchitecture includeEvidence must be a boolean."));
        }

        if (Request.Params->HasField(TEXT("maxResultsPerSection")))
        {
            double Value = DefaultMaxResults;
            if (!Request.Params->TryGetNumberField(TEXT("maxResultsPerSection"), Value)
                || !FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value))
                || Value < 1.0 || Value > MaximumMaxResults)
            {
                return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeProjectArchitecture maxResultsPerSection must be an integer from 1 to 100."));
            }
            MaxResults = FMath::RoundToInt(Value);
        }

        if (Request.Params->HasField(TEXT("featureBoundaries")))
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!Request.Params->TryGetArrayField(TEXT("featureBoundaries"), Values) || Values == nullptr || Values->Num() > MaximumFeatureBoundaries)
            {
                return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeProjectArchitecture featureBoundaries must be an array containing at most 20 /Game paths."));
            }
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                FString Boundary;
                if (!Value.IsValid() || !Value->TryGetString(Boundary))
                {
                    return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeProjectArchitecture featureBoundaries must contain only strings."));
                }
                Boundary = NormalizePackagePath(Boundary);
                if (!IsGamePath(Boundary))
                {
                    return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Every AnalyzeProjectArchitecture feature boundary must be /Game or a path under /Game."));
                }
                FeatureBoundaries.AddUnique(Boundary);
            }
        }
    }

    FeatureBoundaries.Sort();

    RootPath = NormalizePackagePath(RootPath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!RootPath.IsEmpty() && !PackagePath.IsEmpty() && !RootPath.Equals(PackagePath, ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeProjectArchitecture rootPath and packagePath must match when both are supplied."));
    }
    const FString ScopePath = !PackagePath.IsEmpty() ? PackagePath : RootPath;
    if (!ScopePath.IsEmpty() && !IsGamePath(ScopePath))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeProjectArchitecture rootPath/packagePath must be /Game or a path under /Game."));
    }

    const FUnrealMCPProjectIndex::FStatusSnapshot Snapshot = FUnrealMCPModule::Get().GetProjectIndex().GetStatusSnapshot();
    if (!Snapshot.bHasUsableIndex)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, TEXT("AnalyzeProjectArchitecture requires a usable project index. Initialize or manually rebuild the index, then retry."));
    }

    TSharedRef<FJsonObject> CommonParams = MakeParams(ScopePath, MaxResults);
    TSharedRef<FJsonObject> CycleParams = MakeParams(ScopePath, MaxResults);
    CycleParams->SetNumberField(TEXT("minCycleSize"), 1);

    const UnrealMCP::FMCPResponse CyclesResponse = RunTool<FFindCircularDependenciesTool>(Request, CycleParams);
    const UnrealMCP::FMCPResponse CouplingResponse = RunTool<FAnalyzeBlueprintCouplingTool>(Request, CommonParams);
    const UnrealMCP::FMCPResponse BrokenResponse = RunTool<FFindBrokenBlueprintReferencesTool>(Request, CommonParams);
    const UnrealMCP::FMCPResponse UnusedResponse = RunTool<FFindUnusedBlueprintAssetsTool>(Request, CommonParams);

    TSharedRef<FJsonObject> CyclesSection = MakeSection(CyclesResponse, bIncludeEvidence);
    TSharedRef<FJsonObject> CouplingSection = MakeSection(CouplingResponse, bIncludeEvidence);
    TSharedRef<FJsonObject> BrokenSection = MakeSection(BrokenResponse, bIncludeEvidence);
    TSharedRef<FJsonObject> UnusedSection = MakeSection(UnusedResponse, bIncludeEvidence);
    TMap<FString, FHotspot> Hotspots;

    int32 CycleCount = 0;
    int32 HighCouplingCount = 0;
    int32 MediumCouplingCount = 0;
    int32 BrokenCount = 0;
    int32 UnusedCount = 0;
    if (CyclesResponse.Result.IsValid())
    {
        CycleCount = FMath::RoundToInt(NumberField(CyclesResponse.Result, TEXT("cycleCount")));
        CyclesSection->SetNumberField(TEXT("cycleCount"), CycleCount);
        CyclesSection->SetNumberField(TEXT("cyclicBlueprintCount"), NumberField(CyclesResponse.Result, TEXT("cyclicBlueprintCount")));
        CollectCycleHotspots(CyclesResponse.Result, Hotspots);
    }
    if (CouplingResponse.Result.IsValid())
    {
        const TSharedPtr<FJsonObject> Summary = ObjectField(CouplingResponse.Result, TEXT("summary"));
        HighCouplingCount = FMath::RoundToInt(NumberField(Summary, TEXT("highRiskAssetCount")));
        MediumCouplingCount = FMath::RoundToInt(NumberField(Summary, TEXT("mediumRiskAssetCount")));
        CouplingSection->SetNumberField(TEXT("analyzedBlueprintCount"), NumberField(Summary, TEXT("matchingAssetCount")));
        CouplingSection->SetNumberField(TEXT("highRiskCount"), HighCouplingCount);
        CouplingSection->SetNumberField(TEXT("mediumRiskCount"), MediumCouplingCount);
        CouplingSection->SetNumberField(TEXT("maximumTotalDegree"), NumberField(Summary, TEXT("maximumTotalDegree")));
        CollectCouplingHotspots(CouplingResponse.Result, Hotspots);
    }
    if (BrokenResponse.Result.IsValid())
    {
        BrokenCount = FMath::RoundToInt(NumberField(BrokenResponse.Result, TEXT("total")));
        BrokenSection->SetNumberField(TEXT("findingCount"), BrokenCount);
        BrokenSection->SetObjectField(TEXT("countsByCategory"), ObjectField(BrokenResponse.Result, TEXT("countsByCategory")));
        CollectBrokenHotspots(BrokenResponse.Result, Hotspots);
    }
    if (UnusedResponse.Result.IsValid())
    {
        const TSharedPtr<FJsonObject> Summary = ObjectField(UnusedResponse.Result, TEXT("summary"));
        UnusedCount = FMath::RoundToInt(NumberField(Summary, TEXT("eligibleCandidateCount")));
        UnusedSection->SetNumberField(TEXT("candidateCount"), UnusedCount);
        UnusedSection->SetNumberField(TEXT("excludedPossibleRootCount"), NumberField(Summary, TEXT("excludedPossibleRootCount")));
        CollectUnusedHotspots(UnusedResponse.Result, Hotspots);
    }

    TArray<TSharedPtr<FJsonValue>> BoundarySections;
    int32 HighBoundaryCount = 0;
    int32 MediumBoundaryCount = 0;
    for (const FString& Boundary : FeatureBoundaries)
    {
        TSharedRef<FJsonObject> Params = MakeParams(Boundary, MaxResults);
        const UnrealMCP::FMCPResponse BoundaryResponse = RunTool<FAnalyzeFeatureBoundaryTool>(Request, Params);
        TSharedRef<FJsonObject> Section = MakeSection(BoundaryResponse, bIncludeEvidence);
        Section->SetStringField(TEXT("rootPath"), Boundary);
        if (BoundaryResponse.Result.IsValid())
        {
            const FString Risk = StringField(BoundaryResponse.Result, TEXT("riskLevel"));
            Section->SetStringField(TEXT("riskLevel"), Risk);
            Section->SetObjectField(TEXT("summary"), ObjectField(BoundaryResponse.Result, TEXT("summary")));
            HighBoundaryCount += Risk == TEXT("high") ? 1 : 0;
            MediumBoundaryCount += Risk == TEXT("medium") ? 1 : 0;
            CollectBoundaryHotspots(BoundaryResponse.Result, Hotspots);
        }
        BoundarySections.Add(MakeShared<FJsonValueObject>(Section));
    }

    const bool bHighRisk = CycleCount > 0 || BrokenCount > 0 || HighCouplingCount > 0 || HighBoundaryCount > 0;
    const bool bMediumRisk = MediumCouplingCount > 0 || MediumBoundaryCount > 0 || UnusedCount > 0;
    const FString OverallRisk = bHighRisk ? TEXT("high") : bMediumRisk ? TEXT("medium") : TEXT("low");

    TSharedRef<FJsonObject> RiskOverview = MakeShared<FJsonObject>();
    RiskOverview->SetStringField(TEXT("overallRiskLevel"), OverallRisk);
    RiskOverview->SetNumberField(TEXT("circularDependencyGroupCount"), CycleCount);
    RiskOverview->SetNumberField(TEXT("brokenReferenceFindingCount"), BrokenCount);
    RiskOverview->SetNumberField(TEXT("highCouplingBlueprintCount"), HighCouplingCount);
    RiskOverview->SetNumberField(TEXT("mediumCouplingBlueprintCount"), MediumCouplingCount);
    RiskOverview->SetNumberField(TEXT("unusedCandidateCount"), UnusedCount);
    RiskOverview->SetNumberField(TEXT("highRiskFeatureBoundaryCount"), HighBoundaryCount);
    RiskOverview->SetNumberField(TEXT("mediumRiskFeatureBoundaryCount"), MediumBoundaryCount);
    RiskOverview->SetStringField(TEXT("priorityRule"), TEXT("Cycles and broken references first, then high coupling and high-risk feature boundaries; unused results are review candidates only."));

    TSharedRef<FJsonObject> Sections = MakeShared<FJsonObject>();
    Sections->SetObjectField(TEXT("cycles"), CyclesSection);
    Sections->SetObjectField(TEXT("coupling"), CouplingSection);
    Sections->SetObjectField(TEXT("brokenReferences"), BrokenSection);
    Sections->SetObjectField(TEXT("unusedCandidates"), UnusedSection);
    Sections->SetArrayField(TEXT("featureBoundaries"), BoundarySections);

    TSharedRef<FJsonObject> Index = MakeShared<FJsonObject>();
    Index->SetBoolField(TEXT("hasUsableIndex"), Snapshot.bHasUsableIndex);
    Index->SetBoolField(TEXT("isDirty"), Snapshot.bIndexDirty);
    Index->SetStringField(TEXT("indexedScope"), Snapshot.IndexedScope);
    Index->SetNumberField(TEXT("schemaVersion"), Snapshot.SchemaVersion);
    Index->SetNumberField(TEXT("indexedProjectBlueprintCount"), static_cast<double>(Snapshot.IndexedProjectBlueprintCount));
    Index->SetStringField(TEXT("lastFullBuildUtc"), Snapshot.LastFullBuildUtc);
    Index->SetStringField(TEXT("lastUpdateUtc"), Snapshot.LastUpdateUtc);

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("analysisType"), TEXT("composed_indexed_static_project_architecture"));
    Result->SetStringField(TEXT("rootPath"), ScopePath);
    Result->SetNumberField(TEXT("maxResultsPerSection"), MaxResults);
    Result->SetBoolField(TEXT("includeEvidence"), bIncludeEvidence);
    Result->SetObjectField(TEXT("index"), Index);
    Result->SetObjectField(TEXT("sections"), Sections);
    Result->SetObjectField(TEXT("riskOverview"), RiskOverview);
    Result->SetArrayField(TEXT("prioritizedHotspots"), SerializeHotspots(Hotspots, MaxResults));
    Result->SetStringField(TEXT("limitation"), TEXT("This is static indexed package/reference evidence. It does not prove runtime execution, dynamic loading, reflection use, gameplay correctness, or architectural intent."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAnalyzeProjectArchitectureTool::BuildInputSchema() const
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
    Boundaries->SetStringField(TEXT("description"), TEXT("Optional /Game feature boundaries to summarize independently, up to 20."));
    Boundaries->SetNumberField(TEXT("maxItems"), MaximumFeatureBoundaries);
    Boundaries->SetBoolField(TEXT("uniqueItems"), true);
    TSharedRef<FJsonObject> BoundaryItem = MakeShared<FJsonObject>();
    BoundaryItem->SetStringField(TEXT("type"), TEXT("string"));
    Boundaries->SetObjectField(TEXT("items"), BoundaryItem);
    Properties->SetObjectField(TEXT("featureBoundaries"), Boundaries);

    TSharedRef<FJsonObject> MaxResults = MakeShared<FJsonObject>();
    MaxResults->SetStringField(TEXT("type"), TEXT("integer"));
    MaxResults->SetNumberField(TEXT("minimum"), 1);
    MaxResults->SetNumberField(TEXT("maximum"), MaximumMaxResults);
    MaxResults->SetNumberField(TEXT("default"), DefaultMaxResults);
    MaxResults->SetStringField(TEXT("description"), TEXT("Maximum evidence items requested from each section and maximum prioritized hotspots returned."));
    Properties->SetObjectField(TEXT("maxResultsPerSection"), MaxResults);

    TSharedRef<FJsonObject> IncludeEvidence = MakeShared<FJsonObject>();
    IncludeEvidence->SetStringField(TEXT("type"), TEXT("boolean"));
    IncludeEvidence->SetBoolField(TEXT("default"), false);
    IncludeEvidence->SetStringField(TEXT("description"), TEXT("Include each bounded composed tool result. False returns compact section counts and top hotspots only."));
    Properties->SetObjectField(TEXT("includeEvidence"), IncludeEvidence);

    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    return Schema;
}
