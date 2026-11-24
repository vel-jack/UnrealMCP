#include "Tools/Blueprint/Members/BlueprintMemberGraphTools.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchString.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintMemberToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"

namespace
{
    namespace GraphEdit = UnrealMCP::BlueprintGraphEditToolUtils;
    namespace Member = UnrealMCP::BlueprintMemberToolUtils;
    struct FEndpoint { UEdGraphNode* Node; FName Pin; EEdGraphPinDirection Direction; };
    struct FPinState
    {
        FName Name; EEdGraphPinDirection Direction; FEdGraphPinType Type;
        FString Value; UObject* Object; FText Text;
        TArray<FEndpoint> Links;
    };
    UEdGraphPin* Pin(UEdGraphNode* Node, FName Name, EEdGraphPinDirection Direction)
    {
        for (UEdGraphPin* P : Node->Pins) if (P && !P->bOrphanedPin && P->PinName == Name && P->Direction == Direction) return P;
        return nullptr;
    }
    bool ReadMap(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TMap<FName, FName>& Map, FString& Error)
    {
        if (!Params->HasField(Key)) return true;
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (!Params->TryGetObjectField(Key, Object) || (*Object)->Values.Num() > 128)
        { Error = TEXT("Pin/case mappings must be objects of at most 128 string pairs."); return false; }
        TSet<FName> Destinations;
        for (const auto& Pair : (*Object)->Values)
        {
            FString Target;
            if (!Pair.Value || !Pair.Value->TryGetString(Target) || Pair.Key.IsEmpty() || Target.IsEmpty()
                || Map.Contains(FName(*Pair.Key)) || Destinations.Contains(FName(*Target)))
            { Error = TEXT("Pin/case mappings require non-empty names and unique destinations."); return false; }
            Map.Add(FName(*Pair.Key), FName(*Target)); Destinations.Add(FName(*Target));
        }
        return true;
    }
    // Snapshot before reconstruction, then reconnect only direct compatible links. Never insert conversions or replace other links.
    bool Rewrite(UEdGraphNode* Node, const TMap<FName, FName>& Map,
        TFunctionRef<bool(UEdGraphNode*, FString&)> Configure, bool bLive,
        TArray<TSharedPtr<FJsonValue>>& Dropped, int32& Kept, FString& Error)
    {
        TArray<FPinState> States; int32 TotalLinks = 0;
        if (Node->Pins.Num() > 512) { Error = TEXT("Node exceeds the 512-pin edit limit."); return false; }
        for (UEdGraphPin* P : Node->Pins) if (P)
        {
            FPinState S; S.Name = P->PinName; S.Direction = P->Direction; S.Type = P->PinType;
            S.Value = P->DefaultValue; S.Object = P->DefaultObject; S.Text = P->DefaultTextValue;
            for (UEdGraphPin* Other : P->LinkedTo) if (Other)
                S.Links.Add({Other->GetOwningNode(), Other->PinName, Other->Direction});
            TotalLinks += S.Links.Num(); States.Add(MoveTemp(S));
        }
        if (TotalLinks > 2048) { Error = TEXT("Node exceeds the 2048-link edit limit."); return false; }
        for (const auto& Pair : Map) if (!States.ContainsByPredicate([&](const FPinState& S) { return S.Name == Pair.Key; }))
        { Error = TEXT("Mapped source pin does not exist: ") + Pair.Key.ToString(); return false; }
        TMap<FName, uint8> DestinationDirections;
        for (const auto& S : States)
        {
            const FName Destination = Map.Contains(S.Name) ? Map.FindChecked(S.Name) : S.Name;
            const uint8 DirectionBit = 1 << static_cast<uint8>(S.Direction);
            uint8& Directions = DestinationDirections.FindOrAdd(Destination);
            if ((Directions & DirectionBit) != 0)
            { Error = TEXT("Multiple old pins would map to the same destination: ") + Destination.ToString(); return false; }
            Directions |= DirectionBit;
        }
        if (bLive)
        {
            Node->SetFlags(RF_Transactional); Node->Modify();
            for (const auto& S : States) for (const auto& E : S.Links) if (E.Node)
            { E.Node->SetFlags(RF_Transactional); E.Node->Modify(); }
        }
        for (UEdGraphPin* P : Node->Pins) if (P) P->BreakAllPinLinks(false);
        for (UEdGraphPin* P : Node->Pins) if (P) if (const FName* Renamed = Map.Find(P->PinName)) P->PinName = *Renamed;
        if (!Configure(Node, Error)) return false;
        Node->ReconstructNode();
        Member::EnsureLocalVariablePins(Cast<UK2Node_Variable>(Node));
        TArray<UEdGraphPin*> Orphans;
        for (UEdGraphPin* P : Node->Pins) if (P && P->bOrphanedPin) Orphans.Add(P);
        for (UEdGraphPin* P : Orphans) Node->RemovePin(P);
        for (const auto& S : States) if (const FName* Mapped = Map.Find(S.Name))
            if (!Pin(Node, *Mapped, S.Direction)) { Error = TEXT("Mapped destination pin does not exist: ") + Mapped->ToString(); return false; }
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        TSet<FString> Seen;
        for (const auto& S : States)
        {
            const FName NewName = Map.Contains(S.Name) ? Map.FindChecked(S.Name) : S.Name;
            UEdGraphPin* NewPin = Pin(Node, NewName, S.Direction);
            if (NewPin && NewPin->Direction == EGPD_Input && NewPin->PinType == S.Type
                && Schema->IsPinDefaultValid(NewPin, S.Value, S.Object, S.Text).IsEmpty())
            { NewPin->DefaultValue = S.Value; NewPin->DefaultObject = S.Object; NewPin->DefaultTextValue = S.Text; }
            for (const auto& E : S.Links)
            {
                const FName OtherName = E.Node == Node && Map.Contains(E.Pin) ? Map.FindChecked(E.Pin) : E.Pin;
                UEdGraphPin* Other = E.Node ? Pin(E.Node, OtherName, E.Direction) : nullptr;
                FString A = GraphEdit::GetNodeGuid(Node) + TEXT(":") + S.Name.ToString();
                FString B = GraphEdit::GetNodeGuid(E.Node) + TEXT(":") + E.Pin.ToString();
                const FString Identity = A < B ? A + TEXT("|") + B : B + TEXT("|") + A;
                if (Seen.Contains(Identity)) continue; Seen.Add(Identity);
                FString Reason = TEXT("Pin removed or incompatible");
                bool bConnected = NewPin && Other && NewPin->LinkedTo.Contains(Other);
                if (!bConnected && NewPin && Other && !(NewPin->bHidden && NewPin->PinName == UEdGraphSchema_K2::PN_Self))
                {
                    const auto Response = Schema->CanCreateConnection(NewPin, Other);
                    Reason = Response.Message.ToString();
                    if (Response.Response == CONNECT_RESPONSE_MAKE) bConnected = Schema->TryCreateConnection(NewPin, Other);
                }
                if (bConnected) { ++Kept; continue; }
                auto Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("pinName"), S.Name.ToString());
                Entry->SetStringField(TEXT("otherNodeGuid"), GraphEdit::GetNodeGuid(E.Node)); Entry->SetStringField(TEXT("otherPinName"), E.Pin.ToString());
                Entry->SetStringField(TEXT("reason"), Reason); Dropped.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
        return true;
    }
    TSharedRef<FJsonObject> CommonSchema()
    {
        using namespace UnrealMCP::BlueprintEditToolUtils;
        auto P = MakeShared<FJsonObject>();
        for (const TCHAR* Key : {TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("nodeGuid"), TEXT("expectedGraphRevision")})
            P->SetObjectField(Key, BuildStringProperty(TEXT("Exact Blueprint/graph/node identity or optional revision guard.")));
        for (const TCHAR* Key : {TEXT("dryRun"), TEXT("confirm"), TEXT("compileAfterEdit"), TEXT("saveAfterEdit")})
            P->SetObjectField(Key, BuildBoolProperty(TEXT("Apply requires confirm. Compile defaults false; saving forces a successful compile.")));
        return P;
    }
}

FBlueprintMemberGraphTool::FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit InEdit)
    : FMCPToolBase(InEdit == EBlueprintMemberGraphEdit::RetargetMember ? TEXT("RetargetBlueprintMemberNode") : TEXT("SetSwitchCases"),
        InEdit == EBlueprintMemberGraphEdit::RetargetMember
            ? TEXT("Retargets a variable Get/Set, ordinary function call, or inherited event override, even if its old member no longer resolves. Inherited members use self context. Preserves GUID, position, comment, compatible defaults and direct links; pinMap handles renamed parameters. Dry-run simulates reconstruction. Dropped links are explicit. Requires confirm to apply; compile is opt-in, save forces compile with rollback.")
            : TEXT("Edits Switch on String and Switch on Name cases; renames preserve compatible exec links, removed cases report dropped links, and Default is retained. Enum cases are defined by their enum and cannot be arbitrarily renamed here. Dry-run simulates reconstruction. Requires confirm to apply; save forces successful compile.")), Edit(InEdit) {}

UnrealMCP::FMCPResponse FBlueprintMemberGraphTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    namespace Utils = BlueprintEditToolUtils;
    FString Path, GraphName, GraphGuid, NodeGuid, MemberName, OwnerPath, Expected;
    if (!Request.Params || !Request.Params->TryGetStringField(TEXT("objectPath"), Path) || !Request.Params->TryGetStringField(TEXT("nodeGuid"), NodeGuid))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Requires objectPath, nodeGuid and a graph selector."));
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName); Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("newMemberName"), MemberName); Request.Params->TryGetStringField(TEXT("newOwnerClassPath"), OwnerPath);
    Request.Params->TryGetStringField(TEXT("expectedGraphRevision"), Expected);
    const bool bDry = Utils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = Utils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    const bool bCompile = bSave || Utils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), false);
    if (!bDry && !Utils::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Apply requires confirm=true."));
    TMap<FName, FName> Mapping; FString Error;
    if (!ReadMap(Request.Params, Edit == EBlueprintMemberGraphEdit::SwitchCases ? TEXT("renames") : TEXT("pinMap"), Mapping, Error))
        return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    auto Result = BuildBooleanResult(true);
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* BP = nullptr; UEdGraph* Graph = nullptr; UEdGraphNode* Node = nullptr;
        if (!Utils::ResolveBlueprint(Path, BP, OutError) || !GraphEdit::ResolveGraph(BP, GraphName, GraphGuid, Graph, OutError)
            || !GraphEdit::ResolveNode(Graph, NodeGuid, Node, OutError)) return false;
        const FString Before = GraphEdit::ComputeGraphRevision(Graph);
        if (!Expected.IsEmpty() && Before != Expected) { OutError = TEXT("Graph revision changed; inspect the live graph before retrying."); return false; }
        Member::FResolvedVariable Variable;
        UFunction* Function = nullptr; TArray<FName> Cases;
        if (Edit == EBlueprintMemberGraphEdit::SwitchCases)
        {
            const auto* StringSwitch = Cast<UK2Node_SwitchString>(Node);
            const auto* NameSwitch = Cast<UK2Node_SwitchName>(Node);
            if (!StringSwitch && !NameSwitch) { OutError = TEXT("SetSwitchCases edits Switch on String and Switch on Name. Enum cases must follow the enum definition."); return false; }
            const TArray<FName>& ExistingCases = StringSwitch ? StringSwitch->PinNames : NameSwitch->PinNames;
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr; TArray<FString> TextCases;
            if (!Request.Params->TryGetArrayField(TEXT("cases"), Values)) { OutError = TEXT("cases[] is required."); return false; }
            for (const auto& V : *Values) { FString Text; if (!V || !V->TryGetString(Text)) { OutError = TEXT("cases must be strings."); return false; } TextCases.Add(Text); }
            if (!Member::ValidateStringCases(TextCases, OutError)) return false;
            for (const auto& Text : TextCases) Cases.Add(FName(*Text));
            for (const auto& Pair : Mapping)
                if (!ExistingCases.Contains(Pair.Key) || !Cases.Contains(Pair.Value))
                { OutError = TEXT("Case renames must map an existing case to a requested case."); return false; }
        }
        else if (auto* V = Cast<UK2Node_Variable>(Node))
        {
            if ((!Node->IsA<UK2Node_VariableGet>() && !Node->IsA<UK2Node_VariableSet>()) || MemberName.IsEmpty()
                || !Member::ResolveVariable(BP, Graph, MemberName, OwnerPath, Node->IsA<UK2Node_VariableSet>(), Variable, OutError)) return false;
            if (V->VariableReference.GetMemberName() != Variable.Name && !Mapping.Contains(V->VariableReference.GetMemberName()))
                Mapping.Add(V->VariableReference.GetMemberName(), Variable.Name);
            Result->SetStringField(TEXT("variableScope"), Variable.IsLocalContext() ? TEXT("local") : TEXT("member"));
            Result->SetStringField(TEXT("scopeName"), Variable.LocalScope);
            Result->SetBoolField(TEXT("selfContext"), Variable.IsSelfContext()); Result->SetStringField(TEXT("declaredIn"), Variable.DeclarationPath);
        }
        else if (Node->GetClass() == UK2Node_Event::StaticClass() && CastChecked<UK2Node_Event>(Node)->bOverrideFunction)
        {
            UClass* Owner = BP->ParentClass;
            if (!OwnerPath.IsEmpty() && !Utils::ResolveClass(OwnerPath, Owner, OutError)) return false;
            Function = Owner ? Owner->FindFunctionByName(*MemberName) : nullptr;
            if (!BP->ParentClass || !Owner || !BP->ParentClass->IsChildOf(Owner) || !Function
                || !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent) || Function->HasAnyFunctionFlags(FUNC_Final)
                || Function->HasAnyFunctionFlags(FUNC_HasOutParms) || !BP->UbergraphPages.Contains(Graph))
            {
                OutError = TEXT("Select an overridable inherited Blueprint event with no output parameters in the Event Graph.");
                return false;
            }
            TArray<UEdGraph*> Graphs;
            BP->GetAllGraphs(Graphs);
            for (const UEdGraph* OtherGraph : Graphs)
            {
                if (OtherGraph->GetFName() == Function->GetFName())
                {
                    OutError = TEXT("The Blueprint already defines a graph for the target event.");
                    return false;
                }
                for (const UEdGraphNode* OtherNode : OtherGraph->Nodes)
                {
                    const auto* OtherEvent = Cast<UK2Node_Event>(OtherNode);
                    if (OtherEvent && OtherNode != Node && OtherEvent->bOverrideFunction
                        && OtherEvent->EventReference.GetMemberName() == Function->GetFName())
                    {
                        OutError = TEXT("The Blueprint already overrides the target event.");
                        return false;
                    }
                }
            }
            Result->SetBoolField(TEXT("selfContext"), true);
            Result->SetStringField(TEXT("declaredIn"), Function->GetOwnerClass()->GetAuthoritativeClass()->GetPathName());
        }
        else if (Node->IsA<UK2Node_CallFunction>() && !Node->IsA<UK2Node_CallParentFunction>())
        {
            UClass* Owner = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass : BP->GeneratedClass;
            if (!OwnerPath.IsEmpty() && !Utils::ResolveClass(OwnerPath, Owner, OutError)) return false;
            Function = Owner ? Owner->FindFunctionByName(*MemberName) : nullptr;
            if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
            { OutError = TEXT("newMemberName must identify a Blueprint-callable function."); return false; }
            Result->SetBoolField(TEXT("selfContext"), Member::IsSelfOrAncestor(BP, Owner)); Result->SetStringField(TEXT("declaredIn"), Function->GetOwnerClass()->GetAuthoritativeClass()->GetPathName());
        }
        else { OutError = TEXT("Supported member nodes are variable Get/Set, ordinary function calls, and inherited event overrides."); return false; }
        auto Configure = [&](UEdGraphNode* Target, FString&) -> bool
        {
            if (auto* Switch = Cast<UK2Node_SwitchString>(Target)) { Switch->PinNames = Cases; Switch->bHasDefaultPin = true; }
            else if (auto* NameSwitch = Cast<UK2Node_SwitchName>(Target)) { NameSwitch->PinNames = Cases; NameSwitch->bHasDefaultPin = true; }
            else if (auto* V = Cast<UK2Node_Variable>(Target))
            {
                if (Variable.IsLocalContext()) V->VariableReference.SetLocalMember(Variable.Name, Variable.LocalScope, Variable.Guid);
                else if (Variable.ExternalOwner) V->VariableReference.SetExternalMember(Variable.Name, Variable.ExternalOwner, Variable.Guid);
                else V->VariableReference.SetSelfMember(Variable.Name, Variable.Guid);
            }
            else if (auto* Event = Cast<UK2Node_Event>(Target))
                Event->EventReference.SetExternalMember(Function->GetFName(), Function->GetOwnerClass()->GetAuthoritativeClass());
            else CastChecked<UK2Node_CallFunction>(Target)->SetFromFunction(Function);
            return true;
        };
        // K2 connection previews mark their Blueprint modified; keep that state off the live asset.
        UBlueprint* SimulationOwner = NewObject<UBlueprint>(GetTransientPackage(), BP->GetClass(), NAME_None, RF_Transient);
        SimulationOwner->ParentClass = BP->ParentClass;
        SimulationOwner->GeneratedClass = BP->GeneratedClass;
        SimulationOwner->SkeletonGeneratedClass = BP->SkeletonGeneratedClass;
        SimulationOwner->BlueprintType = BP->BlueprintType;
        SimulationOwner->Status = BS_BeingCreated;
        UEdGraph* Simulation = DuplicateObject<UEdGraph>(Graph, SimulationOwner, Graph->GetFName());
        if (!Simulation) { OutError = TEXT("Could not duplicate the graph for isolated preflight."); return false; }
        Simulation->SetFlags(RF_Transient);
        if (BP->FunctionGraphs.Contains(Graph)) SimulationOwner->FunctionGraphs.Add(Simulation);
        else SimulationOwner->UbergraphPages.Add(Simulation);
        UEdGraphNode* Preview = nullptr;
        for (UEdGraphNode* N : Simulation->Nodes) if (N && N->NodeGuid == Node->NodeGuid) Preview = N;
        TArray<TSharedPtr<FJsonValue>> Dropped; int32 Kept = 0;
        if (!Preview || !Rewrite(Preview, Mapping, Configure, false, Dropped, Kept, OutError)) return false;
        Result->SetStringField(TEXT("nodeGuid"), GraphEdit::GetNodeGuid(Node)); Result->SetStringField(TEXT("initialGraphRevision"), Before);
        Result->SetBoolField(TEXT("dryRun"), bDry); Result->SetArrayField(TEXT("droppedLinks"), Dropped); Result->SetNumberField(TEXT("linksKept"), Kept);
        Result->SetBoolField(TEXT("changed"), false); Result->SetBoolField(TEXT("saved"), false);
        if (bDry) { Result->SetArrayField(TEXT("pins"), GraphEdit::SerializePins(Preview)); return true; }
        if (!GEditor || GEditor->IsTransactionActive()) { OutError = TEXT("An idle transaction system is required."); return false; }
        const bool bDirtyBefore = BP->GetPackage()->IsDirty();
        bool bRewritten;
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "EditMemberNode", "UnrealMCP Edit Member Node"));
            if (!GUndo) { OutError = TEXT("Undo recording is unavailable; no node edit applied."); return false; }
            BP->Modify(); Graph->Modify(); Dropped.Reset(); Kept = 0;
            bRewritten = Rewrite(Node, Mapping, Configure, true, Dropped, Kept, OutError);
            if (bRewritten) { Graph->NotifyGraphChanged(); FBlueprintEditorUtils::MarkBlueprintAsModified(BP); }
        }
        bool bCompiled = false;
        if (bRewritten && bCompile)
        {
            TArray<TSharedPtr<FJsonValue>> E, W; bCompiled = Utils::CompileWithLocatedMessages(BP, E, W);
            Result->SetArrayField(TEXT("errors"), E); Result->SetArrayField(TEXT("warnings"), W);
            if (!bCompiled) { OutError = TEXT("Compilation failed; node edit rolled back where indicated."); bRewritten = false; }
        }
        if (!bRewritten)
        {
            const bool bRolledBack = GEditor->UndoTransaction(); Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
            Result->SetBoolField(TEXT("changed"), !bRolledBack);
            if (bRolledBack)
            {
                if (bCompile) { TArray<TSharedPtr<FJsonValue>> E, W; Utils::CompileWithLocatedMessages(BP, E, W); }
                BP->GetPackage()->SetDirtyFlag(bDirtyBefore);
            }
            return false;
        }
        Result->SetBoolField(TEXT("changed"), true); Result->SetBoolField(TEXT("compiled"), bCompiled);
        Result->SetArrayField(TEXT("droppedLinks"), Dropped); Result->SetNumberField(TEXT("linksKept"), Kept); Result->SetArrayField(TEXT("pins"), GraphEdit::SerializePins(Node));
        Result->SetStringField(TEXT("finalGraphRevision"), GraphEdit::ComputeGraphRevision(Graph));
        if (bSave)
        {
            FString File, IndexError; if (!Utils::SaveAsset(BP, File, OutError)) return false;
            Result->SetBoolField(TEXT("saved"), true); Result->SetBoolField(TEXT("indexRefreshed"), Utils::RefreshAssetIndex(Path, IndexError));
        }
        return true;
    }, Error);
    if (!bOk) { Result->SetBoolField(TEXT("success"), false); return BuildError(Request, EMCPErrorCode::InvalidParams, Error, Result); }
    FMCPResponse Response; Response.Id = Request.Id; Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FBlueprintMemberGraphTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>(); auto P = CommonSchema(); S->SetStringField(TEXT("type"), TEXT("object"));
    auto Map = MakeShared<FJsonObject>(); Map->SetStringField(TEXT("type"), TEXT("object")); Map->SetObjectField(TEXT("additionalProperties"), BuildStringProperty(TEXT("Exact replacement pin/case name.")));
    TArray<TSharedPtr<FJsonValue>> Required = {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("nodeGuid"))};
    if (Edit == EBlueprintMemberGraphEdit::RetargetMember)
    {
        P->SetObjectField(TEXT("newMemberName"), BuildStringProperty(TEXT("New variable, callable function, or inherited event name.")));
        P->SetObjectField(TEXT("newOwnerClassPath"), BuildStringProperty(TEXT("Optional owner; self/ancestor uses self context, unrelated class uses external context.")));
        P->SetObjectField(TEXT("pinMap"), Map); Required.Add(MakeShared<FJsonValueString>(TEXT("newMemberName")));
    }
    else
    {
        auto Cases = MakeShared<FJsonObject>(); Cases->SetStringField(TEXT("type"), TEXT("array")); Cases->SetObjectField(TEXT("items"), BuildStringProperty(TEXT("Final String or Name switch label."))); Cases->SetNumberField(TEXT("maxItems"), 128);
        P->SetObjectField(TEXT("cases"), Cases); P->SetObjectField(TEXT("renames"), Map); Required.Add(MakeShared<FJsonValueString>(TEXT("cases")));
    }
    S->SetObjectField(TEXT("properties"), P); S->SetArrayField(TEXT("required"), Required); return S;
}

FOverrideBlueprintFunctionTool::FOverrideBlueprintFunctionTool()
    : FMCPToolBase(TEXT("OverrideBlueprintFunction"), TEXT("Creates a parent function override with its inherited signature and optional wired parent implementation. Undoable; supports dry-run, compile and explicit save."))
{
}

UnrealMCP::FMCPResponse FOverrideBlueprintFunctionTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    namespace Utils = BlueprintEditToolUtils;
    FString Path, FunctionName, Error;
    if (!Request.Params || !Request.Params->TryGetStringField(TEXT("objectPath"), Path)
        || !Request.Params->TryGetStringField(TEXT("functionName"), FunctionName) || Path.IsEmpty() || FunctionName.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Requires objectPath and functionName."));
    const bool bDry = Utils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bParent = Utils::GetOptionalBool(Request.Params, TEXT("addParentCall"), true);
    const bool bSave = Utils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    const bool bCompile = bSave || Utils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    auto Result = BuildBooleanResult(true);
    Result->SetBoolField(TEXT("changed"), false); Result->SetBoolField(TEXT("saved"), false); Result->SetBoolField(TEXT("dryRun"), bDry);
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* BP = nullptr;
        if (!Utils::ResolveBlueprint(Path, BP, OutError)) return false;
        UFunction* Function = BP->ParentClass ? BP->ParentClass->FindFunctionByName(*FunctionName) : nullptr;
        if (!Function || !UEdGraphSchema_K2::CanKismetOverrideFunction(Function)
            || Function->HasAnyFunctionFlags(FUNC_Final | FUNC_Static | FUNC_Private)
            || UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function))
        { OutError = TEXT("The parent member is not an overrideable function graph; use AddBlueprintOverrideEventNode for events."); return false; }
        if (bParent && !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
        { OutError = TEXT("This parent has no callable implementation; set addParentCall=false."); return false; }
        for (UEdGraph* Existing : BP->FunctionGraphs)
            if (Existing && Existing->GetFName() == FName(*FunctionName))
            { OutError = TEXT("A function graph with this name already exists; inspect it before editing."); return false; }
        Result->SetStringField(TEXT("functionName"), FunctionName);
        Result->SetStringField(TEXT("declaredIn"), Function->GetOwnerClass()->GetAuthoritativeClass()->GetPathName());
        Result->SetBoolField(TEXT("addParentCall"), bParent);
        if (bDry) return true;
        if (!GEditor || GEditor->IsTransactionActive()) { OutError = TEXT("An idle transaction system is required."); return false; }
        const bool bDirtyBefore = BP->GetPackage()->IsDirty();
        UEdGraph* Graph = nullptr;
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "OverrideFunction", "UnrealMCP Override Function"));
            if (!GUndo) { OutError = TEXT("Undo recording is unavailable; no override applied."); return false; }
            BP->Modify();
            Graph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, Graph, false, BP->ParentClass);
            if (!bParent)
            {
                TArray<UEdGraphNode*> Nodes = Graph->Nodes;
                for (UEdGraphNode* Node : Nodes) if (Node && Node->IsA<UK2Node_CallParentFunction>()) Graph->RemoveNode(Node);
                UK2Node_FunctionEntry* Entry = nullptr; UK2Node_FunctionResult* Return = nullptr;
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (auto* E = Cast<UK2Node_FunctionEntry>(Node)) Entry = E;
                    if (auto* R = Cast<UK2Node_FunctionResult>(Node)) Return = R;
                }
                if (Entry && Return)
                {
                    UEdGraphPin* Out = Entry->FindPin(UEdGraphSchema_K2::PN_Then);
                    UEdGraphPin* In = Return->FindPin(UEdGraphSchema_K2::PN_Execute);
                    if (Out && In) Graph->GetSchema()->TryCreateConnection(Out, In);
                }
            }
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        }
        bool bCompiled = false;
        if (bCompile)
        {
            TArray<TSharedPtr<FJsonValue>> E, W; bCompiled = Utils::CompileWithLocatedMessages(BP, E, W);
            Result->SetArrayField(TEXT("errors"), E); Result->SetArrayField(TEXT("warnings"), W);
            if (!bCompiled)
            {
                const bool bRolledBack = GEditor->UndoTransaction(); Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
                Result->SetBoolField(TEXT("changed"), !bRolledBack);
                if (bRolledBack) { E.Reset(); W.Reset(); Utils::CompileWithLocatedMessages(BP, E, W); BP->GetPackage()->SetDirtyFlag(bDirtyBefore); }
                OutError = TEXT("Override failed compilation; see rollback and diagnostic evidence."); return false;
            }
        }
        Result->SetBoolField(TEXT("changed"), true); Result->SetBoolField(TEXT("compiled"), bCompiled);
        Result->SetStringField(TEXT("graphGuid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            auto N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("nodeGuid"), GraphEdit::GetNodeGuid(Node));
            N->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName()); N->SetArrayField(TEXT("pins"), GraphEdit::SerializePins(Node));
            Nodes.Add(MakeShared<FJsonValueObject>(N));
        }
        Result->SetArrayField(TEXT("nodes"), Nodes);
        if (bSave)
        {
            FString File, IndexError; if (!Utils::SaveAsset(BP, File, OutError)) return false;
            Result->SetBoolField(TEXT("saved"), true); Result->SetBoolField(TEXT("indexRefreshed"), Utils::RefreshAssetIndex(Path, IndexError));
            Result->SetStringField(TEXT("indexRefreshError"), IndexError);
        }
        return true;
    }, Error);
    if (!bOk) { Result->SetBoolField(TEXT("success"), false); return BuildError(Request, EMCPErrorCode::InvalidParams, Error, Result); }
    FMCPResponse Response; Response.Id = Request.Id; Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FOverrideBlueprintFunctionTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>(); auto Props = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    for (const TCHAR* Key : {TEXT("objectPath"), TEXT("functionName")}) Props->SetObjectField(Key, BuildStringProperty(TEXT("Exact Blueprint object path or inherited function name.")));
    Props->SetObjectField(TEXT("addParentCall"), BuildBoolProperty(TEXT("Wire a callable parent implementation; defaults true.")));
    Props->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without creating a graph.")));
    Props->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile and roll back on failure; defaults true.")));
    Props->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Explicitly save after successful compilation.")));
    Schema->SetObjectField(TEXT("properties"), Props);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("functionName"))});
    return Schema;
}
