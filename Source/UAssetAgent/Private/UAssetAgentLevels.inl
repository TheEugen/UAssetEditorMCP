static FString LevelFilename(const FString& Package)
{
    return FPaths::ConvertRelativePathToFull(
        FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension()));
}

static FString LevelRevision(const FString& MapFile, const FString& ExternalDirectory)
{
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *ExternalDirectory, TEXT("*.uasset"), true, false);
    Files.Sort();
    FSHA1 Hash;
    const auto Add = [&Hash](const FString& Value)
    {
        const FTCHARToUTF8 Bytes(*Value);
        Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
    };
    Add(Revision(MapFile));
    for (const FString& File : Files)
    {
        Add(File);
        Add(Revision(File));
    }
    Hash.Final();
    uint8 Bytes[20];
    Hash.GetHash(Bytes);
    return BytesToHex(Bytes, 20).ToLower();
}

static bool EditableInstanceProperty(const FProperty* Property)
{
    return Property && Property->HasAnyPropertyFlags(CPF_Edit) &&
        !Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditConst | CPF_DisableEditOnInstance) &&
        Property->GetFName() != GET_MEMBER_NAME_CHECKED(AActor, Tags);
}

static TArray<TSharedPtr<FJsonValue>> LevelStrings(const TArray<FString>& Strings)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FString& Value : Strings) { Values.Add(MakeShared<FJsonValueString>(Value)); }
    return Values;
}

static TArray<TSharedPtr<FJsonValue>> LevelVector(double X, double Y, double Z)
{
    return {MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(Y), MakeShared<FJsonValueNumber>(Z)};
}

static FVector LevelVector(const FJson& Json, const TCHAR* Field, const FVector& Default)
{
    if (!Json->HasField(Field)) { return Default; }
    const auto Values = Array(Json, Field, true);
    double Components[3];
    Require(Values.Num() == 3, FString(Field) + TEXT(" must contain three numbers"));
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Require(Values[Index]->TryGetNumber(Components[Index]) && FMath::IsFinite(Components[Index]),
            FString(Field) + TEXT(" must contain three finite numbers"));
    }
    return FVector(Components[0], Components[1], Components[2]);
}

static FJson DescribeLevelActor(AActor* Actor)
{
    FJson Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("guid"), Guid(Actor->GetActorGuid()));
    Item->SetStringField(TEXT("name"), Actor->GetName());
    Item->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Item->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
    Item->SetStringField(TEXT("package"), Actor->GetPackage()->GetName());
    const FVector Location = Actor->GetActorLocation();
    const FRotator Rotation = Actor->GetActorRotation();
    const FVector Scale = Actor->GetActorScale3D();
    FJson Transform = MakeShared<FJsonObject>();
    Transform->SetArrayField(TEXT("location"), LevelVector(Location.X, Location.Y, Location.Z));
    Transform->SetArrayField(TEXT("rotation"), LevelVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
    Transform->SetArrayField(TEXT("scale"), LevelVector(Scale.X, Scale.Y, Scale.Z));
    Item->SetObjectField(TEXT("transform"), Transform);
    TArray<FString> Tags;
    for (FName Tag : Actor->Tags) { Tags.Add(Tag.ToString()); }
    Item->SetArrayField(TEXT("tags"), LevelStrings(Tags));
    FJson Overrides = MakeShared<FJsonObject>();
    UObject* Defaults = Actor->GetClass()->GetDefaultObject();
    TSet<FString> References;
    for (TFieldIterator<FProperty> It(Actor->GetClass()); It; ++It)
    {
        if (!EditableInstanceProperty(*It)) { continue; }
        if (!It->Identical_InContainer(Actor, Defaults))
        {
            FString Value;
            It->ExportText_InContainer(0, Value, Actor, Actor, Actor, PPF_None);
            Overrides->SetStringField(It->GetName(), Value);
        }
        if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(*It))
        {
            const FSoftObjectPtr* Value = Soft->ContainerPtrToValuePtr<FSoftObjectPtr>(Actor);
            if (!Value->IsNull()) { References.Add(Value->ToSoftObjectPath().ToString()); }
        }
    }
    TArray<UObject*> Objects;
    FReferenceFinder Finder(Objects, nullptr, false, true, false, true);
    Finder.FindReferences(Actor);
    for (UActorComponent* Component : Actor->GetComponents())
    {
        if (IsValid(Component)) { Finder.FindReferences(Component); }
    }
    for (const UObject* Object : Objects)
    {
        if (const UClass* Class = Cast<UClass>(Object))
        {
            if (Class->ClassGeneratedBy) { References.Add(Class->ClassGeneratedBy->GetPathName()); }
        }
        else if (Object->IsAsset()) { References.Add(Object->GetPathName()); }
    }
    TArray<FString> SortedReferences = References.Array();
    SortedReferences.Sort();
    Item->SetObjectField(TEXT("instance_properties"), Overrides);
    Item->SetArrayField(TEXT("referenced_assets"), LevelStrings(SortedReferences));
    return Item;
}

static AActor* SelectLevelActor(const TArray<AActor*>& Actors, const FString& Selector)
{
    FGuid ActorGuid;
    const bool ByGuid = FGuid::Parse(Selector, ActorGuid);
    AActor* Found = nullptr;
    for (AActor* Actor : Actors)
    {
        if (!IsValid(Actor)) { continue; }
        if (ByGuid ? Actor->GetActorGuid() == ActorGuid :
            Actor->GetName() == Selector || Actor->GetActorLabel() == Selector)
        {
            Require(Found == nullptr, TEXT("Actor selector is ambiguous; use its GUID: ") + Selector);
            Found = Actor;
        }
    }
    Require(Found != nullptr, TEXT("Actor not found: ") + Selector);
    return Found;
}

static UClass* LevelActorClass(const FJson& Op, const FJson& Result)
{
    const FString Path = String(Op, TEXT("class"));
    UClass* Class = LoadObject<UClass>(nullptr, *Path);
    Require(Class && Class->IsChildOf(AActor::StaticClass()) &&
        !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists),
        TEXT("Expected a concrete actor class: ") + Path);
    if (UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
    {
        Require(Compile(Blueprint, Result), TEXT("Actor Blueprint failed to compile: ") + Path);
        return Blueprint->GeneratedClass;
    }
    return Class;
}

static void CopyLevelActorOverrides(AActor* Old, AActor* New, const FJson& Result)
{
    UObject* Defaults = Old->GetClass()->GetDefaultObject();
    TArray<FString> Copied;
    TArray<FString> Skipped;
    for (TFieldIterator<FProperty> It(Old->GetClass()); It; ++It)
    {
        if (!EditableInstanceProperty(*It) ||
            It->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference) ||
            It->Identical_InContainer(Old, Defaults)) { continue; }
        FProperty* Target = FindFProperty<FProperty>(New->GetClass(), It->GetFName());
        if (EditableInstanceProperty(Target) && Target->SameType(*It) && Target->ArrayDim == It->ArrayDim)
        {
            Target->CopyCompleteValue_InContainer(New, Old);
            Copied.Add(It->GetName());
        }
        else { Skipped.Add(It->GetName()); }
    }
    Result->SetArrayField(TEXT("copied_properties"), LevelStrings(Copied));
    Result->SetArrayField(TEXT("skipped_properties"), LevelStrings(Skipped));
}

static void ReplaceLevelActorReferences(ULevel* Level, const TArray<AActor*>& Actors, AActor* Old,
    AActor* New, TMap<UPackage*, UObject*>& Changes)
{
    const TMap<AActor*, AActor*> Replacements{{Old, New}};
    const auto ReplaceIn = [&](UObject* Root)
    {
        FArchiveReplaceObjectRef<AActor> Archive(Root, Replacements, EArchiveReplaceObjectFlags::IgnoreOuterRef |
            EArchiveReplaceObjectFlags::IgnoreArchetypeRef | EArchiveReplaceObjectFlags::TrackReplacedReferences);
        for (const auto& Entry : Archive.GetReplacedReferences())
        {
            UObject* Object = Entry.Key;
            if (Object == Old || Object->IsIn(Old) || Object->HasAnyFlags(RF_Transient)) { continue; }
            for (const FProperty* Property : Entry.Value)
            {
                if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
                {
                    Require(!New || New->IsA(ObjectProperty->PropertyClass),
                        TEXT("Incompatible actor reference: ") + Object->GetPathName() + TEXT(".") + Property->GetName());
                }
            }
            AActor* Owner = Cast<AActor>(Object);
            if (!Owner) { Owner = Object->GetTypedOuter<AActor>(); }
            Changes.FindOrAdd(Object->GetPackage(), Owner);
        }
    };
    ReplaceIn(Level);
    if (New) { ReplaceIn(New); }
    for (AActor* Actor : Actors)
    {
        if (IsValid(Actor) && Actor != Old && Actor->GetExternalPackage()) { ReplaceIn(Actor); }
    }
}

struct FLevelWrite
{
    UPackage* Package = nullptr;
    UObject* Top = nullptr;
    FString Filename;
    FString Backup;
    bool Delete = false;
};

static void SaveLevelChanges(const FString& MapFile, const FString& ExternalDirectory, const FString& Before,
    const TMap<UPackage*, UObject*>& Changes, const TSet<UPackage*>& Deletions, UWorld* World, const FJson& Response)
{
    IFileManager& Files = IFileManager::Get();
    const FString BackupRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("UAssetAgent/Backups"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    TArray<FLevelWrite> Writes;
    FJson Backups = MakeShared<FJsonObject>();
    for (const auto& Change : Changes)
    {
        const FString Package = Change.Key->GetName();
        const bool IsMap = Change.Key == World->GetPackage();
        const FString Filename = IsMap ? MapFile : AssetFilename(Package);
        Require(IsMap || FPaths::IsUnderDirectory(Filename, ExternalDirectory), TEXT("Unexpected changed package: ") + Package);
        Require(!Files.IsReadOnly(*Filename) &&
            !Files.FileExists(*FPaths::ChangeExtension(Filename, TEXT("uexp"))) &&
            !Files.FileExists(*FPaths::ChangeExtension(Filename, TEXT("ubulk"))),
            TEXT("Package is read-only or split: ") + Package);
        const FString Backup = Files.FileExists(*Filename) ?
            FPaths::Combine(BackupRoot, Package.Mid(1) + FPaths::GetExtension(Filename, true)) : FString();
        if (!Backup.IsEmpty())
        {
            Require(Files.MakeDirectory(*FPaths::GetPath(Backup), true) &&
                Files.Copy(*Backup, *Filename, false) == COPY_OK, TEXT("Cannot back up package: ") + Package);
            Backups->SetStringField(Package, Backup);
        }
        Writes.Add({Change.Key, IsMap ? World : Change.Value, Filename, Backup, Deletions.Contains(Change.Key)});
    }
    Response->SetObjectField(TEXT("backups"), Backups);
    Require(LevelRevision(MapFile, ExternalDirectory) == Before, TEXT("Level or external actors changed during the request"));
    Writes.Sort([](const FLevelWrite& A, const FLevelWrite& B) { return A.Filename < B.Filename; });
    TArray<FString> Changed;
    for (int32 Index = 0; Index < Writes.Num(); ++Index)
    {
        const FLevelWrite& Write = Writes[Index];
        if (Write.Delete) { ResetLoaders(Write.Package); }
        FSavePackageArgs Save;
        Save.TopLevelFlags = Write.Package == World->GetPackage() ? RF_Standalone : RF_NoFlags;
        Save.SaveFlags = SAVE_NoError;
        const bool Success = Write.Delete ? !Files.FileExists(*Write.Filename) || Files.Delete(*Write.Filename, true) :
            UPackage::SavePackage(Write.Package, Write.Top, *Write.Filename, Save);
        if (!Success)
        {
            TArray<FString> Unrestored;
            for (int32 Restore = Index; Restore >= 0; --Restore)
            {
                const FLevelWrite& Previous = Writes[Restore];
                const bool Restored = Previous.Backup.IsEmpty() ?
                    !Files.FileExists(*Previous.Filename) || Files.Delete(*Previous.Filename, true) :
                    Files.Copy(*Previous.Filename, *Previous.Backup, true) == COPY_OK;
                if (!Restored) { Unrestored.Add(Previous.Package->GetName()); }
            }
            Response->SetArrayField(TEXT("rollback_failures"), LevelStrings(Unrestored));
            Require(false, TEXT("Cannot write level package: ") + Write.Package->GetName());
        }
        Changed.Add(Write.Package->GetName());
    }
    Response->SetArrayField(TEXT("changed_packages"), LevelStrings(Changed));
    Response->SetBoolField(TEXT("saved"), !Writes.IsEmpty());
    Response->SetStringField(TEXT("revision"), LevelRevision(MapFile, ExternalDirectory));
}

static void LevelAction(const FJson& Request, const FJson& Response, bool Apply)
{
    const FString Package = PackagePath(Request);
    const FString MapFile = LevelFilename(Package);
    const FString ExternalDirectory = FPaths::ConvertRelativePathToFull(
        FPackageName::LongPackageNameToFilename(ULevel::GetExternalActorsPath(Package)));
    const FString Before = LevelRevision(MapFile, ExternalDirectory);
    Response->SetStringField(TEXT("asset"), Package);
    Response->SetStringField(TEXT("revision"), Before);
    const bool DryRun = Boolean(Request, TEXT("dry_run"), true);
    TArray<TSharedPtr<FJsonValue>> Operations;
    if (Apply)
    {
        Require(String(Request, TEXT("expected_revision")) == Before, TEXT("Revision conflict"));
        Operations = Array(Request, TEXT("operations"), true);
        Require(Operations.Num() <= 1000, TEXT("At most 1000 operations are allowed"));
        Response->SetBoolField(TEXT("dry_run"), DryRun);
    }
    const UWorld::InitializationValues Init = UWorld::InitializationValues().RequiresHitProxies(false)
        .ShouldSimulatePhysics(false).EnableTraceCollision(false).CreateNavigation(false).CreateAISystem(false)
        .AllowAudioPlayback(false);
    FScopedEditorWorld Scope{FStringView(Package), Init};
    UWorld* World = Scope.GetWorld();
    Require(World && World->PersistentLevel, TEXT("Could not open level: ") + Package);
    ULevel* Level = World->PersistentLevel;
    UWorldPartition* Partition = World->GetWorldPartition();
    FWorldPartitionHelpers::FForEachActorWithLoadingResult LoadedActors;
    if (Partition)
    {
        FWorldPartitionHelpers::FForEachActorWithLoadingParams Params;
        Params.bKeepReferences = true;
        FWorldPartitionHelpers::ForEachActorWithLoading(Partition, [](const FWorldPartitionActorDescInstance* Desc)
        {
            Require(IsValid(Desc->GetActor()), TEXT("Could not load external actor: ") + Desc->GetActorPackage().ToString());
            return true;
        }, Params, LoadedActors);
    }
    Response->SetBoolField(TEXT("world_partition"), Partition != nullptr);
    TArray<AActor*> Actors;
    for (AActor* Actor : Level->Actors)
    {
        if (IsValid(Actor) && Actor->IsUserManaged() && !Actor->HasAnyFlags(RF_Transient)) { Actors.Add(Actor); }
    }
    if (!Apply)
    {
        Actors.Sort([](const AActor& A, const AActor& B) { return A.GetName() < B.GetName(); });
        const FString Search = String(Request, TEXT("search"), FString(), false);
        TArray<TSharedPtr<FJsonValue>> Items;
        for (AActor* Actor : Actors)
        {
            if (Search.IsEmpty() || Actor->GetName().Contains(Search) || Actor->GetActorLabel().Contains(Search) ||
                Actor->GetClass()->GetPathName().Contains(Search) || Guid(Actor->GetActorGuid()) == Search)
            { Items.Add(JsonValue(DescribeLevelActor(Actor))); }
        }
        Response->SetArrayField(TEXT("actors"), Items);
        return;
    }
    TMap<UPackage*, UObject*> Changes;
    TSet<UPackage*> Deletions;
    TArray<TSharedPtr<FJsonValue>> Results;
    for (const auto& Value : Operations)
    {
        const FJson Op = AsObject(Value);
        const FString Kind = String(Op, TEXT("op"));
        FJson Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("op"), Kind);
        AActor* Actor = Kind == TEXT("spawn_actor") ? nullptr : SelectLevelActor(Actors, String(Op, TEXT("actor")));
        if (Kind == TEXT("spawn_actor") || Kind == TEXT("replace_actor_class"))
        {
            UClass* Class = LevelActorClass(Op, Result);
            FActorSpawnParameters Spawn;
            Spawn.OverrideLevel = Level;
            Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            FTransform Transform;
            if (Actor)
            {
                FText Reason;
                Require(Actor != Level->GetWorldSettings() && Actor->CanDeleteSelectedActor(Reason),
                    TEXT("Actor cannot be replaced: ") + Actor->GetName() + TEXT(" ") + Reason.ToString());
                Transform = Actor->GetActorTransform();
                Spawn.Name = Actor->GetFName();
                Spawn.OverrideActorGuid = Actor->GetActorGuid();
                Spawn.OverridePackage = Actor->GetExternalPackage();
                Spawn.bCreateActorPackage = false;
                Require(!Spawn.OverridePackage || Class->GetDefaultObject<AActor>()->SupportsExternalPackaging(),
                    TEXT("Replacement class does not support external actor packaging"));
                Actor->UnregisterAllComponents();
                const FName RetiredName = MakeUniqueObjectName(Level, Actor->GetClass(), TEXT("UAssetAgentReplacedActor"));
                Require(Actor->UObject::Rename(*RetiredName.ToString(), Level,
                    REN_DoNotDirty | REN_DontCreateRedirectors | REN_ForceNoResetLoaders), TEXT("Cannot reserve the actor name"));
            }
            else
            {
                const FString Name = String(Op, TEXT("name"), FString(), false);
                if (!Name.IsEmpty()) { ValidName(Name); Spawn.Name = FName(*Name); }
                const FJson Input = Op->HasField(TEXT("transform")) ? Object(Op, TEXT("transform")) : MakeShared<FJsonObject>();
                const FVector Location = LevelVector(Input, TEXT("location"), FVector::ZeroVector);
                const FVector Rotation = LevelVector(Input, TEXT("rotation"), FVector::ZeroVector);
                const FVector Scale = LevelVector(Input, TEXT("scale"), FVector::OneVector);
                Transform = FTransform(FRotator(Rotation.X, Rotation.Y, Rotation.Z), Location, Scale);
            }
            Spawn.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
            AActor* New = World->SpawnActor<AActor>(Class, Transform, Spawn);
            Require(New != nullptr, TEXT("Could not spawn the actor"));
            New->SpawnCollisionHandlingMethod = Class->GetDefaultObject<AActor>()->SpawnCollisionHandlingMethod;
            if (Actor)
            {
                New->Tags = Actor->Tags;
                New->Layers = Actor->Layers;
                New->SetActorLabel(Actor->GetActorLabel());
                for (const UDataLayerInstance* Layer : Actor->GetDataLayerInstances())
                {
                    Require(New->GetDataLayerInstances().Contains(Layer) || New->AddDataLayer(Layer),
                        TEXT("Could not preserve actor data layers"));
                }
                CopyLevelActorOverrides(Actor, New, Result);
                New->EditorReplacedActor(Actor);
                if (USceneComponent* Parent = Actor->GetAttachParentActor() && Actor->GetRootComponent() ?
                    Actor->GetRootComponent()->GetAttachParent() : nullptr)
                {
                    Require(New->AttachToComponent(Parent, FAttachmentTransformRules::KeepWorldTransform,
                        Actor->GetAttachParentSocketName()), TEXT("Could not preserve parent attachment"));
                }
                TArray<AActor*> Children;
                Actor->GetAttachedActors(Children);
                for (AActor* Child : Children)
                {
                    Require(Child->AttachToActor(New, FAttachmentTransformRules::KeepWorldTransform,
                        Child->GetAttachParentSocketName()), TEXT("Could not preserve child attachment: ") + Child->GetName());
                    Changes.FindOrAdd(Child->GetPackage(), Child);
                }
                if (ULevelScriptBlueprint* Script = Level->GetLevelScriptBlueprint(true))
                {
                    FBlueprintEditorUtils::ReplaceAllActorRefrences(Script, Actor, New);
                    Require(Compile(Script, Result), TEXT("Level script failed to compile after actor replacement"));
                    Changes.Add(World->GetPackage(), World);
                }
                Require(World->EditorDestroyActor(Actor, true), TEXT("Could not remove the old actor"));
                Actors.Remove(Actor);
                ReplaceLevelActorReferences(Level, Actors, Actor, New, Changes);
            }
            else if (Op->HasField(TEXT("label"))) { New->SetActorLabel(String(Op, TEXT("label"))); }
            Actor = New;
            Actor->SetActorTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
            Actor->PostEditMove(true);
            Actors.Add(Actor);
        }
        else if (Kind == TEXT("set_actor_property"))
        {
            const FString Name = String(Op, TEXT("property"));
            FString Top = Name;
            Name.Split(TEXT("."), &Top, nullptr);
            Require(EditableInstanceProperty(FindFProperty<FProperty>(Actor->GetClass(), FName(*Top))),
                TEXT("Editable instance property not found: ") + Name);
            SetNestedDefault(Actor, Name, ValueString(Op, TEXT("value")), true);
        }
        else if (Kind == TEXT("set_tags"))
        {
            TArray<FName> Tags;
            for (const auto& Tag : Array(Op, TEXT("tags"), true))
            {
                FString Name;
                Require(Tag->TryGetString(Name) && !Name.IsEmpty() && !FName(*Name).IsNone(),
                    TEXT("Actor tags must be nonempty names"));
                Tags.AddUnique(FName(*Name));
            }
            Actor->Tags = MoveTemp(Tags);
        }
        else
        {
            Require(Kind == TEXT("delete_actor"), TEXT("Unknown level operation: ") + Kind);
            FText Reason;
            Require(Actor != Level->GetWorldSettings() && Actor->CanDeleteSelectedActor(Reason),
                TEXT("Actor cannot be deleted: ") + Actor->GetName() + TEXT(" ") + Reason.ToString());
            TArray<AActor*> Children;
            Actor->GetAttachedActors(Children);
            for (AActor* Child : Children) { Changes.FindOrAdd(Child->GetPackage(), Child); }
            UPackage* External = Actor->GetExternalPackage();
            Require(World->EditorDestroyActor(Actor, true), TEXT("Could not delete the actor"));
            Actors.Remove(Actor);
            ReplaceLevelActorReferences(Level, Actors, Actor, nullptr, Changes);
            if (ULevelScriptBlueprint* Script = Level->GetLevelScriptBlueprint(true))
            {
                Require(Compile(Script, Result), TEXT("Level script failed to compile after actor deletion"));
                Changes.Add(World->GetPackage(), World);
            }
            if (External) { Changes.Add(External, nullptr); Deletions.Add(External); }
            else { Changes.Add(World->GetPackage(), World); }
        }
        if (Kind != TEXT("delete_actor")) { Changes.Add(Actor->GetPackage(), Actor); }
        Result->SetStringField(TEXT("guid"), Guid(Actor->GetActorGuid()));
        Result->SetStringField(TEXT("name"), Actor->GetName());
        Results.Add(JsonValue(Result));
    }
    Response->SetArrayField(TEXT("operations"), Results);
    if (!DryRun) { SaveLevelChanges(MapFile, ExternalDirectory, Before, Changes, Deletions, World, Response); }
}
