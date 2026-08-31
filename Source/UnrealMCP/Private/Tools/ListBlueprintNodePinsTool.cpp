#include "Tools/ListBlueprintNodePinsTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FListBlueprintNodePinsTool::FListBlueprintNodePinsTool()
    : FMCPToolBase(TEXT("ListBlueprintNodePins"), TEXT("Lists live pins for one Blueprint node by stable graph and node GUID, including IDs, directions, types, defaults, and exact linked endpoints.")) {}

UnrealMCP::FMCPResponse FListBlueprintNodePinsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString O, G, GG, N;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), O) || !Request.Params->TryGetStringField(TEXT("nodeGuid"), N))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ListBlueprintNodePins requires objectPath, nodeGuid, and graph selector."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), G);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GG);
    if (G.IsEmpty() && GG.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    TArray<TSharedPtr<FJsonValue>> Pins;
    FString Title, ExecErr;
    bool Ok = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& E)
    {
        UBlueprint* B = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(O, B, E))
        {
            return false;
        }

        UEdGraph* Graph = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(B, G, GG, Graph, E))
        {
            return false;
        }

        UEdGraphNode* Node = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Graph, N, Node, E))
        {
            return false;
        }

        Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin)
            {
                continue;
            }

            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("pinId"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Pin));
            Item->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
            Item->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            Item->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
            Item->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
            Item->SetStringField(TEXT("subCategoryObjectPath"), Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : FString());
            Item->SetStringField(TEXT("defaultValue"), UnrealMCP::BlueprintGraphEditToolUtils::GetEffectivePinDefaultValue(Pin));
            Item->SetStringField(TEXT("literalDefaultValue"), Pin->DefaultValue);
            Item->SetStringField(TEXT("defaultObjectPath"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinDefaultObjectPath(Pin));
            Item->SetStringField(TEXT("defaultTextValue"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinDefaultTextValue(Pin));
            Item->SetStringField(TEXT("defaultValueSource"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinDefaultValueSource(Pin));
            Item->SetStringField(TEXT("parentPinId"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Pin->ParentPin));
            Item->SetNumberField(TEXT("subPinCount"), Pin->SubPins.Num());

            TArray<TSharedPtr<FJsonValue>> Links;
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (!Linked || !Linked->GetOwningNode())
                {
                    continue;
                }

                auto Link = MakeShared<FJsonObject>();
                Link->SetStringField(TEXT("nodeGuid"), UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Linked->GetOwningNode()));
                Link->SetStringField(TEXT("pinId"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Linked));
                Link->SetStringField(TEXT("pinName"), Linked->PinName.ToString());
                Links.Add(MakeShared<FJsonValueObject>(Link));
            }

            Item->SetNumberField(TEXT("linkedPinCount"), Links.Num());
            Item->SetArrayField(TEXT("links"), Links);
            Pins.Add(MakeShared<FJsonValueObject>(Item));
        }

        return true;
    }, ExecErr);

    if (!Ok)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecErr);
    }

    UnrealMCP::FMCPResponse R;
    R.Id = Request.Id;
    auto J = BuildBooleanResult(true);
    J->SetStringField(TEXT("objectPath"), O);
    J->SetStringField(TEXT("nodeGuid"), N);
    J->SetStringField(TEXT("nodeTitle"), Title);
    J->SetNumberField(TEXT("pinCount"), Pins.Num());
    J->SetArrayField(TEXT("pins"), Pins);
    R.Result = J;
    return R;
}

TSharedPtr<FJsonObject> FListBlueprintNodePinsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    auto P = MakeShared<FJsonObject>();
    for (const TCHAR* N : { TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("nodeGuid") })
    {
        P->SetObjectField(N, BuildStringProperty(TEXT("Stable Blueprint graph/node selector.")));
    }

    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> Q{ MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("nodeGuid")) };
    S->SetArrayField(TEXT("required"), Q);
    return S;
}
