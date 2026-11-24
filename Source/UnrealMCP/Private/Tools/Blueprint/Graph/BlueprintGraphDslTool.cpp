#include "Tools/Blueprint/Graph/BlueprintGraphDslTool.h"

#include "Dom/JsonObject.h"
#include "Tools/Blueprint/Graph/ApplyBlueprintGraphPatchTool.h"
#include "Tools/Blueprint/Graph/LayoutBlueprintNodesTool.h"
#include "Tools/Blueprint/Inspection/InspectLiveBlueprintTool.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"

namespace
{
    const TCHAR* const Format = TEXT("unrealmcp.graph-patch/1");

    FString Read(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
    {
        FString Value;
        if (Object) Object->TryGetStringField(Key, Value);
        return Value;
    }
}

FBlueprintGraphDslTool::FBlueprintGraphDslTool(bool bInWrite)
    : FMCPToolBase(bInWrite ? TEXT("WriteBlueprintGraphDsl") : TEXT("ReadBlueprintGraphDsl"),
        bInWrite
            ? TEXT("Apply a revision-bound UnrealMCP graph patch document. Defaults to dry run; confirm to apply. Existing graph content is preserved unless explicitly edited.")
            : TEXT("Read a revision-bound UnrealMCP graph patch document and exact node, pin, and link evidence for one resident Blueprint graph.")),
      bWrite(bInWrite)
{
}

UnrealMCP::FMCPResponse FBlueprintGraphDslTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FMCPRequest Inner = Request;
    Inner.Params = MakeShared<FJsonObject>();
    if (Request.Params) Inner.Params->Values = Request.Params->Values;
    if (bWrite)
    {
        const TSharedPtr<FJsonObject>* Document = nullptr;
        const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), true);
        if (!Request.Params || !Request.Params->TryGetObjectField(TEXT("document"), Document)
            || Read(*Document, TEXT("format")) != Format
            || Read(*Document, TEXT("objectPath")).IsEmpty() || Read(*Document, TEXT("graphGuid")).IsEmpty()
            || Read(*Document, TEXT("expectedGraphRevision")).IsEmpty()
            || (!bDryRun && !BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false)))
        {
            return BuildError(Request, EMCPErrorCode::InvalidParams,
                TEXT("Provide a version 1 graph document with an exact graph GUID and expected revision. Applying requires confirm=true."));
        }

        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if ((*Document)->TryGetArrayField(TEXT("nodes"), Nodes))
        {
            for (const auto& Value : *Nodes)
            {
                const auto Node = Value->AsObject();
                if (Node && Node->HasField(TEXT("expression")))
                    return BuildError(Request, EMCPErrorCode::InvalidParams,
                        TEXT("Inline expressions are unsupported. Declare typed operators and calls as explicit nodes with exact connections."));
            }
        }
        Inner.Params->Values = (*Document)->Values;
        Inner.Params->SetStringField(TEXT("operationId"), Read(Request.Params, TEXT("operationId")));
        Inner.Params->SetBoolField(TEXT("dryRun"), bDryRun);
        Inner.Params->SetBoolField(TEXT("saveAfterEdit"), false);
        FMCPResponse Response = FApplyBlueprintGraphPatchTool().Execute(Inner);
        if (Response.Error.IsSet() || !Response.Result
            || !BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("arrangeAfterWrite"), false))
            return Response;
        if (bDryRun)
        {
            Response.Result->SetStringField(TEXT("layoutStatus"), TEXT("deferred_until_apply"));
            return Response;
        }
        FMCPRequest LayoutRequest = Request;
        LayoutRequest.Params = MakeShared<FJsonObject>();
        for (const TCHAR* Key : {TEXT("objectPath"), TEXT("graphGuid")})
            LayoutRequest.Params->SetStringField(Key, Read(Inner.Params, Key));
        LayoutRequest.Params->SetStringField(TEXT("expectedGraphRevision"), Read(Response.Result, TEXT("finalGraphRevision")));
        const FMCPResponse Layout = FLayoutBlueprintNodesTool().Execute(LayoutRequest);
        if (Layout.Error.IsSet() || !Layout.Result)
        {
            Response.Result->SetStringField(TEXT("layoutStatus"), TEXT("failed_after_write"));
            if (Layout.Error.IsSet()) Response.Result->SetStringField(TEXT("layoutError"), Layout.Error->Message);
            return Response;
        }
        Response.Result->SetStringField(TEXT("layoutStatus"), TEXT("completed"));
        Response.Result->SetObjectField(TEXT("layout"), Layout.Result);
        Response.Result->SetStringField(TEXT("finalGraphRevision"), Read(Layout.Result, TEXT("finalGraphRevision")));
        if (Layout.Result->GetBoolField(TEXT("changed"))) Response.Result->SetBoolField(TEXT("changed"), true);
        return Response;
    }

    if (Read(Request.Params, TEXT("objectPath")).IsEmpty() || Read(Request.Params, TEXT("graphPath")).IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide the resident Blueprint objectPath and exact graphPath."));
    Inner.Params->SetStringField(TEXT("mode"), TEXT("nodes"));
    Inner.Params->SetStringField(TEXT("outputMode"), TEXT("full"));
    FMCPResponse Response = FInspectLiveBlueprintTool().Execute(Inner);
    if (Response.Error.IsSet() || !Response.Result) return Response;

    const TSharedPtr<FJsonObject>* Graph = nullptr;
    if (!Response.Result->TryGetObjectField(TEXT("selectedGraph"), Graph))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Expected one exact graph."));

    const auto& Evidence = Response.Result->GetArrayField(TEXT("nodes"));
    TArray<TSharedPtr<FJsonValue>> Nodes;
    TArray<TSharedPtr<FJsonValue>> Connections;
    TSet<FString> NodeIds;
    for (const auto& Entry : Evidence)
    {
        const FString Guid = Read(Entry->AsObject(), TEXT("nodeGuid"));
        NodeIds.Add(Guid);
        auto Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("id"), Guid);
        Node->SetStringField(TEXT("nodeGuid"), Guid);
        Node->SetStringField(TEXT("kind"), TEXT("existingNode"));
        Nodes.Add(MakeShared<FJsonValueObject>(Node));
    }
    for (const auto& Entry : Evidence)
    {
        const auto& Node = Entry->AsObject();
        const FString From = Read(Node, TEXT("nodeGuid"));
        for (const auto& PinEntry : Node->GetArrayField(TEXT("pins")))
        {
            const auto& Pin = PinEntry->AsObject();
            if (Read(Pin, TEXT("direction")) != TEXT("output")) continue;
            for (const auto& Linked : Pin->GetArrayField(TEXT("links")))
            {
                const auto& Endpoint = Linked->AsObject();
                const FString To = Read(Endpoint, TEXT("nodeGuid"));
                if (!NodeIds.Contains(To)) continue;
                auto Link = MakeShared<FJsonObject>();
                Link->SetStringField(TEXT("sourceNodeId"), From);
                Link->SetStringField(TEXT("sourcePinName"), Read(Pin, TEXT("pinName")));
                Link->SetStringField(TEXT("targetNodeId"), To);
                Link->SetStringField(TEXT("targetPinName"), Read(Endpoint, TEXT("pinName")));
                Connections.Add(MakeShared<FJsonValueObject>(Link));
            }
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Selection = nullptr;
    const bool bPortable = Request.Params->HasField(TEXT("portableNodeGuids"));
    if (bPortable)
    {
        if (!Request.Params->TryGetArrayField(TEXT("portableNodeGuids"), Selection) || Selection->IsEmpty())
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("portableNodeGuids requires a nonempty array of exact node GUIDs."));
        TSet<FString> Selected;
        for (const auto& Id : *Selection)
        {
            FString Guid;
            if (!Id->TryGetString(Guid) || !NodeIds.Contains(Guid) || Selected.Contains(Guid))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Each portable GUID must be unique and present on the current read page."));
            Selected.Add(Guid);
        }
        Nodes.Reset();
        Connections.Reset();
        TArray<TSharedPtr<FJsonValue>> Boundary, Locals;
        TSet<FString> ExportedLocals;
        for (const auto& Entry : Evidence)
        {
            const auto& Node = Entry->AsObject();
            const FString Guid = Read(Node, TEXT("nodeGuid"));
            if (!Selected.Contains(Guid)) continue;
            const FString Class = Read(Node, TEXT("nodeClassPath"));
            FString Kind;
            if (Class == TEXT("/Script/BlueprintGraph.K2Node_IfThenElse")) Kind = TEXT("branch");
            else if (Class == TEXT("/Script/BlueprintGraph.K2Node_Knot")) Kind = TEXT("reroute");
            else if (Class == TEXT("/Script/BlueprintGraph.K2Node_Self")) Kind = TEXT("self");
            else if (Class == TEXT("/Script/BlueprintGraph.K2Node_VariableGet")) Kind = TEXT("variableGet");
            else if (Class == TEXT("/Script/BlueprintGraph.K2Node_VariableSet")) Kind = TEXT("variableSet");
            else if (Class == TEXT("/Script/BlueprintGraph.K2Node_CallFunction")) Kind = TEXT("functionCall");
            else if (Class == TEXT("/Script/BlueprintGraph.K2Node_CallParentFunction")) Kind = TEXT("parentCall");
            else if (Class == TEXT("/Script/BlueprintGraph.K2Node_Message")) Kind = TEXT("interfaceMessage");
            else if (Class == TEXT("/Script/BlueprintGraph.K2Node_MacroInstance")) Kind = TEXT("macro");
            else if (Class == TEXT("/Script/BlueprintGraph.K2Node_MakeArray")) Kind = TEXT("makeArray");
            else
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Unsupported portable node: ") + Class);
            auto Spec = MakeShared<FJsonObject>();
            Spec->SetStringField(TEXT("id"), Guid);
            Spec->SetStringField(TEXT("kind"), Kind);
            Spec->SetStringField(TEXT("comment"), Read(Node, TEXT("comment")));
            const FString Member = Read(Node, TEXT("memberName"));
            const FString Owner = Read(Node, TEXT("memberOwnerClass"));
            if (Kind == TEXT("variableGet") || Kind == TEXT("variableSet"))
            {
                Spec->SetStringField(TEXT("variableName"), Member);
                const FString LocalGuid = Read(Node, TEXT("localDeclarationGuid"));
                if (!LocalGuid.IsEmpty() && !ExportedLocals.Contains(LocalGuid))
                {
                    const TArray<TSharedPtr<FJsonValue>>* Declarations = nullptr;
                    if (!(*Graph)->TryGetArrayField(TEXT("localVariables"), Declarations))
                        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Could not resolve source local declaration."));
                    const auto* Declaration = Declarations->FindByPredicate([&](const auto& V)
                    { return Read(V->AsObject(), TEXT("guid")) == LocalGuid; });
                    if (!Declaration)
                        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Could not resolve source local declaration."));
                    const auto& SourceLocal = (*Declaration)->AsObject();
                    const auto& Type = SourceLocal->GetObjectField(TEXT("type"));
                    auto Local = MakeShared<FJsonObject>();
                    auto SetType = [&](const TCHAR* TypeKey, const TCHAR* ObjectName, const TCHAR* Cat, const TCHAR* Sub, const TCHAR* Obj)
                    {
                        const FString Category = Read(Type, Cat);
                        Local->SetStringField(TypeKey, Category == TEXT("real") ? TEXT("float") : Category);
                        Local->SetStringField(ObjectName, Category == TEXT("real") ? Read(Type, Sub) : Read(Type, Obj));
                    };
                    SetType(TEXT("type"), TEXT("typeObjectPath"), TEXT("category"), TEXT("subCategory"), TEXT("subCategoryObjectPath"));
                    SetType(TEXT("valueType"), TEXT("valueTypeObjectPath"), TEXT("valueCategory"), TEXT("valueSubCategory"), TEXT("valueSubCategoryObjectPath"));
                    Local->SetStringField(TEXT("name"), Member);
                    Local->SetStringField(TEXT("containerType"), Read(Type, TEXT("containerType")));
                    Local->SetStringField(TEXT("defaultValue"), Read(SourceLocal, TEXT("defaultValue")));
                    Locals.Add(MakeShared<FJsonValueObject>(Local));
                    ExportedLocals.Add(LocalGuid);
                }
                else if (LocalGuid.IsEmpty())
                {
                    if (Owner.IsEmpty()) return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Could not resolve source member owner."));
                    Spec->SetStringField(TEXT("ownerClassPath"), Owner);
                }
            }
            if (Kind == TEXT("functionCall") || Kind == TEXT("parentCall") || Kind == TEXT("interfaceMessage"))
            {
                if (Member.IsEmpty() || Owner.IsEmpty())
                    return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Could not resolve source function."));
                Spec->SetStringField(TEXT("functionName"), Member);
                Spec->SetStringField(TEXT("ownerClassPath"), Owner);
            }
            if (Kind == TEXT("macro"))
            {
                const FString MacroPath = Read(Node, TEXT("macroGraphPath"));
                if (MacroPath.IsEmpty()) return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Could not resolve source macro."));
                Spec->SetStringField(TEXT("macroPath"), MacroPath);
            }
            int32 ArrayInputs = 0;
            TArray<TSharedPtr<FJsonValue>> Defaults;
            for (const auto& PinValue : Node->GetArrayField(TEXT("pins")))
            {
                const auto& Pin = PinValue->AsObject();
                const FString PinName = Read(Pin, TEXT("pinName"));
                const bool bOutput = Read(Pin, TEXT("direction")) == TEXT("output");
                if (Kind == TEXT("makeArray") && !bOutput && PinName.StartsWith(TEXT("["))) ++ArrayInputs;
                if (!bOutput && Pin->GetArrayField(TEXT("links")).IsEmpty()
                    && PinName != TEXT("self") && PinName != TEXT("execute"))
                {
                    auto Default = MakeShared<FJsonObject>();
                    Default->SetStringField(TEXT("pinName"), PinName);
                    Default->SetStringField(TEXT("defaultValue"), Read(Pin, TEXT("defaultValue")));
                    const FString ObjectPath = Read(Pin, TEXT("defaultObjectPath"));
                    if (!ObjectPath.IsEmpty())
                    {
                        Default->RemoveField(TEXT("defaultValue"));
                        Default->SetStringField(TEXT("defaultObjectPath"), ObjectPath);
                    }
                    if (!ObjectPath.IsEmpty() || !Read(Pin, TEXT("defaultValue")).IsEmpty())
                        Defaults.Add(MakeShared<FJsonValueObject>(Default));
                }
                for (const auto& Linked : Pin->GetArrayField(TEXT("links")))
                {
                    const auto& Endpoint = Linked->AsObject();
                    const FString Other = Read(Endpoint, TEXT("nodeGuid"));
                    auto Link = MakeShared<FJsonObject>();
                    Link->SetStringField(TEXT("sourceNodeId"), bOutput ? Guid : Other);
                    Link->SetStringField(TEXT("sourcePinName"), bOutput ? PinName : Read(Endpoint, TEXT("pinName")));
                    Link->SetStringField(TEXT("targetNodeId"), bOutput ? Other : Guid);
                    Link->SetStringField(TEXT("targetPinName"), bOutput ? Read(Endpoint, TEXT("pinName")) : PinName);
                    if (!Selected.Contains(Other)) Boundary.Add(MakeShared<FJsonValueObject>(Link));
                    else if (bOutput) Connections.Add(MakeShared<FJsonValueObject>(Link));
                }
            }
            if (Kind == TEXT("makeArray")) Spec->SetNumberField(TEXT("arrayInputCount"), ArrayInputs);
            if (!Defaults.IsEmpty()) Spec->SetArrayField(TEXT("inputDefaults"), Defaults);
            Nodes.Add(MakeShared<FJsonValueObject>(Spec));
        }
        auto Fragment = MakeShared<FJsonObject>();
        Fragment->SetStringField(TEXT("format"), Format);
        Fragment->SetArrayField(TEXT("nodes"), Nodes);
        Fragment->SetArrayField(TEXT("connections"), Connections);
        if (!Locals.IsEmpty()) Fragment->SetArrayField(TEXT("locals"), Locals);
        Response.Result->SetObjectField(TEXT("fragment"), Fragment);
        Response.Result->SetArrayField(TEXT("boundaryLinks"), Boundary);
        return Response;
    }

    auto Document = MakeShared<FJsonObject>();
    const FString GraphGuid = Read(*Graph, TEXT("graphGuid"));
    Document->SetStringField(TEXT("format"), Format);
    Document->SetStringField(TEXT("objectPath"), Read(Response.Result, TEXT("objectPath")));
    Document->SetStringField(TEXT("graphGuid"), GraphGuid);
    Document->SetStringField(TEXT("expectedGraphRevision"), Read(*Graph, TEXT("revision")));
    Document->SetStringField(TEXT("patchId"), TEXT("GraphDsl_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Document->SetArrayField(TEXT("nodes"), Nodes);
    Document->SetArrayField(TEXT("connections"), Connections);
    Response.Result->SetObjectField(TEXT("document"), Document);
    return Response;
}

TSharedPtr<FJsonObject> FBlueprintGraphDslTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    if (!bWrite)
    {
        auto Schema = FInspectLiveBlueprintTool().GetDefinition().InputSchema;
        const auto& Properties = Schema->GetObjectField(TEXT("properties"));
        for (const TCHAR* Key : {TEXT("mode"), TEXT("outputMode"), TEXT("mapPath"), TEXT("level")})
            Properties->RemoveField(Key);
        auto Portable = MakeShared<FJsonObject>();
        Portable->SetStringField(TEXT("type"), TEXT("array"));
        Portable->SetObjectField(TEXT("items"), BuildStringProperty(TEXT("Exact selected node GUID on this page.")));
        Portable->SetStringField(TEXT("description"), TEXT("Export selected supported nodes, their local declarations, and explicit boundary links."));
        Properties->SetObjectField(TEXT("portableNodeGuids"), Portable);
        Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("graphPath"))});
        return Schema;
    }

    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    auto Document = FApplyBlueprintGraphPatchTool().GetDefinition().InputSchema;
    const auto& DocumentProperties = Document->GetObjectField(TEXT("properties"));
    DocumentProperties->SetObjectField(TEXT("format"), BuildStringProperty(TEXT("unrealmcp.graph-patch/1")));
    for (const TCHAR* Key : {TEXT("operationId"), TEXT("dryRun"), TEXT("saveAfterEdit"), TEXT("graphName")})
        DocumentProperties->RemoveField(Key);
    TArray<TSharedPtr<FJsonValue>> Required;
    for (const TCHAR* Key : {TEXT("format"), TEXT("objectPath"), TEXT("graphGuid"), TEXT("expectedGraphRevision"), TEXT("patchId"), TEXT("nodes")})
        Required.Add(MakeShared<FJsonValueString>(Key));
    Document->SetArrayField(TEXT("required"), Required);

    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Properties->SetObjectField(TEXT("document"), Document);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preview only. Defaults to true.")));
    Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required when applying.")));
    Properties->SetObjectField(TEXT("arrangeAfterWrite"), BuildBoolProperty(
        TEXT("Arrange the entire graph after a successful apply. This is a separate undo step and is deferred in dry runs.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("document"))});
    return Schema;
}
