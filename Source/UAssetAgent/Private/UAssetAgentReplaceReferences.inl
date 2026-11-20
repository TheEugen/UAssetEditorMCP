static FString ScopedReferencePath(const FJson& Request, const TCHAR* Field, bool Folder)
{
    const FString Path = ReferencePath(Request, Field, Folder);
    Require((Folder && Path == TEXT("/Game")) || Path.StartsWith(TEXT("/Game/")),
        FString(Field) + TEXT(" must be under /Game"));
    return Path;
}

struct FScopedReplacement
{
    TMap<UObject*, UObject*> Objects;
    TMap<FSoftObjectPath, FSoftObjectPath> Paths;
    UBlueprint* OldBlueprint = nullptr;
    UBlueprint* NewBlueprint = nullptr;

    void Add(UObject* Old, UObject* New)
    {
        if (!Old || !New || Old == New) { return; }
        Objects.Add(Old, New);
        Paths.Add(FSoftObjectPath(Old), FSoftObjectPath(New));
    }

    bool MatchesClass(const UClass* Class) const
    {
        return OldBlueprint && Class && (Class == OldBlueprint->GeneratedClass ||
            Class == OldBlueprint->SkeletonGeneratedClass);
    }
};

static FScopedReplacement BuildScopedReplacement(UObject* Old, UObject* New)
{
    Require(!Old->IsA<UObjectRedirector>() && !New->IsA<UObjectRedirector>(), TEXT("Select the assets themselves using package paths"));
    const bool Materials = Old->IsA<UMaterialInterface>() && New->IsA<UMaterialInterface>();
    const bool Textures = Old->IsA<UTexture>() && New->IsA<UTexture>();
    Require(New->IsA(Old->GetClass()) || Materials || Textures, TEXT("The assets must have compatible classes"));
    FScopedReplacement Pair;
    Pair.Add(Old, New);
    Pair.OldBlueprint = Cast<UBlueprint>(Old);
    Pair.NewBlueprint = Cast<UBlueprint>(New);
    if (!Pair.OldBlueprint) { return Pair; }

    UClass* OldClass = Pair.OldBlueprint->GeneratedClass;
    UClass* NewClass = Pair.NewBlueprint ? Pair.NewBlueprint->GeneratedClass.Get() : nullptr;
    Require(OldClass && NewClass && Pair.OldBlueprint->BlueprintType == Pair.NewBlueprint->BlueprintType &&
        !NewClass->IsChildOf(OldClass), TEXT("Incompatible Blueprint types or a replacement that inherits from the source"));
    Pair.Add(OldClass, NewClass);
    Pair.Add(Pair.OldBlueprint->SkeletonGeneratedClass, Pair.NewBlueprint->SkeletonGeneratedClass);
    Pair.Add(OldClass->GetDefaultObject(), NewClass->GetDefaultObject());
    for (TFieldIterator<UFunction> It(OldClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        if (UFunction* Target = NewClass->FindFunctionByName(It->GetFName())) { Pair.Add(*It, Target); }
    }
    return Pair;
}

class FScopedReferenceArchive final : public FArchiveReplaceObjectAndStructPropertyRef<UObject>
{
    using Super = FArchiveReplaceObjectAndStructPropertyRef<UObject>;
    const FScopedReplacement& Pair;
    UPackage* Package;

public:
    FScopedReferenceArchive(UObject* Root, const FScopedReplacement& Replacement)
        : Super(Root, Replacement.Objects, EArchiveReplaceObjectFlags::DelayStart |
              EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef)
        , Pair(Replacement)
        , Package(Root->GetPackage())
    {
        SerializeSearchObject();
    }

    using Super::operator<<;
    FArchive& operator<<(UObject*& Value) override
    {
        if (UObject* Target = Pair.Objects.FindRef(Value))
        {
            if (const FObjectPropertyBase* Property = CastField<FObjectPropertyBase>(GetSerializedProperty()))
            {
                UClass* Expected = Property->PropertyClass;
                if (Property->GetOwnerClass() && !Property->GetOwnerClass()->HasAnyClassFlags(CLASS_Native) &&
                    Pair.MatchesClass(Expected))
                {
                    Expected = Pair.NewBlueprint->GeneratedClass;
                }
                Require(Target->IsA(Expected), TEXT("Replacement is incompatible with property: ") + Property->GetName());
                if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
                {
                    const UClass* Class = Cast<UClass>(Target);
                    // Declared fields on Blueprints are retyped and rebuilt by
                    // the compiler. Native metaclass constraints are fixed.
                    if (ClassProperty->GetOwnerClass() && ClassProperty->GetOwnerClass()->HasAnyClassFlags(CLASS_Native))
                    {
                        Require(Class && Class->IsChildOf(ClassProperty->MetaClass),
                            TEXT("Replacement is incompatible with native class property: ") + Property->GetName());
                    }
                }
            }
        }
        else if (Value && Value->GetPackage() != Package)
        {
            return *this;
        }
        return FArchiveReplaceObjectRef<UObject>::operator<<(Value);
    }

    FArchive& operator<<(FSoftObjectPath& Path) override
    {
        if (const FSoftObjectPath* Target = Pair.Paths.Find(Path))
        {
            Path = *Target;
            ++Count;
        }
        return *this;
    }
};

static int32 RetargetScopedNodes(UBlueprint* Blueprint, const FScopedReplacement& Pair)
{
    int32 Changed = 0;
    UClass* NewClass = Pair.NewBlueprint ? Pair.NewBlueprint->GeneratedClass.Get() : nullptr;
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) { continue; }
            if (UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
            {
                FMemberReference& Ref = Variable->VariableReference;
                if (!Ref.IsSelfContext() && Pair.MatchesClass(Ref.GetMemberParentClass()))
                {
                    const FName Name = Ref.GetMemberName();
                    const FProperty* Property = FindFProperty<FProperty>(NewClass, Name);
                    Require(Property && Property->HasAnyPropertyFlags(CPF_BlueprintVisible) &&
                        (!Variable->IsA<UK2Node_VariableSet>() || !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly)),
                        TEXT("Replacement class has no compatible member: ") + Name.ToString());
                    const UClass* Owner = Property->GetOwnerClass();
                    UBlueprint* OwnerBlueprint = Owner ? Cast<UBlueprint>(Owner->ClassGeneratedBy) : nullptr;
                    const FGuid Id = OwnerBlueprint ? FBlueprintEditorUtils::FindMemberVariableGuidByName(OwnerBlueprint, Name) : FGuid();
                    Ref.SetExternalMember(Name, NewClass, Id);
                    ++Changed;
                }
            }
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
            {
                FMemberReference& Ref = Call->FunctionReference;
                if (!Ref.IsSelfContext() && Pair.MatchesClass(Ref.GetMemberParentClass()))
                {
                    UFunction* Function = NewClass->FindFunctionByName(Ref.GetMemberName());
                    Require(Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure),
                        TEXT("Replacement class has no callable function: ") + Ref.GetMemberName().ToString());
                    Call->SetFromFunction(Function);
                    ++Changed;
                }
            }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) { continue; }
                const FName Category = Pin->PinType.PinCategory;
                if (Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_Object ||
                    Category == UEdGraphSchema_K2::PC_SoftClass || Category == UEdGraphSchema_K2::PC_SoftObject)
                {
                    for (FString* Default : {&Pin->DefaultValue, &Pin->AutogeneratedDefaultValue})
                    {
                        if (const FSoftObjectPath* Target = Pair.Paths.Find(FSoftObjectPath(*Default)))
                        { *Default = Target->ToString(); ++Changed; }
                    }
                }
            }
        }
    }
    return Changed;
}

static FMemberNodeSnapshot ScopedBlueprintWiring(UBlueprint* Blueprint)
{
    FMemberNodeSnapshot Snapshot;
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph->GetSchema()->IsA<UEdGraphSchema_K2>()) { continue; }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) { continue; }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output || Pin->bOrphanedPin) { continue; }
                for (UEdGraphPin* To : Pin->LinkedTo)
                {
                    if (To && !To->bOrphanedPin) { Snapshot.Links.Add({Node, Pin->PinName, To->GetOwningNode(), To->PinName}); }
                }
            }
        }
    }
    return Snapshot;
}

static FString ReferrerOwner(IAssetRegistry& Registry, FName Package)
{
    TArray<FAssetData> Data;
    Registry.GetAssetsByPackageName(Package, Data, true, false);
    return Data.IsEmpty() ? Package.ToString() : Data[0].GetSoftObjectPath().GetLongPackageName();
}

static TMap<UBlueprint*, FMemberNodeSnapshot> ReplaceReferencesInPackage(const FScopedReplacement& Pair, UObject* Asset,
    UPackage* Package, const FJson& Report)
{
    TArray<UObject*> Objects;
    GetObjectsWithPackage(Package, Objects, false, RF_Transient);
    if (Asset) { Objects.AddUnique(Asset); }
    TArray<UBlueprint*> Blueprints;
    for (UObject* Root : Objects)
    {
        if (AActor* Actor = Cast<AActor>(Root))
        {
            Require(!Pair.MatchesClass(Actor->GetClass()), TEXT("Use level_apply to replace the class of placed actor: ") + Actor->GetPathName());
        }
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Root)) { Blueprints.AddUnique(Blueprint); }
    }
    if (UWorld* World = Cast<UWorld>(Asset))
    {
        if (ULevel* Level = World->PersistentLevel)
        {
            for (AActor* Actor : Level->Actors)
            {
                Require(!Actor || !Pair.MatchesClass(Actor->GetClass()), TEXT("Use level_apply to replace the class of placed actor: ") + GetPathNameSafe(Actor));
            }
            if (ULevelScriptBlueprint* Script = Level->GetLevelScriptBlueprint(true)) { Blueprints.AddUnique(Script); }
        }
    }
    int64 Changes = 0;
    TMap<UBlueprint*, FMemberNodeSnapshot> Wiring;
    for (UBlueprint* Blueprint : Blueprints)
    {
        Wiring.Add(Blueprint, ScopedBlueprintWiring(Blueprint));
        Changes += RetargetScopedNodes(Blueprint, Pair);
    }
    for (UObject* Root : Objects)
    {
        if (IsValid(Root)) { Changes += FScopedReferenceArchive(Root, Pair).GetCount(); }
    }
    Report->SetNumberField(TEXT("replacements"), Changes);
    Require(Changes > 0, TEXT("The registry reports a dependency but no replaceable references were found"));
    return Wiring;
}

static void CompileScopedReferences(const TMap<UBlueprint*, FMemberNodeSnapshot>& Wiring, const FJson& Report)
{
    bool Compiled = true;
    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    for (const auto& Edit : Wiring)
    {
        UBlueprint* Blueprint = Edit.Key;
        FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
        ReconstructMemberNodes(Blueprint, Edit.Value);
        FJson Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
        const bool Success = Compile(Blueprint, Result);
        Result->SetBoolField(TEXT("ok"), Success);
        Compiled &= Success;
        Diagnostics.Add(JsonValue(Result));
    }
    Report->SetArrayField(TEXT("compiles"), Diagnostics);
    Require(Compiled, TEXT("Blueprint compilation failed after reference replacement"));
}

static void ReplaceReferencesAction(const FJson& Request, const FJson& Response)
{
    const FString From = ScopedReferencePath(Request, TEXT("from"), false);
    const FString To = ScopedReferencePath(Request, TEXT("to"), false);
    Require(!From.Equals(To, ESearchCase::IgnoreCase), TEXT("from and to must be different packages"));
    const auto Entries = Array(Request, TEXT("scope"), true);
    Require(!Entries.IsEmpty() && Entries.Num() <= 1000, TEXT("scope requires 1 to 1000 package or folder paths"));
    TArray<FString> Scope;
    for (const auto& Entry : Entries)
    {
        FJson Path = MakeShared<FJsonObject>();
        Path->SetField(TEXT("scope entry"), Entry);
        Scope.Add(ScopedReferencePath(Path, TEXT("scope entry"), true));
    }
    const bool DryRun = Boolean(Request, TEXT("dry_run"), true);
    const bool ExcludeMaps = Boolean(Request, TEXT("exclude_maps"), true);
    Response->SetBoolField(TEXT("dry_run"), DryRun);
    Response->SetBoolField(TEXT("exclude_maps"), ExcludeMaps);
    Response->SetStringField(TEXT("from"), From);
    Response->SetStringField(TEXT("to"), To);
    const FJson Expected = !DryRun || Request->HasField(TEXT("expected_revisions")) ?
        Object(Request, TEXT("expected_revisions")) : nullptr;

    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.SearchAllAssets(true);
    TArray<FName> Referrers = PackageReferences(Registry, FName(*From), true, true);
    Referrers.Sort(FNameLexicalLess());
    struct FTarget
    {
        FString Name;
        FString File;
        bool External;
        UObject* Asset = nullptr;
        UPackage* Package = nullptr;
        FJson Report = MakeShared<FJsonObject>();
        TMap<UBlueprint*, FMemberNodeSnapshot> Wiring;
    };
    TArray<FTarget> Targets;
    TArray<FString> SkippedMaps;
    TMap<FString, FString> Files{{From, AssetFilename(From)}, {To, AssetFilename(To)}};
    for (FName Name : Referrers)
    {
        const FString Package = Name.ToString();
        if (!Package.StartsWith(TEXT("/Game/")) || Package.Equals(From, ESearchCase::IgnoreCase) ||
            Package.Equals(To, ESearchCase::IgnoreCase)) { continue; }
        const bool External = Package.Contains(TEXT("/__ExternalActors__/")) || Package.Contains(TEXT("/__ExternalObjects__/"));
        const FString Owner = External ? ReferrerOwner(Registry, Name) : Package;
        const bool InScope = Scope.ContainsByPredicate([&](const FString& Path)
        {
            return WithinReferencePath(Package, Path) || WithinReferencePath(Owner, Path);
        });
        if (!InScope) { continue; }
        const FString MapFile = LevelFilename(Package);
        const bool IsMap = IFileManager::Get().FileExists(*MapFile);
        if (ExcludeMaps && (IsMap || External)) { SkippedMaps.Add(Package); continue; }
        const FString Filename = IsMap ? MapFile : AssetFilename(Package);
        Targets.Add({Package, Filename, External});
        Files.Add(Package, Filename);
    }
    Response->SetArrayField(TEXT("excluded_maps"), LevelStrings(SkippedMaps));
    FJson Revisions = MakeShared<FJsonObject>();
    for (const auto& Entry : Files) { Revisions->SetStringField(Entry.Key, Revision(Entry.Value)); }
    Response->SetObjectField(TEXT("revisions"), Revisions);
    const auto Verify = [&]()
    {
        if (Expected)
        {
            Require(Expected->Values.Num() == Revisions->Values.Num(), TEXT("Replacement plan changed; repeat the dry run"));
            for (const auto& Entry : Revisions->Values)
            {
                Require(String(Expected, *Entry.Key) == Entry.Value->AsString(), TEXT("Revision conflict: ") + Entry.Key);
            }
        }
        for (const auto& Entry : Files)
        { Require(Revision(Entry.Value) == String(Revisions, *Entry.Key), TEXT("Package changed during replacement: ") + Entry.Key); }
    };
    Verify();
    const FScopedReplacement Pair = BuildScopedReplacement(LoadExactAsset(From), LoadExactAsset(To));
    TArray<TSharedPtr<FJsonValue>> Reports;
    bool Valid = true;
    const auto RecordFailure = [&](FTarget& Target, const FFailure& Failure)
    {
        Target.Report->SetBoolField(TEXT("ok"), false);
        Target.Report->SetStringField(TEXT("error"), Failure.Message);
        Valid = false;
    };
    for (FTarget& Target : Targets)
    {
        Target.Report->SetStringField(TEXT("asset"), Target.Name);
        Reports.Add(JsonValue(Target.Report));
        try
        {
            Require(!IFileManager::Get().IsReadOnly(*Target.File) &&
                !IFileManager::Get().FileExists(*FPaths::ChangeExtension(Target.File, TEXT("uexp"))) &&
                !IFileManager::Get().FileExists(*FPaths::ChangeExtension(Target.File, TEXT("ubulk"))),
                TEXT("The package is read-only or split"));
            if (!Target.External) { Target.Asset = LoadExactAsset(Target.Name); }
            Require(!Target.Asset || !Target.Asset->IsA<UObjectRedirector>(), TEXT("The scope includes an asset redirector"));
            Target.Package = Target.Asset ? Target.Asset->GetPackage() : LoadPackage(nullptr, *Target.Name, LOAD_None);
            Require(Target.Package != nullptr, TEXT("Could not load the referring package"));
            Target.Package->FullyLoad();
            if (Target.External)
            {
                TArray<UObject*> Objects;
                GetObjectsWithPackage(Target.Package, Objects, true, RF_Transient);
                for (UObject* Object : Objects)
                {
                    if (Object->IsAsset() && Object->GetExternalPackage() == Target.Package)
                    {
                        Require(!Target.Asset, TEXT("External package contains multiple top-level assets"));
                        Target.Asset = Object;
                    }
                }
                Require(Target.Asset != nullptr, TEXT("External package has no saveable asset"));
            }
        }
        catch (const FFailure& Failure) { RecordFailure(Target, Failure); }
    }
    // Rewrite the entire scope before recompiling any Blueprint. A referring
    // Blueprint can depend on another package in the same replacement batch.
    for (FTarget& Target : Targets)
    {
        if (Target.Report->HasField(TEXT("error"))) { continue; }
        try { Target.Wiring = ReplaceReferencesInPackage(Pair, Target.Asset, Target.Package, Target.Report); }
        catch (const FFailure& Failure) { RecordFailure(Target, Failure); }
    }
    for (FTarget& Target : Targets)
    {
        if (Target.Report->HasField(TEXT("error"))) { continue; }
        try
        {
            CompileScopedReferences(Target.Wiring, Target.Report);
            Target.Report->SetBoolField(TEXT("ok"), true);
        }
        catch (const FFailure& Failure) { RecordFailure(Target, Failure); }
    }
    Response->SetArrayField(TEXT("packages"), Reports);
    Require(Valid, TEXT("One or more packages could not be migrated"));
    if (DryRun || Targets.IsEmpty()) { return; }
    Verify();

    IFileManager& FileManager = IFileManager::Get();
    const FString BackupRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("UAssetAgent/Backups"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    FJson Backups = MakeShared<FJsonObject>();
    for (const FTarget& Target : Targets)
    {
        const FString Backup = FPaths::Combine(BackupRoot, Target.Name.Mid(1) + FPaths::GetExtension(Target.File, true));
        Require(FileManager.MakeDirectory(*FPaths::GetPath(Backup), true) &&
            FileManager.Copy(*Backup, *Target.File, false) == COPY_OK, TEXT("Could not back up package: ") + Target.Name);
        Backups->SetStringField(Target.Name, Backup);
    }
    Response->SetObjectField(TEXT("backups"), Backups);
    Verify();
    TArray<FString> Saved;
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        const FTarget& Target = Targets[Index];
        FSavePackageArgs Save;
        Save.TopLevelFlags = Target.External ? RF_NoFlags : RF_Standalone;
        Save.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Target.Package, Target.Asset, *Target.File, Save))
        {
            TArray<FString> Unrestored;
            for (int32 Restore = Index; Restore >= 0; --Restore)
            {
                const FTarget& Previous = Targets[Restore];
                ResetLoaders(Previous.Package);
                if (FileManager.Copy(*Previous.File, *String(Backups, *Previous.Name), true) != COPY_OK)
                { Unrestored.Add(Previous.Name); }
            }
            Response->SetArrayField(TEXT("rollback_failures"), LevelStrings(Unrestored));
            Require(false, TEXT("Could not write package: ") + Target.Name);
        }
        Saved.Add(Target.Name);
    }
    Response->SetArrayField(TEXT("changed_packages"), LevelStrings(Saved));
    Response->SetBoolField(TEXT("saved"), true);
}
