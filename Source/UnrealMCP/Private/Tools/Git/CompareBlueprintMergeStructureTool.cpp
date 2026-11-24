#include "Tools/Git/CompareBlueprintMergeStructureTool.h"
#include "Tools/Git/ConflictResolution/ApplyBlueprintMergeStructureOps.h"
#include "Tools/Blueprint/Graph/ApplyBlueprintGraphPatchTool.h"

#include "Algo/AllOf.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "DiffUtils.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Misc/FileHelper.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Misc/EngineVersionComparison.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Tools/Blueprint/Members/BlueprintClassDefaultsTools.h"
#include "Tools/Blueprint/Members/AddBlueprintInterfaceTool.h"
#include "Tools/Blueprint/Members/AddBlueprintVariableTool.h"
#include "Tools/Blueprint/Members/SetBlueprintComponentDefaultsTool.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"
#include "UObject/LinkerLoad.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/UObjectHash.h"

namespace
{
    constexpr int32 MaxGitOutput = 64 * 1024 * 1024;

    // Read bounded binary output from Git without invoking a command shell.
    bool RunGit(const FString& ProjectDir, const FString& Args, TArray<uint8>& Output)
    {
        Output.Reset();
        void* Read = nullptr;
        void* Write = nullptr;
        if (!FPlatformProcess::CreatePipe(Read, Write)) return false;
        ON_SCOPE_EXIT { FPlatformProcess::ClosePipe(Read, Write); };
        FProcHandle Process = FPlatformProcess::CreateProc(TEXT("git"), *Args, true, true, true, nullptr, 0, *ProjectDir, Write);
        if (!Process.IsValid()) return false;
        ON_SCOPE_EXIT { FPlatformProcess::CloseProc(Process); };
        const double Deadline = FPlatformTime::Seconds() + 10.0;
        TArray<uint8> Chunk;
        bool bRunning;
        do
        {
            bRunning = FPlatformProcess::IsProcRunning(Process);
            while (FPlatformProcess::ReadPipeToArray(Read, Chunk))
            {
                if (Chunk.Num() > MaxGitOutput - Output.Num() || FPlatformTime::Seconds() > Deadline)
                {
                    if (FPlatformProcess::IsProcRunning(Process)) FPlatformProcess::TerminateProc(Process);
                    return false;
                }
                Output.Append(Chunk);
            }
            if (bRunning && FPlatformTime::Seconds() > Deadline)
            {
                FPlatformProcess::TerminateProc(Process);
                return false;
            }
            if (bRunning) FPlatformProcess::Sleep(0.005f);
        } while (bRunning);
        int32 ExitCode = -1;
        return FPlatformProcess::GetProcReturnCode(Process, &ExitCode) && ExitCode == 0;
    }

    bool ReadGitText(const FString& ProjectDir, const FString& Args, FString& Text, bool bTrimEnd = true)
    {
        TArray<uint8> Bytes;
        if (!RunGit(ProjectDir, Args, Bytes)) return false;
        if (Bytes.IsEmpty()) { Text.Reset(); return true; }
        const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
        Text = FString(Converted.Length(), Converted.Get());
        if (bTrimEnd) Text.TrimEndInline();
        return true;
    }

    bool SafeProjectPath(const FString& Path)
    {
        for (TCHAR C : Path)
            if (C < 32 || FCString::Strchr(TEXT("\"\\:*?<>|"), C)) return false;
        TArray<FString> Parts;
        Path.ParseIntoArray(Parts, TEXT("/"), false);
        for (const FString& Part : Parts)
            if (Part.IsEmpty() || Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" "))) return false;
        return true;
    }

    bool SafeContentPath(const FString& Path)
    {
        return Path.StartsWith(TEXT("Content/")) && Path.EndsWith(TEXT(".uasset")) && SafeProjectPath(Path);
    }

    bool IsGitObjectId(const FString& Id)
    {
        return (Id.Len() == 40 || Id.Len() == 64) && Algo::AllOf(Id, [](TCHAR C) { return FChar::IsHexDigit(C); });
    }

    bool ReadGitFields(const FString& ProjectDir, const FString& Args, TArray<FString>& Fields)
    {
        TArray<uint8> Bytes;
        if (!RunGit(ProjectDir, Args, Bytes)) return false;
        int32 Start = 0;
        for (int32 Index = 0; Index < Bytes.Num(); ++Index)
        {
            if (Bytes[Index] != 0) continue;
            const FUTF8ToTCHAR Field(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Start), Index - Start);
            Fields.Emplace(Field.Length(), Field.Get());
            Start = Index + 1;
        }
        return Start == Bytes.Num();
    }

    const TCHAR* const Sides[] = {TEXT("base"), TEXT("current"), TEXT("incoming")};

    bool ReadUnmergedSlots(const FString& ProjectDir, const FString& RepoPath, FString (&Ids)[3], FString& Error)
    {
        TArray<FString> Records;
        if (!ReadGitFields(ProjectDir, FString::Printf(TEXT("ls-files --unmerged --full-name -z -- \":(top,literal)%s\""), *RepoPath), Records))
        { Error = TEXT("Could not read unmerged Git index entries."); return false; }
        for (FString& Record : Records)
        {
            FString Header, EntryPath;
            TArray<FString> Fields;
            if (!Record.Split(TEXT("\t"), &Header, &EntryPath) || EntryPath != RepoPath) continue;
            Header.ParseIntoArrayWS(Fields);
            int32 Stage = 0;
            if (Fields.Num() != 3 || !LexTryParseString(Stage, *Fields[2]) || Stage < 1 || Stage > 3 || !IsGitObjectId(Fields[1]))
            { Error = TEXT("Malformed unmerged Git index entry."); return false; }
            if (Fields[0] != TEXT("100644") && Fields[0] != TEXT("100755"))
            { Error = TEXT("A Git conflict stage is not a regular asset file."); return false; }
            Ids[Stage - 1] = Fields[1];
        }
        return true;
    }

    bool ReadReviewStages(const FString& ProjectDir, const FString (&RepoPaths)[3], FString (&Ids)[3], FString& Error)
    {
        TMap<FString, TArray<FString>> Seen;
        for (int32 Side = 0; Side < 3; ++Side)
        {
            if (const auto* Existing = Seen.Find(RepoPaths[Side]))
            {
                Ids[Side] = (*Existing)[Side];
                continue;
            }
            FString Slots[3];
            if (!ReadUnmergedSlots(ProjectDir, RepoPaths[Side], Slots, Error)) return false;
            Seen.Add(RepoPaths[Side], {Slots[0], Slots[1], Slots[2]});
            Ids[Side] = Slots[Side];
        }
        if (Ids[0].IsEmpty() && Ids[1].IsEmpty() && Ids[2].IsEmpty())
        { Error = TEXT("No selected Git conflict stages exist at these paths."); return false; }
        return true;
    }

    void HashEvidence(FMD5& Hash, const FString& Value)
    {
        const FTCHARToUTF8 Bytes(*Value);
        const uint32 Length = Bytes.Length();
        Hash.Update(reinterpret_cast<const uint8*>(&Length), sizeof(Length));
        Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()), Length);
    }

    FString FinishEvidence(FMD5& Hash)
    {
        uint8 Digest[16];
        Hash.Final(Digest);
        return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
    }

    bool ReadStageIds(const FString& ProjectDir, const FString& RepoPath, FString (&Ids)[3], FString& Error)
    {
        for (int32 Index = 0; Index < 3; ++Index)
        {
            if (!ReadGitText(ProjectDir, FString::Printf(TEXT("rev-parse --verify --quiet \":%d:%s\""), Index + 1, *RepoPath), Ids[Index]))
            {
                Error = FString::Printf(TEXT("Git stage %d (%s) is missing or unreadable."), Index + 1, Sides[Index]);
                return false;
            }
            if (!IsGitObjectId(Ids[Index]))
            {
                Error = TEXT("Git returned an invalid stage object ID.");
                return false;
            }
        }
        return true;
    }

    bool ResolveRepoPath(const FString& ProjectDir, const FString& Path, FString& RepoPath, FString& Error)
    {
        FString Prefix;
        if (!ReadGitText(ProjectDir, TEXT("rev-parse --show-prefix"), Prefix))
        { Error = TEXT("Git is unavailable or the selected project is outside a Git worktree."); return false; }
        RepoPath = Prefix + Path;
        if (!SafeContentPath(TEXT("Content/") + RepoPath))
        { Error = TEXT("Git returned an unsupported project prefix."); return false; }
        return true;
    }

    FString StageRevision(const FString (&Ids)[3])
    {
        return Ids[0] + TEXT(":") + Ids[1] + TEXT(":") + Ids[2];
    }

    FString ReviewRevision(const FString& Path, const FString (&Paths)[3], const FString (&Ids)[3])
    {
        if (Paths[0] == Path && Paths[1] == Path && Paths[2] == Path) return StageRevision(Ids);
        FMD5 Hash;
        for (int32 Side = 0; Side < 3; ++Side)
        {
            HashEvidence(Hash, Paths[Side]);
            HashEvidence(Hash, Ids[Side]);
        }
        return FinishEvidence(Hash);
    }

    TArray<FString> DirtyContentPackages()
    {
        TArray<UPackage*> Dirty;
        FEditorFileUtils::GetDirtyContentPackages(Dirty);
        FEditorFileUtils::GetDirtyWorldPackages(Dirty);
        TArray<FString> Names;
        for (const UPackage* Package : Dirty)
            if (Package) Names.AddUnique(Package->GetName());
        Names.Sort();
        return Names;
    }

    using FStructure = TMap<FString, FString>;

    TSharedRef<FJsonObject> BuildGraphReviewView(UBlueprint* Blueprint, const FGuid& GraphGuid, const FGuid& FocusGuid)
    {
        using namespace UnrealMCP::BlueprintGraphEditToolUtils;
        const auto View = MakeShared<FJsonObject>();
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        const auto* Found = Graphs.FindByPredicate([&](const UEdGraph* Candidate)
            { return Candidate && Candidate->GraphGuid == GraphGuid; });
        if (!Found) { View->SetStringField(TEXT("state"), TEXT("graph_absent")); return View; }
        const UEdGraph* Graph = *Found;
        View->SetStringField(TEXT("graph"), Graph->GetPathName(Blueprint));
        View->SetStringField(TEXT("graphGuid"), GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        TArray<const UEdGraphNode*> Nodes;
        for (const UEdGraphNode* Node : Graph->Nodes) if (Node) Nodes.Add(Node);
        const UEdGraphNode* Focus = nullptr;
        if (FocusGuid.IsValid())
        {
            const auto* Match = Nodes.FindByPredicate([&](const UEdGraphNode* Node) { return Node->NodeGuid == FocusGuid; });
            if (!Match) { View->SetStringField(TEXT("state"), TEXT("node_absent")); return View; }
            Focus = *Match;
            Nodes.Reset();
            Nodes.Add(Focus);
        }
        View->SetStringField(TEXT("state"), TEXT("loaded"));
        View->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
        TArray<TSharedPtr<FJsonValue>> Links;
        int32 Total = 0;
        auto Endpoint = [](const UEdGraphNode* Node, const UEdGraphPin* Pin)
        {
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Item->SetStringField(TEXT("nodeGuid"), GetNodeGuid(Node));
            Item->SetStringField(TEXT("pin"), Pin->PinName.ToString());
            Item->SetStringField(TEXT("pinId"), GetPinId(Pin));
            Item->SetStringField(TEXT("graphGuid"), Node->GetGraph()->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
            return Item;
        };
        if (Focus)
        {
            View->SetStringField(TEXT("focusTitle"), Focus->GetNodeTitle(ENodeTitleType::ListView).ToString());
            View->SetStringField(TEXT("focusNodeGuid"), GetNodeGuid(Focus));
        }
        for (const UEdGraphNode* Node : Nodes)
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || (!Focus && (Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != TEXT("exec")))) continue;
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    const UEdGraphNode* Other = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
                    if (!Other || !Other->GetGraph() || (Other == Focus && Pin->Direction == EGPD_Input)) continue;
                    if (++Total > 32) continue;
                    const bool bOutput = Pin->Direction == EGPD_Output;
                    auto Link = MakeShared<FJsonObject>();
                    Link->SetStringField(TEXT("kind"), Pin->PinType.PinCategory == TEXT("exec") ? TEXT("execution") : TEXT("data"));
                    Link->SetObjectField(TEXT("from"), bOutput ? Endpoint(Node, Pin) : Endpoint(Other, Linked));
                    Link->SetObjectField(TEXT("to"), bOutput ? Endpoint(Other, Linked) : Endpoint(Node, Pin));
                    Links.Add(MakeShared<FJsonValueObject>(Link));
                }
            }
        View->SetNumberField(TEXT("linkCount"), Total);
        View->SetBoolField(TEXT("truncated"), Total > Links.Num());
        View->SetArrayField(TEXT("links"), Links);
        return View;
    }

    // Snapshot logic adapted from GraphXPort's MIT-licensed historical Blueprint reader.
    FStructure SnapshotStructure(UBlueprint* Blueprint, bool bIncludeGraphBodies, bool& bReferencesAvailable, const FString& DependencyMount = FString(), const FGuid& GraphScope = FGuid())
    {
        FStructure Fields;
        const FString Package = Blueprint->GetOutermost()->GetName();
        auto Add = [&](FString Key, FString Value)
        {
            Value.ReplaceInline(*Package, TEXT("<self>"));
            if (!DependencyMount.IsEmpty())
            {
                Key.ReplaceInline(*DependencyMount, TEXT("/Game/"));
                Value.ReplaceInline(*DependencyMount, TEXT("/Game/"));
            }
            Fields.Add(MoveTemp(Key), MoveTemp(Value));
        };
        auto PathOf = [](const UObject* Object)
        {
            return Object ? Object->GetPathName() : FString();
        };
        auto Export = [&](const UObject* Object, const FProperty* Property, const FString& Key)
        {
            for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
            {
                FString Value;
                Property->ExportText_InContainer(Index, Value, Object, Object, nullptr, PPF_None);
                Add(Property->ArrayDim > 1 ? Key + FString::Printf(TEXT("[%d]"), Index) : Key, MoveTemp(Value));
            }
        };
        auto AddVariable = [&](const FString& Prefix, const FBPVariableDescription& Variable)
        {
            FString Value;
            FBPVariableDescription::StaticStruct()->ExportText(Value, &Variable, nullptr, nullptr, PPF_None, nullptr);
            Add(Prefix + (Variable.VarGuid.IsValid() ? Variable.VarGuid.ToString() : Variable.VarName.ToString()), MoveTemp(Value));
        };
        auto AddTemplate = [&](const FString& Prefix, const UActorComponent* Template)
        {
            if (!Template) return;
            Add(Prefix + TEXT("/class"), PathOf(Template->GetClass()));
            for (TFieldIterator<FProperty> Property(Template->GetClass()); Property; ++Property)
                if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
                    Export(Template, *Property, Prefix + TEXT("/property/") + Property->GetName());
        };

        if (const UClass* Class = Blueprint->GeneratedClass)
            if (const UObject* Default = Class->GetDefaultObject(false))
                for (TFieldIterator<FProperty> Property(Class); Property; ++Property)
                    if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
                        Export(Default, *Property, TEXT("classDefault/") + Property->GetName());

        Add(TEXT("parentClass"), PathOf(Blueprint->ParentClass));
        for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
            if (Interface.Interface) Add(TEXT("interface/") + PathOf(Interface.Interface), TEXT("implemented"));
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables) AddVariable(TEXT("member/"), Variable);
        for (const TCHAR* Setting : {TEXT("BlueprintType"), TEXT("BlueprintDisplayName"), TEXT("BlueprintDescription"),
            TEXT("BlueprintCategory"), TEXT("bRunConstructionScriptOnDrag"), TEXT("bGenerateAbstractClass"), TEXT("bDeprecate")})
            if (const FProperty* Property = FindFProperty<FProperty>(Blueprint->GetClass(), Setting))
                Export(Blueprint, Property, FString(TEXT("classSetting/")) + Setting);
        for (TFieldIterator<FProperty> Property(Blueprint->GetClass()); Property; ++Property)
            if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
                Export(Blueprint, *Property, TEXT("classSetting/") + Property->GetName());

        if (FLinkerLoad* Linker = Blueprint->GetLinker())
        {
            bReferencesAvailable = true;
            for (int32 Index = 0; Index < Linker->ImportMap.Num(); ++Index)
            {
                const FString Reference = Linker->GetImportPathName(Index);
                if (Reference.StartsWith(TEXT("/")) && !Reference.StartsWith(TEXT("/Script/"))
                    && !Reference.StartsWith(Package + TEXT(".")) && Reference != Package)
                    Add(TEXT("reference/hard/") + Reference, TEXT("imported"));
            }
            for (const FName Soft : Linker->SoftPackageReferenceList)
            {
                const FString Reference = Soft.ToString();
                if (Reference != Package) Add(TEXT("reference/softPackage/") + Reference, TEXT("referenced"));
            }
        }
        for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (!Graph) continue;
            const FString Prefix = TEXT("function/") + (Graph->GraphGuid.IsValid() ? Graph->GraphGuid.ToString() : Graph->GetName());
            Add(Prefix + TEXT("/name"), Graph->GetName());
            if (const UFunction* Function = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->FindFunctionByName(Graph->GetFName()) : nullptr)
                Add(Prefix + TEXT("/flags"), FString::Printf(TEXT("%08x"), static_cast<uint32>(Function->FunctionFlags)));
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                const auto* Entry = Cast<UK2Node_FunctionEntry>(Node);
                const bool bResult = Node && Node->IsA<UK2Node_FunctionResult>();
                if (!Entry && !bResult) continue;
                if (Entry)
                {
                    if (const FProperty* Metadata = FindFProperty<FProperty>(Entry->GetClass(), TEXT("MetaData")))
                        Export(Entry, Metadata, Prefix + TEXT("/metadata"));
                    for (const FBPVariableDescription& Local : Entry->LocalVariables) AddVariable(Prefix + TEXT("/local/"), Local);
                }
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin || Pin->ParentPin || Pin->bOrphanedPin || Pin->PinType.PinCategory == TEXT("exec")) continue;
                    const FString Key = Prefix + (Entry ? TEXT("/input/") : TEXT("/output/")) + Pin->PinName.ToString();
                    FString Type;
                    FEdGraphPinType::StaticStruct()->ExportText(Type, &Pin->PinType, nullptr, nullptr, PPF_None, nullptr);
                    Add(Key + TEXT("/type"), MoveTemp(Type));
                    Add(Key + TEXT("/default"), Pin->GetDefaultAsString());
                }
            }
        }
        if (bIncludeGraphBodies)
        {
            auto NodeId = [](const UEdGraphNode* Node)
            {
                return Node->NodeGuid.IsValid() ? Node->NodeGuid.ToString() : Node->GetName();
            };
            auto PinId = [](const UEdGraphPin* Pin)
            {
                return Pin->PinId.IsValid() ? Pin->PinId.ToString()
                    : FString::Printf(TEXT("%d:%s"), Pin->Direction, *Pin->PinName.ToString());
            };
            auto GraphId = [Blueprint](const UEdGraph* Graph)
            {
                return Graph->GraphGuid.IsValid() ? Graph->GraphGuid.ToString() : Graph->GetPathName(Blueprint);
            };
            TArray<UEdGraph*> Graphs;
            Blueprint->GetAllGraphs(Graphs);
            for (const UEdGraph* Graph : Graphs)
            {
                if (!Graph || (GraphScope.IsValid() && Graph->GraphGuid != GraphScope)) continue;
                const FString GraphKey = TEXT("graph/") + GraphId(Graph);
                Add(GraphKey + TEXT("/path"), Graph->GetPathName(Blueprint));
                for (const UEdGraphNode* Node : Graph->Nodes)
                {
                    if (!Node) continue;
                    const FString NodeKey = GraphKey + TEXT("/node/") + NodeId(Node);
                    Add(NodeKey + TEXT("/class"), PathOf(Node->GetClass()));
                    Add(NodeKey + TEXT("/position"), FString::Printf(TEXT("%d,%d"), Node->NodePosX, Node->NodePosY));
                    Add(NodeKey + TEXT("/comment"), Node->NodeComment);
                    for (const TCHAR* Field : {TEXT("FunctionReference"), TEXT("VariableReference"), TEXT("EventReference"),
                        TEXT("MacroGraphReference"), TEXT("CustomFunctionName"), TEXT("TargetType"), TEXT("EnabledState")})
                        if (const FProperty* Property = FindFProperty<FProperty>(Node->GetClass(), Field))
                            Export(Node, Property, NodeKey + TEXT("/property/") + Field);
                    for (TFieldIterator<FProperty> Property(Node->GetClass()); Property; ++Property)
                        if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
                            Export(Node, *Property, NodeKey + TEXT("/property/") + Property->GetName());
                    for (const UEdGraphPin* Pin : Node->Pins)
                    {
                        if (!Pin) continue;
                        const FString Key = NodeKey + TEXT("/pin/") + PinId(Pin);
                        Add(Key + TEXT("/name"), Pin->PinName.ToString());
                        Add(Key + TEXT("/direction"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
                        FString Type;
                        FEdGraphPinType::StaticStruct()->ExportText(Type, &Pin->PinType, nullptr, nullptr, PPF_None, nullptr);
                        Add(Key + TEXT("/type"), MoveTemp(Type));
                        Add(Key + TEXT("/default"), UnrealMCP::BlueprintGraphEditToolUtils::GetEffectivePinDefaultValue(Pin));
                        if (Pin->ParentPin) Add(Key + TEXT("/parentPin"), PinId(Pin->ParentPin));
                        if (Pin->bOrphanedPin) Add(Key + TEXT("/orphaned"), TEXT("true"));
                        if (Pin->Direction != EGPD_Output) continue;
                        for (const UEdGraphPin* Linked : Pin->LinkedTo)
                        {
                            const UEdGraphNode* Other = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
                            if (Other && Other->GetGraph())
                                Add(Key + TEXT("/link/") + GraphId(Other->GetGraph()) + TEXT("/") + NodeId(Other) + TEXT("/") + PinId(Linked), TEXT("connected"));
                        }
                    }
                }
            }
        }
        if (const USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
        {
            auto Identity = [](const USCS_Node* Node)
            {
                return Node->VariableGuid.IsValid() ? Node->VariableGuid.ToString() : Node->GetVariableName().ToString();
            };
            for (const USCS_Node* Node : SCS->GetAllNodes())
            {
                if (!Node) continue;
                const FString Prefix = TEXT("component/") + Identity(Node);
                Add(Prefix + TEXT("/name"), Node->GetVariableName().ToString());
                Add(Prefix + TEXT("/attachSocket"), Node->AttachToName.ToString());
                Add(Prefix + TEXT("/externalParent"), Node->ParentComponentOwnerClassName.ToString() + TEXT(":") + Node->ParentComponentOrVariableName.ToString());
                for (const USCS_Node* Child : Node->GetChildNodes())
                    if (Child) Add(TEXT("component/") + Identity(Child) + TEXT("/parent"), Identity(Node));
                AddTemplate(Prefix, Node->ComponentTemplate);
            }
        }
        if (auto* Handler = Blueprint->GetInheritableComponentHandler(false))
            for (auto It = Handler->CreateRecordIterator(); It; ++It)
            {
                const FString Prefix = TEXT("componentOverride/") + It->ComponentKey.GetAssociatedGuid().ToString();
                Add(Prefix + TEXT("/name"), It->ComponentKey.GetSCSVariableName().ToString());
                Add(Prefix + TEXT("/owner"), PathOf(It->ComponentKey.GetComponentOwner()));
                AddTemplate(Prefix, It->ComponentTemplate);
            }
        return Fields;
    }

    bool ReadLfsPointer(const FString& Text, FString& ObjectId, int64& Size)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines, true);
        if (Lines.Num() != 3 || Lines[0] != TEXT("version https://git-lfs.github.com/spec/v1")
            || !Lines[1].RemoveFromStart(TEXT("oid sha256:")) || !Lines[2].RemoveFromStart(TEXT("size ")))
            return false;
        ObjectId = Lines[1];
        return ObjectId.Len() == 64 && IsGitObjectId(ObjectId)
            && LexTryParseString(Size, *Lines[2]) && Size > 0 && Size <= MaxGitOutput;
    }

    bool ReadCachedLfsObject(const FString& ProjectDir, const FString& ObjectId, int64 Size,
        TArray<uint8>& Payload, FString& Error)
    {
        FString Environment;
        if (!ReadGitText(ProjectDir, TEXT("lfs env"), Environment))
        { Error = TEXT("Git LFS is required to locate the historical object cache."); return false; }
        TArray<FString> Lines;
        Environment.ParseIntoArrayLines(Lines, true);
        for (FString& Line : Lines)
            if (Line.RemoveFromStart(TEXT("LocalMediaDir=")) && !Line.IsEmpty() && !FPaths::IsRelative(Line))
            {
                const FString Filename = Line / ObjectId.Left(2) / ObjectId.Mid(2, 2) / ObjectId;
                if (IFileManager::Get().FileSize(*Filename) == Size
                    && FFileHelper::LoadFileToArray(Payload, *Filename) && Payload.Num() == Size) return true;
                break;
            }
        Error = FString::Printf(TEXT("Historical Git LFS object %s is missing locally or has the wrong size. Cache the exact object and retry."), *ObjectId);
        return false;
    }

    FString ResolveStageCommit(const FString& ProjectDir, int32 Side, const FString& RepoPath, const FString& BlobId)
    {
        FString Commit, AtPath;
        const TCHAR* Query = Side == 0 ? TEXT("merge-base HEAD MERGE_HEAD")
            : Side == 1 ? TEXT("rev-parse --verify HEAD^{commit}") : TEXT("rev-parse --verify MERGE_HEAD^{commit}");
        if (ReadGitText(ProjectDir, Query, Commit) && IsGitObjectId(Commit)
            && ReadGitText(ProjectDir, FString::Printf(TEXT("rev-parse --verify --quiet \"%s:%s\""), *Commit, *RepoPath), AtPath)
            && AtPath == BlobId)
            return Commit;
        return FString();
    }

    bool MaterializeHistoricalDependencies(const FString& ProjectDir, const FString& Commit, const FString& RootFile,
        const FString& TempDir, const FString& MountRoot, FLinkerInstancingContext& Context, TArray<FString>& Files,
        TArray<FString>& Missing, TArray<FString>& Resolved, FString& Error)
    {
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FString> Pending{RootFile};
        TSet<FName> Seen;
        int64 Bytes = 0;
        for (int32 Index = 0; Index < Pending.Num(); ++Index)
        {
            IAssetRegistry::FLoadPackageRegistryData Metadata(true);
            Registry.LoadPackageRegistryData(Pending[Index], Metadata);
            for (const FName Dependency : Metadata.DataDependencies)
            {
                const FString Name = Dependency.ToString();
                if (Seen.Contains(Dependency) || !Name.StartsWith(TEXT("/")) || Name.StartsWith(TEXT("/Script/"))) continue;
                Seen.Add(Dependency);
                if (FindPackage(nullptr, *Name) || FPackageName::DoesPackageExist(Name)) continue;
                FString RepoPath, BlobId;
                const FString Relative = Name.Mid(6) + TEXT(".uasset");
                TArray<uint8> Blob;
                if (Commit.IsEmpty() || !Name.StartsWith(TEXT("/Game/")) || !SafeContentPath(TEXT("Content/") + Relative)
                    || !ResolveRepoPath(ProjectDir, TEXT("Content/") + Relative, RepoPath, Error)
                    || !ReadGitText(ProjectDir, FString::Printf(TEXT("rev-parse --verify --quiet \"%s:%s\""), *Commit, *RepoPath), BlobId)
                    || !IsGitObjectId(BlobId) || !RunGit(ProjectDir, TEXT("cat-file blob ") + BlobId, Blob) || Blob.IsEmpty())
                {
                    Missing.AddUnique(Name);
                    continue;
                }
                FString LfsId;
                int64 LfsSize = 0;
                static const ANSICHAR Prefix[] = "version https://git-lfs.github.com/spec/v1";
                if (Blob.Num() >= sizeof(Prefix) - 1 && FMemory::Memcmp(Blob.GetData(), Prefix, sizeof(Prefix) - 1) == 0)
                {
                    // Dependency pointers must be verified before use, just like the root package.
                    const FUTF8ToTCHAR Pointer(reinterpret_cast<const ANSICHAR*>(Blob.GetData()), Blob.Num());
                    if (Blob.Num() > 1024 || !ReadLfsPointer(FString(Pointer.Length(), Pointer.Get()), LfsId, LfsSize))
                    { Missing.AddUnique(Name); continue; }
                    TArray<uint8> Payload;
                    if (!ReadCachedLfsObject(ProjectDir, LfsId, LfsSize, Payload, Error))
                    { Missing.AddUnique(Name); continue; }
                    Blob = MoveTemp(Payload);
                }
                Bytes += Blob.Num();
                if (Resolved.Num() >= 64 || Bytes > 256ll * 1024 * 1024)
                { Error = TEXT("Historical dependency recovery exceeded 64 packages or 256 MiB."); return false; }
                const FString Filename = TempDir / TEXT("Dependencies") / Relative;
                if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true)
                    || !FFileHelper::SaveArrayToFile(Blob, *Filename))
                { Error = TEXT("Could not materialize a historical dependency."); return false; }
                Files.Add(Filename);
                if (!LfsId.IsEmpty())
                {
                    const FString PointerFile = Filename + TEXT(".pointer");
                    ON_SCOPE_EXIT { IFileManager::Get().Delete(*PointerFile, false, true); };
                    const FString Canonical = FString::Printf(TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:%s\nsize %lld\n"),
                        *LfsId, static_cast<long long>(LfsSize));
                    TArray<uint8> Ignored;
                    if (!FFileHelper::SaveStringToFile(Canonical, *PointerFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
                        || !RunGit(ProjectDir, FString::Printf(TEXT("lfs pointer --file=\"%s\" --pointer=\"%s\""), *Filename, *PointerFile), Ignored))
                    { Missing.AddUnique(Name); continue; }
                }
                Pending.Add(Filename);
                Context.AddPackageMapping(Dependency, FName(*(MountRoot + Name.Mid(6))));
                Resolved.Add(Name);
            }
        }
        Missing.Sort();
        Resolved.Sort();
        if (Missing.IsEmpty()) return true;
        Error = TEXT("Saved package references dependencies unavailable from the active project or verified Git revision.");
        return false;
    }

    bool WithHistoricalBlueprint(const FString& ProjectDir, const FString& AssetPath, const TArray<uint8>& Blob,
        TFunctionRef<bool(UBlueprint*)> Read, FString& Error, TArray<FString>* MissingDependencies = nullptr,
        const FString& Commit = FString(), TArray<FString>* RecoveredDependencies = nullptr, FString* DependencyMount = nullptr)
    {
        static const ANSICHAR LfsPrefix[] = "version https://git-lfs.github.com/spec/v1";
        FString LfsId;
        int64 LfsSize = 0;
        TArray<uint8> LfsPayload;
        if (Blob.Num() >= sizeof(LfsPrefix) - 1 && FMemory::Memcmp(Blob.GetData(), LfsPrefix, sizeof(LfsPrefix) - 1) == 0)
        {
            if (Blob.Num() > 1024)
            { Error = TEXT("Historical Git LFS pointer exceeds the supported metadata size."); return false; }
            const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Blob.GetData()), Blob.Num());
            if (!ReadLfsPointer(FString(Text.Length(), Text.Get()), LfsId, LfsSize))
            { Error = TEXT("Historical Git LFS pointer is invalid, uses an extension, or exceeds the 64 MiB limit."); return false; }
            if (!ReadCachedLfsObject(ProjectDir, LfsId, LfsSize, LfsPayload, Error)) return false;
        }

        const FString TempDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealMCP/MergeDiff") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString TempFile = TempDir / FPaths::GetCleanFilename(AssetPath);
        if (!IFileManager::Get().MakeDirectory(*TempDir, true))
        { Error = TEXT("Could not create a temporary revision folder."); return false; }
        ON_SCOPE_EXIT
        {
            IFileManager::Get().Delete(*TempFile, false, true);
            IFileManager::Get().DeleteDirectory(*TempDir, false, false);
        };
        if (!FFileHelper::SaveArrayToFile(LfsId.IsEmpty() ? Blob : LfsPayload, *TempFile))
        { Error = TEXT("Could not write the temporary revision file."); return false; }

        if (!LfsId.IsEmpty())
        {
            const FString ExpectedFile = TempDir / TEXT("expected-lfs-pointer");
            ON_SCOPE_EXIT { IFileManager::Get().Delete(*ExpectedFile, false, true); };
            const FString Canonical = FString::Printf(TEXT("%s\noid sha256:%s\nsize %lld\n"),
                TEXT("version https://git-lfs.github.com/spec/v1"), *LfsId, static_cast<long long>(LfsSize));
            TArray<uint8> Ignored;
            if (!FFileHelper::SaveStringToFile(Canonical, *ExpectedFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
                || !RunGit(ProjectDir, FString::Printf(TEXT("lfs pointer --file=\"%s\" --pointer=\"%s\""), *TempFile, *ExpectedFile), Ignored))
            { Error = TEXT("Cached historical Git LFS object failed SHA-256 verification."); return false; }
        }

        const FString MountRoot = TEXT("/UnrealMCPHistory") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
        const FString MountDirectory = TempDir / TEXT("Dependencies/");
        TArray<FString> DependencyFiles, Recovered;
        FLinkerInstancingContext Context;
        bool bMounted = false;
        ON_SCOPE_EXIT
        {
            if (bMounted)
            {
                for (const FString& Name : Recovered)
                    if (UPackage* Dependency = FindPackage(nullptr, *(MountRoot + Name.Mid(6))))
                    {
                        ResetLoaders(Dependency);
                        Dependency->SetDirtyFlag(false);
                    }
                FPackageName::UnRegisterMountPoint(MountRoot, MountDirectory);
            }
            for (const FString& File : DependencyFiles) IFileManager::Get().Delete(*File, false, true);
            if (!DependencyFiles.IsEmpty()) IFileManager::Get().DeleteDirectory(*MountDirectory, false, true);
        };
        if (MissingDependencies)
        {
            if (!MaterializeHistoricalDependencies(ProjectDir, Commit, TempFile, TempDir, MountRoot, Context,
                DependencyFiles, *MissingDependencies, Recovered, Error))
                return false;
            if (RecoveredDependencies) *RecoveredDependencies = Recovered;
            if (DependencyMount && !Recovered.IsEmpty()) *DependencyMount = MountRoot;
        }

        const FPackagePath Temporary = FPackagePath::FromLocalPath(TempFile);
        const FPackagePath Original = FPackagePath::FromLocalPath(ProjectDir / AssetPath);
        if (!FPackageName::IsTempPackage(Temporary.GetPackageName()))
        { Error = TEXT("Temporary file did not resolve to an isolated Unreal diff package."); return false; }
        UPackage* Package = nullptr;
        if (Recovered.IsEmpty()) Package = DiffUtils::LoadPackageForDiff(Temporary, Original);
        else
        {
            FPackageName::RegisterMountPoint(MountRoot, MountDirectory);
            bMounted = true;
            Package = LoadPackage(nullptr, Temporary, LOAD_ForDiff | LOAD_DisableCompileOnLoad, nullptr, &Context, &Original);
        }
        if (!Package) { Error = TEXT("Could not load the historical package. Check its dependencies and engine version."); return false; }
        ON_SCOPE_EXIT { ResetLoaders(Package); Package->SetDirtyFlag(false); };

        UBlueprint* Blueprint = nullptr;
        TArray<UObject*> PackageObjects;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
        GetObjectsWithOuter(Package, PackageObjects, false);
#else
        GetObjectsWithOuter(Package, PackageObjects, EGetObjectsFlags::None);
#endif
        for (UObject* Object : PackageObjects)
        {
            if (auto* Candidate = Cast<UBlueprint>(Object))
            {
                if (Blueprint) { Error = TEXT("The historical package contains multiple Blueprints."); return false; }
                Blueprint = Candidate;
            }
        }
        if (!Blueprint) { Error = TEXT("The historical package does not contain a Blueprint."); return false; }
        return Read(Blueprint);
    }

    bool Equal(const FString* A, const FString* B)
    {
        return A && B ? *A == *B : A == B;
    }
}

FCompareBlueprintMergeStructureTool::FCompareBlueprintMergeStructureTool(bool bInCompareSnapshots)
    : FMCPToolBase(bInCompareSnapshots ? TEXT("CompareBlueprintGitSnapshots") : TEXT("CompareBlueprintMergeStructure"),
        bInCompareSnapshots ? TEXT("Compare Blueprint declarations, defaults, components, nodes and wiring between two Git or saved worktree snapshots.")
        : TEXT("Read-only three-way Blueprint comparison from exact Git conflict stages. Pages declarations, defaults, components, direct references and optional graph details.")),
      bCompareSnapshots(bInCompareSnapshots)
{
}

UnrealMCP::FMCPResponse FCompareBlueprintMergeStructureTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString Path, ExpectedRevision, Cursor, CursorRevision, OutputMode = TEXT("compact");
    int32 Offset = 0, Limit = 50;
    bool bIncludeGraphBodies = bCompareSnapshots;
    const int32 SideCount = bCompareSnapshots ? 2 : 3;
    const TCHAR* SnapshotSides[] = {TEXT("before"), TEXT("after")};
    const TCHAR* const* Labels = bCompareSnapshots ? SnapshotSides : Sides;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("path"), Path) || !SafeContentPath(Path)
        || (Request.Params->HasField(TEXT("offset")) && !Request.Params->TryGetNumberField(TEXT("offset"), Offset))
        || (Request.Params->HasField(TEXT("limit")) && !Request.Params->TryGetNumberField(TEXT("limit"), Limit))
        || (Request.Params->HasField(TEXT("expectedStageRevision")) && !Request.Params->TryGetStringField(TEXT("expectedStageRevision"), ExpectedRevision))
        || (Request.Params->HasField(TEXT("outputMode")) && !Request.Params->TryGetStringField(TEXT("outputMode"), OutputMode))
        || (Request.Params->HasField(TEXT("includeGraphBodies")) && !Request.Params->TryGetBoolField(TEXT("includeGraphBodies"), bIncludeGraphBodies))
        || (OutputMode != TEXT("compact") && OutputMode != TEXT("full"))
        || Offset < 0 || Limit < 1 || Limit > 200 || (Offset > 0 && ExpectedRevision.IsEmpty()))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Use a project-relative Content/*.uasset path, limit 1..200, outputMode compact or full, and the previous stage revision for later pages."));
    if (Request.Params->HasField(TEXT("cursor")))
    {
        FString CursorOffset;
        if (Request.Params->HasField(TEXT("offset")) || !Request.Params->TryGetStringField(TEXT("cursor"), Cursor)
            || !Cursor.Split(TEXT(":"), &CursorOffset, &CursorRevision) || CursorRevision.Len() != 32
            || !LexTryParseString(Offset, *CursorOffset) || Offset <= 0)
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Use the exact nextCursor with the same path, stagePaths and graph scope, without offset."));
    }
    FString GraphText, FocusText;
    FGuid GraphScope, FocusNode;
    if ((Request.Params->HasField(TEXT("graphGuid")) && (!Request.Params->TryGetStringField(TEXT("graphGuid"), GraphText)
        || !FGuid::Parse(GraphText, GraphScope) || !GraphScope.IsValid()))
        || (Request.Params->HasField(TEXT("focusNodeGuid")) && (!GraphScope.IsValid()
        || !Request.Params->TryGetStringField(TEXT("focusNodeGuid"), FocusText) || !FGuid::Parse(FocusText, FocusNode) || !FocusNode.IsValid())))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide a valid graphGuid and optional focusNodeGuid."));
    bIncludeGraphBodies |= GraphScope.IsValid();
    const bool bCompact = OutputMode == TEXT("compact");

    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    FString RepoPaths[3], StagePaths[3] = {Path, Path, Path}, Ids[3], Error;
    FString SnapshotSources[2], SnapshotCommits[2];
    if (bCompareSnapshots)
    {
        for (int32 Side = 0; Side < SideCount; ++Side)
        {
            const TSharedPtr<FJsonObject>* Endpoint = nullptr;
            if (!Request.Params->TryGetObjectField(Labels[Side], Endpoint)
                || !(*Endpoint)->TryGetStringField(TEXT("source"), SnapshotSources[Side])
                || ((*Endpoint)->HasField(TEXT("path")) && (!(*Endpoint)->TryGetStringField(TEXT("path"), StagePaths[Side]) || !SafeContentPath(StagePaths[Side]))))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Each endpoint requires source and accepts a project-relative Content/*.uasset path override."));
            const FString& Source = SnapshotSources[Side];
            if (Source == TEXT("commit"))
            {
                FString Commit;
                if (!(*Endpoint)->TryGetStringField(TEXT("commit"), Commit) || !IsGitObjectId(Commit)
                    || !ReadGitText(ProjectDir, TEXT("rev-parse --verify --quiet ") + Commit + TEXT("^{commit}"), SnapshotCommits[Side])
                    || !IsGitObjectId(SnapshotCommits[Side]))
                    return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Commit endpoints require a full locally available commit hash."));
            }
            else if ((Source != TEXT("head") && Source != TEXT("index") && Source != TEXT("worktree")) || (*Endpoint)->HasField(TEXT("commit")))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Use source commit, head, index or worktree. Only source commit accepts a commit hash."));
            else if (Source == TEXT("head") && (!ReadGitText(ProjectDir, TEXT("rev-parse --verify --quiet HEAD^{commit}"), SnapshotCommits[Side])
                || !IsGitObjectId(SnapshotCommits[Side])))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("HEAD does not resolve to a commit."));
        }
    }
    const TSharedPtr<FJsonObject>* PathOverrides = nullptr;
    if (!bCompareSnapshots && Request.Params->HasField(TEXT("stagePaths")))
    {
        if (!Request.Params->TryGetObjectField(TEXT("stagePaths"), PathOverrides))
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("stagePaths must map base, current or incoming to a project-relative .uasset path."));
        for (const auto& Field : (*PathOverrides)->Values)
        {
            const FString Key(Field.Key);
            int32 Side = 0;
            while (Side < 3 && Key != Sides[Side]) ++Side;
            if (Side == 3 || !Field.Value || !Field.Value->TryGetString(StagePaths[Side]) || !SafeContentPath(StagePaths[Side]))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Unknown side or invalid path in stagePaths."));
        }
    }
    for (int32 Side = 0; Side < 3; ++Side)
        if (!ResolveRepoPath(ProjectDir, StagePaths[Side], RepoPaths[Side], Error))
            return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    TArray<uint8> Blobs[3];
    auto ReadSnapshots = [&](FString (&OutIds)[3], TArray<uint8>* SavedBytes)
    {
        for (int32 Side = 0; Side < SideCount; ++Side)
        {
            if (SnapshotSources[Side] == TEXT("worktree"))
            {
                const FString File = FPaths::Combine(ProjectDir, StagePaths[Side]);
                const int64 Size = IFileManager::Get().FileSize(*File);
                TArray<uint8> Bytes;
                if (Size <= 0 || Size > MaxGitOutput || !FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.Num() != Size)
                    return false;
                FMD5 Hash;
                Hash.Update(Bytes.GetData(), Bytes.Num());
                OutIds[Side] = FinishEvidence(Hash);
                if (SavedBytes) SavedBytes[Side] = MoveTemp(Bytes);
            }
            else
            {
                const FString Spec = SnapshotSources[Side] == TEXT("index")
                    ? TEXT(":0:") + RepoPaths[Side] : SnapshotCommits[Side] + TEXT(":") + RepoPaths[Side];
                if (!ReadGitText(ProjectDir, FString::Printf(TEXT("rev-parse --verify --quiet \"%s\""), *Spec), OutIds[Side])
                    || !IsGitObjectId(OutIds[Side])) return false;
            }
        }
        return true;
    };
    if (bCompareSnapshots ? !ReadSnapshots(Ids, Blobs) : !ReadReviewStages(ProjectDir, RepoPaths, Ids, Error))
        return BuildError(Request, EMCPErrorCode::InvalidParams, bCompareSnapshots
            ? TEXT("Both Blueprint snapshots must exist and be readable. For an unmerged index path select a commit instead.") : Error);
    const bool bRenamed = StagePaths[0] != Path || StagePaths[1] != Path || StagePaths[2] != Path;
    const FString Revision = ReviewRevision(Path, StagePaths, Ids);
    if (!ExpectedRevision.IsEmpty() && ExpectedRevision != Revision)
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Git stages changed. Restart comparison at offset 0."));

    FStructure Structures[3];
    TSharedPtr<FJsonObject> GraphViews[3];
    bool HasDefaults[3] = {};
    bool HasReferences[3] = {};
    FString SideErrors[3];
    TArray<FString> Missing[3], Recovered[3];
    for (int32 Side = 0; Side < SideCount; ++Side)
        if (!Ids[Side].IsEmpty() && Blobs[Side].IsEmpty() && (!RunGit(ProjectDir, TEXT("cat-file blob ") + Ids[Side], Blobs[Side]) || Blobs[Side].IsEmpty()))
            SideErrors[Side] = TEXT("Could not read the saved Git blob within the 64 MiB and 10 second limits.");
    TSet<FString> Keys;
    if (!BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString&)
    {
        for (int32 Side = 0; Side < SideCount; ++Side)
        {
            if (Ids[Side].IsEmpty() || !SideErrors[Side].IsEmpty()) continue;
            FString DependencyMount;
            if (WithHistoricalBlueprint(ProjectDir, StagePaths[Side], Blobs[Side], [&](UBlueprint* Blueprint)
            {
                HasDefaults[Side] = Blueprint->GeneratedClass && Blueprint->GeneratedClass->GetDefaultObject(false);
                if (GraphScope.IsValid()) GraphViews[Side] = BuildGraphReviewView(Blueprint, GraphScope, FocusNode);
                Structures[Side] = SnapshotStructure(Blueprint, bIncludeGraphBodies, HasReferences[Side], DependencyMount, GraphScope);
                return true;
            }, SideErrors[Side], &Missing[Side], bCompareSnapshots ? SnapshotCommits[Side] : ResolveStageCommit(ProjectDir, Side, RepoPaths[Side], Ids[Side]), &Recovered[Side], &DependencyMount))
                for (const auto& Pair : Structures[Side]) Keys.Add(Pair.Key);
        }
        return true;
    }, Error)) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);

    FString AfterIds[3];
    if ((bCompareSnapshots ? !ReadSnapshots(AfterIds, nullptr) : !ReadReviewStages(ProjectDir, RepoPaths, AfterIds, Error)) || ReviewRevision(Path, StagePaths, AfterIds) != Revision)
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Git stages changed during comparison. Restart at offset 0."));

    const bool bReadable = SideErrors[0].IsEmpty() && SideErrors[1].IsEmpty() && SideErrors[2].IsEmpty();
    const bool bAsymmetric = Ids[0].IsEmpty() || Ids[1].IsEmpty() || (!bCompareSnapshots && Ids[2].IsEmpty());
    // An unreadable side is unknown. Null values are reserved for actual absence.
    if (!bReadable) Keys.Reset();
    TArray<FString> SortedKeys = Keys.Array();
    SortedKeys.Sort();
    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 Total = 0;
    auto CanCompare = [&](const bool (&Available)[3])
    {
        return bReadable && (Ids[0].IsEmpty() || Available[0]) && (Ids[1].IsEmpty() || Available[1]) && (Ids[2].IsEmpty() || Available[2]);
    };
    const bool bCompareDefaults = CanCompare(HasDefaults);
    const bool bCompareReferences = CanCompare(HasReferences);
    FMD5 EvidenceHash;
    HashEvidence(EvidenceHash, Path);
    HashEvidence(EvidenceHash, Revision);
    HashEvidence(EvidenceHash, GraphScope.ToString());
    HashEvidence(EvidenceHash, FocusNode.ToString());
    HashEvidence(EvidenceHash, bIncludeGraphBodies ? TEXT("graphs") : TEXT("structure"));
    HashEvidence(EvidenceHash, bCompareDefaults ? TEXT("defaults") : TEXT("no_defaults"));
    HashEvidence(EvidenceHash, bCompareReferences ? TEXT("references") : TEXT("no_references"));
    for (int32 Side = 0; Side < 3; ++Side)
    {
        HashEvidence(EvidenceHash, SideErrors[Side]);
        for (const FString& Dependency : Missing[Side]) HashEvidence(EvidenceHash, Dependency);
        for (const FString& Dependency : Recovered[Side]) HashEvidence(EvidenceHash, Dependency);
    }
    for (const FString& Key : SortedKeys)
    {
        if (GraphScope.IsValid() && !Key.StartsWith(TEXT("graph/"))) continue;
        if (!bCompareDefaults && Key.StartsWith(TEXT("classDefault/"))) continue;
        if (!bCompareReferences && Key.StartsWith(TEXT("reference/"))) continue;
        const FString* Values[] = {Structures[0].Find(Key), Structures[1].Find(Key), Structures[2].Find(Key)};
        if (Equal(Values[0], Values[1]) && (bCompareSnapshots || Equal(Values[0], Values[2]))) continue;
        HashEvidence(EvidenceHash, Key);
        for (const FString* Value : Values)
        {
            HashEvidence(EvidenceHash, Value ? TEXT("present") : TEXT("absent"));
            if (Value) HashEvidence(EvidenceHash, *Value);
        }
        if (Total++ < Offset || Rows.Num() >= Limit) continue;
        TArray<TSharedPtr<FJsonValue>> Cells;
        Cells.Add(MakeShared<FJsonValueString>(Key));
        for (int32 Index = 0; Index < SideCount; ++Index)
        {
            if (Values[Index]) Cells.Add(MakeShared<FJsonValueString>(*Values[Index]));
            else Cells.Add(MakeShared<FJsonValueNull>());
        }
        if (!bCompareSnapshots) Cells.Add(MakeShared<FJsonValueBoolean>(!Equal(Values[0], Values[1]) && !Equal(Values[0], Values[2]) && !Equal(Values[1], Values[2])));
        if (bCompact)
        {
            Rows.Add(MakeShared<FJsonValueArray>(MoveTemp(Cells)));
        }
        else
        {
            auto Row = MakeShared<FJsonObject>();
            Row->SetField(TEXT("identity"), Cells[0]);
            for (int32 Index = 0; Index < SideCount; ++Index) Row->SetField(Labels[Index], Cells[Index + 1]);
            if (!bCompareSnapshots) Row->SetField(TEXT("overlapCandidate"), Cells[4]);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    const FString EvidenceRevision = FinishEvidence(EvidenceHash);
    if (!CursorRevision.IsEmpty() && CursorRevision != EvidenceRevision)
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Review evidence changed. Restart from the first page."));

    auto Result = BuildBooleanResult(true);
    auto StageStates = MakeShared<FJsonObject>();
    auto Stages = MakeShared<FJsonObject>();
    auto Defaults = MakeShared<FJsonObject>();
    auto References = MakeShared<FJsonObject>();
    for (int32 Index = 0; Index < SideCount; ++Index)
    {
        auto State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("path"), StagePaths[Index]);
        if (bCompareSnapshots)
        {
            State->SetStringField(TEXT("source"), SnapshotSources[Index]);
            if (!SnapshotCommits[Index].IsEmpty()) State->SetStringField(TEXT("commit"), SnapshotCommits[Index]);
        }
        State->SetStringField(TEXT("state"), Ids[Index].IsEmpty() ? TEXT("absent") : SideErrors[Index].IsEmpty() ? TEXT("loaded") : TEXT("unreadable"));
        if (!SideErrors[Index].IsEmpty()) State->SetStringField(TEXT("error"), SideErrors[Index]);
        if (!Recovered[Index].IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> Names;
            for (const FString& Dependency : Recovered[Index]) Names.Add(MakeShared<FJsonValueString>(Dependency));
            State->SetArrayField(TEXT("recoveredHistoricalDependencies"), Names);
        }
        if (!Missing[Index].IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> Names;
            for (const FString& Dependency : Missing[Index]) Names.Add(MakeShared<FJsonValueString>(Dependency));
            State->SetArrayField(TEXT("missingDependencies"), Names);
        }
        StageStates->SetObjectField(Labels[Index], State);
        Stages->SetField(Labels[Index], Ids[Index].IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())
            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Ids[Index])));
        Defaults->SetBoolField(Labels[Index], HasDefaults[Index]);
        References->SetBoolField(Labels[Index], HasReferences[Index]);
    }
    Result->SetObjectField(bCompareSnapshots ? TEXT("snapshotIds") : TEXT("stageBlobs"), Stages);
    Result->SetObjectField(bCompareSnapshots ? TEXT("snapshotStates") : TEXT("stageStates"), StageStates);
    Result->SetBoolField(TEXT("evidenceComplete"), bReadable && bCompareDefaults && bCompareReferences);
    Result->SetBoolField(TEXT("requiresExplicitResolution"), bAsymmetric || bRenamed || !bCompareDefaults || !bCompareReferences);
    Result->SetObjectField(TEXT("classDefaultsAvailable"), Defaults);
    Result->SetBoolField(TEXT("classDefaultsCompared"), bCompareDefaults);
    Result->SetObjectField(TEXT("directReferencesAvailable"), References);
    Result->SetBoolField(TEXT("directReferencesCompared"), bCompareReferences);
    Result->SetBoolField(TEXT("graphBodiesCompared"), bIncludeGraphBodies);
    if (GraphScope.IsValid())
    {
        auto Views = MakeShared<FJsonObject>();
        for (int32 Index = 0; Index < SideCount; ++Index)
        {
            if (!GraphViews[Index])
            {
                GraphViews[Index] = MakeShared<FJsonObject>();
                GraphViews[Index]->SetStringField(TEXT("state"), Ids[Index].IsEmpty() ? TEXT("stage_absent") : TEXT("stage_unreadable"));
            }
            Views->SetObjectField(Labels[Index], GraphViews[Index]);
        }
        Result->SetObjectField(TEXT("graphViews"), Views);
        Result->SetStringField(TEXT("graphGuid"), GraphScope.ToString(EGuidFormats::DigitsWithHyphensLower));
    }
    Result->SetStringField(TEXT("stageRevision"), Revision);
    Result->SetStringField(TEXT("path"), Path);
    if (bCompact && bCompareSnapshots)
        Result->SetArrayField(TEXT("columns"), {MakeShared<FJsonValueString>(TEXT("identity")),
            MakeShared<FJsonValueString>(Labels[0]), MakeShared<FJsonValueString>(Labels[1])});
    else if (bCompact)
        Result->SetArrayField(TEXT("columns"), {MakeShared<FJsonValueString>(TEXT("identity")),
            MakeShared<FJsonValueString>(Sides[0]), MakeShared<FJsonValueString>(Sides[1]), MakeShared<FJsonValueString>(Sides[2]),
            MakeShared<FJsonValueString>(TEXT("overlapCandidate"))});
    Result->SetArrayField(TEXT("rows"), Rows);
    Result->SetNumberField(TEXT("total"), Total);
    if (Total > static_cast<int64>(Offset) + Rows.Num())
    {
        const int32 Next = Offset + Rows.Num();
        Result->SetNumberField(TEXT("nextOffset"), Next);
        Result->SetStringField(TEXT("nextCursor"), FString::FromInt(Next) + TEXT(":") + EvidenceRevision);
    }
    Result->SetStringField(TEXT("coverage"), TEXT("Declarations, function signatures and locals, editable asset/class settings and defaults, parent/interfaces, SCS and component overrides, and direct hard/soft package references when available. Optional graph rows include nodes, pins, links, editable fields and common K2 references. Available dependencies resolve from the active project. Missing project packages may be recovered from verified stage commits. Native/custom serialized state, dependency closure and runtime behavior are outside coverage."));
    FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FCompareBlueprintMergeStructureTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("path"), BuildStringProperty(TEXT("Project-relative Blueprint file path under Content, using forward slashes.")));
    Properties->SetObjectField(TEXT("cursor"), BuildStringProperty(TEXT("Pass nextCursor with the same path, stagePaths and graph scope. Replaces offset and verifies unchanged comparison evidence.")));
    auto Paths = MakeShared<FJsonObject>();
    Paths->SetStringField(TEXT("type"), TEXT("object"));
    auto SidePaths = MakeShared<FJsonObject>();
    for (const TCHAR* Side : Sides) SidePaths->SetObjectField(Side, BuildStringProperty(TEXT("Exact project-relative path of this unmerged Git stage.")));
    Paths->SetObjectField(TEXT("properties"), SidePaths);
    Paths->SetBoolField(TEXT("additionalProperties"), false);
    if (bCompareSnapshots)
    {
        auto Endpoint = MakeShared<FJsonObject>();
        Endpoint->SetStringField(TEXT("type"), TEXT("object"));
        auto Fields = MakeShared<FJsonObject>();
        Fields->SetObjectField(TEXT("source"), BuildStringProperty(TEXT("head, index (stage 0), worktree (saved file), or commit.")));
        Fields->SetObjectField(TEXT("path"), BuildStringProperty(TEXT("Optional project-relative path override, including the historical name after a rename.")));
        Fields->SetObjectField(TEXT("commit"), BuildStringProperty(TEXT("Full locally available commit hash for source commit.")));
        Endpoint->SetObjectField(TEXT("properties"), Fields);
        Endpoint->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("source"))});
        Properties->SetObjectField(TEXT("before"), Endpoint);
        Properties->SetObjectField(TEXT("after"), Endpoint);
    }
    else Properties->SetObjectField(TEXT("stagePaths"), Paths);
    Properties->SetObjectField(TEXT("expectedStageRevision"), BuildStringProperty(TEXT("Stage revision returned on the first page; required when offset is nonzero.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Compare only this exact graph and return bounded named execution links for each stage.")));
    Properties->SetObjectField(TEXT("focusNodeGuid"), BuildStringProperty(TEXT("With graphGuid, return exact incoming and outgoing execution and data links for this node on each side.")));
    Properties->SetObjectField(TEXT("includeGraphBodies"), BuildBoolProperty(TEXT("Defaults false. Include graph nodes, pins, links, editable fields and common K2 references. Keep this setting unchanged between pages.")));
    auto OutputMode = BuildStringProperty(TEXT("compact (default): rows are arrays in columns order, with null for absent values. full: each row is an object."));
    OutputMode->SetArrayField(TEXT("enum"), {MakeShared<FJsonValueString>(TEXT("compact")), MakeShared<FJsonValueString>(TEXT("full"))});
    Properties->SetObjectField(TEXT("outputMode"), OutputMode);
    for (const TCHAR* Field : {TEXT("offset"), TEXT("limit")})
    {
        auto Number = MakeShared<FJsonObject>();
        Number->SetStringField(TEXT("type"), TEXT("integer"));
        Number->SetStringField(TEXT("description"), FString(Field) == TEXT("limit") ? TEXT("Page size 1..200; default 50.") : TEXT("Zero-based offset; default 0."));
        Properties->SetObjectField(Field, Number);
    }
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required = {MakeShared<FJsonValueString>(TEXT("path"))};
    if (bCompareSnapshots)
    {
        Required.Add(MakeShared<FJsonValueString>(TEXT("before")));
        Required.Add(MakeShared<FJsonValueString>(TEXT("after")));
    }
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}

FApplyBlueprintMergeTool::FApplyBlueprintMergeTool(EOperation InOperation)
    : FMCPToolBase(InOperation == EOperation::Structure ? TEXT("ApplyBlueprintMergeStructure")
        : InOperation == EOperation::AddInterface ? TEXT("ApplyBlueprintMergeAddInterface")
        : InOperation == EOperation::AddVariable ? TEXT("ApplyBlueprintMergeAddVariable")
        : InOperation == EOperation::ComponentDefaults ? TEXT("ApplyBlueprintMergeComponentDefaults") : TEXT("ApplyBlueprintMergeClassDefaults"),
        InOperation == EOperation::Structure
            ? TEXT("Preview and apply a guarded batch of Blueprint structural edits with three Git conflict stages, dependent compilation and per-package receipts.")
        : InOperation == EOperation::AddInterface
            ? TEXT("Preview or attach a Blueprint interface on a clean working Blueprint with three Git conflict stages. Apply requires the preview token. Never stages files.")
            : InOperation == EOperation::AddVariable
                ? TEXT("Preview or add a typed member variable to a clean Blueprint with three Git conflict stages and no known descendants. Compile and verify the declaration before optional save.")
            : InOperation == EOperation::ComponentDefaults
            ? TEXT("Preview or apply defaults on a local component or existing inherited override in a clean Blueprint with three Git conflict stages. Apply requires the preview token. Never stages files.")
            : TEXT("Preview or apply explicit class-default edits for a clean Blueprint with all three Git conflict stages. Apply requires the preview token. Never stages files.")),
      Operation(InOperation)
{
}

UnrealMCP::FMCPResponse FApplyBlueprintMergeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    const bool bComponentDefaults = Operation == EOperation::ComponentDefaults;
    const bool bAddInterface = Operation == EOperation::AddInterface;
    const bool bStructure = Operation == EOperation::Structure;
    const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
    const bool bAddVariable = Operation == EOperation::AddVariable;
    FString Path, ExpectedPreview, ComponentName, OverrideGuidText, InterfacePath;
    FGuid OverrideGuid;
    const TSharedPtr<FJsonObject>* Values = nullptr;
    bool bDryRun = true, bConfirm = false, bSave = false;
    bool bCreateOverride = false;
    const auto& Params = Request.Params;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("path"), Path) || !SafeContentPath(Path)
        || (bComponentDefaults && (!Params->TryGetStringField(TEXT("componentName"), ComponentName) || ComponentName.IsEmpty()))
        || (Params->HasField(TEXT("inheritedOverrideGuid")) && (!bComponentDefaults
            || !Params->TryGetStringField(TEXT("inheritedOverrideGuid"), OverrideGuidText)
            || !FGuid::Parse(OverrideGuidText, OverrideGuid) || !OverrideGuid.IsValid()))
        || (Params->HasField(TEXT("createInheritedOverride")) && (!bComponentDefaults
            || !Params->TryGetBoolField(TEXT("createInheritedOverride"), bCreateOverride)))
        || (bCreateOverride && !OverrideGuid.IsValid())
        || (bAddInterface && (!Params->TryGetStringField(TEXT("interfaceClassPath"), InterfacePath) || InterfacePath.IsEmpty()))
        || (bStructure && (!Params->TryGetArrayField(TEXT("operations"), Operations) || !Operations
            || Operations->IsEmpty() || Operations->Num() > 16
            || Operations->ContainsByPredicate([](const auto& Value)
            {
                if (!Value || Value->Type != EJson::Object || !Value->AsObject() || Value->AsObject()->Values.Num() > 8) return true;
                for (const auto& Field : Value->AsObject()->Values)
                    if (!Field.Value || Field.Value->Type != EJson::String || Field.Value->AsString().Len() > 4096) return true;
                return false;
            })))
        || (!bStructure && !bAddInterface && (!Params->TryGetObjectField(bAddVariable ? TEXT("declaration") : TEXT("values"), Values) || !Values || !Values->IsValid()
            || (*Values)->Values.IsEmpty() || (*Values)->Values.Num() > 64))
        || (Params->HasField(TEXT("dryRun")) && !Params->TryGetBoolField(TEXT("dryRun"), bDryRun))
        || (Params->HasField(TEXT("confirm")) && !Params->TryGetBoolField(TEXT("confirm"), bConfirm))
        || (Params->HasField(TEXT("save")) && !Params->TryGetBoolField(TEXT("save"), bSave))
        || (Params->HasField(TEXT("previewToken")) && !Params->TryGetStringField(TEXT("previewToken"), ExpectedPreview))
        || (!bDryRun && (!bConfirm || ExpectedPreview.IsEmpty())))
        return BuildError(Request, EMCPErrorCode::InvalidParams,
            TEXT("Provide a project-relative Blueprint path and the operation's required fields. Apply requires dryRun=false, confirm=true and previewToken."));

    TArray<TSharedPtr<FJsonValue>> SortedOperations;
    if (Operations)
        for (const auto& Value : *Operations)
        {
            const auto& Op = Value->AsObject();
            TArray<FString> Keys;
            for (const auto& Field : Op->Values) Keys.Add(FString(Field.Key));
            Keys.Sort();
            auto Sorted = MakeShared<FJsonObject>();
            for (const FString& Key : Keys) Sorted->SetStringField(Key, Op->GetStringField(Key));
            SortedOperations.Add(MakeShared<FJsonValueObject>(Sorted));
        }
    auto SortedValues = MakeShared<FJsonObject>();
    const FAddBlueprintVariableTool VariableTool;
    const auto VariableSchema = bAddVariable ? VariableTool.GetDefinition().InputSchema->GetObjectField(TEXT("properties")) : nullptr;
    TArray<FString> Names;
    if (Values) for (const auto& Pair : (*Values)->Values) Names.Add(FString(Pair.Key));
    Names.Sort();
    int64 TextLength = 0;
    for (const FString& PropertyName : Names)
    {
        if (bAddVariable)
        {
            if (!VariableSchema->HasField(PropertyName) || PropertyName == TEXT("objectPath")
                || PropertyName == TEXT("dryRun") || PropertyName == TEXT("compileAfterEdit") || PropertyName == TEXT("saveAfterEdit"))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Unknown variable declaration field."));
            if (PropertyName == TEXT("isArray") || PropertyName == TEXT("instanceEditable"))
            {
                bool bValue;
                if (!(*Values)->TryGetBoolField(PropertyName, bValue))
                    return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Variable declaration flags must be booleans."));
                SortedValues->SetBoolField(PropertyName, bValue);
                continue;
            }
        }
        FString Value;
        if (PropertyName.IsEmpty() || !(*Values)->TryGetStringField(PropertyName, Value))
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Each property requires an Unreal export-text string."));
        TextLength += PropertyName.Len() + static_cast<int64>(Value.Len());
        if (TextLength > 131072)
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Class-default plan exceeds 128K characters."));
        SortedValues->SetStringField(PropertyName, Value);
    }

    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    const FString Filename = ProjectDir / Path;
    FString RepoPath, PackageName, Error;
    if (!ResolveRepoPath(ProjectDir, Path, RepoPath, Error))
        return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    if (!FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName) || !PackageName.StartsWith(TEXT("/Game/")))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Path must resolve to a Blueprint package under /Game."));
    const FString ObjectPath = PackageName + TEXT(".") + FPaths::GetBaseFilename(Path);

    auto Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("path"), Path);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("staged"), false);
    if (bComponentDefaults) Result->SetStringField(TEXT("componentName"), ComponentName);
    if (OverrideGuid.IsValid()) Result->SetStringField(TEXT("inheritedOverrideGuid"), OverrideGuid.ToString());
    FMCPResponse Response;
    Response.Id = Request.Id;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        FString Ids[3];
        if (!ReadStageIds(ProjectDir, RepoPath, Ids, OutError)) return false;
        const FString Revision = StageRevision(Ids);
        Result->SetStringField(TEXT("stageRevision"), Revision);
        auto ReadWorkingHash = [&]()
        {
            const int64 Size = IFileManager::Get().FileSize(*Filename);
            TArray<uint8> Bytes;
            return Size > 0 && Size <= MaxGitOutput && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes.Num() == Size
                ? FMD5::HashBytes(Bytes.GetData(), Bytes.Num()) : FString();
        };
        const FString Hash = ReadWorkingHash();
        if (Hash.IsEmpty()) { OutError = TEXT("Working Blueprint file is missing, unreadable or exceeds 64 MiB."); return false; }

        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
        if (!Blueprint || !Blueprint->GetPathName().Equals(ObjectPath, ESearchCase::IgnoreCase)
            || Blueprint->GetOutermost()->IsDirty())
        { OutError = TEXT("The exact working Blueprint must exist and have no unsaved changes."); return false; }

        FMCPRequest Native;
        Native.Id = Request.Id;
        Native.Params = MakeShared<FJsonObject>();
        Native.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        TArray<FString> InterfaceSignatures;
        if (bAddInterface)
        {
            const UClass* Interface = LoadObject<UClass>(nullptr, *InterfacePath);
            if (!Interface || !Interface->HasAnyClassFlags(CLASS_Interface)
                || !Interface->GetPathName().Equals(InterfacePath, ESearchCase::IgnoreCase))
            { OutError = TEXT("Provide the exact interface class path, including _C for a Blueprint interface."); return false; }
            const UPackage* InterfacePackage = Interface->GetOutermost();
            if (bSave && !InterfacePackage->HasAnyPackageFlags(PKG_CompiledIn)
                && (InterfacePackage->IsDirty() || !FPackageName::DoesPackageExist(InterfacePackage->GetName())))
            { OutError = TEXT("Save the interface asset separately before saving a Blueprint that implements it."); return false; }
            Native.Params->SetStringField(TEXT("interfaceClassPath"), Interface->GetPathName());
            for (TFieldIterator<UFunction> Function(Interface); Function; ++Function)
            {
                FString Signature = FString::Printf(TEXT("%s:%u"), *Function->GetName(), static_cast<uint32>(Function->FunctionFlags));
                for (TFieldIterator<FProperty> Parameter(*Function); Parameter && Parameter->HasAnyPropertyFlags(CPF_Parm); ++Parameter)
                    Signature += FString::Printf(TEXT("|%s:%s:%llu"), *Parameter->GetName(), *Parameter->GetCPPType(),
                        static_cast<unsigned long long>(Parameter->GetPropertyFlags()));
                InterfaceSignatures.Add(MoveTemp(Signature));
            }
            InterfaceSignatures.Sort();
        }
        else if (bAddVariable)
        {
            auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
            if (!Blueprint->GeneratedClass || Registry.IsLoadingAssets())
            { OutError = TEXT("Variable merge requires a generated class and a ready Asset Registry."); return false; }
            TSet<FTopLevelAssetPath> Descendants;
            const FTopLevelAssetPath TargetClassPath = Blueprint->GeneratedClass->GetClassPathName();
            Registry.GetDerivedClassNames({TargetClassPath}, {}, Descendants);
            Descendants.Remove(TargetClassPath);
            TArray<UClass*> Loaded;
            GetDerivedClasses(Blueprint->GeneratedClass, Loaded, true);
            if (!Descendants.IsEmpty() || Loaded.ContainsByPredicate([Blueprint](const UClass* Class)
                { return Class && !Class->HasAnyClassFlags(CLASS_NewerVersionExists) && Class->GetOutermost() != GetTransientPackage()
                    && Class->ClassGeneratedBy != Blueprint && !Class->GetOutermost()->HasAnyPackageFlags(PKG_ForDiffing); }))
            { OutError = TEXT("Variable merge refuses Blueprints with known descendants. Adding a member can rename a descendant's declaration."); return false; }
            for (const auto& Pair : SortedValues->Values) Native.Params->SetField(FString(Pair.Key), Pair.Value);
        }
        else if (bStructure) Native.Params->SetArrayField(TEXT("operations"), SortedOperations);
        else Native.Params->SetObjectField(bComponentDefaults ? TEXT("propertyDefaults") : TEXT("values"), SortedValues);
        if (bComponentDefaults) Native.Params->SetStringField(TEXT("componentName"), ComponentName);
        if (OverrideGuid.IsValid()) Native.Params->SetStringField(TEXT("inheritedOverrideGuid"), OverrideGuid.ToString());
        if (bCreateOverride) Native.Params->SetBoolField(TEXT("createInheritedOverride"), true);
        Native.Params->SetBoolField(TEXT("dryRun"), true);
        const FSetBlueprintClassDefaultsTool ClassTool;
        const FSetBlueprintComponentDefaultsTool ComponentTool;
        const FAddBlueprintInterfaceTool InterfaceTool;
        const FApplyBlueprintMergeStructureOps StructureTool;
        const FMCPToolBase& Tool = bStructure ? static_cast<const FMCPToolBase&>(StructureTool)
            : bAddVariable ? static_cast<const FMCPToolBase&>(VariableTool)
            : bAddInterface ? static_cast<const FMCPToolBase&>(InterfaceTool)
            : bComponentDefaults ? static_cast<const FMCPToolBase&>(ComponentTool) : ClassTool;
        const auto Preview = Tool.Execute(Native);
        if (!Preview.Result || !Preview.Result->GetBoolField(TEXT("success")))
        {
            if (Preview.Error.IsSet()) OutError = Preview.Error->Message;
            else if (!Preview.Result || !Preview.Result->TryGetStringField(TEXT("error"), OutError))
                OutError = TEXT("Merge preflight failed.");
            return false;
        }
        if (bAddVariable && Preview.Result->GetBoolField(TEXT("alreadyExists")))
        { OutError = TEXT("The variable already exists. Inspect the working declaration before planning a merge."); return false; }

        auto Evidence = MakeShared<FJsonObject>();
        Evidence->SetStringField(TEXT("project"), ProjectDir);
        Evidence->SetStringField(TEXT("path"), Path);
        if (bComponentDefaults) Evidence->SetStringField(TEXT("componentName"), ComponentName);
        if (OverrideGuid.IsValid()) Evidence->SetStringField(TEXT("inheritedOverrideGuid"), OverrideGuid.ToString());
        Evidence->SetStringField(TEXT("stageRevision"), Revision);
        Evidence->SetStringField(TEXT("workingHash"), Hash);
        Evidence->SetBoolField(TEXT("save"), bSave);
        if (bAddInterface)
        {
            Evidence->SetStringField(TEXT("interfaceClassPath"), Preview.Result->GetStringField(TEXT("interfaceClassPath")));
            Evidence->SetBoolField(TEXT("wouldAdd"), Preview.Result->GetBoolField(TEXT("wouldAdd")));
            Evidence->SetBoolField(TEXT("inherited"), Preview.Result->GetBoolField(TEXT("inherited")));
            TArray<TSharedPtr<FJsonValue>> Signatures;
            for (const FString& Signature : InterfaceSignatures) Signatures.Add(MakeShared<FJsonValueString>(Signature));
            Evidence->SetArrayField(TEXT("signatures"), Signatures);
        }
        else if (bStructure)
        {
            const auto& Packages = Preview.Result->GetArrayField(TEXT("packages"));
            for (int32 Index = 1; Index < Packages.Num(); ++Index)
            {
                const FString PackageName = FPackageName::ObjectPathToPackageName(Packages[Index]->AsObject()->GetStringField(TEXT("objectPath")));
                FString CallerRepoPath, Unmerged;
                const FString CallerPath = TEXT("Content/") + PackageName.Mid(6) + TEXT(".uasset");
                if (!PackageName.StartsWith(TEXT("/Game/")) || !SafeContentPath(CallerPath)
                    || !ResolveRepoPath(ProjectDir, CallerPath, CallerRepoPath, OutError)
                    || !ReadGitText(ProjectDir, FString::Printf(TEXT("ls-files -u -- \":(top,literal)%s\""), *CallerRepoPath), Unmerged))
                { OutError = FString::Printf(TEXT("Could not verify Git conflict state for dependent package %s."), *PackageName); return false; }
                if (!Unmerged.IsEmpty())
                { OutError = FString::Printf(TEXT("Dependent package has its own Git conflict: %s."), *PackageName); return false; }
            }
            Evidence->SetArrayField(TEXT("operations"), SortedOperations);
            Evidence->SetArrayField(TEXT("packages"), Packages);
            Result->SetArrayField(TEXT("operations"), SortedOperations);
            Result->SetArrayField(TEXT("packages"), Preview.Result->GetArrayField(TEXT("packages")));
            Native.Params->SetArrayField(TEXT("expectedPackages"), Preview.Result->GetArrayField(TEXT("packages")));
        }
        else if (bAddVariable) Evidence->SetObjectField(TEXT("declaration"), SortedValues);
        else
        {
            Evidence->SetObjectField(TEXT("values"), SortedValues);
            Evidence->SetArrayField(TEXT("properties"), Preview.Result->GetArrayField(TEXT("properties")));
        }
        if (bCreateOverride)
        {
            Evidence->SetBoolField(TEXT("createInheritedOverride"), true);
            Result->SetBoolField(TEXT("wouldCreateOverride"), Preview.Result->GetBoolField(TEXT("wouldCreateOverride")));
        }
        FString Serialized;
        FJsonSerializer::Serialize(Evidence, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized));
        const FTCHARToUTF8 Utf8(*Serialized);
        const FString Token = FMD5::HashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        if (!bDryRun && Token != ExpectedPreview)
        { OutError = TEXT("Preview token mismatch. The stages, working file, defaults or requested plan changed."); return false; }

        FString AfterIds[3];
        if (!ReadStageIds(ProjectDir, RepoPath, AfterIds, OutError) || StageRevision(AfterIds) != Revision
            || ReadWorkingHash() != Hash || Blueprint->GetOutermost()->IsDirty())
        { OutError = TEXT("Git stages or the working Blueprint changed during preflight. Preview again."); return false; }
        Result->SetStringField(TEXT("previewToken"), Token);
        if (bAddInterface)
        {
            Result->SetStringField(TEXT("interfaceClassPath"), Preview.Result->GetStringField(TEXT("interfaceClassPath")));
            for (const TCHAR* Field : {TEXT("wouldAdd"), TEXT("alreadyExists"), TEXT("inherited")})
                Result->SetBoolField(Field, Preview.Result->GetBoolField(Field));
        }
        else if (bAddVariable)
        {
            Result->SetObjectField(TEXT("declaration"), SortedValues);
            Result->SetBoolField(TEXT("wouldAdd"), true);
        }
        else if (!bStructure)
        {
            Result->SetArrayField(TEXT("columns"), {MakeShared<FJsonValueString>(TEXT("name")),
                MakeShared<FJsonValueString>(TEXT("previousValue")), MakeShared<FJsonValueString>(TEXT("value"))});
            TArray<TSharedPtr<FJsonValue>> Rows;
            for (const auto& Property : Preview.Result->GetArrayField(TEXT("properties")))
            {
                const auto& Entry = Property->AsObject();
                if (!Entry->GetBoolField(TEXT("changed"))) continue;
                TArray<TSharedPtr<FJsonValue>> Row;
                for (const TCHAR* Field : {TEXT("name"), TEXT("previousValue"), TEXT("value")})
                    Row.Add(MakeShared<FJsonValueString>(Entry->GetStringField(Field)));
                Rows.Add(MakeShared<FJsonValueArray>(MoveTemp(Row)));
            }
            Result->SetArrayField(TEXT("rows"), Rows);
        }
        Result->SetBoolField(TEXT("saveRequested"), bSave);
        if (bDryRun) return true;
        if (!GEditor || GEditor->IsTransactionActive())
        { OutError = TEXT("Apply requires an idle editor transaction system."); return false; }

        const auto DirtyBefore = DirtyContentPackages();
        Native.Params->SetBoolField(TEXT("dryRun"), false);
        Native.Params->SetBoolField(TEXT("confirm"), true);
        Native.Params->SetBoolField(TEXT("compile"), true);
        Native.Params->SetBoolField(TEXT("save"), bSave);
        if (bComponentDefaults || bAddInterface || bAddVariable || bStructure)
        {
            Native.Params->SetBoolField(TEXT("compileAfterEdit"), true);
            Native.Params->SetBoolField(TEXT("saveAfterEdit"), bSave);
        }
        const auto Applied = Tool.Execute(Native);
        TArray<TSharedPtr<FJsonValue>> Unsaved;
        for (const FString& Package : DirtyContentPackages())
            if (!DirtyBefore.Contains(Package)) Unsaved.Add(MakeShared<FJsonValueString>(Package));
        Result->SetArrayField(TEXT("newlyDirtiedPackages"), Unsaved);
        Result->SetBoolField(TEXT("isDirty"), Blueprint->GetOutermost()->IsDirty());
        if (bStructure && Applied.Result)
        {
            const TArray<TSharedPtr<FJsonValue>>* Packages;
            if (Applied.Result->TryGetArrayField(TEXT("packages"), Packages))
                Result->SetArrayField(TEXT("packages"), *Packages);
            for (const TCHAR* Field : {TEXT("applied"), TEXT("rolledBack"), TEXT("saveFailed")})
            {
                bool bValue;
                if (Applied.Result->TryGetBoolField(Field, bValue)) Result->SetBoolField(Field, bValue);
            }
        }
        if (bAddInterface && Applied.Result)
            for (const TCHAR* Field : {TEXT("added"), TEXT("rolledBack")})
                Result->SetBoolField(Field, Applied.Result->GetBoolField(Field));
        if (!Applied.Result || !Applied.Result->GetBoolField(TEXT("success")))
        {
            if (Applied.Error.IsSet()) OutError = Applied.Error->Message;
            else if (!Applied.Result || !Applied.Result->TryGetStringField(TEXT("error"), OutError))
                OutError = TEXT("Merge apply failed.");
            return false;
        }
        if (bStructure) return true;
        if (bAddInterface) Result->SetArrayField(TEXT("createdGraphs"), Applied.Result->GetArrayField(TEXT("createdGraphs")));
        else if (bAddVariable) Result->SetBoolField(TEXT("added"), Applied.Result->GetBoolField(TEXT("added")));
        else Result->SetBoolField(TEXT("changed"), Applied.Result->GetBoolField(TEXT("changed")));
        for (const TCHAR* Field : {TEXT("compiled"), TEXT("saved")})
            Result->SetBoolField(Field, Applied.Result->GetBoolField(Field));
        if (bCreateOverride) Result->SetBoolField(TEXT("createdOverride"), Applied.Result->GetBoolField(TEXT("createdOverride")));
        return true;
    }, Error);
    if (!bOk)
    {
        Result->SetBoolField(TEXT("success"), false);
        return BuildError(Request, EMCPErrorCode::InvalidParams, Error, Result);
    }
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FApplyBlueprintMergeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    auto Values = MakeShared<FJsonObject>();
    Values->SetStringField(TEXT("type"), TEXT("object"));
    Values->SetStringField(TEXT("description"), TEXT("1..64 editable class-default property names mapped to Unreal export text."));
    Values->SetObjectField(TEXT("additionalProperties"), BuildStringProperty(TEXT("Unreal export text.")));
    if (Operation == EOperation::ClassDefaults || Operation == EOperation::ComponentDefaults)
        Properties->SetObjectField(TEXT("values"), Values);
    Properties->SetObjectField(TEXT("path"), BuildStringProperty(TEXT("Project-relative working Blueprint path under Content.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Defaults true. Returns compact changed values and a token without editing.")));
    Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required for dryRun=false.")));
    Properties->SetObjectField(TEXT("save"), BuildBoolProperty(TEXT("Defaults false. Use the same value for preview and apply. Saves only the reviewed Blueprint.")));
    Properties->SetObjectField(TEXT("previewToken"), BuildStringProperty(TEXT("Required on apply. Pass the exact token from preview with unchanged values and save intent.")));
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required = {MakeShared<FJsonValueString>(TEXT("path"))};
    if (Operation == EOperation::AddInterface)
    {
        Properties->SetObjectField(TEXT("interfaceClassPath"), BuildStringProperty(TEXT("Exact interface class path, including _C for Blueprint interfaces.")));
        Required.Add(MakeShared<FJsonValueString>(TEXT("interfaceClassPath")));
    }
    else if (Operation == EOperation::AddVariable)
    {
        const auto Definition = FAddBlueprintVariableTool().GetDefinition();
        auto Declaration = MakeShared<FJsonObject>(*Definition.InputSchema);
        auto Fields = MakeShared<FJsonObject>(*Declaration->GetObjectField(TEXT("properties")));
        for (const TCHAR* Field : {TEXT("objectPath"), TEXT("dryRun"), TEXT("compileAfterEdit"), TEXT("saveAfterEdit")})
            Fields->RemoveField(Field);
        Declaration->SetObjectField(TEXT("properties"), Fields);
        Declaration->SetBoolField(TEXT("additionalProperties"), false);
        Declaration->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("variableName")), MakeShared<FJsonValueString>(TEXT("type"))});
        Properties->SetObjectField(TEXT("declaration"), Declaration);
        Required.Add(MakeShared<FJsonValueString>(TEXT("declaration")));
    }
    else if (Operation == EOperation::Structure)
    {
        auto Operations = MakeShared<FJsonObject>();
        Operations->SetStringField(TEXT("type"), TEXT("array"));
        Operations->SetNumberField(TEXT("minItems"), 1);
        Operations->SetNumberField(TEXT("maxItems"), 16);
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("type"), TEXT("object"));
        Item->SetObjectField(TEXT("additionalProperties"), BuildStringProperty(TEXT("Operation field.")));
        Operations->SetObjectField(TEXT("items"), Item);
        Operations->SetStringField(TEXT("description"), TEXT("Ordered operations: renameVariable(variableName,newName), removeVariable(variableName), setVariableType(variableName,type,defaultValue[,typeObjectPath,containerType,valueType,valueTypeObjectPath]), setParameterType(nodeGuid,parameterName,type[,type fields]), removeInterface(interfaceClassPath), reparent(newParentClassPath), moveComponent(componentName,parentComponentName)."));
        Properties->SetObjectField(TEXT("operations"), Operations);
        Properties->SetObjectField(TEXT("save"), BuildBoolProperty(TEXT("Defaults false. Use the same value for preview and apply. Saves changed reviewed packages after all successfully compile.")));
        Required.Add(MakeShared<FJsonValueString>(TEXT("operations")));
    }
    else Required.Add(MakeShared<FJsonValueString>(TEXT("values")));
    if (Operation == EOperation::ComponentDefaults)
    {
        Properties->SetObjectField(TEXT("componentName"), BuildStringProperty(TEXT("Exact SCS component variable name.")));
        Properties->SetObjectField(TEXT("inheritedOverrideGuid"), BuildStringProperty(TEXT("Exact inherited SCS GUID from the comparison; requires matching componentName.")));
        Properties->SetObjectField(TEXT("createInheritedOverride"), BuildBoolProperty(TEXT("Explicitly create a missing child override when values differ from inherited defaults. Defaults false.")));
        Required.Add(MakeShared<FJsonValueString>(TEXT("componentName")));
    }
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}

FUnrealMCPGitHistoryTool::FUnrealMCPGitHistoryTool(EOperation InOperation)
    : FMCPToolBase(InOperation == EOperation::History ? TEXT("GetGitCommitHistory")
        : InOperation == EOperation::Changes ? TEXT("GetGitCommitChanges") : TEXT("InspectBlueprintCommit"),
        InOperation == EOperation::History ? TEXT("Page recent commits in the selected project, optionally filtered by file.")
        : InOperation == EOperation::Changes ? TEXT("List committed, staged or tracked unstaged Git changes. Optionally return a bounded text patch for one file.")
        : TEXT("Read historical Blueprint graphs, nodes or pins from an exact Git commit without changing the working copy.")),
      Operation(InOperation)
{
}

UnrealMCP::FMCPResponse FUnrealMCPGitHistoryTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString Path, Commit, GraphGuidText, NodeGuidText, PinIdText;
    int32 Offset = 0, Limit = 25, ParentIndex = 1, PatchOffset = 0;
    bool bIncludePatch = false;
    FString Source = TEXT("commit"), BaseCommit;
    const auto& Params = Request.Params;
    const bool bHistory = Operation == EOperation::History;
    const bool bChanges = Operation == EOperation::Changes;
    if (!Params.IsValid()
        || (Params->HasField(TEXT("path")) && (!Params->TryGetStringField(TEXT("path"), Path) || !SafeProjectPath(Path)))
        || (Params->HasField(TEXT("commit")) && (!Params->TryGetStringField(TEXT("commit"), Commit) || !IsGitObjectId(Commit)))
        || (bChanges && Params->HasField(TEXT("source")) && !Params->TryGetStringField(TEXT("source"), Source))
        || (Source != TEXT("commit") && Source != TEXT("staged") && Source != TEXT("unstaged"))
        || (bChanges && Params->HasField(TEXT("baseCommit")) && (!Params->TryGetStringField(TEXT("baseCommit"), BaseCommit) || !IsGitObjectId(BaseCommit)))
        || (!bHistory && Source == TEXT("commit") && Commit.IsEmpty())
        || (Source != TEXT("commit") && (!Commit.IsEmpty() || !BaseCommit.IsEmpty() || Params->HasField(TEXT("parentIndex"))))
        || (!BaseCommit.IsEmpty() && Params->HasField(TEXT("parentIndex")))
        || (Params->HasField(TEXT("offset")) && !Params->TryGetNumberField(TEXT("offset"), Offset))
        || (Params->HasField(TEXT("limit")) && !Params->TryGetNumberField(TEXT("limit"), Limit))
        || Offset < 0 || Offset > MAX_int32 - 101 || Limit < 1 || Limit > 100
        || (bHistory && Offset > 0 && Commit.IsEmpty()))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Use a project-relative path, exact commit hash when required, and limit 1..100. History pages after the first require the returned commit anchor."));

    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    FString ResolvedCommit;
    if (Source == TEXT("commit") && (!ReadGitText(ProjectDir, TEXT("rev-parse --verify --quiet ") + (Commit.IsEmpty() ? TEXT("HEAD") : Commit)
        + TEXT("^{commit}"), ResolvedCommit) || !IsGitObjectId(ResolvedCommit)))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Git could not resolve the requested commit in this project's worktree."));

    auto Result = BuildBooleanResult(true);
    if (!ResolvedCommit.IsEmpty()) Result->SetStringField(TEXT("commit"), ResolvedCommit);
    if (!Path.IsEmpty()) Result->SetStringField(TEXT("path"), Path);
    TArray<TSharedPtr<FJsonValue>> Rows;
    FString DependencyMount;
    auto AddRow = [&](std::initializer_list<FString> Cells)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (FString Cell : Cells)
        {
            if (!DependencyMount.IsEmpty()) Cell.ReplaceInline(*DependencyMount, TEXT("/Game/"));
            Values.Add(MakeShared<FJsonValueString>(MoveTemp(Cell)));
        }
        Rows.Add(MakeShared<FJsonValueArray>(MoveTemp(Values)));
    };
    auto Columns = [&](std::initializer_list<const TCHAR*> Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const TCHAR* Column : Names) Values.Add(MakeShared<FJsonValueString>(Column));
        Result->SetArrayField(TEXT("columns"), Values);
    };
    bool bHasMore = false;
    TArray<FString> Fields;
    const FString Pathspec = FString::Printf(TEXT(" -- \"%s\""), Path.IsEmpty() ? TEXT(".") : *Path);
    if (bHistory)
    {
        const FString Args = FString::Printf(TEXT("--literal-pathspecs log --full-history -z --format=%%H%%x00%%P%%x00%%aI%%x00%%s --skip=%d -n %d %s"),
            Offset, Limit + 1, *ResolvedCommit) + Pathspec;
        if (!ReadGitFields(ProjectDir, Args, Fields) || Fields.Num() % 4)
            return BuildError(Request, EMCPErrorCode::InternalError, TEXT("Git history could not be read."));
        bHasMore = Fields.Num() / 4 > Limit;
        Columns({TEXT("commit"), TEXT("parents"), TEXT("authoredAt"), TEXT("subject")});
        for (int32 Index = 0; Index < FMath::Min(Fields.Num(), Limit * 4); Index += 4)
            AddRow({Fields[Index], Fields[Index + 1], Fields[Index + 2], Fields[Index + 3]});
    }
    else if (bChanges)
    {
        if ((Params->HasField(TEXT("parentIndex")) && !Params->TryGetNumberField(TEXT("parentIndex"), ParentIndex))
            || (Params->HasField(TEXT("includePatch")) && !Params->TryGetBoolField(TEXT("includePatch"), bIncludePatch))
            || (Params->HasField(TEXT("patchOffset")) && !Params->TryGetNumberField(TEXT("patchOffset"), PatchOffset))
            || ParentIndex < 1 || PatchOffset < 0 || (bIncludePatch && Path.IsEmpty()))
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Use a positive parentIndex, nonnegative patchOffset, and an exact path when includePatch is true."));
        Result->SetStringField(TEXT("source"), Source);
        const FString DiffOptions = TEXT(" --relative --no-ext-diff --no-textconv -M ");
        FString Diff = TEXT("--literal-pathspecs diff") + DiffOptions;
        FString Revisions;
        if (Source == TEXT("staged"))
        {
            Diff += TEXT("--cached ");
        }
        else if (!BaseCommit.IsEmpty())
        {
            FString ResolvedBase;
            if (!ReadGitText(ProjectDir, TEXT("rev-parse --verify --quiet ") + BaseCommit + TEXT("^{commit}"), ResolvedBase)
                || !IsGitObjectId(ResolvedBase))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Git could not resolve baseCommit."));
            Result->SetStringField(TEXT("baseCommit"), ResolvedBase);
            Revisions = ResolvedBase + TEXT(" ") + ResolvedCommit;
        }
        else if (Source == TEXT("commit"))
        {
            if (!ReadGitFields(ProjectDir, TEXT("log -1 -z --format=%P%x00%aI%x00%s ") + ResolvedCommit, Fields) || Fields.Num() != 3)
                return BuildError(Request, EMCPErrorCode::InternalError, TEXT("Git commit metadata could not be read."));
            TArray<FString> Parents;
            Fields[0].ParseIntoArrayWS(Parents);
            if (ParentIndex > FMath::Max(1, Parents.Num()))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("parentIndex exceeds the commit's parent count."));
            const FString Parent = Parents.IsEmpty() ? FString() : Parents[ParentIndex - 1];
            if (!Parent.IsEmpty() && !IsGitObjectId(Parent))
                return BuildError(Request, EMCPErrorCode::InternalError, TEXT("Git returned an invalid parent commit ID."));
            Result->SetStringField(TEXT("parent"), Parent);
            Result->SetNumberField(TEXT("parentCount"), Parents.Num());
            Result->SetStringField(TEXT("authoredAt"), Fields[1]);
            Result->SetStringField(TEXT("subject"), Fields[2]);
            Diff = TEXT("--literal-pathspecs diff-tree --root -r --no-commit-id") + DiffOptions;
            Revisions = Parent.IsEmpty() ? ResolvedCommit : Parent + TEXT(" ") + ResolvedCommit;
        }
        Fields.Reset();
        if (!ReadGitFields(ProjectDir, Diff + TEXT("--name-status -z ") + Revisions + TEXT(" -- ."), Fields))
            return BuildError(Request, EMCPErrorCode::InternalError, TEXT("Git file changes could not be read."));
        Columns({TEXT("status"), TEXT("path"), TEXT("previousPath")});
        int32 Total = 0;
        FString PatchPaths = Pathspec;
        for (int32 Index = 0; Index < Fields.Num();)
        {
            const FString& Status = Fields[Index++];
            const bool bRename = Status.StartsWith(TEXT("R")) || Status.StartsWith(TEXT("C"));
            if (Status.IsEmpty() || Index >= Fields.Num() || (bRename && Index + 1 >= Fields.Num()))
                return BuildError(Request, EMCPErrorCode::InternalError, TEXT("Git returned an invalid change record."));
            const FString& First = Fields[Index++];
            const FString& Current = bRename ? Fields[Index++] : First;
            if (!Path.IsEmpty() && !Path.Equals(Current, ESearchCase::CaseSensitive)
                && (!bRename || !Path.Equals(First, ESearchCase::CaseSensitive))) continue;
            if (bIncludePatch && bRename)
            {
                if (!SafeProjectPath(First) || !SafeProjectPath(Current))
                    return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("A rename endpoint cannot be passed safely to Git for patch lookup."));
                PatchPaths = FString::Printf(TEXT(" -- \"%s\" \"%s\""), *First, *Current);
            }
            if (Total++ >= Offset && Rows.Num() < Limit) AddRow({Status, Current, bRename ? First : FString()});
        }
        bHasMore = Offset + Rows.Num() < Total;
        Result->SetNumberField(TEXT("total"), Total);
        if (bIncludePatch)
        {
            FString Patch;
            if (!ReadGitText(ProjectDir, Diff + TEXT("--no-color --no-prefix -p ") + Revisions + PatchPaths, Patch, false))
                return BuildError(Request, EMCPErrorCode::InternalError, TEXT("Git patch could not be read within the output limit."));
            int32 Count = FMath::Min(8000, FMath::Max(0, Patch.Len() - PatchOffset));
            if (Count > 0 && Patch[PatchOffset + Count - 1] >= 0xD800 && Patch[PatchOffset + Count - 1] <= 0xDBFF) --Count;
            Result->SetStringField(TEXT("patch"), Patch.Mid(PatchOffset, Count));
            if (PatchOffset + Count < Patch.Len()) Result->SetNumberField(TEXT("nextPatchOffset"), PatchOffset + Count);
        }
    }
    else
    {
        FGuid GraphGuid, NodeGuid, PinId;
        if (!SafeContentPath(Path)
            || (Params->HasField(TEXT("graphGuid")) && (!Params->TryGetStringField(TEXT("graphGuid"), GraphGuidText) || !FGuid::Parse(GraphGuidText, GraphGuid) || !GraphGuid.IsValid()))
            || (Params->HasField(TEXT("nodeGuid")) && (!Params->TryGetStringField(TEXT("nodeGuid"), NodeGuidText) || !FGuid::Parse(NodeGuidText, NodeGuid) || !NodeGuid.IsValid()))
            || (Params->HasField(TEXT("pinId")) && (!Params->TryGetStringField(TEXT("pinId"), PinIdText) || !FGuid::Parse(PinIdText, PinId) || !PinId.IsValid()))
            || (NodeGuid.IsValid() && !GraphGuid.IsValid()) || (PinId.IsValid() && !NodeGuid.IsValid()))
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide a Blueprint Content/*.uasset path. nodeGuid requires graphGuid, and pinId requires both."));
        FString RepoPath, Error;
        if (!ResolveRepoPath(ProjectDir, Path, RepoPath, Error))
            return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
        FString BlobId;
        if (!ReadGitText(ProjectDir, FString::Printf(TEXT("rev-parse --verify --quiet \"%s:%s\""), *ResolvedCommit, *RepoPath), BlobId)
            || !IsGitObjectId(BlobId))
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("The Blueprint path is absent from this commit. Use its historical name."));
        TArray<uint8> Blob;
        if (!RunGit(ProjectDir, TEXT("cat-file blob ") + BlobId, Blob) || Blob.IsEmpty())
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Historical Blueprint blob could not be read within the output limit."));
        Result->SetStringField(TEXT("blobId"), BlobId);
        Result->SetStringField(TEXT("dependencyScope"), TEXT("active_project_with_historical_fallback"));
        TArray<FString> MissingDependencies, RecoveredDependencies;
        if (!BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
        {
            return WithHistoricalBlueprint(ProjectDir, Path, Blob, [&](UBlueprint* Blueprint)
            {
                int32 Total = 0;
                auto OnPage = [&]() { return Total++ >= Offset && Rows.Num() < Limit; };
                TArray<UEdGraph*> Graphs;
                Blueprint->GetAllGraphs(Graphs);
                if (!GraphGuid.IsValid())
                {
                    Columns({TEXT("graphGuid"), TEXT("graphPath"), TEXT("nodeCount")});
                    for (const UEdGraph* Graph : Graphs)
                        if (Graph && OnPage()) AddRow({Graph->GraphGuid.ToString(), Graph->GetPathName(Blueprint), FString::FromInt(Graph->Nodes.Num())});
                }
                else
                {
                    const auto* Match = Graphs.FindByPredicate([&](const UEdGraph* Graph) { return Graph && Graph->GraphGuid == GraphGuid; });
                    if (!Match) { OutError = TEXT("Graph GUID not found in the committed Blueprint."); return false; }
                    Result->SetStringField(TEXT("graphGuid"), GraphGuid.ToString());
                    const UEdGraph* Graph = *Match;
                    if (!NodeGuid.IsValid())
                    {
                        Columns({TEXT("nodeGuid"), TEXT("class"), TEXT("title")});
                        for (const UEdGraphNode* Node : Graph->Nodes)
                            if (Node && OnPage()) AddRow({Node->NodeGuid.ToString(), Node->GetClass()->GetName(), Node->GetNodeTitle(ENodeTitleType::ListView).ToString()});
                    }
                    else
                    {
                        const auto* NodeMatch = Graph->Nodes.FindByPredicate([&](const UEdGraphNode* Node) { return Node && Node->NodeGuid == NodeGuid; });
                        if (!NodeMatch) { OutError = TEXT("Node GUID not found in the committed graph."); return false; }
                        Result->SetStringField(TEXT("nodeGuid"), NodeGuid.ToString());
                        if (PinId.IsValid())
                        {
                            const UEdGraphPin* const* MatchPin = (*NodeMatch)->Pins.FindByPredicate(
                                [&](const UEdGraphPin* Pin) { return Pin && Pin->PinId == PinId; });
                            if (!MatchPin) { OutError = TEXT("Pin ID not found on the committed node."); return false; }
                            Result->SetStringField(TEXT("pinId"), PinId.ToString());
                            Columns({TEXT("graphGuid"), TEXT("nodeGuid"), TEXT("pinId"), TEXT("pinName")});
                            for (const UEdGraphPin* Linked : (*MatchPin)->LinkedTo)
                                if (Linked && Linked->GetOwningNodeUnchecked() && OnPage())
                                {
                                    const UEdGraphNode* Owner = Linked->GetOwningNode();
                                    AddRow({Owner->GetGraph() ? Owner->GetGraph()->GraphGuid.ToString() : FString(),
                                        Owner->NodeGuid.ToString(), Linked->PinId.ToString(), Linked->PinName.ToString()});
                                }
                        }
                        else
                        {
                            Columns({TEXT("pinId"), TEXT("name"), TEXT("direction"), TEXT("type"), TEXT("default"), TEXT("links"), TEXT("linkCount")});
                            const FString Package = Blueprint->GetOutermost()->GetName();
                            for (const UEdGraphPin* Pin : (*NodeMatch)->Pins)
                            {
                                if (!Pin || !OnPage()) continue;
                                TArray<TSharedPtr<FJsonValue>> Links;
                                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                                {
                                    if (!Linked || !Linked->GetOwningNodeUnchecked() || Links.Num() >= 32) continue;
                                    Links.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
                                        MakeShared<FJsonValueString>(Linked->GetOwningNode()->NodeGuid.ToString()),
                                        MakeShared<FJsonValueString>(Linked->PinId.ToString()),
                                        MakeShared<FJsonValueString>(Linked->PinName.ToString())}));
                                }
                                FString Default = Pin->GetDefaultAsString();
                                Default.ReplaceInline(*Package, TEXT("<self>"));
                                auto Type = BlueprintToolUtils::SerializePinType(Pin->PinType);
                                for (const TCHAR* Field : {TEXT("subCategoryObjectPath"), TEXT("valueSubCategoryObjectPath")})
                                {
                                    FString TypePath = Type->GetStringField(Field);
                                    if (TypePath.ReplaceInline(*Package, TEXT("<self>"))) Type->SetStringField(Field, TypePath);
                                }
                                Rows.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
                                    MakeShared<FJsonValueString>(Pin->PinId.ToString()),
                                    MakeShared<FJsonValueString>(Pin->PinName.ToString()),
                                    MakeShared<FJsonValueString>(Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out")),
                                    MakeShared<FJsonValueObject>(Type), MakeShared<FJsonValueString>(Default),
                                    MakeShared<FJsonValueArray>(MoveTemp(Links)), MakeShared<FJsonValueNumber>(Pin->LinkedTo.Num())}));
                            }
                            Result->SetArrayField(TEXT("linkColumns"), {MakeShared<FJsonValueString>(TEXT("nodeGuid")),
                                MakeShared<FJsonValueString>(TEXT("pinId")), MakeShared<FJsonValueString>(TEXT("pinName"))});
                        }
                    }
                }
                bHasMore = Offset + Rows.Num() < Total;
                Result->SetNumberField(TEXT("total"), Total);
                return true;
            }, OutError, &MissingDependencies, ResolvedCommit, &RecoveredDependencies, &DependencyMount);
        }, Error))
        {
            Result->SetBoolField(TEXT("success"), false);
            Result->SetBoolField(TEXT("evidenceComplete"), false);
            TArray<TSharedPtr<FJsonValue>> Dependencies;
            for (const FString& Dependency : MissingDependencies) Dependencies.Add(MakeShared<FJsonValueString>(Dependency));
            if (!Dependencies.IsEmpty()) Result->SetArrayField(TEXT("missingDependencies"), Dependencies);
            return BuildError(Request, EMCPErrorCode::InvalidParams, Error, Result);
        }
        TArray<TSharedPtr<FJsonValue>> RecoveredNames;
        for (const FString& Dependency : RecoveredDependencies) RecoveredNames.Add(MakeShared<FJsonValueString>(Dependency));
        if (!RecoveredNames.IsEmpty()) Result->SetArrayField(TEXT("recoveredHistoricalDependencies"), RecoveredNames);
        Result->SetBoolField(TEXT("evidenceComplete"), true);
    }
    Result->SetArrayField(TEXT("rows"), Rows);
    if (bHasMore) Result->SetNumberField(TEXT("nextOffset"), Offset + Rows.Num());
    FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPGitHistoryTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    const bool bHistory = Operation == EOperation::History;
    const bool bGraph = Operation == EOperation::BlueprintGraph;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Properties->SetObjectField(TEXT("commit"), BuildStringProperty(bHistory
        ? TEXT("Optional full commit hash. Defaults to HEAD; pass returned commit for later history pages.")
        : TEXT("Full commit hash.")));
    Properties->SetObjectField(TEXT("path"), BuildStringProperty(bGraph
        ? TEXT("Project-relative historical Blueprint Content/*.uasset path.")
        : TEXT("Optional exact project-relative file path using forward slashes.")));
    auto Integer = [&](const TCHAR* Field, const TCHAR* FieldDescription)
    {
        auto Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("type"), TEXT("integer"));
        Value->SetStringField(TEXT("description"), FieldDescription);
        Properties->SetObjectField(Field, Value);
    };
    Integer(TEXT("offset"), TEXT("Zero-based row offset; pass nextOffset."));
    Integer(TEXT("limit"), TEXT("Rows per page, default 25, max 100."));
    if (Operation == EOperation::Changes)
    {
        Properties->SetObjectField(TEXT("source"), BuildStringProperty(TEXT("commit (default): commit comparison; staged: index vs HEAD; unstaged: tracked worktree vs index.")));
        Properties->SetObjectField(TEXT("baseCommit"), BuildStringProperty(TEXT("Optional full base commit hash for comparing two commits. Omit to compare with the selected parent.")));
        Integer(TEXT("parentIndex"), TEXT("One-based parent selector for merge commits; defaults to 1."));
        Integer(TEXT("patchOffset"), TEXT("Text patch offset from nextPatchOffset."));
        Properties->SetObjectField(TEXT("includePatch"), BuildBoolProperty(TEXT("Return up to 8,000 characters of patch text for the exact path. Binary files return Git's binary diff marker.")));
    }
    if (bGraph)
    {
        Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Omit for graph inventory; pass a graph GUID for node rows.")));
        Properties->SetObjectField(TEXT("nodeGuid"), BuildStringProperty(TEXT("Provide with graphGuid for pin rows and up to 32 links per pin.")));
        Properties->SetObjectField(TEXT("pinId"), BuildStringProperty(TEXT("Provide with graphGuid and nodeGuid to page all linked endpoints of one historical pin.")));
    }
    TArray<TSharedPtr<FJsonValue>> Required;
    if (bGraph) Required.Add(MakeShared<FJsonValueString>(TEXT("commit")));
    if (bGraph) Required.Add(MakeShared<FJsonValueString>(TEXT("path")));
    Schema->SetArrayField(TEXT("required"), Required);
    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}

namespace
{
    FString ReadMergeWorkingHash(const FString& Filename)
    {
        const int64 Size = IFileManager::Get().FileSize(*Filename);
        TArray<uint8> Bytes;
        if (Size <= 0 || Size > MaxGitOutput || !FFileHelper::LoadFileToArray(Bytes, *Filename) || Bytes.Num() != Size) return {};
        return FMD5::HashBytes(Bytes.GetData(), Bytes.Num());
    }

    void HashPlanValue(FMD5& Hash, const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value) { HashEvidence(Hash, TEXT("invalid")); return; }
        HashEvidence(Hash, FString::FromInt(static_cast<int32>(Value->Type)));
        if (Value->Type == EJson::Object)
        {
            const auto& Object = Value->AsObject();
            TArray<FString> Keys;
            for (const auto& Field : Object->Values) Keys.Add(FString(Field.Key));
            Keys.Sort();
            HashEvidence(Hash, FString::FromInt(Keys.Num()));
            for (const FString& Key : Keys)
            {
                HashEvidence(Hash, Key);
                HashPlanValue(Hash, Object->TryGetField(Key));
            }
        }
        else if (Value->Type == EJson::Array)
        {
            HashEvidence(Hash, FString::FromInt(Value->AsArray().Num()));
            for (const auto& Item : Value->AsArray()) HashPlanValue(Hash, Item);
        }
        else
        {
            FString Literal;
            FJsonSerializer::Serialize(Value, FString(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Literal));
            HashEvidence(Hash, Literal);
        }
    }

    TUniquePtr<FMCPToolBase> MakePlanStep(const FString& Kind)
    {
        using Op = FApplyBlueprintMergeTool::EOperation;
        if (Kind == TEXT("graphPatch")) return MakeUnique<FApplyBlueprintGraphPatchTool>();
        if (Kind == TEXT("classDefaults")) return MakeUnique<FApplyBlueprintMergeTool>(Op::ClassDefaults);
        if (Kind == TEXT("componentDefaults")) return MakeUnique<FApplyBlueprintMergeTool>(Op::ComponentDefaults);
        if (Kind == TEXT("addInterface")) return MakeUnique<FApplyBlueprintMergeTool>(Op::AddInterface);
        if (Kind == TEXT("addVariable")) return MakeUnique<FApplyBlueprintMergeTool>(Op::AddVariable);
        if (Kind == TEXT("structure")) return MakeUnique<FApplyBlueprintMergeTool>(Op::Structure);
        return nullptr;
    }

    bool WritePlanRecord(const FString& Filename, const TSharedRef<FJsonObject>& Record)
    {
        const FString Temporary = Filename + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
        ON_SCOPE_EXIT { IFileManager::Get().Delete(*Temporary, false, true); };
        FString Text;
        return FJsonSerializer::Serialize(Record, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text))
            && FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            && IFileManager::Get().Move(*Filename, *Temporary, false, false, false, true);
    }

    bool ReadPlanRecord(const FString& Filename, TSharedPtr<FJsonObject>& Record)
    {
        const int64 Size = IFileManager::Get().FileSize(*Filename);
        FString Text;
        return Size > 0 && Size <= MaxGitOutput && FFileHelper::LoadFileToString(Text, *Filename)
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Record) && Record.IsValid();
    }

    TSharedRef<FJsonValueObject> SummarizeCheckpoint(const TSharedPtr<FJsonObject>& Receipt)
    {
        auto Summary = MakeShared<FJsonObject>();
        for (const TCHAR* Field : {TEXT("stepIndex"), TEXT("kind"), TEXT("success"),
            TEXT("workingFileChanged"), TEXT("isDirty"), TEXT("workingHashAfter")})
            if (const auto Value = Receipt->TryGetField(Field)) Summary->SetField(Field, Value);
        return MakeShared<FJsonValueObject>(Summary);
    }

    const TSet<FString> PlanOwnedArgs = {TEXT("path"), TEXT("objectPath"), TEXT("dryRun"), TEXT("confirm"),
        TEXT("save"), TEXT("saveAfterEdit"), TEXT("compileAfterEdit"), TEXT("previewToken"), TEXT("operationId")};
}

FApplyBlueprintMergePlanTool::FApplyBlueprintMergePlanTool()
    : FMCPToolBase(TEXT("ApplyBlueprintMergePlan"),
        TEXT("Preview or apply one checkpoint in a client-held sequence of graph and structural merge steps. Saves each successful step, persists replay receipts, and never stages files."))
{
}

UnrealMCP::FMCPResponse FApplyBlueprintMergePlanTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    const auto& Params = Request.Params;
    FString Path, Stage, GuidText, ExpectedToken;
    FGuid Guid;
    int32 StepIndex = 0;
    bool bDry = true, bConfirm = false;
    const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
    if (!Params || !Params->TryGetStringField(TEXT("path"), Path) || !SafeContentPath(Path)
        || !Params->TryGetStringField(TEXT("stageRevision"), Stage) || Stage.IsEmpty()
        || !Params->TryGetStringField(TEXT("planId"), GuidText) || !FGuid::Parse(GuidText, Guid) || !Guid.IsValid()
        || !Params->TryGetArrayField(TEXT("steps"), Steps) || Steps->IsEmpty() || Steps->Num() > 16
        || !Params->TryGetNumberField(TEXT("stepIndex"), StepIndex) || !Steps->IsValidIndex(StepIndex)
        || (Params->HasField(TEXT("dryRun")) && !Params->TryGetBoolField(TEXT("dryRun"), bDry))
        || (Params->HasField(TEXT("confirm")) && !Params->TryGetBoolField(TEXT("confirm"), bConfirm))
        || (!bDry && (!bConfirm || !Params->TryGetStringField(TEXT("previewToken"), ExpectedToken) || ExpectedToken.IsEmpty())))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide path, stageRevision, a planId UUID, ordered steps, and stepIndex. Apply requires the step preview token and confirm=true."));

    TArray<FString> Kinds;
    TArray<TSharedPtr<FJsonObject>> Arguments;
    for (const auto& Step : *Steps)
    {
        FString Kind;
        const TSharedPtr<FJsonObject>* Item = nullptr;
        const TSharedPtr<FJsonObject>* Args = nullptr;
        if (!Step || !Step->TryGetObject(Item) || !Item || !*Item || (*Item)->Values.Num() != 2
            || !(*Item)->TryGetStringField(TEXT("kind"), Kind) || !(*Item)->TryGetObjectField(TEXT("arguments"), Args))
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Each step requires only kind and arguments."));
        const auto Tool = MakePlanStep(Kind);
        if (!Tool)
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Unknown merge plan step kind: ") + Kind);
        const auto Schema = Tool->GetDefinition().InputSchema->GetObjectField(TEXT("properties"));
        for (const auto& Field : (*Args)->Values)
            if (PlanOwnedArgs.Contains(FString(Field.Key)) || !Schema->HasField(FString(Field.Key)))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Unsupported or plan-owned step argument: ") + FString(Field.Key));
        TArray<TSharedPtr<FJsonValue>> Required = Tool->GetDefinition().InputSchema->GetArrayField(TEXT("required"));
        for (const auto& Field : Required)
            if (!PlanOwnedArgs.Contains(Field->AsString()) && !(*Args)->HasField(Field->AsString()))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Missing required step argument: ") + Field->AsString());
        Kinds.Add(Kind);
        Arguments.Add(*Args);
    }

    const FString PlanId = Guid.ToString(EGuidFormats::Digits);
    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    FString RepoPath, PackageName, Error;
    const FString Filename = ProjectDir / Path;
    if (!ResolveRepoPath(ProjectDir, Path, RepoPath, Error) || !FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName)
        || !PackageName.StartsWith(TEXT("/Game/")))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("The working path must resolve to a project Blueprint."));
    const FString ObjectPath = PackageName + TEXT(".") + FPaths::GetBaseFilename(Path);
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("UnrealMCP/MergePlans") / PlanId;
    auto RecordPath = [&](int32 Index, const TCHAR* Suffix)
    {
        return Directory / FString::Printf(TEXT("%02d.%s.json"), Index, Suffix);
    };
    FMD5 PlanHash;
    HashEvidence(PlanHash, PlanId);
    HashEvidence(PlanHash, Path);
    HashEvidence(PlanHash, Stage);
    for (const auto& Item : *Steps) HashPlanValue(PlanHash, Item);
    const FString PlanRevision = FinishEvidence(PlanHash);

    auto Result = BuildBooleanResult(false);
    Result->SetStringField(TEXT("planId"), PlanId);
    Result->SetStringField(TEXT("planRevision"), PlanRevision);
    Result->SetStringField(TEXT("stageRevision"), Stage);
    Result->SetNumberField(TEXT("stepIndex"), StepIndex);
    Result->SetStringField(TEXT("kind"), Kinds[StepIndex]);
    Result->SetBoolField(TEXT("staged"), false);
    Result->SetStringField(TEXT("transactionBoundary"), TEXT("Each step applies and saves independently. Completed checkpoints stay saved if a later step fails."));
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        TArray<TSharedPtr<FJsonValue>> Completed;
        FString PriorHash;
        int32 Next = 0;
        for (; Next < Kinds.Num(); ++Next)
        {
            const FString ReceiptFile = RecordPath(Next, TEXT("receipt"));
            const FString IntentFile = RecordPath(Next, TEXT("intent"));
            if (!IFileManager::Get().FileExists(*IntentFile) && !IFileManager::Get().FileExists(*ReceiptFile)) break;
            TSharedPtr<FJsonObject> Intent, Receipt;
            if (!ReadPlanRecord(IntentFile, Intent) || Intent->GetStringField(TEXT("planRevision")) != PlanRevision)
            { OutError = TEXT("This plan ID already belongs to different decisions or an unreadable intent record."); return false; }
            if (!ReadPlanRecord(ReceiptFile, Receipt))
            {
                Result->SetStringField(TEXT("state"), TEXT("indeterminate"));
                Result->SetArrayField(TEXT("completedSteps"), Completed);
                OutError = TEXT("A checkpoint began without a terminal receipt. Inspect the working package before authoring a fresh plan.");
                return false;
            }
            if (Receipt->GetStringField(TEXT("planRevision")) != PlanRevision)
            { OutError = TEXT("This checkpoint receipt belongs to a different plan."); return false; }
            const bool bSucceeded = Receipt->GetBoolField(TEXT("success"));
            if (bSucceeded) Completed.Add(SummarizeCheckpoint(Receipt));
            Result->SetArrayField(TEXT("completedSteps"), Completed);
            if (!bDry && StepIndex == Next)
            {
                if (ExpectedToken != Intent->GetStringField(TEXT("previewToken")))
                { OutError = TEXT("The original preview token is required to replay this checkpoint."); return false; }
                Result->SetBoolField(TEXT("replayed"), true);
                Result->SetObjectField(TEXT("receipt"), Receipt);
                Result->SetStringField(TEXT("state"), bSucceeded ? TEXT("completed") : TEXT("failed"));
                if (!bSucceeded) OutError = Receipt->GetStringField(TEXT("error"));
                return bSucceeded;
            }
            if (!bSucceeded)
            {
                Result->SetObjectField(TEXT("receipt"), Receipt);
                Result->SetStringField(TEXT("state"), TEXT("failed"));
                OutError = TEXT("A previous checkpoint failed. Review its receipt before starting a fresh plan.");
                return false;
            }
            PriorHash = Receipt->GetStringField(TEXT("workingHashAfter"));
        }
        Result->SetArrayField(TEXT("completedSteps"), Completed);
        Result->SetNumberField(TEXT("nextStepIndex"), Next);
        if (StepIndex != Next)
        { OutError = TEXT("Only the next checkpoint can be previewed or applied. See nextStepIndex."); return false; }

        auto VerifyWorking = [&](UBlueprint* Working, const FString& Hash)
        {
            FString Ids[3];
            return Working && !Working->GetOutermost()->IsDirty() && !Hash.IsEmpty()
                && ReadStageIds(ProjectDir, RepoPath, Ids, OutError) && StageRevision(Ids) == Stage
                && ReadMergeWorkingHash(Filename) == Hash;
        };
        UBlueprint* Working = LoadObject<UBlueprint>(nullptr, *ObjectPath);
        const FString HashBefore = ReadMergeWorkingHash(Filename);
        if (!VerifyWorking(Working, HashBefore) || !Working->GetPathName().Equals(ObjectPath, ESearchCase::IgnoreCase)
            || (!PriorHash.IsEmpty() && PriorHash != HashBefore))
        { OutError = TEXT("Stages or working file changed, or the exact Blueprint is missing or dirty. Review again."); return false; }

        const bool bGraph = Kinds[StepIndex] == TEXT("graphPatch");
        const auto Tool = MakePlanStep(Kinds[StepIndex]);
        FMCPRequest Native;
        Native.Id = Request.Id;
        Native.Params = MakeShared<FJsonObject>(*Arguments[StepIndex]);
        Native.Params->SetStringField(bGraph ? TEXT("objectPath") : TEXT("path"), bGraph ? ObjectPath : Path);
        Native.Params->SetBoolField(bGraph ? TEXT("saveAfterEdit") : TEXT("save"), true);
        Native.Params->SetBoolField(TEXT("dryRun"), true);
        const auto Preview = Tool->Execute(Native);
        if (Preview.Error.IsSet() || !Preview.Result)
        {
            OutError = Preview.Error.IsSet() ? Preview.Error->Message : TEXT("Could not preflight the next checkpoint.");
            if (Preview.Error.IsSet() && Preview.Error->Data) Result->SetObjectField(TEXT("preflight"), Preview.Error->Data);
            return false;
        }
        FString NativeRevision;
        if (!Preview.Result->TryGetStringField(bGraph ? TEXT("initialGraphRevision") : TEXT("previewToken"), NativeRevision)
            || NativeRevision.IsEmpty() || !VerifyWorking(Working, HashBefore))
        { OutError = TEXT("The checkpoint could not be tied to a stable live revision."); return false; }
        FMD5 TokenHash;
        HashEvidence(TokenHash, PlanRevision);
        HashEvidence(TokenHash, FString::FromInt(StepIndex));
        HashEvidence(TokenHash, HashBefore);
        HashEvidence(TokenHash, NativeRevision);
        const FString Token = FinishEvidence(TokenHash);
        Result->SetStringField(TEXT("previewToken"), Token);
        Result->SetStringField(TEXT("workingHash"), HashBefore);
        if (bDry)
        {
            Result->SetStringField(TEXT("state"), TEXT("preview"));
            Result->SetObjectField(TEXT("preflight"), Preview.Result);
            return true;
        }
        if (ExpectedToken != Token)
        { OutError = TEXT("Checkpoint preview is stale. Preview this step again."); return false; }

        auto Intent = MakeShared<FJsonObject>();
        Intent->SetStringField(TEXT("planRevision"), PlanRevision);
        Intent->SetStringField(TEXT("previewToken"), Token);
        Intent->SetStringField(TEXT("workingHashBefore"), HashBefore);
        if (!IFileManager::Get().MakeDirectory(*Directory, true)
            || !WritePlanRecord(RecordPath(StepIndex, TEXT("intent")), Intent))
        { OutError = TEXT("Could not reserve a durable checkpoint. No edit was started."); return false; }

        Native.Params->SetBoolField(TEXT("dryRun"), false);
        if (bGraph) Native.Params->SetStringField(TEXT("expectedGraphRevision"), NativeRevision);
        else
        {
            Native.Params->SetBoolField(TEXT("confirm"), true);
            Native.Params->SetStringField(TEXT("previewToken"), NativeRevision);
        }
        const auto Applied = VerifyWorking(Working, HashBefore) ? Tool->Execute(Native)
            : BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Stages or working file changed before checkpoint execution."));
        const auto Evidence = Applied.Error.IsSet() ? Applied.Error->Data : Applied.Result;
        const FString HashAfter = ReadMergeWorkingHash(Filename);
        const bool bSavedClean = VerifyWorking(Working, HashAfter);
        const bool bSuccess = !Applied.Error.IsSet() && Applied.Result.IsValid() && bSavedClean;
        auto Receipt = MakeShared<FJsonObject>();
        Receipt->SetStringField(TEXT("planRevision"), PlanRevision);
        Receipt->SetStringField(TEXT("kind"), Kinds[StepIndex]);
        Receipt->SetNumberField(TEXT("stepIndex"), StepIndex);
        Receipt->SetBoolField(TEXT("success"), bSuccess);
        Receipt->SetBoolField(TEXT("workingFileChanged"), HashBefore != HashAfter);
        Receipt->SetBoolField(TEXT("isDirty"), Working->GetOutermost()->IsDirty());
        Receipt->SetStringField(TEXT("workingHashAfter"), HashAfter);
        if (Evidence) Receipt->SetObjectField(TEXT("toolReceipt"), Evidence);
        if (!bSuccess)
        {
            OutError = Applied.Error.IsSet() ? Applied.Error->Message : TEXT("Checkpoint did not finish with stable Git stages and a clean saved target.");
            Receipt->SetStringField(TEXT("error"), OutError);
        }
        Result->SetObjectField(TEXT("receipt"), Receipt);
        if (!WritePlanRecord(RecordPath(StepIndex, TEXT("receipt")), Receipt))
        {
            Result->SetStringField(TEXT("state"), TEXT("indeterminate"));
            OutError = TEXT("A checkpoint ran but its terminal receipt could not be persisted. Inspect the working package before continuing.");
            return false;
        }
        Result->SetStringField(TEXT("state"), bSuccess ? TEXT("completed") : TEXT("failed"));
        if (bSuccess)
        {
            Completed.Add(SummarizeCheckpoint(Receipt));
            Result->SetArrayField(TEXT("completedSteps"), Completed);
            Result->SetNumberField(TEXT("nextStepIndex"), StepIndex + 1);
        }
        return bSuccess;
    }, Error);
    if (!bOk) return BuildError(Request, EMCPErrorCode::InvalidParams, Error, Result);
    Result->SetBoolField(TEXT("success"), true);
    FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FApplyBlueprintMergePlanTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("path"), BuildStringProperty(TEXT("Project-relative Blueprint asset file under Content.")));
    Properties->SetObjectField(TEXT("stageRevision"), BuildStringProperty(TEXT("Exact stageRevision from a same-path review with all three Git stages.")));
    Properties->SetObjectField(TEXT("planId"), BuildStringProperty(TEXT("Client-generated UUID. Preserve it and the same ordered steps to resume or replay a checkpoint.")));
    auto Index = MakeShared<FJsonObject>();
    Index->SetStringField(TEXT("type"), TEXT("integer"));
    Index->SetStringField(TEXT("description"), TEXT("Zero-based checkpoint to preview, apply, or replay."));
    Properties->SetObjectField(TEXT("stepIndex"), Index);
    auto Steps = MakeShared<FJsonObject>();
    Steps->SetStringField(TEXT("type"), TEXT("array"));
    Steps->SetNumberField(TEXT("minItems"), 1);
    Steps->SetNumberField(TEXT("maxItems"), 16);
    auto Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("type"), TEXT("object"));
    auto Fields = MakeShared<FJsonObject>();
    Fields->SetObjectField(TEXT("kind"), BuildStringProperty(TEXT("graphPatch, classDefaults, componentDefaults, addInterface, addVariable or structure.")));
    auto Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("type"), TEXT("object"));
    Args->SetStringField(TEXT("description"), TEXT("Native tool arguments. Omit target, confirmation, save, dry-run, preview and operation ID fields; the plan supplies them."));
    Fields->SetObjectField(TEXT("arguments"), Args);
    Item->SetObjectField(TEXT("properties"), Fields);
    Item->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("kind")), MakeShared<FJsonValueString>(TEXT("arguments"))});
    Steps->SetObjectField(TEXT("items"), Item);
    Properties->SetObjectField(TEXT("steps"), Steps);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Defaults true. Review the next checkpoint before applying it.")));
    Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required to apply. Each successful checkpoint attempts a save; no Git staging.")));
    Properties->SetObjectField(TEXT("previewToken"), BuildStringProperty(TEXT("Exact token returned by this checkpoint preview, also required for receipt replay.")));
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required;
    for (const TCHAR* Field : {TEXT("path"), TEXT("stageRevision"), TEXT("planId"), TEXT("steps"), TEXT("stepIndex")})
        Required.Add(MakeShared<FJsonValueString>(Field));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}

FBlueprintGitConflictTool::FBlueprintGitConflictTool(bool bInStage)
    : FMCPToolBase(bInStage ? TEXT("ApplyBlueprintConflictStage") : TEXT("ListBlueprintGitConflicts"),
        bInStage ? TEXT("Preview or explicitly stage a reviewed Blueprint resolution, including approved absent paths in asymmetric or rename conflicts.")
        : TEXT("List unmerged project Content .uasset paths and their exact Git stage IDs. Includes asymmetric entries.")),
      bStage(bInStage)
{
}

UnrealMCP::FMCPResponse FBlueprintGitConflictTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    const auto& Params = Request.Params;
    auto Result = BuildBooleanResult(true);
    FString Error;
    if (!bStage)
    {
        int32 Offset = 0, Limit = 50;
        if (Params && ((Params->HasField(TEXT("offset")) && !Params->TryGetNumberField(TEXT("offset"), Offset))
            || (Params->HasField(TEXT("limit")) && !Params->TryGetNumberField(TEXT("limit"), Limit))
            || Offset < 0 || Limit < 1 || Limit > 200))
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Use a nonnegative offset and limit 1..200."));
        FString Prefix;
        TArray<FString> Records;
        if (!ReadGitText(Project, TEXT("rev-parse --show-prefix"), Prefix)
            || !ReadGitFields(Project, TEXT("ls-files --unmerged --full-name -z -- Content/"), Records))
            return BuildError(Request, EMCPErrorCode::InternalError, TEXT("Could not read unmerged Git entries."));
        TMap<FString, TArray<FString>> ByPath;
        for (const FString& Record : Records)
        {
            FString Header, RepoPath;
            TArray<FString> Fields;
            if (!Record.Split(TEXT("\t"), &Header, &RepoPath) || (!Prefix.IsEmpty() && !RepoPath.StartsWith(Prefix, ESearchCase::CaseSensitive))) continue;
            const FString Path = RepoPath.Mid(Prefix.Len());
            if (!SafeContentPath(Path)) continue;
            Header.ParseIntoArrayWS(Fields);
            int32 Slot = 0;
            if (Fields.Num() != 3 || !LexTryParseString(Slot, *Fields[2]) || Slot < 1 || Slot > 3 || !IsGitObjectId(Fields[1]))
                return BuildError(Request, EMCPErrorCode::InternalError, TEXT("Malformed Git stage record."));
            auto& Ids = ByPath.FindOrAdd(Path);
            Ids.SetNum(3);
            Ids[Slot - 1] = Fields[1];
        }
        TArray<FString> Paths;
        ByPath.GetKeys(Paths);
        Paths.Sort();
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (int32 Index = Offset; Index < Paths.Num() && Rows.Num() < Limit; ++Index)
        {
            TArray<TSharedPtr<FJsonValue>> Cells = {MakeShared<FJsonValueString>(Paths[Index])};
            for (const FString& Id : ByPath.FindChecked(Paths[Index]))
                Cells.Add(Id.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Id)));
            Rows.Add(MakeShared<FJsonValueArray>(MoveTemp(Cells)));
        }
        Result->SetArrayField(TEXT("columns"), {MakeShared<FJsonValueString>(TEXT("path")), MakeShared<FJsonValueString>(Sides[0]),
            MakeShared<FJsonValueString>(Sides[1]), MakeShared<FJsonValueString>(Sides[2])});
        Result->SetArrayField(TEXT("rows"), Rows);
        Result->SetNumberField(TEXT("total"), Paths.Num());
        if (static_cast<int64>(Offset) + Rows.Num() < Paths.Num()) Result->SetNumberField(TEXT("nextOffset"), Offset + Rows.Num());
    }
    else
    {
        FString Path, ReviewedStages, ExpectedToken, Resolution = TEXT("keep");
        bool bDryRun = true, bConfirm = false;
        if (!Params || !Params->TryGetStringField(TEXT("path"), Path) || !SafeContentPath(Path)
            || !Params->TryGetStringField(TEXT("stageRevision"), ReviewedStages) || ReviewedStages.IsEmpty()
            || (Params->HasField(TEXT("resolution")) && (!Params->TryGetStringField(TEXT("resolution"), Resolution)
                || (Resolution != TEXT("keep") && Resolution != TEXT("delete"))))
            || (Params->HasField(TEXT("dryRun")) && !Params->TryGetBoolField(TEXT("dryRun"), bDryRun))
            || (Params->HasField(TEXT("confirm")) && !Params->TryGetBoolField(TEXT("confirm"), bConfirm))
            || (!bDryRun && (!bConfirm || !Params->TryGetStringField(TEXT("previewToken"), ExpectedToken) || ExpectedToken.IsEmpty())))
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide the reviewed path and stageRevision. Apply requires confirm=true and the preview token."));

        FString StagePaths[3] = {Path, Path, Path}, RepoPaths[3];
        const TSharedPtr<FJsonObject>* Overrides = nullptr;
        if (Params->HasField(TEXT("stagePaths")))
        {
            if (!Params->TryGetObjectField(TEXT("stagePaths"), Overrides))
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide the same stagePaths object used for comparison."));
            for (const auto& Field : (*Overrides)->Values)
            {
                int32 Side = 0;
                while (Side < 3 && FString(Field.Key) != Sides[Side]) ++Side;
                if (Side == 3 || !Field.Value->TryGetString(StagePaths[Side]) || !SafeContentPath(StagePaths[Side]))
                    return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Invalid stage path override."));
            }
        }
        TArray<FString> Paths = {Path};
        for (int32 Side = 0; Side < 3; ++Side)
        {
            if (!ResolveRepoPath(Project, StagePaths[Side], RepoPaths[Side], Error))
                return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
            Paths.AddUnique(StagePaths[Side]);
        }
        const TArray<TSharedPtr<FJsonValue>>* Removed = nullptr;
        TSet<FString> RemovedPaths;
        if (Params->HasField(TEXT("removedPaths")))
        {
            if (!Params->TryGetArrayField(TEXT("removedPaths"), Removed) || Removed->Num() > 3)
                return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("removedPaths must enumerate explicitly approved secondary paths."));
            for (const auto& Value : *Removed)
            {
                FString Extra;
                if (!Value->TryGetString(Extra) || Extra == Path || !Paths.Contains(Extra) || RemovedPaths.Contains(Extra))
                    return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Each removed path must be a distinct secondary review path."));
                RemovedPaths.Add(Extra);
            }
        }
        if (RemovedPaths.Num() != Paths.Num() - 1)
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Approve each secondary rename path explicitly with removedPaths. Only the primary path may be kept."));
        Paths.Sort();
        Result->SetStringField(TEXT("path"), Path);
        Result->SetBoolField(TEXT("dryRun"), bDryRun);
        Result->SetBoolField(TEXT("staged"), false);
        if (!BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
        {
            auto ReadReview = [&]()
            {
                FString Slots[3];
                return ReadReviewStages(Project, RepoPaths, Slots, OutError) && ReviewRevision(Path, StagePaths, Slots) == ReviewedStages;
            };
            if (!ReadReview()) { OutError = TEXT("The reviewed Git stages have changed or are unavailable."); return false; }

            FString Pathspec = TEXT(" --");
            TArray<FString> TargetRepoPaths, Hashes, StageRecords;
            FMD5 Hash;
            HashEvidence(Hash, Project);
            HashEvidence(Hash, Path);
            HashEvidence(Hash, ReviewedStages);
            HashEvidence(Hash, Resolution);
            TArray<TSharedPtr<FJsonValue>> TargetRows;
            for (const FString& Target : Paths)
            {
                const FString Filename = Project / Target;
                FString RepoPath, PackageName;
                if (!ResolveRepoPath(Project, Target, RepoPath, OutError)
                    || !FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName) || !PackageName.StartsWith(TEXT("/Game/")))
                    return false;
                const bool bKeep = Target == Path && Resolution == TEXT("keep");
                const FString WorkingHash = bKeep ? ReadMergeWorkingHash(Filename) : FString();
                if (bKeep)
                {
                    const FString ObjectPath = PackageName + TEXT(".") + FPaths::GetBaseFilename(Target);
                    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
                    if (WorkingHash.IsEmpty() || !Blueprint || !Blueprint->GetPathName().Equals(ObjectPath, ESearchCase::IgnoreCase)
                        || Blueprint->GetOutermost()->IsDirty()
                        || (Blueprint->Status != BS_UpToDate && Blueprint->Status != BS_UpToDateWithWarnings))
                    { OutError = TEXT("The kept Blueprint must be saved, clean, and successfully compiled."); return false; }
                }
                else
                {
                    UPackage* Loaded = FindPackage(nullptr, *PackageName);
                    if (IFileManager::Get().FileExists(*Filename) || (Loaded && Loaded->IsDirty()))
                    { OutError = TEXT("An approved deletion must already be absent on disk and free of unsaved package edits: ") + Target; return false; }
                }
                FString AllSlots[3];
                if (!ReadUnmergedSlots(Project, RepoPath, AllSlots, OutError)
                    || (AllSlots[0].IsEmpty() && AllSlots[1].IsEmpty() && AllSlots[2].IsEmpty()))
                { OutError = TEXT("Every staging target must have unmerged index entries: ") + Target; return false; }
                Hashes.Add(WorkingHash);
                StageRecords.Add(StageRevision(AllSlots));
                TargetRepoPaths.Add(RepoPath);
                HashEvidence(Hash, Target);
                HashEvidence(Hash, WorkingHash);
                HashEvidence(Hash, StageRecords.Last());
                Pathspec += FString::Printf(TEXT(" \":(top,literal)%s\""), *RepoPath);
                auto TargetRow = MakeShared<FJsonObject>();
                TargetRow->SetStringField(TEXT("path"), Target);
                TargetRow->SetStringField(TEXT("resolution"), bKeep ? TEXT("keep") : TEXT("delete"));
                TargetRows.Add(MakeShared<FJsonValueObject>(TargetRow));
            }
            const FString Token = FinishEvidence(Hash);
            Result->SetStringField(TEXT("previewToken"), Token);
            Result->SetStringField(TEXT("stageRevision"), ReviewedStages);
            Result->SetArrayField(TEXT("targets"), TargetRows);
            if (bDryRun) return true;
            if (ExpectedToken != Token || !ReadReview())
            { OutError = TEXT("The reviewed stages or working file changed. Preview staging again."); return false; }
            auto VerifyFiles = [&]()
            {
                for (int32 Index = 0; Index < Paths.Num(); ++Index)
                {
                    const FString File = Project / Paths[Index];
                    if (Hashes[Index].IsEmpty() ? IFileManager::Get().FileExists(*File) : ReadMergeWorkingHash(File) != Hashes[Index])
                        return false;
                }
                return true;
            };
            for (int32 Index = 0; Index < Paths.Num(); ++Index)
            {
                FString Slots[3];
                if (!ReadUnmergedSlots(Project, TargetRepoPaths[Index], Slots, OutError) || StageRevision(Slots) != StageRecords[Index])
                { OutError = TEXT("A staging target's Git stages changed. Preview staging again."); return false; }
            }
            if (!VerifyFiles()) { OutError = TEXT("A staging target's working state changed."); return false; }
            TArray<uint8> Output;
            Result->SetBoolField(TEXT("stageAttempted"), true);
            if (!RunGit(Project, TEXT("add -A") + Pathspec, Output))
            { OutError = TEXT("Git add failed or timed out. Inspect the index before retrying."); return false; }
            TArray<FString> Entries;
            const int32 Kept = Resolution == TEXT("keep") ? Paths.IndexOfByKey(Path) : INDEX_NONE;
            if (!ReadGitFields(Project, TEXT("ls-files --stage --full-name -z") + Pathspec, Entries)
                || Entries.Num() != (Kept == INDEX_NONE ? 0 : 1) || !VerifyFiles())
            { OutError = TEXT("Git add ran but the final index or reviewed working state could not be verified."); return false; }
            if (Kept != INDEX_NONE)
            {
                FString Header, ActualPath;
                TArray<FString> Fields;
                if (!Entries[0].Split(TEXT("\t"), &Header, &ActualPath) || ActualPath != TargetRepoPaths[Kept])
                { OutError = TEXT("Final index entry did not match the kept asset."); return false; }
                Header.ParseIntoArrayWS(Fields);
                if (Fields.Num() != 3 || Fields[2] != TEXT("0") || !IsGitObjectId(Fields[1]))
                { OutError = TEXT("No resolved index entry could be verified."); return false; }
                Result->SetStringField(TEXT("stagedBlob"), Fields[1]);
            }
            Result->SetBoolField(TEXT("staged"), true);
            return true;
        }, Error))
        {
            Result->SetBoolField(TEXT("success"), false);
            return BuildError(Request, EMCPErrorCode::InvalidParams, Error, Result);
        }
    }
    FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FBlueprintGitConflictTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Schema->SetObjectField(TEXT("properties"), Properties);
    if (bStage)
    {
        Properties->SetObjectField(TEXT("path"), BuildStringProperty(TEXT("Exact project-relative Content .uasset path.")));
        Properties->SetObjectField(TEXT("stageRevision"), BuildStringProperty(TEXT("Exact stageRevision from CompareBlueprintMergeStructure.")));
        Properties->SetObjectField(TEXT("resolution"), BuildStringProperty(TEXT("keep (default) or delete. A deleted asset must already be absent on disk.")));
        auto StagePaths = MakeShared<FJsonObject>();
        StagePaths->SetStringField(TEXT("type"), TEXT("object"));
        StagePaths->SetStringField(TEXT("description"), TEXT("Same optional base/current/incoming path overrides used in comparison."));
        Properties->SetObjectField(TEXT("stagePaths"), StagePaths);
        auto Removed = MakeShared<FJsonObject>();
        Removed->SetStringField(TEXT("type"), TEXT("array"));
        Removed->SetObjectField(TEXT("items"), BuildStringProperty(TEXT("Secondary review path already absent from disk.")));
        Properties->SetObjectField(TEXT("removedPaths"), Removed);
        Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Defaults true. Preview without changing the Git index.")));
        Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Explicitly authorize Git staging of the exact resolved asset.")));
        Properties->SetObjectField(TEXT("previewToken"), BuildStringProperty(TEXT("Required when applying. Binds staging to reviewed stages and saved file bytes.")));
        Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("path")), MakeShared<FJsonValueString>(TEXT("stageRevision"))});
    }
    else
        for (const TCHAR* Field : {TEXT("offset"), TEXT("limit")})
        {
            auto Number = MakeShared<FJsonObject>();
            Number->SetStringField(TEXT("type"), TEXT("integer"));
            Number->SetStringField(TEXT("description"), FString(Field) == TEXT("limit") ? TEXT("Page size 1..200, default 50.") : TEXT("Zero-based offset, default 0."));
            Properties->SetObjectField(Field, Number);
        }
    return Schema;
}

FPreviewBlueprintSideReplayTool::FPreviewBlueprintSideReplayTool()
    : FMCPToolBase(TEXT("PreviewBlueprintSideReplay"),
        TEXT("Translate one reviewed Git side's compatible existing-node link and layout changes into a dry-run graph patch. Reports overlaps and unsupported changes."))
{
}

UnrealMCP::FMCPResponse FPreviewBlueprintSideReplayTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    const auto& Params = Request.Params;
    FString Path, Side, GraphGuid, ReviewedStages, Error, RepoPath, PackageName;
    FGuid Guid;
    if (!Params || !Params->TryGetStringField(TEXT("path"), Path) || !SafeContentPath(Path)
        || !Params->TryGetStringField(TEXT("stageRevision"), ReviewedStages)
        || !Params->TryGetStringField(TEXT("graphGuid"), GraphGuid) || !FGuid::Parse(GraphGuid, Guid) || !Guid.IsValid()
        || !Params->TryGetStringField(TEXT("side"), Side) || (Side != TEXT("current") && Side != TEXT("incoming")))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide path, exact graphGuid, reviewed stageRevision and side current or incoming."));

    const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    const FString Filename = Project / Path;
    if (!ResolveRepoPath(Project, Path, RepoPath, Error) || !FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Could not resolve working Blueprint path."));
    FString Ids[3];
    if (!ReadStageIds(Project, RepoPath, Ids, Error) || StageRevision(Ids) != ReviewedStages)
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Review the three same-path Git stages before selecting edits."));
    TArray<uint8> BaseBytes, SideBytes;
    if (!RunGit(Project, TEXT("cat-file blob ") + Ids[0], BaseBytes)
        || !RunGit(Project, TEXT("cat-file blob ") + Ids[Side == TEXT("current") ? 1 : 2], SideBytes))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Could not read the base and selected asset blobs."));

    auto Result = BuildBooleanResult(true);
    const FString Prefix = TEXT("graph/") + Guid.ToString() + TEXT("/");
    if (!BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        FStructure Base, Selected;
        TArray<FString> Missing;
        auto Read = [&](const TArray<uint8>& Bytes, FStructure& Fields)
        {
            return WithHistoricalBlueprint(Project, Path, Bytes, [&](UBlueprint* BP)
            {
                bool bRefs = false;
                Fields = SnapshotStructure(BP, true, bRefs);
                if (Fields.Contains(Prefix + TEXT("path"))) return true;
                OutError = TEXT("The selected graph must exist in both historical revisions.");
                return false;
            }, OutError, &Missing);
        };
        if (!Read(BaseBytes, Base) || !Read(SideBytes, Selected)) return false;
        const FString ObjectPath = PackageName + TEXT(".") + FPaths::GetBaseFilename(Path);
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
        UEdGraph* Graph = nullptr;
        if (!Blueprint || !Blueprint->GetPathName().Equals(ObjectPath, ESearchCase::IgnoreCase) || Blueprint->GetOutermost()->IsDirty()
            || !BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, FString(), Guid.ToString(), Graph, OutError))
        { if (OutError.IsEmpty()) OutError = TEXT("The exact working graph must be clean and loaded."); return false; }
        const FString WorkingHash = ReadMergeWorkingHash(Filename);
        if (WorkingHash.IsEmpty()) { OutError = TEXT("Could not hash the saved working Blueprint."); return false; }
        bool bRefs = false;
        const auto Working = SnapshotStructure(Blueprint, true, bRefs);
        TSet<FString> KeySet;
        for (const auto& Pair : Base) if (Pair.Key.StartsWith(Prefix)) KeySet.Add(Pair.Key);
        for (const auto& Pair : Selected) if (Pair.Key.StartsWith(Prefix)) KeySet.Add(Pair.Key);
        auto Keys = KeySet.Array();
        Keys.Sort();

        TMap<FString, TSharedPtr<FJsonObject>> Nodes;
        TArray<TSharedPtr<FJsonValue>> Connections, Disconnections, Blockers;
        int32 Already = 0, Supported = 0, Blocked = 0;
        auto Block = [&](const FString& Key, const TCHAR* Reason)
        {
            ++Blocked;
            if (Blockers.Num() < 200)
                Blockers.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
                    MakeShared<FJsonValueString>(Key.Mid(Prefix.Len())), MakeShared<FJsonValueString>(Reason)}));
        };
        auto EnsureNode = [&](const FString& NodeGuid)
        {
            auto& Row = Nodes.FindOrAdd(NodeGuid);
            if (!Row)
            {
                Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("id"), NodeGuid);
                Row->SetStringField(TEXT("kind"), TEXT("existingNode"));
                Row->SetStringField(TEXT("nodeGuid"), NodeGuid);
            }
            return Row;
        };
        auto MatchPin = [&](const FString& NodeGuid, const FString& PinGuid, EEdGraphPinDirection Direction) -> const UEdGraphPin*
        {
            FGuid NodeId, PinId;
            if (!FGuid::Parse(NodeGuid, NodeId) || !FGuid::Parse(PinGuid, PinId)) return nullptr;
            for (const UEdGraphNode* Node : Graph->Nodes)
                if (Node && Node->NodeGuid == NodeId)
                {
                    const UEdGraphPin* Match = nullptr;
                    for (const UEdGraphPin* Pin : Node->Pins)
                        if (Pin && Pin->PinId == PinId && Pin->Direction == Direction && !Pin->bOrphanedPin) Match = Pin;
                    if (!Match || Node->Pins.ContainsByPredicate([&](const UEdGraphPin* Pin)
                        { return Pin && Pin != Match && Pin->Direction == Direction && Pin->PinName == Match->PinName; })) return nullptr;
                    return Match;
                }
            return nullptr;
        };
        for (const FString& Key : Keys)
        {
            const FString* Before = Base.Find(Key);
            const FString* After = Selected.Find(Key);
            if (Equal(Before, After)) continue;
            const FString* Now = Working.Find(Key);
            if (Equal(Now, After)) { ++Already; continue; }
            if (!Equal(Now, Before)) { Block(Key, TEXT("working_overlap")); continue; }
            TArray<FString> Parts;
            Key.Mid(Prefix.Len()).ParseIntoArray(Parts, TEXT("/"));
            if (Parts.Num() < 3 || Parts[0] != TEXT("node"))
            { Block(Key, TEXT("unsupported_change")); continue; }
            const FString* BaseClass = Base.Find(Prefix + TEXT("node/") + Parts[1] + TEXT("/class"));
            const FString* LiveClass = Working.Find(Prefix + TEXT("node/") + Parts[1] + TEXT("/class"));
            if (!BaseClass || !Equal(BaseClass, LiveClass))
            { Block(Key, TEXT("missing_or_changed_node")); continue; }
            if (After && Parts.Num() == 3 && (Parts[2] == TEXT("position") || Parts[2] == TEXT("comment")))
            {
                if (Parts[2] == TEXT("comment")) EnsureNode(Parts[1])->SetStringField(TEXT("comment"), *After);
                else
                {
                    FString XText, YText;
                    int32 X = 0, Y = 0;
                    if (!After->Split(TEXT(","), &XText, &YText) || !LexTryParseString(X, *XText) || !LexTryParseString(Y, *YText))
                    { Block(Key, TEXT("invalid_position")); continue; }
                    const auto Node = EnsureNode(Parts[1]);
                    Node->SetNumberField(TEXT("positionX"), X);
                    Node->SetNumberField(TEXT("positionY"), Y);
                }
            }
            else if (Parts.Num() == 8 && Parts[2] == TEXT("pin") && Parts[4] == TEXT("link") && Parts[5] == Guid.ToString())
            {
                const auto* Source = MatchPin(Parts[1], Parts[3], EGPD_Output);
                const auto* Target = MatchPin(Parts[6], Parts[7], EGPD_Input);
                const FString TargetPrefix = Prefix + TEXT("node/") + Parts[6] + TEXT("/pin/") + Parts[7] + TEXT("/");
                const FString SourcePrefix = Prefix + TEXT("node/") + Parts[1] + TEXT("/pin/") + Parts[3] + TEXT("/");
                if (!Source || !Target || !Equal(Base.Find(TargetPrefix + TEXT("type")), Working.Find(TargetPrefix + TEXT("type")))
                    || !Equal(Base.Find(SourcePrefix + TEXT("type")), Working.Find(SourcePrefix + TEXT("type"))))
                { Block(Key, TEXT("missing_or_changed_pin")); continue; }
                auto Link = MakeShared<FJsonObject>();
                Link->SetStringField(TEXT("sourceNodeId"), Parts[1]);
                Link->SetStringField(TEXT("sourcePinName"), Source->PinName.ToString());
                Link->SetStringField(TEXT("targetNodeId"), Parts[6]);
                Link->SetStringField(TEXT("targetPinName"), Target->PinName.ToString());
                EnsureNode(Parts[1]);
                EnsureNode(Parts[6]);
                if (!After) Link->SetBoolField(TEXT("confirm"), true);
                (After ? Connections : Disconnections).Add(MakeShared<FJsonValueObject>(Link));
            }
            else { Block(Key, TEXT("unsupported_change")); continue; }
            ++Supported;
        }
        FString AfterIds[3];
        if (!ReadStageIds(Project, RepoPath, AfterIds, OutError) || StageRevision(AfterIds) != ReviewedStages
            || ReadMergeWorkingHash(Filename) != WorkingHash || Blueprint->GetOutermost()->IsDirty())
        { OutError = TEXT("Working asset or stages changed during replay preview."); return false; }
        Result->SetStringField(TEXT("stageRevision"), ReviewedStages);
        Result->SetNumberField(TEXT("supportedChanges"), Supported);
        Result->SetNumberField(TEXT("alreadyPresent"), Already);
        Result->SetNumberField(TEXT("blockedChanges"), Blocked);
        Result->SetArrayField(TEXT("blockers"), Blockers);
        Result->SetBoolField(TEXT("blockersTruncated"), Blocked > Blockers.Num());
        if (Nodes.IsEmpty()) return true;
        if (Nodes.Num() > 64 || Connections.Num() + Disconnections.Num() > 64)
        {
            Result->SetStringField(TEXT("patchPreflightError"), TEXT("Replay exceeds the node or link budget. Author separate guarded patches from the review evidence."));
            return true;
        }
        auto Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("graphGuid"), Guid.ToString());
        Args->SetStringField(TEXT("expectedGraphRevision"), BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph));
        Args->SetStringField(TEXT("patchId"), TEXT("side-replay-") + Ids[Side == TEXT("current") ? 1 : 2].Left(12) + TEXT("-") + Guid.ToString());
        TArray<FString> NodeIds;
        Nodes.GetKeys(NodeIds);
        NodeIds.Sort();
        TArray<TSharedPtr<FJsonValue>> NodeRows;
        for (const FString& Id : NodeIds) NodeRows.Add(MakeShared<FJsonValueObject>(Nodes.FindChecked(Id)));
        Args->SetArrayField(TEXT("nodes"), NodeRows);
        Args->SetArrayField(TEXT("connections"), Connections);
        Args->SetArrayField(TEXT("disconnections"), Disconnections);
        FMCPRequest Preview;
        Preview.Id = Request.Id;
        Preview.Params = MakeShared<FJsonObject>(*Args);
        Preview.Params->SetStringField(TEXT("objectPath"), ObjectPath);
        Preview.Params->SetBoolField(TEXT("dryRun"), true);
        const auto Check = FApplyBlueprintGraphPatchTool().Execute(Preview);
        if (Check.Error.IsSet() || !Check.Result)
        {
            Result->SetStringField(TEXT("patchPreflightError"), Check.Error.IsSet() ? Check.Error->Message : TEXT("No preflight result."));
            return true;
        }
        auto Step = MakeShared<FJsonObject>();
        Step->SetStringField(TEXT("kind"), TEXT("graphPatch"));
        Step->SetObjectField(TEXT("arguments"), Args);
        Result->SetObjectField(TEXT("step"), Step);
        return true;
    }, Error)) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FPreviewBlueprintSideReplayTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("path"), BuildStringProperty(TEXT("Working project-relative Blueprint path under Content.")));
    Properties->SetObjectField(TEXT("stageRevision"), BuildStringProperty(TEXT("Exact stage revision from the same-path three-way comparison.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Graph GUID present in the saved base, selected side and working Blueprint.")));
    Properties->SetObjectField(TEXT("side"), BuildStringProperty(TEXT("current or incoming.")));
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required;
    for (const TCHAR* Field : {TEXT("path"), TEXT("stageRevision"), TEXT("graphGuid"), TEXT("side")})
        Required.Add(MakeShared<FJsonValueString>(Field));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
