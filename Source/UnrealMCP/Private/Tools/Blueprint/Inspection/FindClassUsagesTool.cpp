#include "Tools/Blueprint/Inspection/FindClassUsagesTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Components/ActorComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MacroInstance.h"
#include "UObject/LinkerLoad.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "K2Node_Variable.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"

namespace
{
    bool ReadPage(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 Min, int32 Max, int32& Value)
    {
        if (!Params->HasField(Field)) return true;
        double Number;
        if (!Params->TryGetNumberField(Field, Number) || !FMath::IsFinite(Number)
            || Number < Min || Number > Max || FMath::FloorToDouble(Number) != Number) return false;
        Value = static_cast<int32>(Number);
        return true;
    }

    struct FUsageCollector
    {
        FName TargetPackage;
        FString NativeTargetPath; // set for /Script targets, where a package holds many unrelated classes
        int32 Limit = 100;
        int32 Offset = 0;
        int32 ValueVisits = 0;
        bool bValueScanLimited = false;
        const UPackage* SourcePackage = nullptr;
        TSet<const UObject*> ActiveObjects;

        FString FindPathLiteral(const FString& Text) const
        {
            for (int32 Start = 0; Start < Text.Len(); ++Start)
            {
                if (Text[Start] != TEXT('/')) continue;
                int32 End = Start + 1;
                while (End < Text.Len() && !FChar::IsWhitespace(Text[End])
                    && !FCString::Strchr(TEXT("'\",()[]{}=;"), Text[End])) ++End;
                const FString Path = Text.Mid(Start, End - Start);
                if (Path.Contains(TEXT(".")) && MatchesPath(Path)) return Path;
                Start = End - 1;
            }
            return {};
        }

        bool MatchesPath(const FString& Path) const
        {
            return Path.StartsWith(TEXT("/")) && (NativeTargetPath.IsEmpty()
                ? FName(*FPackageName::ObjectPathToPackageName(Path)) == TargetPackage
                : Path.Equals(NativeTargetPath, ESearchCase::IgnoreCase));
        }
        int32 Total = 0;
        TArray<TSharedPtr<FJsonValue>> Hits;

        bool Matches(const UObject* Object) const
        {
            if (Object == nullptr) return false;
            return NativeTargetPath.IsEmpty()
                ? Object->GetOutermost()->GetFName() == TargetPackage
                : Object->GetPathName().Equals(NativeTargetPath, ESearchCase::IgnoreCase);
        }

        void Add(const TCHAR* Kind, const FString& Graph, const FString& NodeGuid, const FString& Pin, const FString& Detail)
        {
            ++Total;
            if (Total <= Offset || Hits.Num() >= Limit)
            {
                return;
            }
            TSharedRef<FJsonObject> Hit = MakeShared<FJsonObject>();
            Hit->SetStringField(TEXT("kind"), Kind);
            Hit->SetStringField(TEXT("graph"), Graph);
            Hit->SetStringField(TEXT("nodeGuid"), NodeGuid);
            Hit->SetStringField(TEXT("pin"), Pin);
            Hit->SetStringField(TEXT("detail"), Detail);
            Hits.Add(MakeShared<FJsonValueObject>(Hit));
        }

        bool PinTypeHits(const FEdGraphPinType& Type) const
        {
            return Matches(Type.PinSubCategoryObject.Get()) || Matches(Type.PinValueType.TerminalSubCategoryObject.Get());
        }
    };

    void ScanValue(const FProperty* Property, const void* Value, const FString& Path, int32 Depth, FUsageCollector& C);

    void ScanFields(const UStruct* Type, const void* Container, const FString& Path, int32 Depth, FUsageCollector& C)
    {
        for (TFieldIterator<FProperty> It(Type); It && !C.bValueScanLimited; ++It)
        {
            const FProperty* Field = *It;
            if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
            for (int32 Index = 0; Index < Field->ArrayDim && !C.bValueScanLimited; ++Index)
            {
                const FString FieldPath = Path + Field->GetName() + (Field->ArrayDim > 1 ? FString::Printf(TEXT("[%d]"), Index) : FString());
                ScanValue(Field, Field->ContainerPtrToValuePtr<void>(Container, Index), FieldPath, Depth, C);
            }
        }
    }

    void ScanOwnedObject(const UObject* Object, const FString& Path, int32 Depth, FUsageCollector& C)
    {
        if (!Object || Object->GetOutermost() != C.SourcePackage || Object->IsAsset()
            || Object->IsA<UField>() || C.ActiveObjects.Contains(Object)) return;
        if (++C.ValueVisits > 50000 || Depth > 32)
        {
            C.bValueScanLimited = true;
            return;
        }
        C.ActiveObjects.Add(Object);
        ScanFields(Object->GetClass(), Object, Path, Depth, C);
        C.ActiveObjects.Remove(Object);
    }

    void ScanValue(const FProperty* Property, const void* Value, const FString& Path, int32 Depth, FUsageCollector& C)
    {
        if (++C.ValueVisits > 50000 || Depth > 32)
        {
            C.bValueScanLimited = true;
            return;
        }
        bool bMatches = false;
        if (const auto* Soft = CastField<FSoftObjectProperty>(Property))
            bMatches = C.MatchesPath(Soft->GetPropertyValue(Value).ToSoftObjectPath().ToString());
        else if (const auto* Object = CastField<FObjectPropertyBase>(Property))
        {
            const UObject* Referenced = Object->GetObjectPropertyValue(Value);
            bMatches = C.Matches(Referenced);
            if (C.Matches(Referenced ? Referenced->GetClass() : nullptr))
                C.Add(TEXT("default_subobject_class"), {}, {}, {}, Path);
            ScanOwnedObject(Referenced, Path + TEXT("."), Depth + 1, C);
        }
        else if (const auto* String = CastField<FStrProperty>(Property))
        {
            const FString Literal = C.FindPathLiteral(String->GetPropertyValue(Value));
            if (!Literal.IsEmpty()) C.Add(TEXT("default_path_literal"), {}, {}, {}, Path + TEXT(" = ") + Literal);
        }
        else if (const auto* Name = CastField<FNameProperty>(Property))
        {
            const FString Literal = C.FindPathLiteral(Name->GetPropertyValue(Value).ToString());
            if (!Literal.IsEmpty()) C.Add(TEXT("default_path_literal"), {}, {}, {}, Path + TEXT(" = ") + Literal);
        }
        else if (const auto* Struct = CastField<FStructProperty>(Property))
        {
            if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get())
                bMatches = C.MatchesPath(static_cast<const FSoftObjectPath*>(Value)->ToString());
            else if (Struct->Struct == TBaseStructure<FSoftClassPath>::Get())
                bMatches = C.MatchesPath(static_cast<const FSoftClassPath*>(Value)->ToString());
            else ScanFields(Struct->Struct, Value, Path + TEXT("."), Depth + 1, C);
        }
        else if (const auto* Array = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper Helper(Array, Value);
            for (int32 I = 0; I < Helper.Num() && !C.bValueScanLimited; ++I)
                ScanValue(Array->Inner, Helper.GetRawPtr(I), Path + FString::Printf(TEXT("[%d]"), I), Depth + 1, C);
        }
        else if (const auto* Set = CastField<FSetProperty>(Property))
        {
            const FScriptSetHelper Helper(Set, Value);
            for (int32 I = 0; I < Helper.GetMaxIndex() && !C.bValueScanLimited; ++I)
                if (Helper.IsValidIndex(I)) ScanValue(Set->ElementProp, Helper.GetElementPtr(I), Path + FString::Printf(TEXT("{set:%d}"), I), Depth + 1, C);
        }
        else if (const auto* Map = CastField<FMapProperty>(Property))
        {
            FScriptMapHelper Helper(Map, Value);
            for (int32 I = 0; I < Helper.GetMaxIndex() && !C.bValueScanLimited; ++I)
            {
                if (!Helper.IsValidIndex(I)) continue;
                ScanValue(Map->KeyProp, Helper.GetKeyPtr(I), Path + FString::Printf(TEXT("{key:%d}"), I), Depth + 1, C);
                if (!C.bValueScanLimited) ScanValue(Map->ValueProp, Helper.GetValuePtr(I), Path + FString::Printf(TEXT("{value:%d}"), I), Depth + 1, C);
            }
        }
        if (bMatches) C.Add(TEXT("class_default"), {}, {}, {}, Path);
    }

    void ScanNode(const UEdGraphNode* Node, const FString& GraphName, FUsageCollector& C)
    {
        const FString NodeGuid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
        const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

        if (const auto* Macro = Cast<UK2Node_MacroInstance>(Node))
        {
            if (C.Matches(Macro->GetSourceBlueprint())) C.Add(TEXT("macro_asset"), GraphName, NodeGuid, {}, Title);
        }
        if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
        {
            if (C.Matches(CastNode->TargetType))
            {
                C.Add(TEXT("cast_target"), GraphName, NodeGuid, FString(), Title);
            }
        }
        if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
        {
            if (const UFunction* Function = Call->GetTargetFunction())
            {
                if (C.Matches(Function->GetOwnerClass()))
                {
                    C.Add(TEXT("function_call"), GraphName, NodeGuid, FString(), Title);
                }
            }
        }
        if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
        {
            if (C.Matches(Var->VariableReference.GetMemberParentClass(nullptr)))
            {
                C.Add(TEXT("variable_access"), GraphName, NodeGuid, FString(), Var->GetVarName().ToString());
            }
        }
        if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            for (const FBPVariableDescription& Local : Entry->LocalVariables)
            {
                if (C.PinTypeHits(Local.VarType))
                {
                    C.Add(TEXT("local_variable"), GraphName, NodeGuid, Local.VarName.ToString(), Title);
                }
            }
        }

        const bool bIsSignature = Node->IsA<UK2Node_FunctionEntry>() || Node->GetClass()->GetName().Contains(TEXT("FunctionResult"));
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            if (C.PinTypeHits(Pin->PinType))
            {
                C.Add(bIsSignature ? TEXT("function_parameter") : TEXT("pin_type"), GraphName, NodeGuid, Pin->PinName.ToString(), Title);
            }
            if (C.Matches(Pin->DefaultObject))
            {
                C.Add(TEXT("pin_default"), GraphName, NodeGuid, Pin->PinName.ToString(), Title);
            }
            const FString Literal = C.FindPathLiteral(Pin->DefaultValue);
            if (!Literal.IsEmpty()) C.Add(TEXT("pin_path_literal"), GraphName, NodeGuid, Pin->PinName.ToString(), Literal);
        }
    }
}

FFindBlueprintNodesTool::FFindBlueprintNodesTool(ESearch InSearch)
    : FMCPToolBase(InSearch == ESearch::PinType ? TEXT("FindNodesByPinType") : TEXT("FindNodesByMember"),
        InSearch == ESearch::PinType
            ? TEXT("Find exact reflected pin types in a live Blueprint, including map keys and values. Returns paged graph, node and pin locations.")
            : TEXT("Find exact function-call and variable member names in a live Blueprint. Returns paged node locations and the declaring class when available."))
    , Search(InSearch)
{
}

UnrealMCP::FMCPResponse FFindBlueprintNodesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const bool bPinType = Search == ESearch::PinType;
    const TCHAR* Filter = bPinType ? TEXT("typePath") : TEXT("memberName");
    FString ObjectPath;
    FString Target;
    int32 Offset = 0;
    int32 Limit = 50;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty()
        || !Request.Params->TryGetStringField(Filter, Target) || Target.IsEmpty()
        || (bPinType && (!Target.StartsWith(TEXT("/")) || !Target.Contains(TEXT("."))))
        || !ReadPage(Request.Params, TEXT("offset"), 0, MAX_int32, Offset)
        || !ReadPage(Request.Params, TEXT("limit"), 1, 200, Limit))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("Provide objectPath and an exact typePath or memberName. offset must be nonnegative and limit must be 1..200."));
    }

    TArray<TSharedPtr<FJsonValue>> Hits;
    int32 Total = 0;
    FString Error;
    const FName MemberName(*Target);
    if (!UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = UnrealMCP::BlueprintToolUtils::LoadBlueprintFromObjectPath(ObjectPath);
        if (!Blueprint)
        {
            OutError = TEXT("Could not load Blueprint from objectPath.");
            return false;
        }
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph) continue;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                auto AddHit = [&](const UEdGraphPin* Pin, const TCHAR* Kind, const UClass* Owner)
                {
                    const int32 Index = Total++;
                    if (Index < Offset || Hits.Num() >= Limit) return;
                    auto Hit = MakeShared<FJsonObject>();
                    Hit->SetStringField(TEXT("graphPath"), Graph->GetPathName(Blueprint));
                    Hit->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
                    Hit->SetStringField(TEXT("kind"), Kind);
                    if (Pin)
                    {
                        Hit->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
                        Hit->SetStringField(TEXT("pinId"), Pin->PinId.ToString());
                    }
                    if (Owner) Hit->SetStringField(TEXT("declaringClass"), Owner->GetPathName());
                    Hits.Add(MakeShared<FJsonValueObject>(Hit));
                };
                if (bPinType)
                {
                    auto MatchesType = [&](const UObject* Type)
                    {
                        return Type && Type->GetPathName().Equals(Target, ESearchCase::IgnoreCase);
                    };
                    for (const UEdGraphPin* Pin : Node->Pins)
                    {
                        if (!Pin) continue;
                        if (MatchesType(Pin->PinType.PinSubCategoryObject.Get())) AddHit(Pin, TEXT("pin_type"), nullptr);
                        if (Pin->PinType.ContainerType == EPinContainerType::Map
                            && MatchesType(Pin->PinType.PinValueType.TerminalSubCategoryObject.Get()))
                        {
                            AddHit(Pin, TEXT("map_value_type"), nullptr);
                        }
                    }
                }
                else if (const auto* Call = Cast<UK2Node_CallFunction>(Node))
                {
                    if (Call->FunctionReference.GetMemberName() == MemberName)
                    {
                        const UFunction* Function = Call->GetTargetFunction();
                        AddHit(nullptr, TEXT("function_call"), Function ? Function->GetOwnerClass() : nullptr);
                    }
                }
                else if (const auto* Variable = Cast<UK2Node_Variable>(Node))
                {
                    if (Variable->GetVarName() == MemberName)
                    {
                        AddHit(Node->FindPin(MemberName), TEXT("variable_access"),
                            Variable->VariableReference.GetMemberParentClass(Blueprint->GeneratedClass));
                    }
                }
            }
        }
        return true;
    }, Error)) return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Error);

    auto Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(Filter, Target);
    Result->SetArrayField(TEXT("hits"), Hits);
    Result->SetNumberField(TEXT("totalHits"), Total);
    if (Total > Offset + Hits.Num()) Result->SetNumberField(TEXT("nextOffset"), Offset + Hits.Num());
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindBlueprintNodesTool::BuildInputSchema() const
{
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    const bool bPinType = Search == ESearch::PinType;
    const TCHAR* Filter = bPinType ? TEXT("typePath") : TEXT("memberName");
    auto Add = [&](const TCHAR* FieldName, const TCHAR* Type, const TCHAR* FieldDescription)
    {
        auto Field = MakeShared<FJsonObject>();
        Field->SetStringField(TEXT("type"), Type);
        Field->SetStringField(TEXT("description"), FieldDescription);
        Properties->SetObjectField(FieldName, Field);
    };
    Add(TEXT("objectPath"), TEXT("string"), TEXT("Blueprint asset object path."));
    Add(Filter, TEXT("string"), bPinType
        ? TEXT("Exact reflected object path such as /Script/Engine.Pawn or /Game/BP_Pawn.BP_Pawn_C.")
        : TEXT("Exact internal function or variable name."));
    Add(TEXT("offset"), TEXT("integer"), TEXT("Zero-based offset; use nextOffset on an unchanged Blueprint."));
    Add(TEXT("limit"), TEXT("integer"), TEXT("Page size, default 50, maximum 200."));
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(Filter)});
    return Schema;
}

FFindClassUsagesTool::FFindClassUsagesTool()
    : FMCPToolBase(TEXT("FindClassUsages"), TEXT("Find live Blueprint references and saved package imports. Omit objectPath for registry candidates, or pass scanBlueprints to scan project Blueprints in bounded pages."))
{
}

UnrealMCP::FMCPResponse FFindClassUsagesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString TargetPath;
    bool bScanBlueprints = false;
    if (!Request.Params.IsValid()
        || (Request.Params->HasField(TEXT("objectPath"))
            && (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty()))
        || !Request.Params->TryGetStringField(TEXT("classOrAssetPath"), TargetPath) || TargetPath.IsEmpty()
        || (Request.Params->HasField(TEXT("scanBlueprints")) && !Request.Params->TryGetBoolField(TEXT("scanBlueprints"), bScanBlueprints))
        || (bScanBlueprints && !ObjectPath.IsEmpty()))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Provide classOrAssetPath and either objectPath or scanBlueprints, or omit both for registry candidates."));
    }

    double LimitValue = 100;
    if (Request.Params->HasField(TEXT("limit")) && !Request.Params->TryGetNumberField(TEXT("limit"), LimitValue))
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("limit must be an integer from 1 to 500."));

    double OffsetValue = 0;
    if ((Request.Params->HasField(TEXT("offset")) && !Request.Params->TryGetNumberField(TEXT("offset"), OffsetValue))
        || !FMath::IsFinite(OffsetValue) || OffsetValue < 0 || OffsetValue > MAX_int32 || FMath::FloorToDouble(OffsetValue) != OffsetValue
        || !FMath::IsFinite(LimitValue) || LimitValue < 1 || LimitValue > 500 || FMath::FloorToDouble(LimitValue) != LimitValue)
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("offset must be a nonnegative integer and limit must be 1..500."));
    FUsageCollector Collector;
    Collector.Offset = static_cast<int32>(OffsetValue);
    Collector.Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
    // Accepts /Game/Pkg.Obj, /Game/Pkg.Obj_C or a bare package name; usage is matched by owning package.
    Collector.TargetPackage = FName(*FPackageName::ObjectPathToPackageName(TargetPath));
    if (TargetPath.StartsWith(TEXT("/Script/")))
    {
        if (!TargetPath.Contains(TEXT(".")))
        {
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
                TEXT("Native targets need a full path such as /Script/Engine.Pawn; a bare /Script package matches every class in it."));
        }
        Collector.NativeTargetPath = TargetPath;
    }

    bool bRegistryDependency = false;
    FString ImportEvidence = TEXT("unavailable");
    int32 ImportMatches = 0;
    int32 GraphCount = 0;
    bool bRegistryLoading = false;
    int32 BlueprintCount = 0;
    int32 BlueprintsScanned = 0;
    TArray<TSharedPtr<FJsonValue>> BlueprintResults;
    FString ExecutionError;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
            bRegistryLoading = Registry.IsLoadingAssets();
            if (bScanBlueprints)
            {
                FARFilter Filter;
                Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
                Filter.bRecursiveClasses = true;
                TArray<FAssetData> Assets;
                Registry.GetAssets(Filter, Assets);
                const auto IsProjectAsset = [](const FString& Path)
                {
                    return Path.StartsWith(TEXT("/Game/"));
                };
                TSet<FString> UniquePaths;
                for (const FAssetData& Asset : Assets)
                    if (IsProjectAsset(Asset.PackageName.ToString())) UniquePaths.Add(Asset.GetObjectPathString());
                for (TObjectIterator<UBlueprint> It; It; ++It)
                    if (It->IsAsset() && IsProjectAsset(It->GetOutermost()->GetName())) UniquePaths.Add(It->GetPathName());
                TArray<FString> Paths = UniquePaths.Array();
                Paths.Sort();
                BlueprintCount = Paths.Num();
                const int32 BatchSize = FMath::Min3(Collector.Limit, 8, FMath::Max(0, BlueprintCount - Collector.Offset));
                for (int32 Index = 0; Index < BatchSize; ++Index)
                {
                    const FString& Path = Paths[Collector.Offset + Index];
                    UnrealMCP::FMCPRequest Scan;
                    Scan.Id = Request.Id;
                    Scan.Params = MakeShared<FJsonObject>();
                    Scan.Params->SetStringField(TEXT("objectPath"), Path);
                    Scan.Params->SetStringField(TEXT("classOrAssetPath"), TargetPath);
                    Scan.Params->SetNumberField(TEXT("limit"), 5);
                    const auto Evidence = Execute(Scan);
                    if (Evidence.Result.IsValid())
                    {
                        ++BlueprintsScanned;
                        if (Evidence.Result->GetIntegerField(TEXT("totalHits")) > 0
                            || Evidence.Result->GetBoolField(TEXT("valueScanLimited"))
                            || Evidence.Result->GetBoolField(TEXT("unexplainedRegistryDependency")))
                            BlueprintResults.Add(MakeShared<FJsonValueObject>(Evidence.Result));
                    }
                    else
                    {
                        auto Failure = MakeShared<FJsonObject>();
                        Failure->SetStringField(TEXT("objectPath"), Path);
                        Failure->SetBoolField(TEXT("success"), false);
                        Failure->SetStringField(TEXT("message"), TEXT("Blueprint could not be scanned. Retry with objectPath."));
                        BlueprintResults.Add(MakeShared<FJsonValueObject>(Failure));
                    }
                }
                return true;
            }
            if (ObjectPath.IsEmpty())
            {
                TArray<FName> Referencers;
                Registry.GetReferencers(Collector.TargetPackage, Referencers);
                Referencers.Remove(Collector.TargetPackage);
                Referencers.Sort(FNameLexicalLess());
                for (const FName Package : Referencers)
                    Collector.Add(TEXT("registry_candidate"), {}, {}, {}, Package.ToString());
                return true;
            }

            UBlueprint* Blueprint = UnrealMCP::BlueprintToolUtils::LoadBlueprintFromObjectPath(ObjectPath);
            if (!Blueprint)
            {
                OutError = TEXT("FindClassUsages could not load a Blueprint from objectPath.");
                return false;
            }
            Collector.SourcePackage = Blueprint->GetOutermost();

            if (Collector.Matches(Blueprint->ParentClass))
            {
                Collector.Add(TEXT("parent_class"), FString(), FString(), FString(), Blueprint->ParentClass->GetName());
            }
            for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
            {
                if (Collector.Matches(Interface.Interface))
                {
                    Collector.Add(TEXT("interface"), FString(), FString(), FString(), Interface.Interface->GetName());
                }
            }
            for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
            {
                if (Collector.PinTypeHits(Variable.VarType))
                {
                    Collector.Add(TEXT("member_variable"), FString(), FString(), Variable.VarName.ToString(), FString());
                }
            }
            if (Blueprint->SimpleConstructionScript)
            {
                for (const USCS_Node* ScsNode : Blueprint->SimpleConstructionScript->GetAllNodes())
                {
                    if (ScsNode && Collector.Matches(ScsNode->ComponentClass))
                    {
                        Collector.Add(TEXT("component"), FString(), FString(), ScsNode->GetVariableName().ToString(), FString());
                    }
                    if (ScsNode)
                        ScanOwnedObject(ScsNode->ComponentTemplate, ScsNode->GetVariableName().ToString() + TEXT("."), 0, Collector);
                }
            }

            if (const UClass* Class = Blueprint->GeneratedClass)
                if (const UObject* CDO = Class->GetDefaultObject(false)) ScanFields(Class, CDO, {}, 0, Collector);

            // GetAllGraphs recurses into sub-graphs, so collapsed graphs are covered.
            TArray<UEdGraph*> Graphs;
            Blueprint->GetAllGraphs(Graphs);
            GraphCount = Graphs.Num();
            for (const UEdGraph* Graph : Graphs)
            {
                for (const UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node)
                    {
                        ScanNode(Node, Graph->GetPathName(Blueprint), Collector);
                    }
                }
            }

            TArray<FName> Dependencies;
            Registry.GetDependencies(Blueprint->GetOutermost()->GetFName(), Dependencies);
            bRegistryDependency = Dependencies.Contains(Collector.TargetPackage);
            UPackage* Package = Blueprint->GetOutermost();
            FLinkerLoad* Linker = Blueprint->GetLinker();
            if (!Linker) Linker = FLinkerLoad::FindExistingLinkerForPackage(Package);
            if (Linker) ImportEvidence = TEXT("loaded_linker");
            FPackagePath PackagePath;
            bool bOpenedSavedLinker = false;
            if (!Linker && FPackagePath::TryFromPackageName(Package->GetFName(), PackagePath)
                && FPackageName::DoesPackageExist(PackagePath))
            {
                Linker = GetPackageLinker(Package, PackagePath, LOAD_NoVerify | LOAD_Quiet, nullptr);
                bOpenedSavedLinker = Linker != nullptr;
                if (bOpenedSavedLinker) ImportEvidence = TEXT("saved_package");
            }
            if (Linker)
            {
                for (int32 Index = 0; Index < Linker->ImportMap.Num(); ++Index)
                {
                    const FString ImportPath = Linker->GetImportPathName(Index);
                    if (Collector.MatchesPath(ImportPath))
                    {
                        ++ImportMatches;
                        Collector.Add(TEXT("package_import"), {}, {}, {}, ImportPath);
                    }
                }
            }
            if (bOpenedSavedLinker) ResetLoaders(Package);
            return true;
        },
        ExecutionError);

    if (!bOk)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    if (bScanBlueprints)
    {
        const int32 Examined = FMath::Min3(Collector.Limit, 8, FMath::Max(0, BlueprintCount - Collector.Offset));
        Result->SetStringField(TEXT("scope"), TEXT("project_blueprints"));
        Result->SetStringField(TEXT("target"), TargetPath);
        Result->SetBoolField(TEXT("registryLoading"), bRegistryLoading);
        Result->SetNumberField(TEXT("totalBlueprints"), BlueprintCount);
        Result->SetNumberField(TEXT("blueprintsScanned"), BlueprintsScanned);
        Result->SetNumberField(TEXT("blueprintsFailed"), Examined - BlueprintsScanned);
        Result->SetArrayField(TEXT("blueprints"), BlueprintResults);
        if (Examined && Collector.Offset + Examined < BlueprintCount)
            Result->SetNumberField(TEXT("nextOffset"), Collector.Offset + Examined);
        Response.Result = Result;
        return Response;
    }
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("target"), Collector.TargetPackage.ToString());
    Result->SetStringField(TEXT("scope"), ObjectPath.IsEmpty() ? TEXT("registry_candidates") : TEXT("blueprint"));
    Result->SetBoolField(TEXT("registryLoading"), bRegistryLoading);
    Result->SetBoolField(TEXT("registryDependency"), bRegistryDependency);
    Result->SetStringField(TEXT("importEvidence"), ImportEvidence);
    Result->SetNumberField(TEXT("importMatches"), ImportMatches);
    Result->SetNumberField(TEXT("liveHits"), ObjectPath.IsEmpty() ? 0 : Collector.Total - ImportMatches);
    Result->SetBoolField(TEXT("unexplainedRegistryDependency"), bRegistryDependency && Collector.Total == ImportMatches);
    Result->SetBoolField(TEXT("valueScanLimited"), Collector.bValueScanLimited);
    Result->SetNumberField(TEXT("graphsScanned"), GraphCount);
    Result->SetNumberField(TEXT("totalHits"), Collector.Total);
    Result->SetBoolField(TEXT("truncated"), Collector.Total > static_cast<int64>(Collector.Offset) + Collector.Hits.Num());
    Result->SetNumberField(TEXT("offset"), Collector.Offset);
    if (Collector.Total > static_cast<int64>(Collector.Offset) + Collector.Hits.Num())
        Result->SetNumberField(TEXT("nextOffset"), Collector.Offset + Collector.Hits.Num());
    Result->SetArrayField(TEXT("usages"), Collector.Hits);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindClassUsagesTool::BuildInputSchema() const
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
    AddProp(TEXT("objectPath"), TEXT("string"), TEXT("Blueprint to scan live. Omit to page package-level Asset Registry candidates, which can miss unsaved or unregistered references."));
    AddProp(TEXT("scanBlueprints"), TEXT("boolean"), TEXT("With no objectPath, scan /Game Blueprint assets, including loaded unsaved assets, up to eight per page. Includes matching Blueprint evidence and scan failures."));
    AddProp(TEXT("classOrAssetPath"), TEXT("string"), TEXT("Class, struct, enum or asset to look for, e.g. /Game/BP_Vehicle.BP_Vehicle or /Game/BP_Vehicle.BP_Vehicle_C"));
    AddProp(TEXT("limit"), TEXT("integer"), TEXT("Hits per page (default 100, max 500), or Blueprints per project scan page (max 8)."));
    AddProp(TEXT("offset"), TEXT("integer"), TEXT("Skip this many hits; use nextOffset for the next page on an unchanged Blueprint."));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("classOrAssetPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
