#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"
#include "Tools/Assets/AssetManagementTools.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Members/ReparentBlueprintTool.h"

namespace UnrealMCPAssetMoveReparent
{
    const FString Root = TEXT("/Game/UnrealMCP_Automation");

    struct FFixture
    {
        FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
        TArray<FString> ObjectPaths;

        UBlueprint* Create(const FString& Name, UClass* Parent, const FString& Folder = Root)
        {
            const FString AssetName = Name + TEXT("_") + Suffix;
            UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(*(Folder / AssetName)), FName(*AssetName),
                BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("UnrealMCPAutomation")));
            FAssetRegistryModule::AssetCreated(Blueprint);
            Track(Blueprint->GetPathName());
            return Blueprint;
        }

        void Track(const FString& ObjectPath) { ObjectPaths.AddUnique(ObjectPath); }

        // Removes fixtures from memory, the registry and disk (move/fixup may have saved some of them).
        ~FFixture()
        {
            for (const FString& Path : ObjectPaths)
            {
                if (UObject* Object = FindObject<UObject>(nullptr, *Path))
                {
                    UPackage* Package = Object->GetPackage();
                    FAssetRegistryModule::AssetDeleted(Object);
                    ObjectTools::DeleteObjectsUnchecked({Object});
                    if (Package) Package->SetDirtyFlag(false);
                }
            }
            TArray<FString> Files;
            IFileManager::Get().FindFilesRecursive(Files, *FPaths::Combine(FPaths::ProjectContentDir(), TEXT("UnrealMCP_Automation")),
                *FString::Printf(TEXT("*%s*"), *Suffix), true, false);
            for (const FString& File : Files) IFileManager::Get().Delete(*File, false, true);
            IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("UnrealMCP_Automation"), TEXT("Moved")), false, false);
        }
    };

    UnrealMCP::FMCPResponse Run(const FMCPToolBase& Tool, const TFunction<void(FJsonObject&)>& Fill)
    {
        UnrealMCP::FMCPRequest Request;
        Request.Id = TEXT("asset-move-reparent");
        Request.Params = MakeShared<FJsonObject>();
        Fill(*Request.Params);
        return Tool.Execute(Request);
    }

    template <typename TNode>
    TNode* Place(UEdGraph* Graph)
    {
        TNode* Node = NewObject<TNode>(Graph);
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, {});
        return Node;
    }

    UK2Node_CustomEvent* AddCustomEvent(UBlueprint* Blueprint, const TCHAR* Name)
    {
        UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Blueprint->UbergraphPages[0]);
        Event->CustomFunctionName = Name;
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Blueprint->UbergraphPages[0], Event, {});
        return Event;
    }

    bool HasEntry(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field, const TCHAR* Key, const FString& Value)
    {
        return Result.IsValid() && Result->GetArrayField(Field).ContainsByPredicate([&](const TSharedPtr<FJsonValue>& V)
        {
            return V->AsObject()->GetStringField(Key).Equals(Value, ESearchCase::IgnoreCase);
        });
    }

    void Compile(UBlueprint* Blueprint)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    }

    bool Compiles(const UBlueprint* Blueprint) { return Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings; }
}

using namespace UnrealMCPAssetMoveReparent;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPReparentBlueprintTest, "UnrealMCP.Blueprint.Authoring.ReparentBlueprint.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPReparentBlueprintTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* OldParent = Fixture.Create(TEXT("BP_ReparentOldParent"), AActor::StaticClass());
    FEdGraphPinType DoubleType;
    DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
    DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    FBlueprintEditorUtils::AddMemberVariable(OldParent, TEXT("ModelScale"), DoubleType, TEXT("1.0"));
    AddCustomEvent(OldParent, TEXT("RotateRPC"));
    Compile(OldParent);
    UBlueprint* NewParent = Fixture.Create(TEXT("BP_ReparentNewParent"), AActor::StaticClass());
    Compile(NewParent);

    UBlueprint* Child = Fixture.Create(TEXT("BP_ReparentChild"), OldParent->GeneratedClass);
    UEdGraph* Graph = Child->UbergraphPages[0];
    UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(Graph);
    Getter->VariableReference.SetSelfMember(TEXT("ModelScale"));
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Getter, {});
    UK2Node_CustomEvent* Go = AddCustomEvent(Child, TEXT("Go"));
    UK2Node_CallFunction* Rotate = Place<UK2Node_CallFunction>(Graph);
    Rotate->FunctionReference.SetSelfMember(TEXT("RotateRPC"));
    Rotate->ReconstructNode();
    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Go->FindPin(UEdGraphSchema_K2::PN_Then), Rotate->GetExecPin());
    Compile(Child);
    if (!TestTrue(TEXT("Child fixture compiles"), Compiles(Child))) return false;
    const FString GetterGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Getter);
    const FString RotateGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Rotate);

    auto Params = [&](bool bDryRun, bool bConfirm)
    {
        return [&, bDryRun, bConfirm](FJsonObject& P)
        {
            P.SetStringField(TEXT("objectPath"), Child->GetPathName());
            P.SetStringField(TEXT("newParentClassPath"), NewParent->GeneratedClass->GetPathName());
            P.SetBoolField(TEXT("dryRun"), bDryRun);
            P.SetBoolField(TEXT("confirm"), bConfirm);
        };
    };

    const UnrealMCP::FMCPResponse Dry = Run(FReparentBlueprintTool(), Params(true, false));
    if (Dry.Error.IsSet()) AddError(Dry.Error->Message);
    if (!TestTrue(TEXT("Dry-run returns a report"), Dry.Result.IsValid())) return false;
    TestTrue(TEXT("Dry-run lists the ModelScale getter"), HasEntry(Dry.Result, TEXT("breakingNodes"), TEXT("nodeGuid"), GetterGuid));
    TestTrue(TEXT("Dry-run lists the RotateRPC call"), HasEntry(Dry.Result, TEXT("breakingNodes"), TEXT("nodeGuid"), RotateGuid));
    TestTrue(TEXT("Dry-run lists ModelScale default as lost"), HasEntry(Dry.Result, TEXT("defaultChanges"), TEXT("property"), TEXT("ModelScale")));
    TestTrue(TEXT("Dry-run did not change the parent"), Child->ParentClass == OldParent->GeneratedClass);
    for (const TSharedPtr<FJsonValue>& Change : Dry.Result->GetArrayField(TEXT("defaultChanges")))
    {
        if (Change->AsObject()->GetStringField(TEXT("property")) == TEXT("ModelScale"))
        {
            TestEqual(TEXT("ModelScale change is 'lost'"), Change->AsObject()->GetStringField(TEXT("change")), FString(TEXT("lost")));
            TestTrue(TEXT("ModelScale current value is 1"), FCString::Atod(*Change->AsObject()->GetStringField(TEXT("currentValue"))) == 1.0);
        }
    }

    TestTrue(TEXT("Apply without confirm is refused"), Run(FReparentBlueprintTool(), Params(false, false)).Error.IsSet());

    const UnrealMCP::FMCPResponse Applied = Run(FReparentBlueprintTool(), Params(false, true));
    if (Applied.Error.IsSet()) AddError(Applied.Error->Message);
    if (!TestTrue(TEXT("Apply returns a result"), Applied.Result.IsValid())) return false;
    TestTrue(TEXT("Parent changed"), Child->ParentClass == NewParent->GeneratedClass);
    TestFalse(TEXT("Compile reported as failed"), Applied.Result->GetBoolField(TEXT("compiled")));
    for (const TSharedPtr<FJsonValue>& E : Applied.Result->GetArrayField(TEXT("errors")))
        AddInfo(FString::Printf(TEXT("reparent error: node=%s graph=%s msg=%s"), *E->AsObject()->GetStringField(TEXT("nodeGuid")),
            *E->AsObject()->GetStringField(TEXT("graph")), *E->AsObject()->GetStringField(TEXT("message"))));
    TestTrue(TEXT("Compile errors point at the broken nodes"),
        HasEntry(Applied.Result, TEXT("errors"), TEXT("nodeGuid"), GetterGuid) || HasEntry(Applied.Result, TEXT("errors"), TEXT("nodeGuid"), RotateGuid));
    TestFalse(TEXT("Not saved"), Applied.Result->GetBoolField(TEXT("saved")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPMoveDuplicateDeleteTest, "UnrealMCP.Asset.MoveDuplicateDelete.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPMoveDuplicateDeleteTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Target = Fixture.Create(TEXT("BP_MoveTarget"), AActor::StaticClass());
    Compile(Target);
    UBlueprint* User = Fixture.Create(TEXT("BP_MoveUser"), AActor::StaticClass());
    UK2Node_CustomEvent* Go = AddCustomEvent(User, TEXT("Go"));
    UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(User->UbergraphPages[0]);
    CastNode->TargetType = Target->GeneratedClass;
    UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(User->UbergraphPages[0], CastNode, {});
    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Go->FindPin(UEdGraphSchema_K2::PN_Then), CastNode->GetExecPin());
    UK2Node_Self* Self = Place<UK2Node_Self>(User->UbergraphPages[0]);
    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Self->FindPin(UEdGraphSchema_K2::PN_Self), CastNode->GetCastSourcePin());
    Compile(User);
    if (!TestTrue(TEXT("User fixture compiles"), Compiles(User))) return false;

    const FString OldPath = Target->GetPathName();
    const FString MovedFolder = Root / TEXT("Moved");
    const FString NewName = TEXT("BP_Moved_") + Fixture.Suffix;
    const FString NewPath = MovedFolder / NewName + TEXT(".") + NewName;
    Fixture.Track(NewPath);
    auto MoveParams = [&](bool bDryRun) { return [&, bDryRun](FJsonObject& P)
    {
        P.SetStringField(TEXT("sourceObjectPath"), OldPath);
        P.SetStringField(TEXT("destinationPackagePath"), MovedFolder);
        P.SetStringField(TEXT("newName"), NewName);
        P.SetBoolField(TEXT("dryRun"), bDryRun);
        P.SetBoolField(TEXT("confirm"), !bDryRun);
    }; };

    const UnrealMCP::FMCPResponse Dry = Run(FMoveAssetTool(), MoveParams(true));
    TestTrue(TEXT("Move dry-run succeeds"), Dry.Result.IsValid());
    TestNotNull(TEXT("Move dry-run leaves the asset in place"), FindObject<UBlueprint>(nullptr, *OldPath));

    const UnrealMCP::FMCPResponse Moved = Run(FMoveAssetTool(), MoveParams(false));
    if (Moved.Error.IsSet()) AddError(Moved.Error->Message);
    if (!TestTrue(TEXT("Move succeeds"), Moved.Result.IsValid())) return false;
    UBlueprint* MovedBlueprint = FindObject<UBlueprint>(nullptr, *NewPath);
    TestNotNull(TEXT("Asset exists at the new path"), MovedBlueprint);
    TestNull(TEXT("No redirector left at the old path"), FindObject<UObjectRedirector>(nullptr, *OldPath));
    TestTrue(TEXT("Cast now targets the moved class"), MovedBlueprint && CastNode->TargetType == MovedBlueprint->GeneratedClass);
    Compile(User);
    TestTrue(TEXT("Blueprint that casts to it still compiles"), Compiles(User));

    TestTrue(TEXT("Moving onto an existing asset is refused"), Run(FMoveAssetTool(), [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("sourceObjectPath"), User->GetPathName());
        P.SetStringField(TEXT("destinationPackagePath"), MovedFolder);
        P.SetStringField(TEXT("newName"), NewName);
        P.SetBoolField(TEXT("confirm"), true);
    }).Error.IsSet());

    const UnrealMCP::FMCPResponse Fixup = Run(FFixupRedirectorsTool(), [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("packagePath"), Root);
        P.SetBoolField(TEXT("dryRun"), true);
    });
    TestTrue(TEXT("Fixup dry-run succeeds"), Fixup.Result.IsValid());

    const FString CopyName = TEXT("BP_Copy_") + Fixture.Suffix;
    const FString CopyPath = Root / CopyName + TEXT(".") + CopyName;
    Fixture.Track(CopyPath);
    const UnrealMCP::FMCPResponse Duplicated = Run(FDuplicateAssetTool(), [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("sourceObjectPath"), NewPath);
        P.SetStringField(TEXT("destinationPackagePath"), Root);
        P.SetStringField(TEXT("newName"), CopyName);
        P.SetBoolField(TEXT("save"), true);
    });
    if (Duplicated.Error.IsSet()) AddError(Duplicated.Error->Message);
    UBlueprint* Copy = FindObject<UBlueprint>(nullptr, *CopyPath);
    TestTrue(TEXT("Duplicate exists and is a separate asset"), Copy != nullptr && Copy != MovedBlueprint);
    TestTrue(TEXT("Duplicating onto an existing asset is refused"), Run(FDuplicateAssetTool(), [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("sourceObjectPath"), NewPath);
        P.SetStringField(TEXT("destinationPackagePath"), Root);
        P.SetStringField(TEXT("newName"), CopyName);
    }).Error.IsSet());

    TestTrue(TEXT("Delete without confirm is refused"), Run(FDeleteAssetTool(), [&](FJsonObject& P) { P.SetStringField(TEXT("objectPath"), CopyPath); }).Error.IsSet());
    const UnrealMCP::FMCPResponse Deleted = Run(FDeleteAssetTool(), [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("objectPath"), CopyPath);
        P.SetBoolField(TEXT("confirm"), true);
    });
    if (Deleted.Error.IsSet()) AddError(Deleted.Error->Message);
    TestTrue(TEXT("Delete reports success"), Deleted.Result.IsValid() && Deleted.Result->GetBoolField(TEXT("deleted")));
    if (Deleted.Result.IsValid())
    {
        TestTrue(TEXT("Deletion verifies an existing package file"), Deleted.Result->GetBoolField(TEXT("packageFileExisted")));
        TestFalse(TEXT("Saved package file is gone"), Deleted.Result->GetBoolField(TEXT("packageFileRemains")));
    }
    UBlueprint* Gone = FindObject<UBlueprint>(nullptr, *CopyPath);
    TestTrue(TEXT("Deleted asset is gone"), Gone == nullptr || !IsValid(Gone));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealMCPRedirectorFixupTest, "UnrealMCP.Asset.RedirectorFixup.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUnrealMCPRedirectorFixupTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UBlueprint* Target = Fixture.Create(TEXT("BP_FixupTarget"), AActor::StaticClass());
    Compile(Target);
    UBlueprint* User = Fixture.Create(TEXT("BP_FixupUser"), AActor::StaticClass());
    FEdGraphPinType ObjectType;
    ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ObjectType.PinSubCategoryObject = UBlueprint::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(User, TEXT("AssetReference"), ObjectType);
    Compile(User);
    FObjectProperty* Property = FindFProperty<FObjectProperty>(User->GeneratedClass, TEXT("AssetReference"));
    Property->SetObjectPropertyValue_InContainer(User->GeneratedClass->GetDefaultObject(), Target);
    FString Filename, Error;
    if (!TestTrue(TEXT("Save target fixture"), UnrealMCP::BlueprintEditToolUtils::SaveAsset(Target, Filename, Error))
        || !TestTrue(TEXT("Save referencer fixture"), UnrealMCP::BlueprintEditToolUtils::SaveAsset(User, Filename, Error))) return false;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles({Filename});
    const FString OldPath = Target->GetPathName();
    UPackage* OldPackage = Target->GetPackage();
    const FName OldName = Target->GetFName();
    const FString Name = TEXT("BP_Fixed_") + Fixture.Suffix;
    UPackage* NewPackage = CreatePackage(*(Root / Name));
    Target->Rename(*Name, NewPackage, REN_DontCreateRedirectors | REN_NonTransactional);
    Fixture.Track(Target->GetPathName());
    UObjectRedirector* Redirector = NewObject<UObjectRedirector>(OldPackage, OldName, RF_Public | RF_Standalone);
    Redirector->DestinationObject = Target;
    FAssetRegistryModule::AssetCreated(Redirector);
    if (!TestTrue(TEXT("Save redirector fixture"), UnrealMCP::BlueprintEditToolUtils::SaveAsset(Redirector, Filename, Error))) return false;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles({Filename});
    TestNotNull(TEXT("Real redirector exists"), FindObject<UObjectRedirector>(nullptr, *OldPath));
    User->MarkPackageDirty();
    auto FixParams = [&](FJsonObject& P)
    {
        P.SetStringField(TEXT("packagePath"), Root);
        P.SetBoolField(TEXT("confirm"), true);
    };
    const auto Refused = Run(FFixupRedirectorsTool(), FixParams);
    TestTrue(TEXT("Dirty referencer is refused"), Refused.Error.IsSet());
    TestTrue(TEXT("Unsaved edits preserved"), User->GetPackage()->IsDirty());
    const auto Fixed = Run(FFixupRedirectorsTool(), [&](FJsonObject& P)
    {
        FixParams(P);
        P.SetBoolField(TEXT("allowSavingDirtyReferencers"), true);
    });
    if (Fixed.Error.IsSet()) AddError(Fixed.Error->Message);
    if (!Fixed.Result.IsValid()) return false;
    TestTrue(TEXT("Fixup succeeds"), Fixed.Result->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Reports saved referencer"), Fixed.Result->GetArrayField(TEXT("referencersResaved")).ContainsByPredicate(
        [&](const TSharedPtr<FJsonValue>& V) { return V->AsString() == User->GetPackage()->GetName(); }));
    TestTrue(TEXT("No redirectors remain"), Fixed.Result->GetArrayField(TEXT("redirectorsRemaining")).IsEmpty());
    TestFalse(TEXT("Referencer saved clean"), User->GetPackage()->IsDirty());
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
