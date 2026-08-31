#include "Tools/AddBlueprintVariableTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintVariableTool::FAddBlueprintVariableTool()
    : FMCPToolBase(TEXT("AddBlueprintVariable"), TEXT("Transactionally adds a typed Blueprint member variable with optional default, instance-editable metadata, compile, and save.")) {}

UnrealMCP::FMCPResponse FAddBlueprintVariableTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, VariableName, TypeName, TypeObjectPath, DefaultValue, Category;
    FString ContainerTypeName, ValueTypeName, ValueTypeObjectPath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("variableName"), VariableName) || !Request.Params->TryGetStringField(TEXT("type"), TypeName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintVariable requires objectPath, variableName, and type."));
    }

    Request.Params->TryGetStringField(TEXT("typeObjectPath"), TypeObjectPath);
    Request.Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);
    Request.Params->TryGetStringField(TEXT("category"), Category);
    Request.Params->TryGetStringField(TEXT("containerType"), ContainerTypeName);
    Request.Params->TryGetStringField(TEXT("valueType"), ValueTypeName);
    Request.Params->TryGetStringField(TEXT("valueTypeObjectPath"), ValueTypeObjectPath);

    const bool bArray = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("isArray"), false);
    const bool bInstanceEditable = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("instanceEditable"), false);
    if (ContainerTypeName.IsEmpty())
    {
        ContainerTypeName = bArray ? TEXT("array") : TEXT("none");
    }

    if (bArray && !ContainerTypeName.Equals(TEXT("array"), ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("isArray=true conflicts with a non-array containerType."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    bool bAlreadyExists = false;
    bool bCompileSucceeded = false;
    bool bIndexRefreshed = false;
    FString SavedFilename, ExecutionError, IndexRefreshError;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
        {
            return false;
        }

        FEdGraphPinType PinType;
        if (!UnrealMCP::BlueprintEditToolUtils::BuildContainerPinType(TypeName, TypeObjectPath, ContainerTypeName, ValueTypeName, ValueTypeObjectPath, PinType, OutError))
        {
            return false;
        }

        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            if (Variable.VarName.ToString().Equals(VariableName, ESearchCase::IgnoreCase))
            {
                bAlreadyExists = true;
                return true;
            }
        }

        if (bDryRun)
        {
            return true;
        }

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AddBlueprintVariable", "UnrealMCP Add Blueprint Variable"));
        Blueprint->Modify();
        if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, *VariableName, PinType, DefaultValue))
        {
            OutError = TEXT("Unreal failed to add the Blueprint member variable.");
            return false;
        }

        FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, *VariableName, !bInstanceEditable);
        if (!Category.IsEmpty())
        {
            FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, *VariableName, nullptr, FText::FromString(Category), true);
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
            bCompileSucceeded = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
            if (!bCompileSucceeded)
            {
                OutError = TEXT("Variable was added, but compilation failed; use CompileBlueprint for diagnostics.");
                return false;
            }
        }

        if (bSave && !UnrealMCP::BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, OutError))
        {
            return false;
        }

        if (bSave)
        {
            bIndexRefreshed = UnrealMCP::BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexRefreshError);
        }

        return true;
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("variableName"), VariableName);
    Result->SetStringField(TEXT("type"), TypeName);
    Result->SetStringField(TEXT("typeObjectPath"), TypeObjectPath);
    Result->SetStringField(TEXT("containerType"), ContainerTypeName.ToLower());
    Result->SetStringField(TEXT("valueType"), ValueTypeName);
    Result->SetStringField(TEXT("valueTypeObjectPath"), ValueTypeObjectPath);
    Result->SetBoolField(TEXT("isArray"), ContainerTypeName.Equals(TEXT("array"), ESearchCase::IgnoreCase));
    Result->SetBoolField(TEXT("instanceEditable"), bInstanceEditable);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyExists"), bAlreadyExists);
    Result->SetBoolField(TEXT("added"), !bDryRun && !bAlreadyExists);
    Result->SetBoolField(TEXT("compiled"), bCompile && !bDryRun && !bAlreadyExists);
    Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bAlreadyExists);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintVariableTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    P->SetObjectField(TEXT("variableName"), BuildStringProperty(TEXT("Unique member variable name.")));
    P->SetObjectField(TEXT("type"), BuildStringProperty(TEXT("Scalar type, or Map key type.")));
    P->SetObjectField(TEXT("typeObjectPath"), BuildStringProperty(TEXT("Required class path for object/class types.")));
    P->SetObjectField(TEXT("containerType"), BuildStringProperty(TEXT("none, array, set, or map.")));
    P->SetObjectField(TEXT("valueType"), BuildStringProperty(TEXT("Required Map value type.")));
    P->SetObjectField(TEXT("valueTypeObjectPath"), BuildStringProperty(TEXT("Class path for object/class Map values.")));
    P->SetObjectField(TEXT("defaultValue"), BuildStringProperty(TEXT("Optional Unreal-exported default literal.")));
    P->SetObjectField(TEXT("category"), BuildStringProperty(TEXT("Optional variable category.")));
    P->SetObjectField(TEXT("isArray"), BuildBoolProperty(TEXT("Backward-compatible alias for containerType=array.")));
    P->SetObjectField(TEXT("instanceEditable"), BuildBoolProperty(TEXT("Allow editing on Blueprint instances.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without mutation.")));
    P->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile only this Blueprint.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save after mutation.")));
    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> R{ MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("variableName")), MakeShared<FJsonValueString>(TEXT("type")) };
    S->SetArrayField(TEXT("required"), R);
    return S;
}
