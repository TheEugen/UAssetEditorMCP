static FJson DiffSnapshot(const FString& Source, FString& Package)
{
    UBlueprint* BP = nullptr;
    if (Source.StartsWith(TEXT("/Game/")))
    {
        FJson Path = MakeShared<FJsonObject>();
        Path->SetStringField(TEXT("asset"), Source);
        Package = PackagePath(Path);
        BP = Cast<UBlueprint>(LoadExactAsset(Package));
    }
    else
    {
        FString Filename = FPaths::ConvertRelativePathToFull(Source);
        FPaths::NormalizeFilename(Filename);
        FPaths::CollapseRelativeDirectories(Filename);
        const FString Backups = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UAssetAgent/Backups"));
        Require(FPaths::IsUnderDirectory(Filename, Backups) && FPaths::GetExtension(Filename) == TEXT("uasset"),
            TEXT("Expected a /Game package or a .uasset in this project's MCP backup directory"));
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
        Require(Reader.IsValid(), TEXT("Cannot read backup"));
        FPackageFileSummary Summary;
        *Reader << Summary;
        Require(!Reader->IsError() && FPackageName::IsValidLongPackageName(Summary.PackageName),
            TEXT("Invalid Blueprint backup header"));
        Reader.Reset();
        const FString Mount = TEXT("/Temp/UAssetAgentDiff_") + Guid(FGuid::NewGuid()) + TEXT("/");
        const FString Directory = FPaths::GetPath(Filename) + TEXT("/");
        FPackageName::RegisterMountPoint(Mount, Directory);
        ON_SCOPE_EXIT { FPackageName::UnRegisterMountPoint(Mount, Directory); };
        const FString TempName = Mount + FPaths::GetBaseFilename(Filename);
        UPackage* Loaded = DiffUtils::LoadPackageForDiff(FPackagePath::FromPackageNameChecked(TempName),
            FPackagePath::FromPackageNameChecked(Summary.PackageName));
        Require(Loaded != nullptr, TEXT("Cannot load Blueprint backup"));
        BP = FindObject<UBlueprint>(Loaded, *FPackageName::GetLongPackageAssetName(Summary.PackageName));
        Package = TempName;
        Require(BP && BP->GetOutermost() == Loaded, TEXT("Backup does not contain its Blueprint"));
        return DescribeBlueprint(BP, MakeShared<FJsonObject>());
    }
    Require(BP && BP->GetOutermost()->GetName() == Package, TEXT("Expected a Blueprint package"));
    return DescribeBlueprint(BP, MakeShared<FJsonObject>());
}

static void DiffInputAction(const FJson& Request, const FJson& Response)
{
    for (const TCHAR* Key : {TEXT("a"), TEXT("b")})
    {
        FString Package;
        FJson Snapshot = DiffSnapshot(String(Request, Key), Package);
        Response->SetObjectField(Key, Snapshot);
    }
}
