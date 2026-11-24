#include "Tools/Blueprint/Graph/ApplyBlueprintGraphPatchTool.h"

#include "MCP/MutationRequestTracker.h"

#include "Dom/JsonObject.h"
#include "Algo/AllOf.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CallArrayFunction.h"
#include "Kismet/KismetArrayLibrary.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Knot.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Self.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Message.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintMemberToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
    struct FPatchDefault
    {
        FString PinName;
        FString LiteralValue;
        FString ObjectPath;
        UObject* Object = nullptr;
        bool bApplied = false;
    };

    struct FPatchNode
    {
        FString Id, Kind, NodeGuid, OwnerClassPath, FunctionName, VariableName, OperatorName, CanonicalOperator;
        FString MacroPath, EnumPath, TargetClassPath;
        TArray<FString> Cases;
        int32 ArrayInputCount = 1;
        UEdGraph* MacroGraph = nullptr;
        UEnum* Enum = nullptr;
        UClass* TargetClass = nullptr;
        bool bPure = false;
        FString Comment;
        bool bHasComment = false, bHasPosition = false, bHasTolerance = false;
        double Tolerance = 0.0;
        int32 X = 0, Y = 0;
        FGuid DeterministicGuid, VariableGuid;
        int32 CommentWidth = 600, CommentHeight = 300;
        bool bShowBubble = true;
        bool bHasWidth = false, bHasHeight = false, bHasColor = false, bHasBubble = false;
        FLinearColor CommentColor = FLinearColor(0.04f, 0.18f, 0.3f, 1.f);
        UFunction* Function = nullptr;
        UClass* VariableOwner = nullptr;
        UnrealMCP::BlueprintMemberToolUtils::FResolvedVariable Variable;
        UEdGraphNode* Preview = nullptr;
        UEdGraphNode* Live = nullptr;
        bool bCreated = false, bReused = false, bMoved = false, bCommentChanged = false;
        TArray<FPatchDefault> Defaults;
    };

    struct FPatchConnection
    {
        FString SourceId, SourcePin, TargetId, TargetPin;
        FString ExistingSourceId, ExistingSourcePin;
        bool bConfirmReplacement = false, bAlreadyConnected = false, bConnected = false, bReplaced = false;
        bool bSourceWildcardBefore = false, bTargetWildcardBefore = false;
        bool bSourceWildcardAfter = false, bTargetWildcardAfter = false, bWildcardResolved = false;
        UEdGraphPin* PreviewSource = nullptr;
        UEdGraphPin* PreviewTarget = nullptr;
    };

    struct FPatchDisconnection
    {
        FString SourceId, SourcePin, TargetId, TargetPin;
        bool bConfirm = false, bAlreadyDisconnected = false, bDisconnected = false;
    };

    bool ParseNode(const TSharedPtr<FJsonObject>& Object, FPatchNode& Out, FString& Error)
    {
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("id"), Out.Id)
            || !Object->TryGetStringField(TEXT("kind"), Out.Kind) || Out.Id.IsEmpty())
        { Error = TEXT("Each patch node requires non-empty id and kind fields."); return false; }
        Object->TryGetStringField(TEXT("nodeGuid"), Out.NodeGuid);
        Object->TryGetStringField(TEXT("ownerClassPath"), Out.OwnerClassPath);
        Object->TryGetStringField(TEXT("functionName"), Out.FunctionName);
        Object->TryGetStringField(TEXT("variableName"), Out.VariableName);
        Object->TryGetStringField(TEXT("operator"), Out.OperatorName);
        Object->TryGetStringField(TEXT("macroPath"), Out.MacroPath);
        Object->TryGetStringField(TEXT("enumPath"), Out.EnumPath);
        Object->TryGetStringField(TEXT("targetClassPath"), Out.TargetClassPath);
        Object->TryGetBoolField(TEXT("pure"), Out.bPure);
        const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
        if (Object->HasField(TEXT("cases")) && !Object->TryGetArrayField(TEXT("cases"), Cases))
        { Error = TEXT("cases must be an array of strings."); return false; }
        if (Object->TryGetArrayField(TEXT("cases"), Cases))
            for (const auto& Value : *Cases) { FString Text; if (!Value || !Value->TryGetString(Text)) { Error = TEXT("cases must contain strings."); return false; } Out.Cases.Add(Text); }
        Out.bHasComment = Object->TryGetStringField(TEXT("comment"), Out.Comment);
        double X = 0.0, Y = 0.0;
        const bool bHasX = Object->TryGetNumberField(TEXT("positionX"), X);
        const bool bHasY = Object->TryGetNumberField(TEXT("positionY"), Y);
        if (bHasX != bHasY) { Error = TEXT("Node placement requires both positionX and positionY."); return false; }
        Out.bHasPosition = bHasX; Out.X = FMath::RoundToInt(X); Out.Y = FMath::RoundToInt(Y);
        Out.bHasTolerance = Object->TryGetNumberField(TEXT("tolerance"), Out.Tolerance);

        const TArray<TSharedPtr<FJsonValue>>* Defaults = nullptr;
        if (Object->TryGetArrayField(TEXT("inputDefaults"), Defaults) && Defaults != nullptr)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Defaults)
            {
                const TSharedPtr<FJsonObject> DefaultObject = Value.IsValid() ? Value->AsObject() : nullptr;
                FPatchDefault Default;
                if (!DefaultObject.IsValid() || !DefaultObject->TryGetStringField(TEXT("pinName"), Default.PinName)
                    || Default.PinName.IsEmpty())
                { Error = TEXT("Each input default requires pinName."); return false; }
                const bool bLiteral = DefaultObject->TryGetStringField(TEXT("defaultValue"), Default.LiteralValue);
                const bool bObject = DefaultObject->TryGetStringField(TEXT("defaultObjectPath"), Default.ObjectPath);
                if (bLiteral == bObject) { Error = TEXT("Each input default requires exactly one value source."); return false; }
                if (Out.Defaults.ContainsByPredicate([&](const FPatchDefault& Existing)
                    { return Existing.PinName.Equals(Default.PinName, ESearchCase::IgnoreCase); }))
                { Error = FString::Printf(TEXT("Duplicate default for pin '%s'."), *Default.PinName); return false; }
                Out.Defaults.Add(MoveTemp(Default));
            }
        }

        if (Out.Kind.Equals(TEXT("existingNode"), ESearchCase::IgnoreCase))
        {
            FGuid Guid;
            if (!FGuid::Parse(Out.NodeGuid, Guid)) { Error = TEXT("existingNode requires a valid nodeGuid."); return false; }
        }
        else if (Out.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase)
            || Out.Kind.Equals(TEXT("interfaceMessage"), ESearchCase::IgnoreCase))
        {
            if (Out.OwnerClassPath.IsEmpty() || Out.FunctionName.IsEmpty())
            { Error = TEXT("functionCall requires ownerClassPath and functionName."); return false; }
        }
        else if (Out.Kind.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase)
            || Out.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase))
        {
            if (Out.VariableName.IsEmpty()) { Error = TEXT("Variable nodes require variableName."); return false; }
        }
        else if (Out.Kind.Equals(TEXT("parentCall"), ESearchCase::IgnoreCase))
        { if (Out.FunctionName.IsEmpty()) { Error = TEXT("parentCall requires functionName."); return false; } }
        else if (Out.Kind.Equals(TEXT("macro"), ESearchCase::IgnoreCase))
        { if (Out.MacroPath.IsEmpty()) { Error = TEXT("macro requires macroPath."); return false; } }
        else if (Out.Kind.Equals(TEXT("cast"), ESearchCase::IgnoreCase))
        { if (Out.TargetClassPath.IsEmpty()) { Error = TEXT("cast requires targetClassPath."); return false; } }
        else if (Out.Kind.Equals(TEXT("switchEnum"), ESearchCase::IgnoreCase))
        { if (Out.EnumPath.IsEmpty()) { Error = TEXT("switchEnum requires enumPath."); return false; } }
        else if (Out.Kind.Equals(TEXT("switchString"), ESearchCase::IgnoreCase)
            || Out.Kind.Equals(TEXT("switchName"), ESearchCase::IgnoreCase))
        { if (!Object->HasField(TEXT("cases"))) { Error = TEXT("String and Name switches require cases[]."); return false; }
          if (!UnrealMCP::BlueprintMemberToolUtils::ValidateStringCases(Out.Cases, Error)) return false; }
        else if (Out.Kind.Equals(TEXT("makeArray"), ESearchCase::IgnoreCase))
        {
            double Count = 1;
            if ((Object->HasField(TEXT("arrayInputCount")) && !Object->TryGetNumberField(TEXT("arrayInputCount"), Count))
                || !FMath::IsFinite(Count) || Count < 0 || Count > 128 || FMath::FloorToDouble(Count) != Count)
            { Error = TEXT("arrayInputCount must be an integer from 0 to 128."); return false; }
            Out.ArrayInputCount = static_cast<int32>(Count);
        }
        else if (Out.Kind.Equals(TEXT("typedOperator"), ESearchCase::IgnoreCase))
        {
            if (Out.OperatorName.IsEmpty()) { Error = TEXT("typedOperator requires operator."); return false; }
        }
        else if (Out.Kind.Equals(TEXT("comment"), ESearchCase::IgnoreCase))
        {
            if (!Out.bHasComment || Out.Comment.IsEmpty()) { Error = TEXT("comment requires authored comment text."); return false; }

        }
        else if (!Out.Kind.Equals(TEXT("self"), ESearchCase::IgnoreCase)
            && !Out.Kind.Equals(TEXT("branch"), ESearchCase::IgnoreCase)
            && !Out.Kind.Equals(TEXT("arrayAddUnique"), ESearchCase::IgnoreCase)
            && !Out.Kind.Equals(TEXT("reroute"), ESearchCase::IgnoreCase))
        { Error = FString::Printf(TEXT("Unsupported patch node kind '%s'."), *Out.Kind); return false; }
        Out.bHasWidth = Object->HasField(TEXT("width")); Out.bHasHeight = Object->HasField(TEXT("height"));
        Out.bHasColor = Object->HasField(TEXT("color")); Out.bHasBubble = Object->HasField(TEXT("showBubbleWhenZoomed"));
        const bool bHasStyle = Out.bHasWidth || Out.bHasHeight || Out.bHasColor || Out.bHasBubble;
        if (bHasStyle && !Out.Kind.Equals(TEXT("comment"), ESearchCase::IgnoreCase) && !Out.Kind.Equals(TEXT("existingNode"), ESearchCase::IgnoreCase))
        { Error = TEXT("Comment styling requires a comment node."); return false; }
        double Width = 600, Height = 300;
        if ((Out.bHasWidth && !Object->TryGetNumberField(TEXT("width"), Width))
            || (Out.bHasHeight && !Object->TryGetNumberField(TEXT("height"), Height))
            || !FMath::IsFinite(Width) || !FMath::IsFinite(Height) || FMath::FloorToDouble(Width) != Width || FMath::FloorToDouble(Height) != Height)
        { Error = TEXT("Comment width and height must be finite integers."); return false; }
        Out.CommentWidth = FMath::RoundToInt(FMath::Clamp(Width, 200.0, 4000.0));
        Out.CommentHeight = FMath::RoundToInt(FMath::Clamp(Height, 100.0, 4000.0));
        if (Out.bHasBubble && (!Object->HasTypedField<EJson::Boolean>(TEXT("showBubbleWhenZoomed")) || !Object->TryGetBoolField(TEXT("showBubbleWhenZoomed"), Out.bShowBubble)))
        { Error = TEXT("showBubbleWhenZoomed must be boolean."); return false; }
        if (Out.bHasColor)
        {
            const TSharedPtr<FJsonObject>* Color = nullptr;
            double R, G, B;
            if (!Object->TryGetObjectField(TEXT("color"), Color) || !(*Color)->TryGetNumberField(TEXT("r"), R)
                || !(*Color)->TryGetNumberField(TEXT("g"), G) || !(*Color)->TryGetNumberField(TEXT("b"), B)
                || !FMath::IsFinite(R) || !FMath::IsFinite(G) || !FMath::IsFinite(B)
                || R < 0 || G < 0 || B < 0 || R > MAX_flt || G > MAX_flt || B > MAX_flt)
            { Error = TEXT("Comment color requires finite nonnegative r/g/b."); return false; }
            Out.CommentColor = FLinearColor(R, G, B, 1.f);
        }
        return true;
    }

    bool ApplyCommentStyle(const FPatchNode& Spec, UEdGraphNode* Node)
    {
        auto* Comment = Cast<UEdGraphNode_Comment>(Node);
        if (!Comment) return false;
        const bool bCreateStyle = Spec.Kind.Equals(TEXT("comment"), ESearchCase::IgnoreCase);
        bool bChanged = false;
        auto Set = [&bChanged](auto& Field, const auto& Value)
        {
            if (Field != Value) { Field = Value; bChanged = true; }
        };
        if (Spec.bHasWidth || bCreateStyle) Set(Comment->NodeWidth, Spec.CommentWidth);
        if (Spec.bHasHeight || bCreateStyle) Set(Comment->NodeHeight, Spec.CommentHeight);
        if (Spec.bHasColor || bCreateStyle) Set(Comment->CommentColor, Spec.CommentColor);
        if (Spec.bHasBubble || bCreateStyle)
        {
            if (Comment->bCommentBubbleVisible_InDetailsPanel != Spec.bShowBubble || Comment->bCommentBubbleVisible != Spec.bShowBubble)
            {
                Comment->bCommentBubbleVisible_InDetailsPanel = Spec.bShowBubble;
                Comment->bCommentBubbleVisible = Spec.bShowBubble;
                bChanged = true;
            }
        }
        if (bCreateStyle && !Comment->bColorCommentBubble) { Comment->bColorCommentBubble = true; bChanged = true; }
        return bChanged;
    }

    bool ParseConnection(const TSharedPtr<FJsonObject>& Object, FPatchConnection& Out, FString& Error)
    {
        if (!Object.IsValid()
            || !Object->TryGetStringField(TEXT("sourceNodeId"), Out.SourceId)
            || !Object->TryGetStringField(TEXT("sourcePinName"), Out.SourcePin)
            || !Object->TryGetStringField(TEXT("targetNodeId"), Out.TargetId)
            || !Object->TryGetStringField(TEXT("targetPinName"), Out.TargetPin))
        { Error = TEXT("Connections require exact source and target node/pin fields."); return false; }
        Object->TryGetStringField(TEXT("existingSourceNodeId"), Out.ExistingSourceId);
        Object->TryGetStringField(TEXT("existingSourcePinName"), Out.ExistingSourcePin);
        Out.bConfirmReplacement = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
            Object, TEXT("confirmDataReplacement"), false);
        if (Out.ExistingSourceId.IsEmpty() != Out.ExistingSourcePin.IsEmpty())
        { Error = TEXT("Data replacement requires both existingSourceNodeId and existingSourcePinName."); return false; }
        return true;
    }

    bool ParseDisconnection(const TSharedPtr<FJsonObject>& Object, FPatchDisconnection& Out, FString& Error)
    {
        if (!Object.IsValid()
            || !Object->TryGetStringField(TEXT("sourceNodeId"), Out.SourceId)
            || !Object->TryGetStringField(TEXT("sourcePinName"), Out.SourcePin)
            || !Object->TryGetStringField(TEXT("targetNodeId"), Out.TargetId)
            || !Object->TryGetStringField(TEXT("targetPinName"), Out.TargetPin))
        { Error = TEXT("Disconnections require exact source and target node/pin fields."); return false; }
        Out.bConfirm = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Object, TEXT("confirm"), false);
        return true;
    }

    FGuid MakeDeterministicGuid(const FString& ObjectPath, const FGuid& GraphGuid, const FString& PatchId, const FString& NodeId)
    {
        const FString Hex = FMD5::HashAnsiString(*(ObjectPath + TEXT("|")
            + GraphGuid.ToString(EGuidFormats::Digits) + TEXT("|") + PatchId + TEXT("|") + NodeId));
        FGuid Result;
        FGuid::ParseExact(Hex, EGuidFormats::Digits, Result);
        return Result;
    }

    UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& Guid)
    {
        for (UEdGraphNode* Node : Graph->Nodes) if (Node != nullptr && Node->NodeGuid == Guid) return Node;
        return nullptr;
    }

    UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction, FString& Error)
    {
        UEdGraphPin* Found = nullptr;
        if (Node == nullptr) { Error = TEXT("Cannot resolve a pin on a null node."); return nullptr; }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin != nullptr && Pin->Direction == Direction
                && Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                if (Found != nullptr) { Error = FString::Printf(TEXT("Pin '%s' is ambiguous."), *Name); return nullptr; }
                Found = Pin;
            }
        }
        if (!Found)
        {
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != Direction || Pin->bOrphanedPin
                    || !Node->GetPinDisplayName(Pin).ToString().Equals(Name, ESearchCase::IgnoreCase))
                    continue;
                if (Found) { Error = FString::Printf(TEXT("Pin display name '%s' is ambiguous; use the exact pin name."), *Name); return nullptr; }
                Found = Pin;
            }
        }
        if (Found == nullptr) Error = FString::Printf(TEXT("Pin '%s' was not found."), *Name);
        return Found;
    }

    bool IsWildcardPin(const UEdGraphPin* Pin)
    {
        return Pin != nullptr
            && (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
                || (Pin->PinType.ContainerType == EPinContainerType::Map
                    && Pin->PinType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Wildcard));
    }

    bool NodeMatches(const FPatchNode& Spec, UEdGraphNode* Node)
    {
        if (Spec.Kind.Equals(TEXT("existingNode"), ESearchCase::IgnoreCase)) return Node != nullptr;
        if (Spec.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase)
            || Spec.Kind.Equals(TEXT("typedOperator"), ESearchCase::IgnoreCase))
        {
            const auto* Call = Cast<UK2Node_CallFunction>(Node);
            const UFunction* Target = Call ? Call->GetTargetFunction() : nullptr;
            return Target && Spec.Function && Target->GetFName() == Spec.Function->GetFName()
                && Target->GetOwnerClass()->GetAuthoritativeClass() == Spec.Function->GetOwnerClass()->GetAuthoritativeClass();
        }
        if (Spec.Kind.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase))
            return Cast<UK2Node_VariableGet>(Node) != nullptr
                && CastChecked<UK2Node_VariableGet>(Node)->VariableReference.GetMemberName() == FName(*Spec.VariableName)
                && (!Spec.VariableGuid.IsValid() || CastChecked<UK2Node_VariableGet>(Node)->VariableReference.GetMemberGuid() == Spec.VariableGuid)
                && (Spec.Variable.IsLocalContext() ? (CastChecked<UK2Node_VariableGet>(Node)->VariableReference.IsLocalScope() && CastChecked<UK2Node_VariableGet>(Node)->VariableReference.GetMemberScopeName() == Spec.Variable.LocalScope) : (Spec.VariableOwner ? CastChecked<UK2Node_VariableGet>(Node)->VariableReference.GetMemberParentClass() == Spec.VariableOwner : CastChecked<UK2Node_VariableGet>(Node)->VariableReference.IsSelfContext()));
        if (Spec.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase))
            return Cast<UK2Node_VariableSet>(Node) != nullptr
                && CastChecked<UK2Node_VariableSet>(Node)->VariableReference.GetMemberName() == FName(*Spec.VariableName)
                && (!Spec.VariableGuid.IsValid() || CastChecked<UK2Node_VariableSet>(Node)->VariableReference.GetMemberGuid() == Spec.VariableGuid)
                && (Spec.Variable.IsLocalContext() ? (CastChecked<UK2Node_VariableSet>(Node)->VariableReference.IsLocalScope() && CastChecked<UK2Node_VariableSet>(Node)->VariableReference.GetMemberScopeName() == Spec.Variable.LocalScope) : (Spec.VariableOwner ? CastChecked<UK2Node_VariableSet>(Node)->VariableReference.GetMemberParentClass() == Spec.VariableOwner : CastChecked<UK2Node_VariableSet>(Node)->VariableReference.IsSelfContext()));
        if (Spec.Kind.Equals(TEXT("arrayAddUnique"), ESearchCase::IgnoreCase))
        {
            const auto* ArrayCall = Cast<UK2Node_CallArrayFunction>(Node);
            return ArrayCall && ArrayCall->GetTargetFunction() == Spec.Function;
        }
        if (Spec.Kind.Equals(TEXT("interfaceMessage"), ESearchCase::IgnoreCase))
        {
            const auto* Message = Cast<UK2Node_Message>(Node);
            return Message && Message->GetTargetFunction() == Spec.Function;
        }
        if (Spec.Kind.Equals(TEXT("parentCall"), ESearchCase::IgnoreCase)) return Node && Node->IsA<UK2Node_CallParentFunction>() && CastChecked<UK2Node_CallParentFunction>(Node)->GetTargetFunction() == Spec.Function;
        if (Spec.Kind.Equals(TEXT("self"), ESearchCase::IgnoreCase)) return Node && Node->IsA<UK2Node_Self>();
        if (Spec.Kind.Equals(TEXT("macro"), ESearchCase::IgnoreCase)) return Node && Node->IsA<UK2Node_MacroInstance>() && CastChecked<UK2Node_MacroInstance>(Node)->GetMacroGraph() == Spec.MacroGraph;
        if (Spec.Kind.Equals(TEXT("cast"), ESearchCase::IgnoreCase)) return Node && Node->IsA<UK2Node_DynamicCast>() && CastChecked<UK2Node_DynamicCast>(Node)->TargetType == Spec.TargetClass && CastChecked<UK2Node_DynamicCast>(Node)->IsNodePure() == Spec.bPure;
        if (Spec.Kind.Equals(TEXT("switchEnum"), ESearchCase::IgnoreCase)) return Node && Node->IsA<UK2Node_SwitchEnum>() && CastChecked<UK2Node_SwitchEnum>(Node)->Enum == Spec.Enum;
        if (Spec.Kind.Equals(TEXT("switchString"), ESearchCase::IgnoreCase)
            || Spec.Kind.Equals(TEXT("switchName"), ESearchCase::IgnoreCase))
        {
            const TArray<FName>* PinNames = nullptr;
            if (Spec.Kind.Equals(TEXT("switchName"), ESearchCase::IgnoreCase))
            {
                if (const auto* Switch = Cast<UK2Node_SwitchName>(Node)) PinNames = &Switch->PinNames;
            }
            else if (const auto* Switch = Cast<UK2Node_SwitchString>(Node)) PinNames = &Switch->PinNames;
            if (!PinNames || PinNames->Num() != Spec.Cases.Num()) return false;
            for (int32 I = 0; I < Spec.Cases.Num(); ++I) if ((*PinNames)[I] != FName(*Spec.Cases[I])) return false;
            return true;
        }
        if (Spec.Kind.Equals(TEXT("makeArray"), ESearchCase::IgnoreCase))
        {
            const auto* Array = Cast<UK2Node_MakeArray>(Node);
            return Array && Array->NumInputs == Spec.ArrayInputCount;
        }
        if (Spec.Kind.Equals(TEXT("comment"), ESearchCase::IgnoreCase)) return Node && Node->IsA<UEdGraphNode_Comment>();
        if (Spec.Kind.Equals(TEXT("branch"), ESearchCase::IgnoreCase)) return Node != nullptr && Node->IsA<UK2Node_IfThenElse>();
        return Node != nullptr && Node->IsA<UK2Node_Knot>();
    }

    UEdGraphNode* CreateNode(UEdGraph* Graph, const FPatchNode& Spec, bool bDetached)
    {
        using namespace UnrealMCP::BlueprintGraphEditToolUtils;
        if (Spec.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase)
            || Spec.Kind.Equals(TEXT("typedOperator"), ESearchCase::IgnoreCase))
            return CreateFunctionCallNode(Graph, Spec.Function, bDetached);
        if (Spec.Kind.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase)
            || Spec.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase))
        {
            return UnrealMCP::BlueprintMemberToolUtils::CreateVariableNode(Graph, Spec.Variable,
                Spec.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase), bDetached);
        }
        UEdGraphNode* Advanced = nullptr;
        if (Spec.Kind.Equals(TEXT("arrayAddUnique"), ESearchCase::IgnoreCase))
        {
            auto* ArrayCall = NewObject<UK2Node_CallArrayFunction>(Graph);
            ArrayCall->SetFromFunction(Spec.Function);
            if (bDetached) ArrayCall->AllocateDefaultPins();
            return ArrayCall;
        }
        if (Spec.Kind.Equals(TEXT("interfaceMessage"), ESearchCase::IgnoreCase))
        {
            auto* Message = NewObject<UK2Node_Message>(Graph);
            Message->SetFromFunction(Spec.Function);
            if (bDetached) Message->AllocateDefaultPins();
            return Message;
        }
        if (Spec.Kind.Equals(TEXT("parentCall"), ESearchCase::IgnoreCase)) return UnrealMCP::BlueprintMemberToolUtils::CreateParentCall(Graph, Spec.Function, bDetached);
        if (Spec.Kind.Equals(TEXT("self"), ESearchCase::IgnoreCase)) Advanced = NewObject<UK2Node_Self>(Graph);
        else if (Spec.Kind.Equals(TEXT("cast"), ESearchCase::IgnoreCase))
        { auto* Cast = NewObject<UK2Node_DynamicCast>(Graph); Cast->TargetType = Spec.TargetClass; Cast->SetPurity(Spec.bPure); Advanced = Cast; }
        else if (Spec.Kind.Equals(TEXT("macro"), ESearchCase::IgnoreCase))
        { auto* Macro = NewObject<UK2Node_MacroInstance>(Graph); Macro->SetMacroGraph(Spec.MacroGraph); Advanced = Macro; }
        else if (Spec.Kind.Equals(TEXT("switchString"), ESearchCase::IgnoreCase))
        { auto* Switch = NewObject<UK2Node_SwitchString>(Graph); for (const auto& Case : Spec.Cases) Switch->PinNames.Add(FName(*Case)); Switch->bHasDefaultPin = true; Advanced = Switch; }
        else if (Spec.Kind.Equals(TEXT("switchName"), ESearchCase::IgnoreCase))
        { auto* Switch = NewObject<UK2Node_SwitchName>(Graph); for (const auto& Case : Spec.Cases) Switch->PinNames.Add(FName(*Case)); Switch->bHasDefaultPin = true; Advanced = Switch; }
        else if (Spec.Kind.Equals(TEXT("switchEnum"), ESearchCase::IgnoreCase))
        { auto* Switch = NewObject<UK2Node_SwitchEnum>(Graph); Switch->SetEnum(Spec.Enum); Switch->bHasDefaultPin = true; Advanced = Switch; }
        else if (Spec.Kind.Equals(TEXT("makeArray"), ESearchCase::IgnoreCase))
        {
            auto* Array = NewObject<UK2Node_MakeArray>(Graph);
            Array->NumInputs = Spec.ArrayInputCount;
            Advanced = Array;
        }
        if (Advanced) { if (bDetached) Advanced->AllocateDefaultPins(); return Advanced; }
        if (Spec.Kind.Equals(TEXT("comment"), ESearchCase::IgnoreCase))
        {
            auto* Comment = NewObject<UEdGraphNode_Comment>(Graph);
            Comment->NodeComment = Spec.Comment; Comment->NodeWidth = Spec.CommentWidth; Comment->NodeHeight = Spec.CommentHeight;
            Comment->CommentColor = Spec.CommentColor; Comment->bCommentBubbleVisible_InDetailsPanel = Spec.bShowBubble;
            Comment->bCommentBubbleVisible = Spec.bShowBubble; Comment->bColorCommentBubble = true;
            return Comment;
        }
        if (Spec.Kind.Equals(TEXT("branch"), ESearchCase::IgnoreCase)) return CreateBranchNode(Graph, bDetached);
        if (Spec.Kind.Equals(TEXT("reroute"), ESearchCase::IgnoreCase)) return CreateRerouteNode(Graph, bDetached);
        return nullptr;
    }

    bool CollapsePatchNodes(UEdGraph* Graph, const TArray<UEdGraphNode*>& Nodes, const FString& GraphName,
        const FGuid& Guid, TSharedPtr<FJsonObject>& Receipt, FString& Error)
    {
        using namespace UnrealMCP::BlueprintGraphEditToolUtils;
        TSet<const UEdGraphNode*> Selection;
        for (const UEdGraphNode* Node : Nodes) Selection.Add(Node);
        struct FBoundary
        {
            UEdGraphPin* Internal;
            TArray<UEdGraphPin*> External;
        };
        TArray<FBoundary> Boundaries;
        for (UEdGraphNode* Node : Nodes)
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                FBoundary Boundary{Pin, {}};
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Owner = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
                    if (!Owner || Selection.Contains(Owner)) continue;
                    if (Owner->GetGraph() != Graph || Pin->bOrphanedPin || Pin->ParentPin
                        || !Pin->SubPins.IsEmpty() || IsWildcardPin(Pin))
                    {
                        Error = TEXT("Collapsed graph boundary contains a cross-graph, split, orphaned, or unresolved pin.");
                        return false;
                    }
                    Boundary.External.Add(Linked);
                }
                if (!Boundary.External.IsEmpty()) Boundaries.Add(MoveTemp(Boundary));
            }
        auto* Composite = NewObject<UK2Node_Composite>(Graph);
        FPlacement Placement;
        Placement.X = Nodes[0]->NodePosX;
        Placement.Y = Nodes[0]->NodePosY;
        Graph->Modify();
        PlaceNewNode(Graph, Composite, Placement);
        Composite->NodeGuid = Guid;
        Composite->OnRenameNode(GraphName);
        UEdGraph* Body = Composite->BoundGraph;
        Body->Modify();
        for (UEdGraphNode* Node : Nodes)
        {
            Node->Modify();
            if (!Node->Rename(nullptr, Body, REN_DontCreateRedirectors))
            { Error = TEXT("Could not move a node into the collapsed graph."); return false; }
            Graph->Nodes.Remove(Node);
            Body->Nodes.Add(Node);
        }

        TArray<TSharedPtr<FJsonValue>> Ports;
        for (const FBoundary& Boundary : Boundaries)
        {
            UEdGraphPin* Internal = Boundary.Internal;
            const auto Direction = Internal->Direction;
            const FName PortName = Composite->CreateUniquePinName(Internal->PinName);
            auto* Tunnel = Direction == EGPD_Input ? Composite->GetEntryNode() : Composite->GetExitNode();
            Tunnel->Modify();
            UEdGraphPin* Outer = Composite->CreatePin(Direction, Internal->PinType, PortName);
            UEdGraphPin* Inner = Tunnel->CreateUserDefinedPin(PortName, Internal->PinType,
                UEdGraphPin::GetComplementaryDirection(Direction));
            if (!Outer || !Inner) { Error = TEXT("Could not create collapsed graph boundary pins."); return false; }
            for (UEdGraphPin* External : Boundary.External)
            {
                External->GetOwningNode()->Modify();
                Internal->BreakLinkTo(External);
                Outer->MakeLinkTo(External);
            }
            Internal->MakeLinkTo(Inner);
            auto Port = MakeShared<FJsonObject>();
            Port->SetStringField(TEXT("name"), Outer->PinName.ToString());
            Port->SetStringField(TEXT("direction"), Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            Port->SetStringField(TEXT("internalNodeGuid"), GetNodeGuid(Internal->GetOwningNode()));
            Port->SetStringField(TEXT("internalPin"), Internal->PinName.ToString());
            Port->SetNumberField(TEXT("externalLinkCount"), Boundary.External.Num());
            Ports.Add(MakeShared<FJsonValueObject>(Port));
        }
        Receipt = MakeShared<FJsonObject>();
        Receipt->SetStringField(TEXT("graphName"), Body->GetName());
        Receipt->SetStringField(TEXT("nodeGuid"), GetNodeGuid(Composite));
        Receipt->SetArrayField(TEXT("ports"), Ports);
        return true;
    }

    bool ApplyDefaults(FPatchNode& Spec, UEdGraphNode* Node, const UEdGraphSchema_K2* Schema, bool bExisting, FString& Error)
    {
        if (Spec.bHasTolerance)
        {
            if (!Spec.Defaults.ContainsByPredicate([](const FPatchDefault& Default)
                { return Default.PinName.Equals(TEXT("ErrorTolerance"), ESearchCase::IgnoreCase); }))
            {
                FPatchDefault Tolerance;
                Tolerance.PinName = TEXT("ErrorTolerance");
                Tolerance.LiteralValue = FString::SanitizeFloat(Spec.Tolerance);
                Spec.Defaults.Add(Tolerance);
            }
        }
        for (FPatchDefault& Default : Spec.Defaults)
        {
            UEdGraphPin* Pin = FindPin(Node, Default.PinName, EGPD_Input, Error);
            if (Pin == nullptr) return false;
            if (!Pin->LinkedTo.IsEmpty()) { Error = FString::Printf(TEXT("Pin '%s.%s' is already connected."), *Spec.Id, *Default.PinName); return false; }
            if (!Default.ObjectPath.IsEmpty())
            {
                Default.Object = LoadObject<UObject>(nullptr, *Default.ObjectPath);
                if (Default.Object == nullptr) { Error = FString::Printf(TEXT("Could not load '%s'."), *Default.ObjectPath); return false; }
                const FString Validation = Schema->IsPinDefaultValid(Pin, FString(), Default.Object, FText::GetEmpty());
                if (!Validation.IsEmpty()) { Error = Validation; return false; }
                if (bExisting)
                {
                    if (Pin->DefaultObject != Default.Object) { Error = FString::Printf(TEXT("Existing default conflicts on '%s.%s'."), *Spec.Id, *Default.PinName); return false; }
                }
                else
                {
                    Schema->TrySetDefaultObject(*Pin, Default.Object);
                    if (Pin->DefaultObject != Default.Object) { Error = TEXT("Unreal rejected an object default."); return false; }
                    Default.bApplied = true;
                }
            }
            else
            {
                const FString Validation = Schema->IsPinDefaultValid(Pin, Default.LiteralValue, nullptr, FText::GetEmpty());
                if (!Validation.IsEmpty()) { Error = Validation; return false; }
                if (bExisting)
                {
                    if (Pin->DefaultValue != Default.LiteralValue) { Error = FString::Printf(TEXT("Existing default conflicts on '%s.%s'."), *Spec.Id, *Default.PinName); return false; }
                }
                else
                {
                    Schema->TrySetDefaultValue(*Pin, Default.LiteralValue);
                    if (Pin->DefaultValue != Default.LiteralValue) { Error = TEXT("Unreal rejected or normalized a literal default."); return false; }
                    Default.bApplied = true;
                }
            }
        }
        return true;
    }
}

FApplyBlueprintGraphPatchTool::FApplyBlueprintGraphPatchTool()
    : FMCPToolBase(TEXT("ApplyBlueprintGraphPatch"),
        TEXT("Atomically preflights and applies a bounded declarative Blueprint graph patch with deterministic node identities, exact data-link replacement, rollback, and compile-before-save.")) {}

UnrealMCP::FMCPResponse FApplyBlueprintGraphPatchTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, GraphName, GraphGuid, PatchId, ExpectedRevision, OperationId;
    const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* ConnectionValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* DisconnectionValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* SpliceValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* LocalValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* RemoveValues = nullptr;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("patchId"), PatchId)
        || !Request.Params->TryGetArrayField(TEXT("nodes"), NodeValues)
        || NodeValues == nullptr || PatchId.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams,
            TEXT("ApplyBlueprintGraphPatch requires objectPath, graph selector, patchId, and non-empty nodes."));
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("expectedGraphRevision"), ExpectedRevision);
    Request.Params->TryGetStringField(TEXT("operationId"), OperationId);
    Request.Params->TryGetArrayField(TEXT("connections"), ConnectionValues);
    Request.Params->TryGetArrayField(TEXT("disconnections"), DisconnectionValues);
    if (Request.Params->HasField(TEXT("spliceExec"))
        && (!Request.Params->TryGetArrayField(TEXT("spliceExec"), SpliceValues) || SpliceValues->Num() > 64))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("spliceExec must be an array of at most 64 operations."));
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));

    if (Request.Params->HasField(TEXT("locals"))
        && (!Request.Params->TryGetArrayField(TEXT("locals"), LocalValues) || LocalValues->Num() > 64))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("locals must be an array of at most 64 declarations."));
    if (Request.Params->HasField(TEXT("removeNodeGuids"))
        && (!Request.Params->TryGetArrayField(TEXT("removeNodeGuids"), RemoveValues) || RemoveValues->Num() > 64))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("removeNodeGuids must be an array of at most 64 exact GUIDs."));
    TSet<FGuid> RemovalGuids;
    if (RemoveValues) for (const auto& Value : *RemoveValues)
    {
        FString Text;
        FGuid Guid;
        if (!Value->TryGetString(Text) || !FGuid::Parse(Text, Guid) || !Guid.IsValid() || RemovalGuids.Contains(Guid))
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Removal GUIDs must be valid and unique."));
        RemovalGuids.Add(Guid);
    }
    if (!RemovalGuids.IsEmpty() && ExpectedRevision.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Node removal requires expectedGraphRevision."));
    if (NodeValues->IsEmpty() && (!LocalValues || LocalValues->IsEmpty()) && RemovalGuids.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide nodes, local declarations, or removals."));

    FString CollapseName;
    TArray<FString> CollapseIds;
    if (Request.Params->HasField(TEXT("collapse")))
    {
        const TSharedPtr<FJsonObject>* Collapse = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* IdValues = nullptr;
        if (!Request.Params->TryGetObjectField(TEXT("collapse"), Collapse)
            || !(*Collapse)->TryGetStringField(TEXT("name"), CollapseName) || CollapseName.IsEmpty() || CollapseName.Len() > 64
            || !Algo::AllOf(CollapseName, [](TCHAR Ch) { return FChar::IsAlnum(Ch) || Ch == TEXT('_'); })
            || !(*Collapse)->TryGetArrayField(TEXT("nodeIds"), IdValues) || IdValues->IsEmpty() || IdValues->Num() > 64
            || ExpectedRevision.IsEmpty())
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("collapse requires expectedGraphRevision, a graph name, and 1..64 new node IDs."));
        for (const auto& Value : *IdValues)
        {
            FString Id;
            if (!Value->TryGetString(Id) || Id.IsEmpty() || CollapseIds.Contains(Id))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Collapse node IDs must be unique nonempty strings."));
            CollapseIds.Add(Id);
        }
    }

    TArray<FPatchNode> Nodes; TSet<FString> NodeIds;
    for (int32 Index = 0; Index < NodeValues->Num(); ++Index)
    {
        FPatchNode Node; FString Error;
        if (!ParseNode((*NodeValues)[Index]->AsObject(), Node, Error) || NodeIds.Contains(Node.Id))
            return BuildError(Request, EMCPErrorCode::InvalidParams,
                FString::Printf(TEXT("Invalid node %d: %s"), Index, NodeIds.Contains(Node.Id) ? TEXT("duplicate id") : *Error));
        NodeIds.Add(Node.Id); Nodes.Add(MoveTemp(Node));
    }
    TArray<FPatchConnection> Connections;
    if (ConnectionValues != nullptr) for (int32 Index = 0; Index < ConnectionValues->Num(); ++Index)
    {
        FPatchConnection Connection; FString Error;
        if (!ParseConnection((*ConnectionValues)[Index]->AsObject(), Connection, Error)
            || !NodeIds.Contains(Connection.SourceId) || !NodeIds.Contains(Connection.TargetId)
            || (!Connection.ExistingSourceId.IsEmpty() && !NodeIds.Contains(Connection.ExistingSourceId)))
            return BuildError(Request, EMCPErrorCode::InvalidParams,
                FString::Printf(TEXT("Invalid connection %d: %s"), Index, Error.IsEmpty() ? TEXT("unknown node id") : *Error));
        Connections.Add(MoveTemp(Connection));
    }
    TArray<FPatchDisconnection> Disconnections;
    if (DisconnectionValues != nullptr) for (int32 Index = 0; Index < DisconnectionValues->Num(); ++Index)
    {
        FPatchDisconnection Disconnection; FString Error;
        if (!ParseDisconnection((*DisconnectionValues)[Index]->AsObject(), Disconnection, Error)
            || !NodeIds.Contains(Disconnection.SourceId) || !NodeIds.Contains(Disconnection.TargetId))
            return BuildError(Request, EMCPErrorCode::InvalidParams,
                FString::Printf(TEXT("Invalid disconnection %d: %s"), Index, Error.IsEmpty() ? TEXT("unknown node id") : *Error));
        Disconnections.Add(MoveTemp(Disconnection));
    }
    const int32 FirstSpliceDisconnection = Disconnections.Num();
    const int32 FirstSpliceConnection = Connections.Num();
    if (SpliceValues) for (const auto& Value : *SpliceValues)
    {
        FPatchDisconnection Direct;
        FString Error, InsertId, InputPin, OutputPin;
        const TSharedPtr<FJsonObject>& Object = Value->AsObject();
        if (!ParseDisconnection(Object, Direct, Error) || !Direct.bConfirm
            || !Object->TryGetStringField(TEXT("insertNodeId"), InsertId)
            || !Object->TryGetStringField(TEXT("insertInputPinName"), InputPin) || InputPin.IsEmpty()
            || !Object->TryGetStringField(TEXT("insertOutputPinName"), OutputPin) || OutputPin.IsEmpty()
            || !NodeIds.Contains(Direct.SourceId) || !NodeIds.Contains(Direct.TargetId) || !NodeIds.Contains(InsertId)
            || InsertId == Direct.SourceId || InsertId == Direct.TargetId)
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("spliceExec requires confirmed endpoints and exact input/output pins on a distinct insert node."));
        FPatchConnection& Into = Connections.AddDefaulted_GetRef();
        Into.SourceId = Direct.SourceId; Into.SourcePin = Direct.SourcePin;
        Into.TargetId = InsertId; Into.TargetPin = InputPin;
        FPatchConnection& OutOf = Connections.AddDefaulted_GetRef();
        OutOf.SourceId = InsertId; OutOf.SourcePin = OutputPin;
        OutOf.TargetId = Direct.TargetId; OutOf.TargetPin = Direct.TargetPin;
        Disconnections.Add(MoveTemp(Direct));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false) || BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    FString InitialRevision, FinalRevision, SavedFilename, IndexError, ExecutionError;
    bool bChanged = false, bCompiled = false, bCompileSucceeded = false, bSaved = false, bIndexRefreshed = false, bRolledBack = false;
    int32 CompileErrors = 0, CompileWarnings = 0;
    TArray<TSharedPtr<FJsonValue>> LocatedErrors, LocatedWarnings;
    TArray<TSharedPtr<FJsonValue>> RemovalResults;
    TSharedPtr<FJsonObject> CollapseResult;

    FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Preflighting,
        TEXT("Resolving the target graph and simulating the complete patch."));

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& Error)
    {
        UBlueprint* Blueprint = nullptr; UEdGraph* Graph = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, Error)
            || !BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, Error)) return false;
        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (Schema == nullptr) { Error = TEXT("Selected graph is not a K2 graph."); return false; }
        const bool bDirtyBefore = Blueprint->GetPackage()->IsDirty();
        const EBlueprintStatus StatusBefore = Blueprint->Status;
        auto RestoreRollbackState = [&]()
        {
            if (bRolledBack)
            {
                Blueprint->GetPackage()->SetDirtyFlag(bDirtyBefore); Blueprint->Status = StatusBefore;
                FinalRevision = BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
            }
        };
        InitialRevision = BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
        if (!ExpectedRevision.IsEmpty() && !ExpectedRevision.Equals(InitialRevision, ESearchCase::IgnoreCase))
        { Error = FString::Printf(TEXT("Graph revision mismatch. expected=%s current=%s"), *ExpectedRevision, *InitialRevision); return false; }

        TArray<UEdGraphNode*> Removals;
        for (const FGuid& Guid : RemovalGuids)
        {
            UEdGraphNode* Node = FindNodeByGuid(Graph, Guid);
            if (!Node || !Node->CanUserDeleteNode() || !Node->GetSubGraphs().IsEmpty())
            {
                Error = TEXT("Removal target is missing, protected, or owns a nested graph: ") + Guid.ToString();
                return false;
            }
            Removals.Add(Node);
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("nodeGuid"), BlueprintGraphEditToolUtils::GetNodeGuid(Node));
            Item->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            TArray<TSharedPtr<FJsonValue>> Links;
            for (const UEdGraphPin* Pin : Node->Pins)
                if (Pin) for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    const UEdGraphNode* Other = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
                    if (!Other) continue;
                    auto Link = MakeShared<FJsonObject>();
                    Link->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
                    Link->SetStringField(TEXT("pinId"), BlueprintGraphEditToolUtils::GetPinId(Pin));
                    Link->SetStringField(TEXT("otherNodeGuid"), BlueprintGraphEditToolUtils::GetNodeGuid(Other));
                    Link->SetStringField(TEXT("otherPinName"), Linked->PinName.ToString());
                    Link->SetStringField(TEXT("otherPinId"), BlueprintGraphEditToolUtils::GetPinId(Linked));
                    Links.Add(MakeShared<FJsonValueObject>(Link));
                }
            Item->SetArrayField(TEXT("disconnectedLinks"), Links);
            RemovalResults.Add(MakeShared<FJsonValueObject>(Item));
        }

        // Preflight on a transient duplicate so TryCreateConnection can reproduce K2 wildcard
        // specialization and connection-order effects without touching the live graph.
        UBlueprint* PreviewBlueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
        PreviewBlueprint->ParentClass = Blueprint->ParentClass;
        PreviewBlueprint->GeneratedClass = Blueprint->GeneratedClass;
        PreviewBlueprint->SkeletonGeneratedClass = Blueprint->SkeletonGeneratedClass;
        PreviewBlueprint->BlueprintType = Blueprint->BlueprintType;
        PreviewBlueprint->Status = BS_BeingCreated;
        UEdGraph* SimulationGraph = DuplicateObject<UEdGraph>(Graph, PreviewBlueprint, Graph->GetFName());
        if (SimulationGraph == nullptr) { Error = TEXT("Could not create a transient graph for patch preflight."); return false; }
        SimulationGraph->SetFlags(RF_Transient);
        if (Blueprint->FunctionGraphs.Contains(Graph)) PreviewBlueprint->FunctionGraphs.Add(SimulationGraph);
        else PreviewBlueprint->UbergraphPages.Add(SimulationGraph);
        for (const UEdGraphNode* Node : Removals)
        {
            UEdGraphNode* Preview = FindNodeByGuid(SimulationGraph, Node->NodeGuid);
            if (!Preview) { Error = TEXT("Removal target is missing in the transient graph."); return false; }
            FBlueprintEditorUtils::RemoveNode(PreviewBlueprint, Preview, true);
        }
        UK2Node_FunctionEntry* LiveEntry = nullptr;
        TArray<FBPVariableDescription> AddedLocals;
        if (LocalValues && !LocalValues->IsEmpty())
        {
            TArray<UK2Node_FunctionEntry*> LiveEntries, PreviewEntries;
            Graph->GetNodesOfClass(LiveEntries);
            SimulationGraph->GetNodesOfClass(PreviewEntries);
            if (!Blueprint->FunctionGraphs.Contains(Graph) || LiveEntries.Num() != 1 || PreviewEntries.Num() != 1
                || !FBlueprintEditorUtils::DoesSupportLocalVariables(Graph))
            {
                Error = TEXT("Local declarations require a top-level function graph with one entry node.");
                return false;
            }
            LiveEntry = LiveEntries[0];
            TSet<FName> DeclaredNames;
            for (const auto& Value : *LocalValues)
            {
                const auto Local = Value->AsObject();
                FString Name, Type, TypeObject, Container, ValueType, ValueObject, Default;
                if (!Local || !Local->TryGetStringField(TEXT("name"), Name) || FName(*Name).IsNone()
                    || !Local->TryGetStringField(TEXT("type"), Type) || DeclaredNames.Contains(FName(*Name)))
                {
                    Error = TEXT("Each local declaration requires a unique non-empty name and type.");
                    return false;
                }
                DeclaredNames.Add(FName(*Name));
                Local->TryGetStringField(TEXT("typeObjectPath"), TypeObject);
                Local->TryGetStringField(TEXT("containerType"), Container);
                Local->TryGetStringField(TEXT("valueType"), ValueType);
                Local->TryGetStringField(TEXT("valueTypeObjectPath"), ValueObject);
                Local->TryGetStringField(TEXT("defaultValue"), Default);
                FEdGraphPinType PinType;
                if (!BlueprintEditToolUtils::BuildContainerPinType(
                    Type, TypeObject, Container.IsEmpty() ? TEXT("none") : Container, ValueType, ValueObject, PinType, Error))
                    return false;
                if (const auto* Existing = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, FName(*Name)))
                {
                    if (Existing->VarType == PinType && Existing->DefaultValue == Default) continue;
                    Error = FString::Printf(TEXT("Local '%s' already exists with a different type or default."), *Name);
                    return false;
                }
                if (!FBlueprintEditorUtils::AddLocalVariable(PreviewBlueprint, SimulationGraph, FName(*Name), PinType, Default))
                {
                    Error = FString::Printf(TEXT("Unreal rejected local declaration '%s'."), *Name);
                    return false;
                }
                auto* Added = PreviewEntries[0]->LocalVariables.FindByPredicate([&](const FBPVariableDescription& Variable)
                {
                    return Variable.VarName == FName(*Name);
                });
                if (!Added) { Error = TEXT("Could not resolve the new local declaration."); return false; }
                Added->VarGuid = MakeDeterministicGuid(ObjectPath, Graph->GraphGuid, PatchId, TEXT("local:") + Name);
                AddedLocals.Add(*Added);
            }
        }

        TMap<FString, FPatchNode*> ById;
        int32 AutoIndex = 0, MaxX = 0;
        for (const UEdGraphNode* Node : Graph->Nodes) if (Node != nullptr) MaxX = FMath::Max(MaxX, Node->NodePosX);
        for (FPatchNode& Spec : Nodes)
        {
            ById.Add(Spec.Id, &Spec);
            if (Spec.Kind.Equals(TEXT("existingNode"), ESearchCase::IgnoreCase))
            {
                FGuid Guid; FGuid::Parse(Spec.NodeGuid, Guid); Spec.Live = FindNodeByGuid(Graph, Guid);
                if (RemovalGuids.Contains(Guid))
                { Error = TEXT("A node being removed cannot also be a patch anchor: ") + Spec.Id; return false; }
                if (Spec.Live == nullptr) { Error = FString::Printf(TEXT("Existing node '%s' was not found."), *Spec.Id); return false; }
                if ((Spec.bHasWidth || Spec.bHasHeight || Spec.bHasColor || Spec.bHasBubble) && !Spec.Live->IsA<UEdGraphNode_Comment>())
                { Error = FString::Printf(TEXT("Node '%s' is not a comment and cannot accept comment styling."), *Spec.Id); return false; }
            }
            else
            {
                Spec.DeterministicGuid = MakeDeterministicGuid(ObjectPath, Graph->GraphGuid, PatchId, Spec.Id);
                const bool bMessage = Spec.Kind.Equals(TEXT("interfaceMessage"), ESearchCase::IgnoreCase);
                if (Spec.Kind.Equals(TEXT("arrayAddUnique"), ESearchCase::IgnoreCase))
                {
                    Spec.Function = UKismetArrayLibrary::StaticClass()->FindFunctionByName(
                        GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_AddUnique));
                    if (!Spec.Function) { Error = TEXT("Could not resolve the Kismet Array AddUnique function."); return false; }
                }
                else if (bMessage || Spec.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase))
                {
                    UClass* Owner = nullptr;
                    if (!BlueprintEditToolUtils::ResolveClass(Spec.OwnerClassPath, Owner, Error)) return false;
                    if (bMessage && !Owner->HasAnyClassFlags(CLASS_Interface))
                    { Error = TEXT("interfaceMessage requires an interface owner class."); return false; }
                    Spec.Function = Owner->FindFunctionByName(*Spec.FunctionName, EIncludeSuperFlag::IncludeSuper);
                    if (Spec.Function == nullptr || !Spec.Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
                    { Error = FString::Printf(TEXT("Function '%s' is not Blueprint-callable."), *Spec.FunctionName); return false; }
                }
                else if (Spec.Kind.Equals(TEXT("typedOperator"), ESearchCase::IgnoreCase))
                {
                    bool bSupportsTolerance = false;
                    if (!BlueprintGraphEditToolUtils::ResolveTypedOperatorFunction(
                        Spec.OperatorName, Spec.CanonicalOperator, Spec.Function, bSupportsTolerance, Error)) return false;
                    if (Spec.bHasTolerance && (!bSupportsTolerance || Spec.Tolerance < 0.0))
                    { Error = bSupportsTolerance ? TEXT("tolerance must be non-negative.") : TEXT("tolerance is only valid for VectorNearlyEqual."); return false; }
                }
                else if (Spec.Kind.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase)
                    || Spec.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase))
                {
                    const bool bDeclaredLocal = Spec.OwnerClassPath.IsEmpty() && AddedLocals.ContainsByPredicate([&](const FBPVariableDescription& Local)
                        { return Local.VarName == FName(*Spec.VariableName); });
                    if (!BlueprintMemberToolUtils::ResolveVariable(bDeclaredLocal ? PreviewBlueprint : Blueprint,
                        bDeclaredLocal ? SimulationGraph : Graph, Spec.VariableName, Spec.OwnerClassPath,
                        Spec.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase), Spec.Variable, Error)) return false;
                    Spec.VariableGuid = Spec.Variable.Guid; Spec.VariableOwner = Spec.Variable.ExternalOwner;
                }
                else if (Spec.Kind.Equals(TEXT("parentCall"), ESearchCase::IgnoreCase))
                {
                    if (!BlueprintMemberToolUtils::ResolveParentFunction(Blueprint, Spec.FunctionName, Spec.Function, Error)) return false;
                    const auto Type = Schema->GetGraphType(Graph);
                    if ((Type != GT_Function && Type != GT_Ubergraph) || (Type == GT_Function && Graph->GetFName() != Spec.Function->GetFName()))
                    { Error = TEXT("Parent calls belong in the matching override function or an event graph."); return false; }
                }
                else if (Spec.Kind.Equals(TEXT("cast"), ESearchCase::IgnoreCase))
                { if (!BlueprintEditToolUtils::ResolveClass(Spec.TargetClassPath, Spec.TargetClass, Error)) return false; }
                else if (Spec.Kind.Equals(TEXT("switchEnum"), ESearchCase::IgnoreCase))
                { Spec.Enum = LoadObject<UEnum>(nullptr, *Spec.EnumPath); if (!Spec.Enum) { Error = TEXT("enumPath did not resolve to an enum."); return false; } }
                else if (Spec.Kind.Equals(TEXT("macro"), ESearchCase::IgnoreCase))
                {
                    Spec.MacroGraph = LoadObject<UEdGraph>(nullptr, *Spec.MacroPath);
                    UBlueprint* Library = Spec.MacroGraph ? Spec.MacroGraph->GetTypedOuter<UBlueprint>() : nullptr;
                    if (!Library || (Library != Blueprint && Library->BlueprintType != BPTYPE_MacroLibrary) || !Library->MacroGraphs.Contains(Spec.MacroGraph))
                    { Error = TEXT("macroPath must identify a macro graph in a macro library or this Blueprint."); return false; }
                    auto* Probe = NewObject<UK2Node_MacroInstance>(Graph); Probe->SetMacroGraph(Spec.MacroGraph);
                    if (!Probe->CanPasteHere(Graph)) { Error = TEXT("Macro is not compatible with this graph."); return false; }
                }
                Spec.Live = FindNodeByGuid(Graph, Spec.DeterministicGuid);
                if (RemovalGuids.Contains(Spec.DeterministicGuid))
                { Error = TEXT("A node being removed cannot also be reused by the patch: ") + Spec.Id; return false; }
                if (Spec.Live != nullptr && !NodeMatches(Spec, Spec.Live))
                { Error = FString::Printf(TEXT("Deterministic node identity collision for '%s'."), *Spec.Id); return false; }
                Spec.bReused = Spec.Live != nullptr;
            }
            if (!Spec.bHasPosition)
            {
                if (Spec.Live != nullptr) { Spec.X = Spec.Live->NodePosX; Spec.Y = Spec.Live->NodePosY; }
                else { Spec.X = MaxX + 380 + AutoIndex * 380; Spec.Y = (AutoIndex % 2) * 180; }
            }
            ++AutoIndex;
        }

        for (FPatchNode& Spec : Nodes)
        {
            if (Spec.Live != nullptr)
            {
                Spec.Preview = FindNodeByGuid(SimulationGraph, Spec.Live->NodeGuid);
            }
            else
            {
                UEdGraphNode* Detached = CreateNode(SimulationGraph, Spec, true);
                Spec.Preview = Detached != nullptr
                    ? DuplicateObject<UEdGraphNode>(Detached, SimulationGraph)
                    : nullptr;
                if (Spec.Preview != nullptr) Spec.Preview->NodeGuid = Spec.DeterministicGuid;
            }
            if (Spec.Preview == nullptr) { Error = FString::Printf(TEXT("Could not preview node '%s'."), *Spec.Id); return false; }
            ApplyCommentStyle(Spec, Spec.Preview);
            if (Spec.bHasComment) Spec.Preview->NodeComment = Spec.Comment;
            if (!ApplyDefaults(Spec, Spec.Preview, Schema, Spec.Live != nullptr, Error)) return false;
        }

        for (int32 Index = FirstSpliceDisconnection; Index < Disconnections.Num(); ++Index)
        {
            const FPatchDisconnection& Direct = Disconnections[Index];
            const int32 ConnectionIndex = FirstSpliceConnection + 2 * (Index - FirstSpliceDisconnection);
            const FPatchConnection& Into = Connections[ConnectionIndex];
            const FPatchConnection& OutOf = Connections[ConnectionIndex + 1];
            const UEdGraphPin* Source = FindPin(ById.FindRef(Direct.SourceId)->Preview, Direct.SourcePin, EGPD_Output, Error);
            const UEdGraphPin* Target = FindPin(ById.FindRef(Direct.TargetId)->Preview, Direct.TargetPin, EGPD_Input, Error);
            const UEdGraphPin* Input = FindPin(ById.FindRef(Into.TargetId)->Preview, Into.TargetPin, EGPD_Input, Error);
            const UEdGraphPin* Output = FindPin(ById.FindRef(OutOf.SourceId)->Preview, OutOf.SourcePin, EGPD_Output, Error);
            for (const UEdGraphPin* Pin : {Source, Target, Input, Output})
                if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                { Error = TEXT("spliceExec requires four execution pins."); return false; }
            if (!Source->LinkedTo.Contains(Target)
                && !(Source->LinkedTo.Contains(Input) && Output->LinkedTo.Contains(Target)))
            { Error = TEXT("spliceExec could not find the exact direct link or the already-spliced route."); return false; }
        }

        // Apply confirmed disconnections to the simulation first, matching real mutation order.
        for (FPatchDisconnection& Disconnection : Disconnections)
        {
            UEdGraphPin* Source = FindPin(ById.FindRef(Disconnection.SourceId)->Preview, Disconnection.SourcePin, EGPD_Output, Error);
            UEdGraphPin* Target = FindPin(ById.FindRef(Disconnection.TargetId)->Preview, Disconnection.TargetPin, EGPD_Input, Error);
            if (Source == nullptr || Target == nullptr) return false;
            Disconnection.bAlreadyDisconnected = !Source->LinkedTo.Contains(Target);
            if (!Disconnection.bAlreadyDisconnected && !Disconnection.bConfirm)
            { Error = TEXT("Exact link disconnection requires confirm=true."); return false; }
            if (!Disconnection.bAlreadyDisconnected) Schema->BreakSinglePinLink(Source, Target);
        }

        for (FPatchConnection& Connection : Connections)
        {
            FPatchNode* SourceNode = ById.FindRef(Connection.SourceId); FPatchNode* TargetNode = ById.FindRef(Connection.TargetId);
            UEdGraphPin* Source = FindPin(SourceNode->Preview, Connection.SourcePin, EGPD_Output, Error);
            UEdGraphPin* Target = FindPin(TargetNode->Preview, Connection.TargetPin, EGPD_Input, Error);
            if (Source == nullptr || Target == nullptr) return false;
            Connection.PreviewSource = Source; Connection.PreviewTarget = Target;
            Connection.bSourceWildcardBefore = IsWildcardPin(Source);
            Connection.bTargetWildcardBefore = IsWildcardPin(Target);
            Connection.bAlreadyConnected = Source->LinkedTo.Contains(Target);
            if (Connection.bAlreadyConnected) continue;
            const bool bExec = Target->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
            if (!bExec && !Target->LinkedTo.IsEmpty())
            {
                if (!Connection.bConfirmReplacement || Connection.ExistingSourceId.IsEmpty())
                { Error = TEXT("Occupied data input replacement requires exact existing source fields and confirmDataReplacement=true."); return false; }
                FPatchNode* ExistingSourceNode = ById.FindRef(Connection.ExistingSourceId);
                UEdGraphPin* ExistingSource = FindPin(ExistingSourceNode->Preview, Connection.ExistingSourcePin, EGPD_Output, Error);
                if (ExistingSource == nullptr || Target->LinkedTo.Num() != 1 || !Target->LinkedTo.Contains(ExistingSource))
                { Error = TEXT("The confirmed existing data link does not exactly match the live graph."); return false; }
                Schema->BreakSinglePinLink(ExistingSource, Target);
            }
            const FPinConnectionResponse Compatibility = Schema->CanCreateConnection(Source, Target);
            if (bExec && Compatibility.Response != CONNECT_RESPONSE_MAKE)
            { Error = TEXT("Execution connection would replace an existing route or is incompatible. Disconnect the exact route explicitly first."); return false; }
            if (Compatibility.Response != CONNECT_RESPONSE_MAKE && Compatibility.Response != CONNECT_RESPONSE_BREAK_OTHERS_A
                && Compatibility.Response != CONNECT_RESPONSE_BREAK_OTHERS_B && Compatibility.Response != CONNECT_RESPONSE_BREAK_OTHERS_AB)
            { Error = FString::Printf(TEXT("Pins '%s.%s' and '%s.%s' are not directly compatible: %s"),
                *Connection.SourceId, *Connection.SourcePin, *Connection.TargetId, *Connection.TargetPin, *Compatibility.Message.ToString()); return false; }
            if (!Schema->TryCreateConnection(Source, Target))
            { Error = TEXT("Unreal rejected a connection during transient wildcard-specialization preflight."); return false; }
        }
        for (FPatchConnection& Connection : Connections)
        {
            Connection.bSourceWildcardAfter = IsWildcardPin(Connection.PreviewSource);
            Connection.bTargetWildcardAfter = IsWildcardPin(Connection.PreviewTarget);
            Connection.bWildcardResolved = (Connection.bSourceWildcardBefore && !Connection.bSourceWildcardAfter)
                || (Connection.bTargetWildcardBefore && !Connection.bTargetWildcardAfter);
        }

        for (const FPatchNode& Spec : Nodes)
        {
            const bool bMessage = Spec.Kind.Equals(TEXT("interfaceMessage"), ESearchCase::IgnoreCase);
            if (!bMessage && (!Spec.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase) || Spec.Function == nullptr
                || Spec.Function->HasAnyFunctionFlags(FUNC_Static)
                || (Blueprint->GeneratedClass != nullptr && Blueprint->GeneratedClass->IsChildOf(Spec.Function->GetOwnerClass())))) continue;
            UEdGraphPin* SelfPin = FindPin(Spec.Preview, UEdGraphSchema_K2::PN_Self.ToString(), EGPD_Input, Error);
            if (SelfPin == nullptr) return false;
            const bool bPlannedTarget = Connections.ContainsByPredicate([&](const FPatchConnection& Connection)
                { return Connection.TargetId == Spec.Id && Connection.TargetPin.Equals(SelfPin->PinName.ToString(), ESearchCase::IgnoreCase); });
            if (SelfPin->LinkedTo.IsEmpty() && !bPlannedTarget)
            { Error = FString::Printf(TEXT("External instance function node '%s' requires an explicit self/target connection."), *Spec.Id); return false; }
        }

        FGuid CollapsedGuid;
        if (!CollapseIds.IsEmpty())
        {
            TArray<UEdGraph*> ExistingGraphs;
            Blueprint->GetAllGraphs(ExistingGraphs);
            if (ExistingGraphs.ContainsByPredicate([&](const UEdGraph* Candidate)
                { return Candidate && Candidate->GetFName() == FName(*CollapseName); }))
            { Error = TEXT("The collapsed graph name is already in use."); return false; }
            TArray<UEdGraphNode*> Selected;
            for (const FString& Id : CollapseIds)
            {
                const FPatchNode* Spec = ById.FindRef(Id);
                if (!Spec || Spec->Live || !Spec->Preview)
                { Error = TEXT("Only newly created patch nodes can be collapsed: ") + Id; return false; }
                Selected.Add(Spec->Preview);
            }
            CollapsedGuid = MakeDeterministicGuid(ObjectPath, Graph->GraphGuid, PatchId, TEXT("collapse:") + CollapseName);
            if (FindNodeByGuid(Graph, CollapsedGuid) || !CollapsePatchNodes(SimulationGraph, Selected, CollapseName, CollapsedGuid, CollapseResult, Error))
                return false;
        }
        if (bDryRun) { FinalRevision = InitialRevision; return true; }

        FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Mutating,
            TEXT("Applying the preflighted graph patch transaction."));

        if ((!Removals.IsEmpty() || !CollapseIds.IsEmpty()) && (!GEditor || GEditor->IsTransactionActive()))
        { Error = TEXT("Removal and collapse require an idle editor transaction system."); return false; }
        bool bApplyFailed = false;
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "ApplyBlueprintGraphPatch", "UnrealMCP Apply Blueprint Graph Patch"));
            if ((!Removals.IsEmpty() || !CollapseIds.IsEmpty()) && !GUndo)
            { Error = TEXT("Undo recording is unavailable."); return false; }
            Blueprint->Modify(); Graph->Modify();
            for (UEdGraphNode* Node : Removals)
            {
                Node->SetFlags(RF_Transactional);
                Node->Modify();
                for (const UEdGraphPin* Pin : Node->Pins)
                    if (Pin) for (const UEdGraphPin* Linked : Pin->LinkedTo)
                        if (UEdGraphNode* Other = Linked ? Linked->GetOwningNodeUnchecked() : nullptr)
                        {
                            Other->SetFlags(RF_Transactional);
                            Other->Modify();
                        }
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
                bChanged = true;
            }
            if (!AddedLocals.IsEmpty())
            {
                LiveEntry->Modify();
                LiveEntry->LocalVariables.Append(AddedLocals);
                bChanged = true;
            }
            for (FPatchNode& Spec : Nodes)
            {
                if (Spec.Live == nullptr)
                {
                    Spec.Live = CreateNode(Graph, Spec, false);
                    if (Spec.Live == nullptr) { Error = TEXT("Failed to create a preflighted node."); bApplyFailed = true; break; }
                    BlueprintGraphEditToolUtils::FPlacement Placement; Placement.X = Spec.X; Placement.Y = Spec.Y;
                    BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Spec.Live, Placement);
                    Spec.Live->NodeGuid = Spec.DeterministicGuid;
                    Spec.bCreated = true; bChanged = true;
                    if (!ApplyDefaults(Spec, Spec.Live, Schema, false, Error)) { bApplyFailed = true; break; }
                }
                else
                {
                    Spec.Live->Modify();
                    if (Spec.Live->NodePosX != Spec.X || Spec.Live->NodePosY != Spec.Y)
                    { Spec.Live->NodePosX = Spec.X; Spec.Live->NodePosY = Spec.Y; Spec.bMoved = true; bChanged = true; }
                }
                if (ApplyCommentStyle(Spec, Spec.Live)) { Spec.bCommentChanged = true; bChanged = true; }
                if (Spec.bHasComment && Spec.Live->NodeComment != Spec.Comment)
                { Spec.Live->NodeComment = Spec.Comment; Spec.bCommentChanged = true; bChanged = true; }
            }
            if (!bApplyFailed) for (FPatchDisconnection& Disconnection : Disconnections)
            {
                UEdGraphPin* Source = FindPin(ById.FindRef(Disconnection.SourceId)->Live, Disconnection.SourcePin, EGPD_Output, Error);
                UEdGraphPin* Target = FindPin(ById.FindRef(Disconnection.TargetId)->Live, Disconnection.TargetPin, EGPD_Input, Error);
                if (Source == nullptr || Target == nullptr) { bApplyFailed = true; break; }
                if (Source->LinkedTo.Contains(Target))
                { Schema->BreakSinglePinLink(Source, Target); Disconnection.bDisconnected = true; bChanged = true; }
            }
            if (!bApplyFailed) for (FPatchConnection& Connection : Connections)
            {
                UEdGraphPin* Source = FindPin(ById.FindRef(Connection.SourceId)->Live, Connection.SourcePin, EGPD_Output, Error);
                UEdGraphPin* Target = FindPin(ById.FindRef(Connection.TargetId)->Live, Connection.TargetPin, EGPD_Input, Error);
                if (Source == nullptr || Target == nullptr) { bApplyFailed = true; break; }
                if (Source->LinkedTo.Contains(Target)) continue;
                const bool bExec = Target->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
                if (!bExec && !Target->LinkedTo.IsEmpty())
                {
                    const FPatchNode* ExistingNode = ById.FindRef(Connection.ExistingSourceId);
                    UEdGraphPin* ExistingSource = ExistingNode && Connection.bConfirmReplacement
                        ? FindPin(ExistingNode->Live, Connection.ExistingSourcePin, EGPD_Output, Error) : nullptr;
                    if (ExistingSource == nullptr || Target->LinkedTo.Num() != 1 || !Target->LinkedTo.Contains(ExistingSource))
                    { Error = TEXT("Confirmed replacement link changed after preflight."); bApplyFailed = true; break; }
                    Schema->BreakSinglePinLink(ExistingSource, Target); Connection.bReplaced = true;
                }
                if (bExec && Schema->CanCreateConnection(Source, Target).Response != CONNECT_RESPONSE_MAKE)
                { Error = TEXT("The execution route changed after preflight."); bApplyFailed = true; break; }
                if (!Schema->TryCreateConnection(Source, Target))
                { Error = TEXT("Unreal rejected a preflighted direct connection."); bApplyFailed = true; break; }
                Connection.bConnected = true; bChanged = true;
            }
            if (!bApplyFailed && !CollapseIds.IsEmpty())
            {
                TArray<UEdGraphNode*> Selected;
                for (const FString& Id : CollapseIds) Selected.Add(ById.FindChecked(Id)->Live);
                bApplyFailed = !CollapsePatchNodes(Graph, Selected, CollapseName, CollapsedGuid, CollapseResult, Error);
            }
            if (!bApplyFailed && bChanged)
            {
                Graph->NotifyGraphChanged();
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            }
        }
        if (bApplyFailed)
        {
            bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
            if (bRolledBack) FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::RolledBack,
                TEXT("Patch application failed and the editor transaction was rolled back."));
                RestoreRollbackState();
            if (!bRolledBack) Error += TEXT(" Immediate transaction rollback failed.");
            return false;
        }

        if (bCompile && bChanged)
        {
            FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Compiling,
                TEXT("Compiling the mutated Blueprint before any save."));
            bCompileSucceeded = BlueprintEditToolUtils::CompileWithLocatedMessages(Blueprint, LocatedErrors, LocatedWarnings);
            bCompiled = true; CompileErrors = LocatedErrors.Num(); CompileWarnings = LocatedWarnings.Num();
            if (!bCompileSucceeded)
            {
                bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
                if (bRolledBack) FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::RolledBack,
                    TEXT("Blueprint compilation failed and the graph patch was rolled back."));
                if (bRolledBack)
                {
                    FCompilerResultsLog RollbackLog;
                    RollbackLog.bSilentMode = true;
                    FKismetEditorUtilities::CompileBlueprint(
                        Blueprint,
                        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection,
                        &RollbackLog);
                }
                RestoreRollbackState();
                Error = FString::Printf(TEXT("Blueprint compilation failed with %d errors; patch rolled back=%s."),
                    CompileErrors, bRolledBack ? TEXT("true") : TEXT("false"));
                return false;
            }
        }
        else bCompileSucceeded = !bCompile || !bChanged;

        if (bSave && bChanged)
        {
            if (!BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, Error))
            {
                bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
                if (bRolledBack) FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::RolledBack,
                    TEXT("Asset save failed and the graph patch was rolled back."));
                RestoreRollbackState();
                return false;
            }
            bSaved = true;
            bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
        }
        FinalRevision = BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
        return true;
    }, ExecutionError);

    if (!bSucceeded)
    {
        auto Evidence = BuildBooleanResult(false);
        Evidence->SetArrayField(TEXT("errors"), LocatedErrors); Evidence->SetArrayField(TEXT("warnings"), LocatedWarnings);
        Evidence->SetBoolField(TEXT("rolledBack"), bRolledBack); Evidence->SetBoolField(TEXT("saved"), bSaved);
        Evidence->SetBoolField(TEXT("changed"), bChanged && !bRolledBack);
        Evidence->SetNumberField(TEXT("compileErrors"), CompileErrors); Evidence->SetNumberField(TEXT("compileWarnings"), CompileWarnings);
        Evidence->SetStringField(TEXT("initialGraphRevision"), InitialRevision); Evidence->SetStringField(TEXT("finalGraphRevision"), FinalRevision);
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError, Evidence);
    }

    FString DetailLevel = TEXT("summary");
    Request.Params->TryGetStringField(TEXT("detailLevel"), DetailLevel);
    const bool bIncludePins = DetailLevel == TEXT("full");
    TArray<TSharedPtr<FJsonValue>> NodeResults;
    for (const FPatchNode& Node : Nodes)
    {
        const UEdGraphNode* ResultNode = bDryRun ? Node.Preview : Node.Live;
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), Node.Id); Item->SetStringField(TEXT("kind"), Node.Kind);
        Item->SetStringField(TEXT("nodeGuid"), bDryRun && !Node.bReused && !Node.Kind.Equals(TEXT("existingNode"), ESearchCase::IgnoreCase)
            ? Node.DeterministicGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
            : BlueprintGraphEditToolUtils::GetNodeGuid(ResultNode));
        Item->SetStringField(TEXT("nodeClass"), ResultNode != nullptr ? ResultNode->GetClass()->GetPathName() : FString());
        Item->SetStringField(TEXT("graphName"), ResultNode != nullptr && ResultNode->GetGraph() != nullptr ? ResultNode->GetGraph()->GetName() : FString());
        Item->SetBoolField(TEXT("created"), Node.bCreated); Item->SetBoolField(TEXT("reused"), Node.bReused);
        Item->SetBoolField(TEXT("wouldCreate"), bDryRun && !Node.bReused && !Node.Kind.Equals(TEXT("existingNode"), ESearchCase::IgnoreCase));
        Item->SetBoolField(TEXT("moved"), Node.bMoved); Item->SetBoolField(TEXT("commentChanged"), Node.bCommentChanged);
        Item->SetNumberField(TEXT("positionX"), Node.X); Item->SetNumberField(TEXT("positionY"), Node.Y);
        if (const auto* Comment = Cast<UEdGraphNode_Comment>(ResultNode))
        {
            Item->SetStringField(TEXT("commentText"), Comment->NodeComment);
            Item->SetNumberField(TEXT("width"), Comment->NodeWidth); Item->SetNumberField(TEXT("height"), Comment->NodeHeight);
            Item->SetBoolField(TEXT("showBubbleWhenZoomed"), Comment->bCommentBubbleVisible_InDetailsPanel);
            auto Color = MakeShared<FJsonObject>(); Color->SetNumberField(TEXT("r"), Comment->CommentColor.R);
            Color->SetNumberField(TEXT("g"), Comment->CommentColor.G); Color->SetNumberField(TEXT("b"), Comment->CommentColor.B);
            Item->SetObjectField(TEXT("color"), Color);
        }
        if (!Node.Variable.Name.IsNone())
        {
            Item->SetStringField(TEXT("variableScope"), Node.Variable.IsLocalContext() ? TEXT("local") : TEXT("member"));
            Item->SetStringField(TEXT("scopeName"), Node.Variable.LocalScope);
            Item->SetStringField(TEXT("variableGuid"), Node.Variable.Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Item->SetBoolField(TEXT("selfContext"), Node.Variable.IsSelfContext());
            Item->SetStringField(TEXT("declaredIn"), Node.Variable.DeclarationPath);
        }
        if (bIncludePins) Item->SetArrayField(TEXT("pins"), BlueprintGraphEditToolUtils::SerializePins(ResultNode));
        NodeResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    TArray<TSharedPtr<FJsonValue>> ConnectionResults;
    for (const FPatchConnection& Connection : Connections)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("sourceNodeId"), Connection.SourceId); Item->SetStringField(TEXT("sourcePinName"), Connection.SourcePin);
        Item->SetStringField(TEXT("targetNodeId"), Connection.TargetId); Item->SetStringField(TEXT("targetPinName"), Connection.TargetPin);
        Item->SetBoolField(TEXT("alreadyConnected"), Connection.bAlreadyConnected);
        Item->SetBoolField(TEXT("connected"), Connection.bConnected); Item->SetBoolField(TEXT("replacedExistingDataLink"), Connection.bReplaced);
        Item->SetBoolField(TEXT("sourceWildcardBefore"), Connection.bSourceWildcardBefore);
        Item->SetBoolField(TEXT("targetWildcardBefore"), Connection.bTargetWildcardBefore);
        Item->SetBoolField(TEXT("sourceWildcardAfter"), Connection.bSourceWildcardAfter);
        Item->SetBoolField(TEXT("targetWildcardAfter"), Connection.bTargetWildcardAfter);
        Item->SetBoolField(TEXT("wildcardResolved"), Connection.bWildcardResolved);
        ConnectionResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    TArray<TSharedPtr<FJsonValue>> DisconnectionResults;
    for (const FPatchDisconnection& Disconnection : Disconnections)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("sourceNodeId"), Disconnection.SourceId); Item->SetStringField(TEXT("sourcePinName"), Disconnection.SourcePin);
        Item->SetStringField(TEXT("targetNodeId"), Disconnection.TargetId); Item->SetStringField(TEXT("targetPinName"), Disconnection.TargetPin);
        Item->SetBoolField(TEXT("alreadyDisconnected"), Disconnection.bAlreadyDisconnected);
        Item->SetBoolField(TEXT("disconnected"), Disconnection.bDisconnected);
        DisconnectionResults.Add(MakeShared<FJsonValueObject>(Item));
    }

    FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    int32 WildcardResolvedConnectionCount = 0;
    for (const FPatchConnection& Connection : Connections)
        if (Connection.bWildcardResolved) ++WildcardResolvedConnectionCount;
    Result->SetArrayField(TEXT("errors"), LocatedErrors); Result->SetArrayField(TEXT("warnings"), LocatedWarnings);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("patchId"), PatchId);
    Result->SetStringField(TEXT("initialGraphRevision"), InitialRevision); Result->SetStringField(TEXT("finalGraphRevision"), FinalRevision);
    Result->SetBoolField(TEXT("dryRun"), bDryRun); Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("wildcardSpecializationSimulated"), true);
    Result->SetNumberField(TEXT("wildcardResolvedConnectionCount"), WildcardResolvedConnectionCount);
    Result->SetBoolField(TEXT("alreadyComplete"), !bDryRun && !bChanged); Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
    if (CollapseResult) Result->SetObjectField(TEXT("collapsedGraph"), CollapseResult);
    Result->SetArrayField(TEXT("nodeRemovals"), RemovalResults);
    Result->SetNumberField(TEXT("removedNodeCount"), bDryRun ? 0 : RemovalResults.Num());
    Result->SetArrayField(TEXT("nodes"), NodeResults); Result->SetArrayField(TEXT("connections"), ConnectionResults);
    Result->SetArrayField(TEXT("disconnections"), DisconnectionResults);
    Result->SetBoolField(TEXT("compiled"), bCompiled); Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetNumberField(TEXT("compileErrorCount"), CompileErrors); Result->SetNumberField(TEXT("compileWarningCount"), CompileWarnings);
    Result->SetBoolField(TEXT("saved"), bSaved); Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed); Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FApplyBlueprintGraphPatchTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("patchId"), TEXT("expectedGraphRevision"), TEXT("operationId")})
        Properties->SetObjectField(FieldName, BuildStringProperty(TEXT("Blueprint, graph, deterministic patch, or revision selector.")));
    TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>(); Nodes->SetStringField(TEXT("type"), TEXT("array"));
    Nodes->SetStringField(TEXT("description"), TEXT("Required node kinds: existingNode, functionCall, interfaceMessage, variableGet, variableSet, typedOperator, branch, reroute, comment, parentCall, switchString, switchName, switchEnum, makeArray, arrayAddUnique, self, macro, and cast. Nodes may include position, comment, tolerance, and inputDefaults. Comment nodes can be resized and styled by existingNode GUID; omitted styles are preserved."));
    TSharedRef<FJsonObject> NodeItem = MakeShared<FJsonObject>(); NodeItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> NodeProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("id"), TEXT("kind"), TEXT("nodeGuid"), TEXT("ownerClassPath"), TEXT("functionName"), TEXT("variableName"), TEXT("operator"), TEXT("comment"), TEXT("macroPath"), TEXT("enumPath"), TEXT("targetClassPath")})
        NodeProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Plan-local identity or node-specific selector.")));
    auto ArrayInputCount = MakeShared<FJsonObject>();
    ArrayInputCount->SetStringField(TEXT("type"), TEXT("integer"));
    ArrayInputCount->SetNumberField(TEXT("minimum"), 0);
    ArrayInputCount->SetNumberField(TEXT("maximum"), 128);
    ArrayInputCount->SetStringField(TEXT("description"), TEXT("Number of positional inputs for makeArray. Defaults to one."));
    NodeProperties->SetObjectField(TEXT("arrayInputCount"), ArrayInputCount);
    NodeProperties->SetObjectField(TEXT("variableName"), BuildStringProperty(TEXT("Function-local variable or class property. Without ownerClassPath the selected function's locals take precedence.")));
    NodeProperties->SetObjectField(TEXT("ownerClassPath"), BuildStringProperty(TEXT("Explicit class owner for member variables or function calls; bypasses function locals.")));
    TSharedRef<FJsonObject> Integer = MakeShared<FJsonObject>(); Integer->SetStringField(TEXT("type"), TEXT("integer"));
    TSharedRef<FJsonObject> Number = MakeShared<FJsonObject>(); Number->SetStringField(TEXT("type"), TEXT("number"));
    NodeProperties->SetObjectField(TEXT("positionX"), Integer); NodeProperties->SetObjectField(TEXT("positionY"), Integer);
    NodeProperties->SetObjectField(TEXT("tolerance"), Number);
    NodeProperties->SetObjectField(TEXT("pure"), BuildBoolProperty(TEXT("Pure cast; default false.")));
    auto CasesSchema = MakeShared<FJsonObject>(); CasesSchema->SetStringField(TEXT("type"), TEXT("array")); CasesSchema->SetObjectField(TEXT("items"), BuildStringProperty(TEXT("String switch case label."))); CasesSchema->SetNumberField(TEXT("maxItems"), 128);
    NodeProperties->SetObjectField(TEXT("cases"), CasesSchema);
    NodeProperties->SetObjectField(TEXT("width"), Integer); NodeProperties->SetObjectField(TEXT("height"), Integer);
    NodeProperties->SetObjectField(TEXT("showBubbleWhenZoomed"), BuildBoolProperty(TEXT("Comment bubble visibility. Defaults true for comment creation; omitted on existingNode preserves its setting.")));
    auto ColorSchema = MakeShared<FJsonObject>(); ColorSchema->SetStringField(TEXT("type"), TEXT("object"));
    auto Channels = MakeShared<FJsonObject>();
    for (const TCHAR* ChannelName : {TEXT("r"), TEXT("g"), TEXT("b")}) Channels->SetObjectField(ChannelName, Number);
    ColorSchema->SetObjectField(TEXT("properties"), Channels);
    ColorSchema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("r")), MakeShared<FJsonValueString>(TEXT("g")), MakeShared<FJsonValueString>(TEXT("b"))});
    NodeProperties->SetObjectField(TEXT("color"), ColorSchema);
    TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>(); Defaults->SetStringField(TEXT("type"), TEXT("array"));
    TSharedRef<FJsonObject> DefaultItem = MakeShared<FJsonObject>(); DefaultItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> DefaultProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("pinName"), TEXT("defaultValue"), TEXT("defaultObjectPath")})
        DefaultProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Exact input pin and one default source.")));
    DefaultItem->SetObjectField(TEXT("properties"), DefaultProperties); DefaultItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("pinName"))});
    Defaults->SetObjectField(TEXT("items"), DefaultItem); NodeProperties->SetObjectField(TEXT("inputDefaults"), Defaults);
    NodeItem->SetObjectField(TEXT("properties"), NodeProperties);
    NodeItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("id")), MakeShared<FJsonValueString>(TEXT("kind"))});
    Nodes->SetObjectField(TEXT("items"), NodeItem); Properties->SetObjectField(TEXT("nodes"), Nodes);

    TSharedRef<FJsonObject> Connections = MakeShared<FJsonObject>(); Connections->SetStringField(TEXT("type"), TEXT("array"));
    Connections->SetStringField(TEXT("description"), TEXT("Exact output-to-input connections. Occupied data inputs require exact existing source fields plus confirmDataReplacement=true."));
    TSharedRef<FJsonObject> ConnectionItem = MakeShared<FJsonObject>(); ConnectionItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> ConnectionProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("sourceNodeId"), TEXT("sourcePinName"), TEXT("targetNodeId"), TEXT("targetPinName"), TEXT("existingSourceNodeId"), TEXT("existingSourcePinName")})
        ConnectionProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Plan node id and exact pin name.")));
    ConnectionProperties->SetObjectField(TEXT("confirmDataReplacement"), BuildBoolProperty(TEXT("Explicitly confirm replacement of the identified occupied data link.")));
    ConnectionItem->SetObjectField(TEXT("properties"), ConnectionProperties);
    ConnectionItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("sourceNodeId")), MakeShared<FJsonValueString>(TEXT("sourcePinName")), MakeShared<FJsonValueString>(TEXT("targetNodeId")), MakeShared<FJsonValueString>(TEXT("targetPinName"))});
    Connections->SetObjectField(TEXT("items"), ConnectionItem); Properties->SetObjectField(TEXT("connections"), Connections);

    TSharedRef<FJsonObject> Disconnections = MakeShared<FJsonObject>(); Disconnections->SetStringField(TEXT("type"), TEXT("array"));
    TSharedRef<FJsonObject> DisconnectionItem = MakeShared<FJsonObject>(); DisconnectionItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> DisconnectionProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("sourceNodeId"), TEXT("sourcePinName"), TEXT("targetNodeId"), TEXT("targetPinName")})
        DisconnectionProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Plan node id and exact linked pin name.")));
    DisconnectionProperties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Confirm this exact link disconnection.")));
    DisconnectionItem->SetObjectField(TEXT("properties"), DisconnectionProperties);
    DisconnectionItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("sourceNodeId")), MakeShared<FJsonValueString>(TEXT("sourcePinName")), MakeShared<FJsonValueString>(TEXT("targetNodeId")), MakeShared<FJsonValueString>(TEXT("targetPinName"))});
    Disconnections->SetObjectField(TEXT("items"), DisconnectionItem); Properties->SetObjectField(TEXT("disconnections"), Disconnections);

    auto Splices = MakeShared<FJsonObject>();
    Splices->SetStringField(TEXT("type"), TEXT("array"));
    Splices->SetStringField(TEXT("description"), TEXT("Insert a node on an exact existing execution link. Requires confirm=true and preserves other incoming routes."));
    Splices->SetNumberField(TEXT("maxItems"), 64);
    auto SpliceItem = MakeShared<FJsonObject>();
    SpliceItem->SetStringField(TEXT("type"), TEXT("object"));
    auto SpliceProperties = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> SpliceRequired;
    for (const TCHAR* Field : {TEXT("sourceNodeId"), TEXT("sourcePinName"), TEXT("targetNodeId"), TEXT("targetPinName"),
        TEXT("insertNodeId"), TEXT("insertInputPinName"), TEXT("insertOutputPinName")})
    {
        SpliceProperties->SetObjectField(Field, BuildStringProperty(TEXT("Plan node id or exact execution pin name.")));
        SpliceRequired.Add(MakeShared<FJsonValueString>(Field));
    }
    SpliceProperties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Confirm removing the exact original execution link.")));
    SpliceRequired.Add(MakeShared<FJsonValueString>(TEXT("confirm")));
    SpliceItem->SetObjectField(TEXT("properties"), SpliceProperties);
    SpliceItem->SetArrayField(TEXT("required"), SpliceRequired);
    Splices->SetObjectField(TEXT("items"), SpliceItem);
    Properties->SetObjectField(TEXT("spliceExec"), Splices);
    Properties->SetObjectField(TEXT("detailLevel"), BuildStringProperty(TEXT("summary (default) returns identities and link results; full includes every node pin.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Run complete preflight without mutation.")));
    Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile the edited Blueprint before any save. Defaults true.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh only after successful compilation. Defaults false.")));
    auto Locals = MakeShared<FJsonObject>();
    auto LocalItem = MakeShared<FJsonObject>();
    auto LocalProperties = MakeShared<FJsonObject>();
    Locals->SetStringField(TEXT("type"), TEXT("array"));
    Locals->SetNumberField(TEXT("maxItems"), 64);
    Locals->SetStringField(TEXT("description"), TEXT("Declare function locals before creating getter/setter nodes. Existing declarations must match."));
    LocalItem->SetStringField(TEXT("type"), TEXT("object"));
    for (const TCHAR* Key : {TEXT("name"), TEXT("type"), TEXT("typeObjectPath"), TEXT("containerType"),
        TEXT("valueType"), TEXT("valueTypeObjectPath"), TEXT("defaultValue")})
        LocalProperties->SetObjectField(Key, BuildStringProperty(Key));
    LocalItem->SetObjectField(TEXT("properties"), LocalProperties);
    LocalItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("name")), MakeShared<FJsonValueString>(TEXT("type"))});
    Locals->SetObjectField(TEXT("items"), LocalItem);
    Properties->SetObjectField(TEXT("locals"), Locals);
    auto Remove = MakeShared<FJsonObject>();
    Remove->SetStringField(TEXT("type"), TEXT("array"));
    Remove->SetNumberField(TEXT("maxItems"), 64);
    Remove->SetObjectField(TEXT("items"), BuildStringProperty(TEXT("Exact GUID of a deletable node.")));
    Remove->SetStringField(TEXT("description"), TEXT("Remove these nodes before rewiring. Requires expectedGraphRevision. Use nodes: [] for removal-only patches."));
    Properties->SetObjectField(TEXT("removeNodeGuids"), Remove);
    auto Collapse = MakeShared<FJsonObject>();
    auto CollapseProperties = MakeShared<FJsonObject>();
    auto CollapseNodes = MakeShared<FJsonObject>();
    Collapse->SetStringField(TEXT("type"), TEXT("object"));
    CollapseNodes->SetStringField(TEXT("type"), TEXT("array"));
    CollapseNodes->SetNumberField(TEXT("maxItems"), 64);
    CollapseNodes->SetObjectField(TEXT("items"), BuildStringProperty(TEXT("ID of a new node from this patch.")));
    CollapseProperties->SetObjectField(TEXT("name"), BuildStringProperty(TEXT("Unique collapsed graph name containing letters, digits or underscores.")));
    CollapseProperties->SetObjectField(TEXT("nodeIds"), CollapseNodes);
    Collapse->SetObjectField(TEXT("properties"), CollapseProperties);
    Collapse->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("name")), MakeShared<FJsonValueString>(TEXT("nodeIds"))});
    Collapse->SetStringField(TEXT("description"), TEXT("Group selected new nodes behind generated boundary pins in the same transaction. Requires expectedGraphRevision."));
    Properties->SetObjectField(TEXT("collapse"), Collapse);
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("patchId")), MakeShared<FJsonValueString>(TEXT("nodes"))});
    return Schema;
}
