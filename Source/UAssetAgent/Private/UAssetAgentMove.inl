// Read the staged bytes using their eventual package path so the registry can resolve imports.
class FMovePackageReader final : public FArchiveProxy
{
    FString Filename;
public:
    FMovePackageReader(FArchive& Source, const FString& Target) : FArchiveProxy(Source), Filename(Target) {}
    FString GetArchiveName() const override { return Filename; }
};

static void MoveAssetAction(const FJson& Request, const FJson& Response)
{
    const FString From = ScopedReferencePath(Request, TEXT("from"), false);
    const FString To = ScopedReferencePath(Request, TEXT("to"), false);
    const bool Preview = Boolean(Request, TEXT("dry_run"), true);
    const bool LeaveRedirector = Boolean(Request, TEXT("leave_redirector"));
    Response->SetBoolField(TEXT("dry_run"), Preview);
    Require(!From.Equals(To, ESearchCase::IgnoreCase), TEXT("Choose a distinct destination package"));
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    IFileManager& Files = IFileManager::Get();
    Registry.SearchAllAssets(true);
    const auto Unoccupied = [&]()
    {
        TArray<FString> Existing;
        Files.FindFiles(Existing, *FPaths::ChangeExtension(AssetFilename(To), TEXT("*")), true, false);
        Require(Existing.IsEmpty() && !FPackageName::DoesPackageExist(To), TEXT("Destination package already exists: ") + To);
    };
    Unoccupied();
    MaintenanceFile(Registry, From);
    const TArray<FName> Referrers = Referencers(Registry, FName(*From));
    TArray<FString> Names;
    for (FName Ref : Referrers) { Names.Add(Ref.ToString()); }
    Response->SetArrayField(TEXT("referencers"), LevelStrings(Names));
    Names.Insert(From, 0);
    const FReferenceScan Scan(Response);
    FJson Revisions = MakeShared<FJsonObject>();
    Response->SetObjectField(TEXT("revisions"), Revisions);
    struct FMoveFile
    {
        FString Name, Filename, OriginalRevision, Staged;
        UPackage* Package = nullptr;
        UObject* Asset = nullptr;
    };
    TArray<FMoveFile> Originals;
    TArray<UPackage*> Rewrite;
    TMap<UBlueprint*, FMemberNodeSnapshot> Wiring;
    for (const FString& Name : Names)
    {
        try
        {
            Require(Name.StartsWith(TEXT("/Game/")), TEXT("Referencer is outside writable project content"));
            FString File;
            Require(FPackageName::DoesPackageExist(Name, &File), TEXT("Missing package"));
            File = FPaths::ConvertRelativePathToFull(File);
            TArray<FString> Parts;
            Files.FindFiles(Parts, *FPaths::ChangeExtension(File, TEXT("*")), true, false);
            Require(Parts.Num() == 1 && !Files.IsReadOnly(*File), TEXT("Package is split or read-only"));
            UPackage* Package = Scan.Load(Name);
            if (!Package) { continue; }
            UObject* Asset = FindPackageAsset(Package);
            TArray<UObject*> Objects;
            GetObjectsWithPackage(Package, Objects, true, RF_Transient);
            for (UObject* Object : Objects)
            {
                if (UBlueprint* BP = Cast<UBlueprint>(Object)) { Wiring.Add(BP, ScopedBlueprintWiring(BP)); }
                if (Object->IsAsset() && Object->GetExternalPackage() == Package)
                {
                    Require(!Asset || Asset == Object, TEXT("Ambiguous external asset"));
                    Asset = Object;
                }
            }
            Require(Asset && Asset->GetPackage() == Package, TEXT("Cannot identify the package asset"));
            if (Name == From) { Require(!Asset->IsA<UObjectRedirector>() && !Asset->IsA<UWorld>(), TEXT("Select a standalone asset to move")); }
            const FString RevisionBefore = Revision(File);
            Revisions->SetStringField(Name, RevisionBefore);
            Originals.Add({Name, File, RevisionBefore, FString(), Package, Asset});
            Rewrite.Add(Package);
        }
        catch (const FFailure& Failure) { Scan.Fail(Name, Failure.Message); }
    }
    Scan.RequireComplete();
    if (!Preview || Request->HasField(TEXT("expected_revisions")))
    {
        FJson Expected = Object(Request, TEXT("expected_revisions"));
        Require(Expected->Values.Num() == Originals.Num(), TEXT("Move plan changed; repeat the preview"));
        for (const auto& File : Originals)
        { Require(String(Expected, *File.Name) == File.OriginalRevision, TEXT("Revision conflict: ") + File.Name); }
    }
    UObject* Asset = Originals[0].Asset;
    ObjectTools::FPackageGroupName Target;
    Target.PackageName = To;
    Target.ObjectName = FPackageName::GetLongPackageAssetName(To);
    const FSoftObjectPath Old(Asset);
    const FSoftObjectPath New(To + TEXT(".") + Target.ObjectName);
    TMap<FSoftObjectPath, FSoftObjectPath> Paths{{Old, New}};
    if (UBlueprint* BP = Cast<UBlueprint>(Asset))
    {
        Require(BP->GeneratedClass != nullptr, TEXT("Blueprint has no generated class"));
        Paths.Add(FSoftObjectPath(BP->GeneratedClass), FSoftObjectPath(New.ToString() + TEXT("_C")));
        Paths.Add(FSoftObjectPath(BP->GeneratedClass->GetDefaultObject()),
            FSoftObjectPath(To + TEXT(".Default__") + Target.ObjectName + TEXT("_C")));
    }
    TSet<UPackage*> Refused;
    FText Error;
    const bool Moved = ObjectTools::RenameSingleObject(Asset, Target, Refused, Error, nullptr, false);
    Require(Moved && Asset->GetPathName() == New.ToString(), TEXT("Rename failed: ") + Error.ToString());
    UPackage* Destination = Asset->GetOutermost();
    Rewrite.Add(Destination);
    FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().RenameReferencingSoftObjectPaths(Rewrite, Paths);
    CompileScopedReferences(Wiring, Response);

    TArray<FMoveFile> Writes{{To, AssetFilename(To), FString(), FString(), Destination, Asset}};
    for (int32 Index = 1; Index < Originals.Num(); ++Index) { Writes.Add(Originals[Index]); }
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UAssetAgent"), FGuid::NewGuid().ToString()));
    ON_SCOPE_EXIT { Files.DeleteDirectory(*Root, false, true); };
    const auto Stage = [&](FMoveFile& File)
    {
        File.Staged = Root / (File.Name.Mid(1) + FPaths::GetExtension(File.Filename, true));
        Require(Files.MakeDirectory(*FPaths::GetPath(File.Staged), true), TEXT("Cannot create staging directory"));
        FSavePackageArgs Args;
        Args.TopLevelFlags = File.Asset->GetExternalPackage() ? RF_NoFlags : RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        Require(UPackage::SavePackage(File.Package, File.Asset, *File.Staged, Args), TEXT("Could not stage package: ") + File.Name);
        TArray<FString> Parts;
        Files.FindFiles(Parts, *FPaths::ChangeExtension(File.Staged, TEXT("*")), true, false);
        Require(Parts.Num() == 1, TEXT("Staging produced a split package: ") + File.Name);
        TUniquePtr<FArchive> Reader(Files.CreateFileReader(*File.Staged));
        Require(Reader.IsValid(), TEXT("Could not read staged package"));
        FMovePackageReader Named(*Reader, File.Filename);
        IAssetRegistry::FLoadPackageRegistryData Data(true);
        Registry.LoadPackageRegistryData(Named, Data);
        Require(!Data.Data.IsEmpty() && !Data.DataDependencies.Contains(FName(*From)),
            TEXT("Staged package could not be verified or still references the old path: ") + File.Name);
    };
    for (auto& File : Writes) { Stage(File); }
    if (LeaveRedirector)
    {
        // Register aliases after staging the moved class. UE 5.4 can otherwise load the old
        // World Partition class descriptor while saving the new class with the same actor GUID.
        FMoveFile Redirect = Originals[0];
        for (const auto& Pair : Paths)
        {
            UObject* TargetObject = Pair.Value.ResolveObject();
            Require(TargetObject && !FindObject<UObject>(Redirect.Package, *Pair.Key.GetAssetName()),
                TEXT("Could not create redirector for ") + Pair.Key.ToString());
            auto* Alias = NewObject<UObjectRedirector>(Redirect.Package, FName(*Pair.Key.GetAssetName()), RF_Public | RF_Standalone);
            Alias->DestinationObject = TargetObject;
            GRedirectCollector.AddAssetPathRedirection(Pair.Key, Pair.Value);
            if (Pair.Key == Old) { Redirect.Asset = Alias; }
        }
        Stage(Redirect);
        Writes.Add(Redirect);
    }
    if (Preview) { return; }
    const FString BackupRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("UAssetAgent/Backups"), FGuid::NewGuid().ToString()));
    FJson Backups = MakeShared<FJsonObject>();
    Response->SetObjectField(TEXT("backups"), Backups);
    for (const auto& File : Originals)
    {
        Require(Revision(File.Filename) == File.OriginalRevision, TEXT("Package changed during move: ") + File.Name);
        const FString Backup = BackupRoot / (File.Name.Mid(1) + FPaths::GetExtension(File.Filename, true));
        Require(Files.MakeDirectory(*FPaths::GetPath(Backup), true) && Files.Copy(*Backup, *File.Filename, false) == COPY_OK &&
            Revision(Backup) == File.OriginalRevision, TEXT("Could not back up package: ") + File.Name);
        Backups->SetStringField(File.Name, Backup);
    }
    // The in-memory destination is registered now. Check the physical files before publishing.
    TArray<FString> Touched;
    try
    {
        for (const auto& File : Writes)
        {
            if (!File.OriginalRevision.IsEmpty())
            { Require(Revision(File.Filename) == File.OriginalRevision, TEXT("Package changed before write: ") + File.Name); }
            else
            {
                Require(!Files.FileExists(*File.Filename) && !Files.FileExists(*LevelFilename(To)), TEXT("Destination appeared during move"));
                Require(Files.MakeDirectory(*FPaths::GetPath(File.Filename), true), TEXT("Cannot create destination folder"));
            }
            ResetLoaders(File.Package);
            Response->SetBoolField(TEXT("save_attempted"), true);
            if (!File.OriginalRevision.IsEmpty()) { Touched.Add(File.Name); }
            Require(Files.Move(*File.Filename, *File.Staged, !File.OriginalRevision.IsEmpty()), TEXT("Could not publish package: ") + File.Name);
            if (File.OriginalRevision.IsEmpty()) { Touched.Add(File.Name); }
        }
        if (!LeaveRedirector)
        {
            const auto& Source = Originals[0];
            Require(Revision(Source.Filename) == Source.OriginalRevision, TEXT("Source changed before removal"));
            ResetLoaders(Source.Package);
            Require(Files.Delete(*Source.Filename, true), TEXT("Could not remove source package"));
            Touched.Add(From);
        }
    }
    catch (const FFailure&)
    {
        TArray<FString> Failures;
        for (const FString& Name : Touched)
        {
            if (Name == To) { if (!Files.Delete(*Writes[0].Filename)) { Failures.Add(Name); } continue; }
            const auto* Original = Originals.FindByPredicate([&](const FMoveFile& File) { return File.Name == Name; });
            if (Files.Copy(*Original->Filename, *String(Backups, *Name), true) != COPY_OK) { Failures.Add(Name); }
        }
        Response->SetArrayField(TEXT("rollback_failures"), LevelStrings(Failures));
        throw;
    }
    Response->SetArrayField(TEXT("changed_packages"), LevelStrings(Touched));
    Response->SetBoolField(TEXT("saved"), true);
    Response->SetStringField(TEXT("revision"), Revision(Writes[0].Filename));
}
