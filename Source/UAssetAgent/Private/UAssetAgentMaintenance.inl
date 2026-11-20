// Headless maintenance. AssetTools::FixupReferencers opens a modal Slate report
// even with checkout prompts disabled in UE 5.4, so use its soft-path rewrite
// API plus explicit preflight, compilation, backups, saves and deletion.
static TArray<FName> Referencers(IAssetRegistry& Registry, FName Package)
{
    TArray<FName> Result;
    Registry.GetReferencers(Package, Result, UE::AssetRegistry::EDependencyCategory::All);
    Result.Remove(Package);
    Result.Sort(FNameLexicalLess());
    return Result;
}

static TArray<UBlueprint*> ReferencingBlueprints(const UBlueprint* Blueprint, bool IncludeChildReferencers = false)
{
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.SearchAllAssets(true);
    const FName Root = Blueprint->GetOutermost()->GetFName();
    TArray<FName> Pending = Referencers(Registry, Root);
    TSet<FName> Visited{Root};
    TArray<UBlueprint*> Result;
    for (int32 I = 0; I < Pending.Num(); ++I)
    {
        const FName Package = Pending[I];
        if (Visited.Contains(Package)) { continue; }
        Visited.Add(Package);
        UPackage* Loaded = LoadPackage(nullptr, *Package.ToString(), LOAD_None);
        Require(Loaded != nullptr, TEXT("Cannot load a referencing package: ") + Package.ToString());
        Loaded->FullyLoad();
        TArray<UObject*> Objects;
        GetObjectsWithOuter(Loaded, Objects, true, RF_Transient);
        for (UObject* Object : Objects)
        {
            if (Object->IsA<UObjectRedirector>() && Object->GetOuter() == Loaded)
            { Pending.Append(Referencers(Registry, Package)); }
            if (UBlueprint* Referencer = Cast<UBlueprint>(Object))
            {
                Result.Add(Referencer);
                if (IncludeChildReferencers && Blueprint->GeneratedClass && Referencer->ParentClass &&
                    Referencer->ParentClass->IsChildOf(Blueprint->GeneratedClass))
                { Pending.Append(Referencers(Registry, Package)); }
            }
        }
    }
    Result.Sort([](const UBlueprint& A, const UBlueprint& B) { return A.GetPathName() < B.GetPathName(); });
    return Result;
}

static FString MaintenanceFile(IAssetRegistry& Registry, const FString& Package)
{
    Require(Package.StartsWith(TEXT("/Game/")) && FPackageName::IsValidLongPackageName(Package), TEXT("Maintenance only supports /Game packages: ") + Package);
    const FString Filename = AssetFilename(Package);
    Require(IFileManager::Get().FileExists(*Filename) && !IFileManager::Get().IsReadOnly(*Filename), TEXT("Asset is missing or read-only: ") + Package);
    Require(!IFileManager::Get().FileExists(*FPaths::ChangeExtension(Filename, TEXT("uexp"))) &&
        !IFileManager::Get().FileExists(*FPaths::ChangeExtension(Filename, TEXT("ubulk"))), TEXT("Split packages are not supported"));
    TArray<FAssetData> Assets;
    Registry.GetAssetsByPackageName(FName(*Package), Assets, true);
    Require(Assets.Num() == 1 && !(Assets[0].PackageFlags & PKG_ContainsMap), TEXT("Maintenance requires single-asset, non-map packages: ") + Package);
    return Filename;
}

static FString BackupMaintenance(const TMap<FString, FString>& Revisions, const FJson& Response)
{
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UAssetAgent/Backups"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    FJson Backups = MakeShared<FJsonObject>();
    for (const auto& Pair : Revisions)
    {
        const FString Filename = AssetFilename(Pair.Key);
        Require(Revision(Filename) == Pair.Value, TEXT("Asset changed during request: ") + Pair.Key);
        const FString Backup = FPaths::Combine(Directory, Pair.Key.Mid(1) + TEXT(".uasset"));
        Require(IFileManager::Get().MakeDirectory(*FPaths::GetPath(Backup), true) &&
            IFileManager::Get().Copy(*Backup, *Filename, false) == COPY_OK, TEXT("Cannot back up asset: ") + Pair.Key);
        Backups->SetStringField(Pair.Key, Backup);
    }
    Response->SetObjectField(TEXT("backups"), Backups);
    return Directory;
}

static UObject* LoadExactAsset(const FString& Package)
{
    UPackage* Loaded = LoadPackage(nullptr, *Package, LOAD_None);
    Require(Loaded != nullptr, TEXT("Cannot load package: ") + Package);
    Loaded->FullyLoad();
    UObject* Asset = FindObject<UObject>(Loaded, *FPackageName::GetLongPackageAssetName(Package));
    Require(Asset && Asset->GetOutermost() == Loaded, TEXT("Asset not found in package: ") + Package);
    return Asset;
}

static void Maintenance(const FJson& Request, const FJson& Response)
{
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    // Referencers can reside outside the selected folder or even /Game.
    Registry.SearchAllAssets(true);
    const bool DryRun = Boolean(Request, TEXT("dry_run"), true);
    Response->SetBoolField(TEXT("dry_run"), DryRun);
    if (String(Request, TEXT("action")) == TEXT("delete_asset"))
    {
        const FString Package = PackagePath(Request);
        const FString Filename = MaintenanceFile(Registry, Package);
        const FString Before = Revision(Filename);
        Response->SetStringField(TEXT("revision"), Before);
        if (Request->HasField(TEXT("expected_revision")))
        { Require(String(Request, TEXT("expected_revision")) == Before, TEXT("Revision conflict")); }
        const TArray<FName> Refs = Referencers(Registry, FName(*Package));
        TArray<TSharedPtr<FJsonValue>> Values;
        for (FName Ref : Refs) { Values.Add(MakeShared<FJsonValueString>(Ref.ToString())); }
        Response->SetArrayField(TEXT("referencers"), Values);
        Require(Refs.IsEmpty() || Boolean(Request, TEXT("force")), TEXT("Asset has referencers; force:true is required to leave broken references"));
        UObject* Asset = LoadExactAsset(Package);
        if (DryRun) { return; }
        BackupMaintenance({{Package, Before}}, Response);
        Require(Revision(Filename) == Before, TEXT("Asset changed before deletion"));
        Response->SetBoolField(TEXT("save_attempted"), true);
        const int32 Deleted = ObjectTools::DeleteObjectsUnchecked({Asset});
        Require(Deleted == 1 && !IFileManager::Get().FileExists(*Filename), TEXT("Deletion failed; inspect backups and logs"));
        Response->SetBoolField(TEXT("deleted"), true);
        Response->SetBoolField(TEXT("saved"), true);
        return;
    }

    const FString Root = String(Request, TEXT("path"), TEXT("/Game"), false);
    Require(Root == TEXT("/Game") || (Root.StartsWith(TEXT("/Game/")) && FPackageName::IsValidLongPackageName(Root)), TEXT("path must be a folder under /Game"));
    Registry.ScanPathsSynchronous({Root}, true);
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Root)); Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName()); Filter.bIncludeOnlyOnDiskAssets = true;
    TArray<FAssetData> Redirectors;
    Registry.GetAssets(Filter, Redirectors);
    TSet<FName> RedirectorPackages;
    TMap<FSoftObjectPath, FSoftObjectPath> RedirectMap;
    TMap<FString, FString> Revisions;
    TSet<FName> ToSave;
    TArray<TSharedPtr<FJsonValue>> Plans;
    for (const FAssetData& Asset : Redirectors)
    {
        FJson Info = DescribeRedirector(Registry, Asset);
        Require(String(Info, TEXT("status")) == TEXT("resolved"), TEXT("Broken, cyclic or unknown redirector; fixup aborted: ") + Asset.PackageName.ToString());
        RedirectorPackages.Add(Asset.PackageName);
        const FSoftObjectPath OldPath = Asset.GetSoftObjectPath();
        const FSoftObjectPath NewPath(String(Info, TEXT("resolved_destination")));
        RedirectMap.Add(OldPath, NewPath);
        UObject* Destination = NewPath.TryLoad();
        Require(Destination != nullptr, TEXT("Redirector destination could not be loaded"));
        if (Destination->IsA<UBlueprint>())
        {
            // Soft class/CDO references are separate paths from the BP asset.
            RedirectMap.Add(FSoftObjectPath(OldPath.ToString() + TEXT("_C")), FSoftObjectPath(NewPath.ToString() + TEXT("_C")));
            RedirectMap.Add(FSoftObjectPath(OldPath.GetLongPackageName() + TEXT(".Default__") + OldPath.GetAssetName() + TEXT("_C")),
                FSoftObjectPath(NewPath.GetLongPackageName() + TEXT(".Default__") + NewPath.GetAssetName() + TEXT("_C")));
        }
        Plans.Add(JsonValue(Info));
        Revisions.Add(Asset.PackageName.ToString(), Revision(MaintenanceFile(Registry, Asset.PackageName.ToString())));
    }
    Response->SetArrayField(TEXT("redirectors"), Plans);
    for (FName Package : RedirectorPackages)
    { for (FName Ref : Referencers(Registry, Package)) { if (!RedirectorPackages.Contains(Ref)) { ToSave.Add(Ref); } } }
    TArray<UObject*> Assets;
    TArray<UPackage*> Packages;
    for (FName Name : ToSave)
    {
        const FString Package = Name.ToString();
        Revisions.Add(Package, Revision(MaintenanceFile(Registry, Package)));
        UObject* Asset = LoadExactAsset(Package);
        Require(!Asset->IsA<UObjectRedirector>(), TEXT("A redirector outside the requested folder refers to this folder; choose a common ancestor folder"));
        Assets.Add(Asset); Packages.Add(Asset->GetOutermost());
    }
    FJson RevisionJson = MakeShared<FJsonObject>();
    for (const auto& Pair : Revisions) { RevisionJson->SetStringField(Pair.Key, Pair.Value); }
    Response->SetObjectField(TEXT("revisions"), RevisionJson);
    if (Request->HasField(TEXT("expected_revisions")))
    {
        FJson Expected = Object(Request, TEXT("expected_revisions"));
        Require(Expected->Values.Num() == Revisions.Num(), TEXT("Fixup plan changed; repeat the dry run"));
        for (const auto& Pair : Revisions)
        { Require(String(Expected, *Pair.Key) == Pair.Value, TEXT("Revision conflict: ") + Pair.Key); }
    }
    FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().RenameReferencingSoftObjectPaths(Packages, RedirectMap);
    // Loading resolves hard references. The soft-path rewrite above handles
    // serialized soft references; compile every affected BP before any save.
    for (UObject* Asset : Assets)
    { if (UBlueprint* BP = Cast<UBlueprint>(Asset)) { Require(Compile(BP, Response), TEXT("Referencing Blueprint failed compilation: ") + BP->GetPathName()); } }
    Response->SetNumberField(TEXT("packages_to_save"), Packages.Num());
    if (DryRun || Redirectors.IsEmpty()) { return; }
    BackupMaintenance(Revisions, Response);
    Response->SetBoolField(TEXT("save_attempted"), true);
    TArray<FString> SavedFiles;
    TArray<TSharedPtr<FJsonValue>> SavedAssets;
    for (UObject* Asset : Assets)
    {
        const FString Package = Asset->GetOutermost()->GetName();
        const FString Filename = AssetFilename(Package);
        Require(Revision(Filename) == Revisions[Package], TEXT("Asset changed during fixup; some packages may already be saved"));
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
        Require(UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, Args), TEXT("Fixup save failed; restore backups if needed"));
        SavedFiles.Add(Filename); SavedAssets.Add(MakeShared<FJsonValueString>(Package));
        Response->SetArrayField(TEXT("saved_assets"), SavedAssets);
        Response->SetBoolField(TEXT("saved"), true);
    }
    Registry.ScanFilesSynchronous(SavedFiles, true);
    TArray<UObject*> Delete;
    for (FName Package : RedirectorPackages)
    {
        for (FName Ref : Referencers(Registry, Package))
        { Require(RedirectorPackages.Contains(Ref), TEXT("References remain after saving; redirectors retained: ") + Ref.ToString()); }
        Require(Revision(AssetFilename(Package.ToString())) == Revisions[Package.ToString()], TEXT("Redirector changed during fixup"));
        UObject* Asset = LoadExactAsset(Package.ToString());
        Require(Asset->IsA<UObjectRedirector>(), TEXT("Expected a redirector"));
        Delete.Add(Asset);
    }
    const int32 Deleted = ObjectTools::DeleteObjectsUnchecked(Delete);
    Response->SetNumberField(TEXT("deleted_redirectors"), Deleted);
    Require(Deleted == Redirectors.Num(), TEXT("Some redirectors could not be deleted; inspect backups"));
    Response->SetBoolField(TEXT("saved"), true);
}
