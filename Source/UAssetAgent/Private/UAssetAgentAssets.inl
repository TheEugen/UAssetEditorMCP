static UObject* CreateReflectedAsset(const FString& Package, const FString& ClassPath)
{
    UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
    Require(Class && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists) &&
        !Class->IsChildOf(UBlueprint::StaticClass()) && !Class->IsChildOf(UWorld::StaticClass()),
        TEXT("Expected a concrete non-Blueprint asset class"));
    UPackage* Outer = CreatePackage(*Package);
    const FName Name(*FPackageName::GetLongPackageAssetName(Package));
    const EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional;
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
        Asset = CreateReflectedAsset(Package, String(Request, TEXT("class")));
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
    const auto Describe = [&]
    {
        Response->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
        if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset)) { DescribeSequence(Sequence, Response); }
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
    for (int32 I = 0; I < Operations.Num(); ++I)
    {
        Response->SetNumberField(TEXT("operation_index"), I);
        FJson Op = AsObject(Operations[I]);
        if (IsOwnedObjectOperation(String(Op, TEXT("op"))))
        {
            EditOwnedObject(Asset, Op);
            EditedOwnedObjects = true;
        }
        else if (!EditAnimation(Asset, Op, Response)) { EditAssetProperty(Asset, Op); }
    }
    Response->RemoveField(TEXT("operation_index"));
    if (EditedOwnedObjects) { ValidateOwnedObjectEdits(Asset, Response); }
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
