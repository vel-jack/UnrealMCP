#include "Tools/SetBlueprintFunctionMetadataTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    UK2Node_FunctionEntry* FindFunctionEntry(UEdGraph* Graph)
    {
        if (Graph == nullptr)
        {
            return nullptr;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                return Entry;
            }
        }

        return nullptr;
    }
}

FSetBlueprintFunctionMetadataTool::FSetBlueprintFunctionMetadataTool()
    : FMCPToolBase(
        TEXT("SetBlueprintFunctionMetadata"),
        TEXT("Idempotently sets category, description, pure, and const metadata on a Blueprint function graph."))
{
}

UnrealMCP::FMCPResponse FSetBlueprintFunctionMetadataTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString GraphGuid;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid)
        || ObjectPath.IsEmpty()
        || GraphGuid.IsEmpty())
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("SetBlueprintFunctionMetadata requires objectPath and graphGuid."));
    }

    const bool bHasCategory = Request.Params->HasField(TEXT("category"));
    const bool bHasDescription = Request.Params->HasField(TEXT("description"));
    const bool bHasPure = Request.Params->HasField(TEXT("pure"));
    const bool bHasConst = Request.Params->HasField(TEXT("const"));
    if (!bHasCategory && !bHasDescription && !bHasPure && !bHasConst)
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("Provide at least one of category, description, pure, or const."));
    }

    FString Category;
    FString FunctionDescription;
    bool bPure = false;
    bool bConst = false;
    if ((bHasCategory && !Request.Params->TryGetStringField(TEXT("category"), Category))
        || (bHasDescription && !Request.Params->TryGetStringField(TEXT("description"), FunctionDescription))
        || (bHasPure && !Request.Params->TryGetBoolField(TEXT("pure"), bPure))
        || (bHasConst && !Request.Params->TryGetBoolField(TEXT("const"), bConst)))
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("category and description must be strings; pure and const must be booleans."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    FString FunctionName;
    FString EntryNodeGuid;
    FString EffectiveCategory;
    FString EffectiveDescription;
    FString SavedFilename;
    FString IndexRefreshError;
    FString ExecutionError;
    bool bEffectivePure = false;
    bool bEffectiveConst = false;
    bool bChanged = false;
    bool bIndexRefreshed = false;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            UBlueprint* Blueprint = nullptr;
            if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
            {
                return false;
            }

            UEdGraph* Graph = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, FString(), GraphGuid, Graph, OutError))
            {
                return false;
            }

            if (!Blueprint->FunctionGraphs.Contains(Graph))
            {
                OutError = TEXT("graphGuid does not identify a Blueprint function graph.");
                return false;
            }

            UK2Node_FunctionEntry* EntryNode = FindFunctionEntry(Graph);
            if (EntryNode == nullptr)
            {
                OutError = TEXT("The function graph has no function entry node.");
                return false;
            }

            FunctionName = Graph->GetName();
            EntryNodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(EntryNode);
            EffectiveCategory = bHasCategory ? Category : EntryNode->MetaData.Category.ToString();
            EffectiveDescription = bHasDescription ? FunctionDescription : EntryNode->MetaData.ToolTip.ToString();

            int32 EffectiveFlags = EntryNode->GetExtraFlags();
            if (bHasPure)
            {
                EffectiveFlags = bPure ? (EffectiveFlags | FUNC_BlueprintPure) : (EffectiveFlags & ~FUNC_BlueprintPure);
            }
            if (bHasConst)
            {
                EffectiveFlags = bConst ? (EffectiveFlags | FUNC_Const) : (EffectiveFlags & ~FUNC_Const);
            }

            bEffectivePure = (EffectiveFlags & FUNC_BlueprintPure) != 0;
            bEffectiveConst = (EffectiveFlags & FUNC_Const) != 0;
            bChanged = (bHasCategory && !EntryNode->MetaData.Category.EqualTo(FText::FromString(Category)))
                || (bHasDescription && !EntryNode->MetaData.ToolTip.EqualTo(FText::FromString(FunctionDescription)))
                || EffectiveFlags != EntryNode->GetExtraFlags();

            if (bDryRun || !bChanged)
            {
                return true;
            }

            const FScopedTransaction Transaction(NSLOCTEXT(
                "UnrealMCP",
                "SetBlueprintFunctionMetadata",
                "UnrealMCP Set Blueprint Function Metadata"));
            Blueprint->Modify();
            Graph->Modify();
            EntryNode->Modify();

            if (bHasCategory)
            {
                EntryNode->MetaData.Category = FText::FromString(Category);
            }
            if (bHasDescription)
            {
                EntryNode->MetaData.ToolTip = FText::FromString(FunctionDescription);
            }
            EntryNode->SetExtraFlags(EffectiveFlags);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

            return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
                Blueprint,
                ObjectPath,
                bSave,
                SavedFilename,
                bIndexRefreshed,
                IndexRefreshError,
                OutError);
        },
        ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetStringField(TEXT("graphGuid"), GraphGuid);
    Result->SetStringField(TEXT("entryNodeGuid"), EntryNodeGuid);
    Result->SetStringField(TEXT("category"), EffectiveCategory);
    Result->SetStringField(TEXT("description"), EffectiveDescription);
    Result->SetBoolField(TEXT("pure"), bEffectivePure);
    Result->SetBoolField(TEXT("const"), bEffectiveConst);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && bChanged);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSetBlueprintFunctionMetadataTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Stable GUID of the target function graph.")));
    Properties->SetObjectField(TEXT("category"), BuildStringProperty(TEXT("Optional function category; an empty string clears it.")));
    Properties->SetObjectField(TEXT("description"), BuildStringProperty(TEXT("Optional function description/tooltip; an empty string clears it.")));
    Properties->SetObjectField(TEXT("pure"), BuildBoolProperty(TEXT("Optional Blueprint pure flag.")));
    Properties->SetObjectField(TEXT("const"), BuildBoolProperty(TEXT("Optional const function flag.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Report the effective metadata without mutation.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index after mutation.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(
        TEXT("required"),
        {
            MakeShared<FJsonValueString>(TEXT("objectPath")),
            MakeShared<FJsonValueString>(TEXT("graphGuid"))
        });
    return Schema;
}
