#include "Tools/Blueprint/Graph/CompareBlueprintGraphPreservationTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"

namespace
{
    namespace GraphEdit = UnrealMCP::BlueprintGraphEditToolUtils;
    constexpr int32 MaxNodes = 6000;
    constexpr int32 MaxFields = 120000;
    constexpr int32 MaxCharacters = 4000000;

    FString PinKey(const UEdGraphPin* Pin)
    {
        return GraphEdit::GetNodeGuid(Pin->GetOwningNode()) + TEXT("/") + GraphEdit::GetPinId(Pin);
    }

    FString LocalKey(const UK2Node_Variable* Node)
    {
        return Node->VariableReference.GetMemberScopeName() + TEXT("/") + Node->GetVarName().ToString();
    }

    FString ResolveSource(const UEdGraphPin* Pin, const TMap<FString, TArray<const UK2Node_VariableSet*>>& Setters,
        TSet<const UEdGraphPin*>& Visited, TSet<FString>& Notes)
    {
        if (!Pin || Visited.Num() >= 64 || Visited.Contains(Pin)) return TEXT("unresolved:cycle_or_depth");
        Visited.Add(Pin);
        const auto* Owner = Pin->GetOwningNode();
        const UEdGraphPin* Bridge = nullptr;
        if (const auto* Knot = Cast<UK2Node_Knot>(Owner))
            Bridge = Knot->GetInputPin();
        else if (const auto* Getter = Cast<UK2Node_VariableGet>(Owner); Getter && Getter->VariableReference.IsLocalScope())
        {
            const FString Local = LocalKey(Getter);
            const auto* Writers = Setters.Find(Local);
            if (!Writers || Writers->Num() != 1)
            {
                Notes.Add(TEXT("Local has no unique writer: ") + Local);
                return TEXT("unresolved:local/") + Local;
            }
            Bridge = (*Writers)[0]->FindPin(Getter->GetVarName(), EGPD_Input);
            Notes.Add(TEXT("Local cache evaluation timing and writer reachability require review: ") + Local);
        }
        else return PinKey(Pin);

        if (!Bridge || Bridge->LinkedTo.Num() > 1) return TEXT("unresolved:bridge");
        if (Bridge->LinkedTo.IsEmpty()) return TEXT("literal:") + GraphEdit::GetEffectivePinDefaultValue(Bridge);
        return ResolveSource(Bridge->LinkedTo[0], Setters, Visited, Notes);
    }

    bool Capture(UBlueprint* Blueprint, FJsonObject& Fields, FString& Error)
    {
        int32 NodeCount = 0;
        int64 Characters = 0;
        auto Add = [&](const FString& Key, const FString& Value)
        {
            Characters += Key.Len() + Value.Len();
            if (Characters <= MaxCharacters && Fields.Values.Num() < MaxFields) Fields.SetStringField(Key, Value);
        };
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph) continue;
            if (!Graph->GraphGuid.IsValid() || (NodeCount += Graph->Nodes.Num()) > MaxNodes)
            { Error = TEXT("Blueprint exceeds the supported snapshot size or has a graph without a stable GUID."); return false; }
            const FString Prefix = Graph->GraphGuid.ToString() + TEXT("/");
            Add(Prefix + TEXT("name"), Graph->GetPathName(Blueprint));
            TMap<FString, TArray<const UK2Node_VariableSet*>> Setters;
            for (const UEdGraphNode* Node : Graph->Nodes)
                if (const auto* Setter = Cast<UK2Node_VariableSet>(Node); Setter && Setter->VariableReference.IsLocalScope())
                    Setters.FindOrAdd(LocalKey(Setter)).Add(Setter);
            TSet<FString> Notes;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                if (!Node->NodeGuid.IsValid()) { Error = TEXT("Graph contains a node without a stable GUID."); return false; }
                const FString NodeKey = Prefix + TEXT("node/") + GraphEdit::GetNodeGuid(Node) + TEXT("/");
                Add(NodeKey + TEXT("class"), Node->GetClass()->GetPathName());
                if (const auto* Call = Cast<UK2Node_CallFunction>(Node))
                {
                    const auto* Owner = Call->FunctionReference.GetMemberParentClass(Blueprint->GeneratedClass);
                    Add(NodeKey + TEXT("function"), GetPathNameSafe(Owner) + TEXT(":") + Call->FunctionReference.GetMemberName().ToString());
                }
                if (const auto* Variable = Cast<UK2Node_Variable>(Node))
                {
                    FString Reference;
                    FMemberReference::StaticStruct()->ExportText(Reference, &Variable->VariableReference, nullptr, nullptr, PPF_None, nullptr);
                    Add(NodeKey + TEXT("member"), Reference);
                }
                if (const auto* Entry = Cast<UK2Node_FunctionEntry>(Node))
                    for (const auto& Local : Entry->LocalVariables)
                    {
                        FString Value;
                        FBPVariableDescription::StaticStruct()->ExportText(Value, &Local, nullptr, nullptr, PPF_None, nullptr);
                        Add(Prefix + TEXT("local/") + Local.VarGuid.ToString(), Value);
                    }
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin) continue;
                    const FString Key = NodeKey + TEXT("pin/") + GraphEdit::GetPinId(Pin) + TEXT("/");
                    FString Type;
                    FEdGraphPinType::StaticStruct()->ExportText(Type, &Pin->PinType, nullptr, nullptr, PPF_None, nullptr);
                    Add(Key + TEXT("type"), Type);
                    Add(Key + TEXT("name"), Pin->PinName.ToString());
                    Add(Key + TEXT("default"), GraphEdit::GetEffectivePinDefaultValue(Pin));
                    TArray<FString> Sources;
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        if (!Linked || !Linked->GetOwningNodeUnchecked()) continue;
                        if (Pin->Direction == EGPD_Output)
                            Add(Key + (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? TEXT("exec/") : TEXT("link/")) + PinKey(Linked), TEXT("connected"));
                        else
                        {
                            TSet<const UEdGraphPin*> Visited;
                            Sources.Add(ResolveSource(Linked, Setters, Visited, Notes));
                        }
                    }
                    Sources.Sort();
                    for (int32 Index = 0; Index < Sources.Num(); ++Index)
                        Add(Key + TEXT("source/") + FString::FromInt(Index), Sources[Index]);
                }
                if (Characters > MaxCharacters || Fields.Values.Num() >= MaxFields)
                { Error = TEXT("Snapshot exceeds the field or character limit. No partial snapshot was returned."); return false; }
            }
            for (const FString& Note : Notes) Add(Prefix + TEXT("review/") + Note, TEXT("review"));
        }
        if (Characters > MaxCharacters || Fields.Values.Num() >= MaxFields)
        { Error = TEXT("Snapshot exceeds the field or character limit."); return false; }
        return true;
    }
}

FCompareBlueprintGraphPreservationTool::FCompareBlueprintGraphPreservationTool()
    : FMCPToolBase(TEXT("CompareBlueprintGraphPreservation"), TEXT("Capture a bounded live Blueprint wiring snapshot or compare it against an earlier snapshot. Reports exact edits, normalized input sources and cache timing to review.")) {}

UnrealMCP::FMCPResponse FCompareBlueprintGraphPreservationTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString Path, Error;
    const TSharedPtr<FJsonObject>* Baseline = nullptr;
    int32 Offset = 0, Limit = 100;
    const auto& Params = Request.Params;
    if (!Params || !Params->TryGetStringField(TEXT("objectPath"), Path) || Path.IsEmpty()
        || (Params->HasField(TEXT("baseline")) && !Params->TryGetObjectField(TEXT("baseline"), Baseline))
        || (Params->HasField(TEXT("offset")) && !Params->TryGetNumberField(TEXT("offset"), Offset))
        || (Params->HasField(TEXT("limit")) && !Params->TryGetNumberField(TEXT("limit"), Limit))
        || Offset < 0 || Limit < 1 || Limit > 200)
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide objectPath, an optional snapshot baseline and a limit of 1..200."));
    const TSharedPtr<FJsonObject>* Old = nullptr;
    if (Baseline)
    {
        FString BaselinePath, Format;
        if (!(*Baseline)->TryGetStringField(TEXT("objectPath"), BaselinePath) || BaselinePath != Path
            || !(*Baseline)->TryGetStringField(TEXT("format"), Format) || Format != TEXT("wiring-v1")
            || !(*Baseline)->TryGetObjectField(TEXT("fields"), Old) || (*Old)->Values.Num() >= MaxFields)
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Use a wiring-v1 snapshot from the same Blueprint."));
        int64 Characters = 0;
        for (const auto& Item : (*Old)->Values)
        {
            if (!Item.Value || Item.Value->Type != EJson::String)
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Baseline fields must be strings."));
            Characters += FString(Item.Key).Len() + Item.Value->AsString().Len();
            if (Characters > MaxCharacters)
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Baseline exceeds the snapshot character limit."));
        }
    }
    auto Current = MakeShared<FJsonObject>();
    if (!BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        return BlueprintEditToolUtils::ResolveBlueprint(Path, Blueprint, OutError) && Capture(Blueprint, *Current, OutError);
    }, Error)) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    auto Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("coverage"), TEXT("Structural wiring evidence. Local source tracing assumes a unique writer and leaves evaluation timing and reachability for review."));
    if (!Old)
    {
        auto Snapshot = MakeShared<FJsonObject>();
        Snapshot->SetStringField(TEXT("format"), TEXT("wiring-v1"));
        Snapshot->SetStringField(TEXT("objectPath"), Path);
        Snapshot->SetObjectField(TEXT("fields"), Current);
        Result->SetObjectField(TEXT("snapshot"), Snapshot);
    }
    else
    {
        TSet<FString> Keys;
        for (const auto& Field : (*Old)->Values) Keys.Add(FString(Field.Key));
        for (const auto& Field : Current->Values) Keys.Add(FString(Field.Key));
        auto Ordered = Keys.Array();
        Ordered.Sort();
        int32 Count = 0;
        TSet<FString> ChangedGraphs;
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const FString& Key : Ordered)
        {
            const auto Before = (*Old)->TryGetField(Key);
            const auto After = Current->TryGetField(Key);
            if (Before && After && Before->AsString().Equals(After->AsString(), ESearchCase::CaseSensitive)) continue;
            FString GraphId, Field;
            Key.Split(TEXT("/"), &GraphId, &Field);
            ChangedGraphs.Add(GraphId);
            if (Count++ < Offset || Rows.Num() >= Limit) continue;
            Rows.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
                MakeShared<FJsonValueString>(GraphId), MakeShared<FJsonValueString>(Field),
                Before ? Before : MakeShared<FJsonValueNull>(), After ? After : MakeShared<FJsonValueNull>()}));
        }
        TArray<TSharedPtr<FJsonValue>> GraphRows;
        auto GraphIds = ChangedGraphs.Array();
        GraphIds.Sort();
        for (const FString& Id : GraphIds)
        {
            const auto Label = Current->TryGetField(Id + TEXT("/name"));
            const auto PreviousLabel = (*Old)->TryGetField(Id + TEXT("/name"));
            GraphRows.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
                MakeShared<FJsonValueString>(Id), Label ? Label : PreviousLabel ? PreviousLabel : MakeShared<FJsonValueNull>()}));
        }
        Result->SetArrayField(TEXT("changedGraphs"), GraphRows);
        Result->SetArrayField(TEXT("columns"), {MakeShared<FJsonValueString>(TEXT("graphGuid")),
            MakeShared<FJsonValueString>(TEXT("field")), MakeShared<FJsonValueString>(TEXT("before")), MakeShared<FJsonValueString>(TEXT("after"))});
        Result->SetArrayField(TEXT("rows"), Rows);
        Result->SetNumberField(TEXT("total"), Count);
        if (static_cast<int64>(Offset) + Rows.Num() < Count) Result->SetNumberField(TEXT("nextOffset"), Offset + Rows.Num());
    }
    FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FCompareBlueprintGraphPreservationTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Exact live Blueprint object path.")));
    auto Baseline = MakeShared<FJsonObject>();
    Baseline->SetStringField(TEXT("type"), TEXT("object"));
    Baseline->SetStringField(TEXT("description"), TEXT("Omit to capture a snapshot. Otherwise pass the returned snapshot object."));
    Properties->SetObjectField(TEXT("baseline"), Baseline);
    for (const TCHAR* Field : {TEXT("offset"), TEXT("limit")})
    {
        auto Number = MakeShared<FJsonObject>();
        Number->SetStringField(TEXT("type"), TEXT("integer"));
        Properties->SetObjectField(Field, Number);
    }
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath"))});
    return Schema;
}
