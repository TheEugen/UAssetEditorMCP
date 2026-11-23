static UObject* CreateReflectedAsset(const FString& Package, const FJson& Request)
{
    const FString ClassPath = String(Request, TEXT("class"));
    UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
    Require(Class && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists) &&
        !Class->IsChildOf(UBlueprint::StaticClass()) && !Class->IsChildOf(UWorld::StaticClass()),
        TEXT("Expected a concrete non-Blueprint asset class"));
    UPackage* Outer = CreatePackage(*Package);
    const FName Name(*FPackageName::GetLongPackageAssetName(Package));
    const EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional;
    if (Class == UAnimMontage::StaticClass())
    {
        const FJson Options = Object(Request, TEXT("montage"));
        UAnimSequence* Source = Cast<UAnimSequence>(LoadExactAsset(String(Options, TEXT("sequence"))));
        Require(Source && Source->GetSkeleton() && Source->GetPlayLength() > 0, TEXT("Expected a nonempty sequence with a skeleton"));
        if (Options->HasField(TEXT("skeleton")))
        { Require(LoadExactAsset(String(Options, TEXT("skeleton"))) == Source->GetSkeleton(), TEXT("Montage and source skeletons must match")); }
        const FName Slot(*String(Options, TEXT("slot"), TEXT("DefaultSlot"), false));
        Require(Source->GetSkeleton()->ContainsSlotName(Slot), TEXT("Slot is not registered on the source skeleton"));
        UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
        Factory->SourceAnimation = Source;
        UFactory* BaseFactory = Factory;
        auto* Montage = Cast<UAnimMontage>(BaseFactory->FactoryCreateNew(Class, Outer, Name, Flags, nullptr, GWarn));
        Require(Montage && Montage->SlotAnimTracks.Num() == 1, TEXT("Montage factory failed"));
        Montage->SlotAnimTracks[0].SlotName = Slot;
        return Montage;
    }
    Require(!Request->HasField(TEXT("montage")), TEXT("montage options require the AnimMontage class"));
    if (Class->IsChildOf(UDataAsset::StaticClass()))
    { return NewObject<UDataAsset>(Outer, Class, Name, Flags); }
    IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    for (UFactory* Factory : Tools.GetNewAssetFactories())
    {
        if (Factory && Factory->CanCreateNew() && Factory->GetSupportedClass() == Class)
        { return Factory->FactoryCreateNew(Class, Outer, Name, Flags, nullptr, GWarn); }
    }
    throw FFailure{TEXT("No headless asset factory for class: ") + ClassPath};
}

static void ReflectedAssetAction(const FString& Action, const FJson& Request, const FJson& Response)
{
    const FString Package = PackagePath(Request);
    const FString Filename = AssetFilename(Package);
    const bool Create = Action == TEXT("asset_create");
    const bool Inspect = Action == TEXT("asset_inspect");
    FString Before;
    UObject* Asset;
    if (Create)
    {
        Require(!FPackageName::DoesPackageExist(Package), TEXT("Asset already exists"));
        Asset = CreateReflectedAsset(Package, Request);
    }
    else
    {
        Before = Revision(Filename);
        Response->SetStringField(TEXT("revision"), Before);
        if (!Inspect) { Require(String(Request, TEXT("expected_revision")) == Before, TEXT("Revision conflict")); }
        Asset = LoadExactAsset(Package);
    }
    Require(Asset && Asset->IsAsset() && Asset->GetOutermost()->GetName() == Package &&
        !Asset->IsA<UBlueprint>() && !Asset->IsA<UObjectRedirector>() && !Asset->IsA<UWorld>(),
        TEXT("Expected a standalone non-Blueprint asset"));
    IUAssetAgentNiagara* Niagara = nullptr;
    if (const UClass* System = FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraSystem")); System && Asset->IsA(System))
    {
        Niagara = FModuleManager::LoadModulePtr<IUAssetAgentNiagara>(TEXT("UAssetAgentNiagara"));
        Require(Niagara != nullptr, TEXT("Could not load the Niagara editing module"));
    }
    const auto Describe = [&]
    {
        Response->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
        if (Niagara) { Niagara->Describe(Asset, *Response); }
        if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset)) { DescribeSequence(Sequence, Response); }
        if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
        {
            FJson Description = MakeShared<FJsonObject>();
            Description->SetStringField(TEXT("skeleton"), GetPathNameSafe(Montage->GetSkeleton()));
            Description->SetBoolField(TEXT("root_motion"), Montage->HasRootMotion());
            Description->SetNumberField(TEXT("length"), Montage->GetPlayLength());
            TArray<TSharedPtr<FJsonValue>> Tracks;
            for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
            {
                FJson Slot = MakeShared<FJsonObject>();
                Slot->SetStringField(TEXT("slot"), Track.SlotName.ToString());
                TArray<TSharedPtr<FJsonValue>> Segments;
                for (const FAnimSegment& Segment : Track.AnimTrack.AnimSegments)
                { Segments.Add(MakeShared<FJsonValueString>(GetPathNameSafe(Segment.GetAnimReference()))); }
                Slot->SetArrayField(TEXT("sequences"), Segments);
                Tracks.Add(JsonValue(Slot));
            }
            Description->SetArrayField(TEXT("tracks"), Tracks);
            Response->SetObjectField(TEXT("montage"), Description);
        }
        Response->SetObjectField(TEXT("properties"), DescribeEditableProperties(Asset));
        FJson Slots = DescribeOwnedSlots(Asset);
        if (!Slots->Values.IsEmpty()) { Response->SetObjectField(TEXT("subobjects"), Slots); }
    };
    if (Inspect) { Describe(); return; }

    const bool DryRun = Boolean(Request, TEXT("dry_run"), true);
    Response->SetBoolField(TEXT("dry_run"), DryRun);
    Asset->Modify();
    if (Create && Request->HasField(TEXT("properties")))
    {
        FJson Initial = Object(Request, TEXT("properties"));
        for (const auto& Pair : Initial->Values)
        {
            FJson Op = MakeShared<FJsonObject>();
            Op->SetStringField(TEXT("op"), TEXT("set_property"));
            Op->SetStringField(TEXT("property"), Pair.Key);
            Op->SetField(TEXT("value"), Pair.Value);
            EditAssetProperty(Asset, Op);
        }
    }
    const auto Operations = Array(Request, TEXT("operations"), !Create);
    Require(Operations.Num() <= 1000, TEXT("At most 1000 asset operations per batch"));
    bool EditedOwnedObjects = false;
    bool CompileNiagara = false;
    for (int32 I = 0; I < Operations.Num(); ++I)
    {
        Response->SetNumberField(TEXT("operation_index"), I);
        FJson Op = AsObject(Operations[I]);
        const FString Kind = String(Op, TEXT("op"));
        if (Kind == TEXT("repair_niagara_interface") || Kind == TEXT("recompile_niagara_system"))
        {
            Require(Niagara != nullptr, TEXT("Expected a Niagara system"));
            if (Kind == TEXT("repair_niagara_interface"))
            {
                FString Error;
                UObject* Interface = Niagara->RepairInterface(Asset, FName(*String(Op, TEXT("parameter"))), Error);
                Require(Interface != nullptr, Error);
                if (Op->HasField(TEXT("properties")))
                {
                    for (const auto& Field : Object(Op, TEXT("properties"))->Values)
                    {
                        FJson Edit = MakeShared<FJsonObject>();
                        Edit->SetStringField(TEXT("op"), TEXT("set_property"));
                        Edit->SetStringField(TEXT("property"), Field.Key);
                        Edit->SetField(TEXT("value"), Field.Value);
                        EditAssetProperty(Interface, Edit, false);
                    }
                }
            }
            CompileNiagara = true;
        }
        else if (IsOwnedObjectOperation(Kind))
        {
            EditOwnedObject(Asset, Op);
            EditedOwnedObjects = true;
        }
        else if (!EditAnimation(Asset, Op, Response)) { EditAssetProperty(Asset, Op); }
    }
    Response->RemoveField(TEXT("operation_index"));
    if (EditedOwnedObjects) { ValidateOwnedObjectEdits(Asset, Response); }
    if (CompileNiagara)
    {
        const bool Valid = Niagara->CompileSystem(Asset);
        Response->SetBoolField(TEXT("niagara_compiled"), Valid);
        Require(Valid, TEXT("Niagara failed system compilation or validation"));
    }
    Describe();
    if (DryRun) { return; }
    if (Create)
    {
        Require(!FPackageName::DoesPackageExist(Package), TEXT("Target appeared during request"));
        Require(IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true), TEXT("Cannot create asset directory"));
    }
    else
    {
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.ScanFilesSynchronous({Filename}, true);
        MaintenanceFile(Registry, Package);
        BackupMaintenance({{Package, Before}}, Response);
        Require(Revision(Filename) == Before, TEXT("Asset changed during request"));
    }
    FSavePackageArgs Save;
    Save.TopLevelFlags = RF_Public | RF_Standalone;
    Response->SetBoolField(TEXT("save_attempted"), true);
    Require(UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, Save), TEXT("Could not save asset"));
    Response->SetBoolField(TEXT("saved"), true);
    Response->SetStringField(TEXT("revision"), Revision(Filename));
}
