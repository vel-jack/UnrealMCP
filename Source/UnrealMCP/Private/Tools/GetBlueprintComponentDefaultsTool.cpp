#include "Tools/GetBlueprintComponentDefaultsTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Tools/BlueprintComponentEditUtils.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FGetBlueprintComponentDefaultsTool::FGetBlueprintComponentDefaultsTool()
    : FMCPToolBase(TEXT("GetBlueprintComponentDefaults"), TEXT("Reads safely editable defaults from one existing Blueprint component template without mutating or compiling the asset."))
{
}

UnrealMCP::FMCPResponse FGetBlueprintComponentDefaultsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, ComponentName, Error;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("componentName"), ComponentName))
    { return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("GetBlueprintComponentDefaults requires objectPath and componentName.")); }

    TArray<FString> PropertyNames;
    const TArray<TSharedPtr<FJsonValue>>* Requested = nullptr;
    if (Request.Params->TryGetArrayField(TEXT("propertyNames"), Requested))
        for (const TSharedPtr<FJsonValue>& Value : *Requested) if (Value.IsValid() && Value->Type == EJson::String) PropertyNames.Add(Value->AsString());
    double RequestedLimit = 200;
    Request.Params->TryGetNumberField(TEXT("maxProperties"), RequestedLimit);
    const int32 MaxProperties = FMath::Clamp(FMath::RoundToInt(RequestedLimit), 1, 500);
    FString ComponentClassPath;
    TArray<BlueprintComponentEditUtils::FPropertyValue> Values;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        USCS_Node* Node = BlueprintComponentEditUtils::FindComponentNode(Blueprint, ComponentName);
        if (Node == nullptr || Node->ComponentTemplate == nullptr) { OutError = TEXT("The requested Blueprint component template was not found."); return false; }
        ComponentName = Node->GetVariableName().ToString();
        ComponentClassPath = Node->ComponentTemplate->GetClass()->GetPathName();
        return BlueprintComponentEditUtils::GetEditablePropertyValues(Node->ComponentTemplate, PropertyNames, MaxProperties, Values, OutError);
    }, Error);
    if (!bSucceeded) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);

    TArray<TSharedPtr<FJsonValue>> Properties;
    for (const BlueprintComponentEditUtils::FPropertyValue& Value : Values)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Value.Name); Item->SetStringField(TEXT("type"), Value.Type); Item->SetStringField(TEXT("value"), Value.Value);
        Properties.Add(MakeShared<FJsonValueObject>(Item));
    }
    FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("componentName"), ComponentName);
    Result->SetStringField(TEXT("componentClassPath"), ComponentClassPath); Result->SetNumberField(TEXT("propertyCount"), Properties.Num());
    Result->SetArrayField(TEXT("properties"), Properties); Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FGetBlueprintComponentDefaultsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("componentName"), BuildStringProperty(TEXT("Exact existing SCS component variable name.")));
    TSharedRef<FJsonObject> Names = MakeShared<FJsonObject>(); Names->SetStringField(TEXT("type"), TEXT("array")); Names->SetObjectField(TEXT("items"), BuildStringProperty(TEXT("Exact reflected property name."))); Properties->SetObjectField(TEXT("propertyNames"), Names);
    TSharedRef<FJsonObject> Limit = MakeShared<FJsonObject>(); Limit->SetStringField(TEXT("type"), TEXT("integer")); Limit->SetStringField(TEXT("description"), TEXT("Maximum returned properties, default 200.")); Properties->SetObjectField(TEXT("maxProperties"), Limit);
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("componentName"))}); return Schema;
}
