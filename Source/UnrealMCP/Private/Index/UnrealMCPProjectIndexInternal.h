#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Templates/Function.h"

struct FIndexedBlueprintGraphRow
{
    FString GraphName;
    FString GraphGuid;
    FString GraphType;
    FString EntryNodeGuid;
    int32 NodeCount = 0;
};

struct FIndexedBlueprintNodeRow
{
    FString GraphName;
    FString NodeGuid;
    FString NodeName;
    FString NodeClassPath;
    FString NodeTitle;
    FString NodeType;
    FString MemberName;
    FString MemberParentPath;
    int32 NodePosX = 0;
    int32 NodePosY = 0;
    bool bIsPure = false;
    int32 InputPinCount = 0;
    int32 OutputPinCount = 0;
};

struct FIndexedBlueprintPinRow
{
    FString GraphName;
    FString NodeGuid;
    FString PinId;
    FString PinName;
    FString Direction;
    FString Category;
    FString Subcategory;
    FString SubcategoryObjectPath;
    FString ContainerType;
    bool bIsReference = false;
    bool bIsConst = false;
    int32 LinkedPinCount = 0;
    FString DefaultValue;
};

struct FIndexedBlueprintEdgeRow
{
    FString SourceGraphName;
    FString SourceNodeGuid;
    FString SourcePinId;
    FString TargetGraphName;
    FString TargetNodeGuid;
    FString TargetPinId;
    FString EdgeKind;
};

inline bool ExecuteStatement(FSQLiteDatabase& Database, const TCHAR* Sql, FString* OutError = nullptr)
{
    if (Database.Execute(Sql))
    {
        return true;
    }

    if (OutError)
    {
        *OutError = Database.GetLastError();
    }
    return false;
}

inline bool ExecuteBoundStatement(FSQLiteDatabase& Database, const TCHAR* Sql, TFunctionRef<bool(FSQLitePreparedStatement&)> Binder, FString* OutError = nullptr)
{
    FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
    if (!Statement.IsValid())
    {
        if (OutError)
        {
            *OutError = Database.GetLastError();
        }
        return false;
    }

    if (!Binder(Statement))
    {
        if (OutError)
        {
            *OutError = TEXT("Failed to bind SQLite statement values.");
        }
        return false;
    }

    if (Statement.Execute())
    {
        return true;
    }

    if (OutError)
    {
        *OutError = Database.GetLastError();
    }
    return false;
}

inline int64 QuerySingleInt64(FSQLiteDatabase& Database, const TCHAR* Sql, FString* OutError = nullptr)
{
    int64 Value = 0;
    bool bHasRow = false;

    FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
    if (!Statement.IsValid())
    {
        if (OutError)
        {
            *OutError = Database.GetLastError();
        }
        return INDEX_NONE;
    }

    const int64 RowCount = Statement.Execute([&Value, &bHasRow](const FSQLitePreparedStatement& Row)
    {
        bHasRow = Row.GetColumnValueByIndex(0, Value);
        return bHasRow ? ESQLitePreparedStatementExecuteRowResult::Continue : ESQLitePreparedStatementExecuteRowResult::Error;
    });

    if (RowCount == INDEX_NONE || !bHasRow)
    {
        if (OutError)
        {
            *OutError = Database.GetLastError();
        }
        return INDEX_NONE;
    }

    return Value;
}

bool ExtractBlueprintGraphRows(
    UBlueprint* Blueprint,
    TArray<FIndexedBlueprintGraphRow>& OutGraphs,
    TArray<FIndexedBlueprintNodeRow>& OutNodes,
    TArray<FIndexedBlueprintPinRow>& OutPins,
    TArray<FIndexedBlueprintEdgeRow>& OutEdges);
