static bool EditableAssetField(const FProperty* Field)
{
    return Field && Field->HasAnyPropertyFlags(CPF_Edit) &&
        !Field->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated) && Field->ArrayDim == 1;
}

static void DescribeReflectedAsset(UObject* Asset, const FJson& Response)
{
    Response->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
    FJson Properties = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
    {
        if (!EditableAssetField(*It)) { continue; }
        FString Text;
        It->ExportText_InContainer(0, Text, Asset, nullptr, Asset, PPF_None);
        FJson Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("type"), It->GetCPPType());
        Entry->SetStringField(TEXT("value"), Text);
        Properties->SetObjectField(It->GetName(), Entry);
    }
    Response->SetObjectField(TEXT("properties"), Properties);
}

static void EditAssetProperty(UObject* Asset, const FJson& Op)
{
    const FString Kind = String(Op, TEXT("op"));
    const FString Path = String(Op, TEXT("property"));
    TArray<FString> Parts;
    Path.ParseIntoArray(Parts, TEXT("."), false);
    UStruct* Type = Asset->GetClass();
    void* Address = Asset;
    FProperty* Top = nullptr;
    FProperty* Field = nullptr;
    for (int32 I = 0; I < Parts.Num(); ++I)
    {
        Field = FindFProperty<FProperty>(Type, FName(*Parts[I]));
        Require(EditableAssetField(Field), TEXT("Editable property not found: ") + Path);
        if (!Top) { Top = Field; }
        Address = Field->ContainerPtrToValuePtr<void>(Address);
        if (I + 1 < Parts.Num())
        {
            auto* Struct = CastField<FStructProperty>(Field);
            Require(Struct != nullptr, TEXT("Property paths may traverse only structs: ") + Path);
            Type = Struct->Struct;
        }
    }
    Require(Field != nullptr, TEXT("Empty property path"));
    Asset->PreEditChange(Top);
    EPropertyChangeType::Type Change = EPropertyChangeType::ValueSet;
    if (Kind != TEXT("set_property"))
    {
        auto* ArrayField = CastField<FArrayProperty>(Field);
        Require(ArrayField && (Kind == TEXT("array_add") || Kind == TEXT("array_remove") || Kind == TEXT("array_set")),
            TEXT("Expected an array property and array_add, array_remove or array_set"));
        FScriptArrayHelper Elements(ArrayField, Address);
        int32 Index;
        if (Kind == TEXT("array_add"))
        {
            Index = Elements.AddValue();
            Change = EPropertyChangeType::ArrayAdd;
        }
        else
        {
            Index = Integer(Op, TEXT("index"), INDEX_NONE);
            Require(Elements.IsValidIndex(Index), TEXT("Array index out of bounds: ") + Path);
        }
        if (Kind == TEXT("array_remove"))
        {
            Elements.RemoveValues(Index);
            Change = EPropertyChangeType::ArrayRemove;
        }
        else { Address = Elements.GetRawPtr(Index); }
        Field = ArrayField->Inner;
    }
    if (Change != EPropertyChangeType::ArrayRemove)
    {
        const FString Value = ValueString(Op, TEXT("value"));
        const TCHAR* End = Field->ImportText_Direct(*Value, Address, Asset, PPF_None);
        Require(End && FString(End).TrimStartAndEnd().IsEmpty(), TEXT("Invalid Unreal property text: ") + Path);
    }
    FPropertyChangedEvent Changed(Field->GetOwnerProperty(), Change);
    Changed.SetActiveMemberProperty(Top);
    Asset->PostEditChangeProperty(Changed);
}

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
    if (Inspect) { DescribeReflectedAsset(Asset, Response); return; }

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
    for (int32 I = 0; I < Operations.Num(); ++I)
    {
        Response->SetNumberField(TEXT("operation_index"), I);
        EditAssetProperty(Asset, AsObject(Operations[I]));
    }
    Response->RemoveField(TEXT("operation_index"));
    DescribeReflectedAsset(Asset, Response);
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
