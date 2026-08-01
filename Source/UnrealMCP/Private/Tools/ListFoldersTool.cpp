#include "Tools/ListFoldersTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Tools/AssetRegistryToolUtils.h"

FListFoldersTool::FListFoldersTool()
    : FMCPToolBase(TEXT("ListFolders"), TEXT("Lists known Asset Registry package folders under a base path."))
{
}

UnrealMCP::FMCPResponse FListFoldersTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString BasePath = UnrealMCP::AssetRegistryToolUtils::GetOptionalString(Request.Params, TEXT("path"), TEXT("/Game"));
    const bool bRecursive = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("recursive"), false);

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    TArray<FName> FolderPaths;
    AssetRegistryModule.Get().GetSubPaths(FName(*BasePath), FolderPaths, bRecursive);
    UnrealMCP::AssetRegistryToolUtils::SortNames(FolderPaths);

    TArray<TSharedPtr<FJsonValue>> Folders;
    Folders.Reserve(FolderPaths.Num());
    for (const FName& FolderPath : FolderPaths)
    {
        Folders.Add(MakeShared<FJsonValueString>(FolderPath.ToString()));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("path"), BasePath);
    Result->SetBoolField(TEXT("recursive"), bRecursive);
    Result->SetNumberField(TEXT("count"), FolderPaths.Num());
    Result->SetArrayField(TEXT("folders"), Folders);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FListFoldersTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> PathProperty = MakeShared<FJsonObject>();
    PathProperty->SetStringField(TEXT("type"), TEXT("string"));
    PathProperty->SetStringField(TEXT("description"), TEXT("Base package path, for example /Game or /Game/MyFolder. Defaults to /Game."));
    Properties->SetObjectField(TEXT("path"), PathProperty);

    TSharedRef<FJsonObject> RecursiveProperty = MakeShared<FJsonObject>();
    RecursiveProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    RecursiveProperty->SetStringField(TEXT("description"), TEXT("Whether to recurse into nested subpaths. Defaults to false."));
    Properties->SetObjectField(TEXT("recursive"), RecursiveProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
