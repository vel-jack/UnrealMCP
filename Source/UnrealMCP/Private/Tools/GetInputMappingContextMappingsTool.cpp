#include "Tools/GetInputMappingContextMappingsTool.h"

#include "Dom/JsonObject.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "Tools/InputAssetToolUtils.h"

FGetInputMappingContextMappingsTool::FGetInputMappingContextMappingsTool()
    : FMCPToolBase(
        TEXT("GetInputMappingContextMappings"),
        TEXT("Reads back every key-mapping row (action, key, triggers, modifiers) in an InputMappingContext without loading it for editing."))
{
}

UnrealMCP::FMCPResponse FGetInputMappingContextMappingsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString ObjectPath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("GetInputMappingContextMappings requires objectPath."));
    }

    TArray<TSharedPtr<FJsonValue>> Rows;
    FString ExecutionError;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UClass* ContextClass = nullptr;
        if (!InputAssetToolUtils::ResolveInputMappingContextClass(ContextClass, OutError))
        {
            return false;
        }

        UObject* ContextObject = nullptr;
        if (!InputAssetToolUtils::ResolveExistingObjectOfClass(ObjectPath, ContextClass, ContextObject, OutError))
        {
            return false;
        }

        FArrayProperty* MappingsProperty = InputAssetToolUtils::FindMappingsProperty(ContextClass, OutError);
        if (MappingsProperty == nullptr)
        {
            return false;
        }
        UScriptStruct* MappingStruct = InputAssetToolUtils::GetMappingElementStruct(MappingsProperty);
        if (MappingStruct == nullptr)
        {
            OutError = TEXT("Could not resolve the FEnhancedActionKeyMapping element struct.");
            return false;
        }

        FScriptArrayHelper Helper(MappingsProperty, MappingsProperty->ContainerPtrToValuePtr<void>(ContextObject));
        for (int32 Idx = 0; Idx < Helper.Num(); ++Idx)
        {
            Rows.Add(MakeShared<FJsonValueObject>(
                InputAssetToolUtils::SerializeMappingRow(Helper.GetRawPtr(Idx), MappingStruct, Idx)));
        }
        return true;
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);
    }

    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetNumberField(TEXT("count"), Rows.Num());
    Result->SetArrayField(TEXT("mappings"), Rows);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetInputMappingContextMappingsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target InputMappingContext asset object path.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{MakeShared<FJsonValueString>(TEXT("objectPath"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
