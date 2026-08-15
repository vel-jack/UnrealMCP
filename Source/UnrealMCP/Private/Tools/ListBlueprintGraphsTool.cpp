#include "Tools/ListBlueprintGraphsTool.h"
#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

FListBlueprintGraphsTool::FListBlueprintGraphsTool()
    : FMCPToolBase(TEXT("ListBlueprintGraphs"), TEXT("Lists indexed graphs for one Blueprint with stable graph identity, GUID, type, entry node, and node count.")) {}

UnrealMCP::FMCPResponse FListBlueprintGraphsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;if(!Request.Params.IsValid()||!Request.Params->TryGetStringField(TEXT("objectPath"),ObjectPath)||ObjectPath.IsEmpty())return BuildError(Request,UnrealMCP::EMCPErrorCode::InvalidParams,TEXT("ListBlueprintGraphs requires objectPath."));
    TArray<TSharedPtr<FJsonValue>>Graphs;FString Error;const bool bSucceeded=UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex([&](FSQLiteDatabase&Database,FString&OutError)
    {
        FSQLitePreparedStatement Statement(Database,TEXT("SELECT graph_name, graph_guid, graph_type, entry_node_guid, node_count FROM blueprint_graphs WHERE blueprint_object_path = ?1 ORDER BY graph_type, graph_name;"),ESQLitePreparedStatementFlags::None);
        if(!Statement.IsValid()||!Statement.SetBindingValueByIndex(1,ObjectPath)){OutError=TEXT("ListBlueprintGraphs could not prepare its query.");return false;}
        const int64 Result=Statement.Execute([&](const FSQLitePreparedStatement&Row){FString Name,Guid,Type,Entry;int32 Nodes=0;if(!Row.GetColumnValueByIndex(0,Name)||!Row.GetColumnValueByIndex(1,Guid)||!Row.GetColumnValueByIndex(2,Type)||!Row.GetColumnValueByIndex(3,Entry)||!Row.GetColumnValueByIndex(4,Nodes))return ESQLitePreparedStatementExecuteRowResult::Error;TSharedRef<FJsonObject>Item=MakeShared<FJsonObject>();Item->SetStringField(TEXT("graphName"),Name);Item->SetStringField(TEXT("graphGuid"),Guid);Item->SetStringField(TEXT("graphType"),Type);Item->SetStringField(TEXT("entryNodeGuid"),Entry);Item->SetNumberField(TEXT("nodeCount"),Nodes);Graphs.Add(MakeShared<FJsonValueObject>(Item));return ESQLitePreparedStatementExecuteRowResult::Continue;});
        if(Result==INDEX_NONE){OutError=Database.GetLastError().IsEmpty()?TEXT("ListBlueprintGraphs query failed."):Database.GetLastError();return false;}return true;
    },Error);
    if(!bSucceeded)return BuildError(Request,UnrealMCP::EMCPErrorCode::InternalError,Error);
    UnrealMCP::FMCPResponse Response;Response.Id=Request.Id;TSharedRef<FJsonObject>Result=BuildBooleanResult(true);Result->SetStringField(TEXT("objectPath"),ObjectPath);Result->SetNumberField(TEXT("count"),Graphs.Num());Result->SetArrayField(TEXT("graphs"),Graphs);Response.Result=Result;return Response;
}

TSharedPtr<FJsonObject> FListBlueprintGraphsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject>S=MakeShared<FJsonObject>();S->SetStringField(TEXT("type"),TEXT("object"));TSharedRef<FJsonObject>P=MakeShared<FJsonObject>();P->SetObjectField(TEXT("objectPath"),UnrealMCP::BlueprintEditToolUtils::BuildStringProperty(TEXT("Indexed Blueprint object path.")));S->SetObjectField(TEXT("properties"),P);TArray<TSharedPtr<FJsonValue>>R{MakeShared<FJsonValueString>(TEXT("objectPath"))};S->SetArrayField(TEXT("required"),R);return S;
}
