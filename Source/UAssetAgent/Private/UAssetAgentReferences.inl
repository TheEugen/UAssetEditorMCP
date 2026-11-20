static FString ReferencePath(const FJson& Request, const TCHAR* Field, bool FolderOrPrefix, const FString& Default = FString())
{
    FString Path = String(Request, Field, Default, Default.IsEmpty());
    if (FolderOrPrefix) { Path.RemoveFromEnd(TEXT("/")); }
    Require(Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("//")) && !Path.Contains(TEXT(".")) &&
        !Path.Contains(TEXT("\\")) && !Path.Contains(TEXT(":")) && !Path.EndsWith(TEXT("/")),
        FString(Field) + TEXT(" must be a mounted Unreal package path"));
    if (FolderOrPrefix && !Path.RightChop(1).Contains(TEXT("/")))
    {
        TArray<FString> Mounts;
        FPackageName::QueryRootContentPaths(Mounts, true);
        Require(Mounts.ContainsByPredicate([&](const FString& Mount)
        {
            return Mount.Equals(Path + TEXT("/"), ESearchCase::IgnoreCase);
        }), TEXT("Unknown content mount: ") + Path);
    }
    else
    {
        Require(FPackageName::IsValidLongPackageName(Path, true),
            FString(Field) + TEXT(" must be a mounted Unreal package path"));
    }
    return Path;
}

static bool WithinReferencePath(const FString& Package, const FString& Path)
{
    return Package.Equals(Path, ESearchCase::IgnoreCase) ||
        (Package.Len() > Path.Len() && Package[Path.Len()] == '/' && Package.StartsWith(Path));
}

static TArray<FName> PackageReferences(IAssetRegistry& Registry, FName Package, bool Incoming, bool Soft)
{
    using namespace UE::AssetRegistry;
    const FDependencyQuery Flags(Soft ? EDependencyQuery::NoRequirements : EDependencyQuery::Hard);
    TArray<FName> Related;
    if (Incoming) { Registry.GetReferencers(Package, Related, EDependencyCategory::Package, Flags); }
    else { Registry.GetDependencies(Package, Related, EDependencyCategory::Package, Flags); }
    return Related;
}

static TArray<TSharedPtr<FJsonValue>> SortedPackageNames(const TSet<FName>& Names)
{
    TArray<FName> Sorted = Names.Array();
    Sorted.Sort(FNameLexicalLess());
    TArray<TSharedPtr<FJsonValue>> List;
    for (FName Name : Sorted) { List.Add(MakeShared<FJsonValueString>(Name.ToString())); }
    return List;
}

static void FindReferencePath(IAssetRegistry& Registry, const FString& Prefix, const FString& Folder,
    bool IncludeSoft, const FJson& Response)
{
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Folder));
    const FString ExternalPath = FPackageName::GetLongPackagePath(
        ULevel::GetExternalActorsPath(Folder + TEXT("/__UAssetAgentScope__")));
    if (!WithinReferencePath(ExternalPath, Folder)) { Filter.PackagePaths.Add(FName(*ExternalPath)); }
    Filter.bRecursivePaths = true;
    Filter.bIncludeOnlyOnDiskAssets = true;
    // An external actor's asset data uses its map in the object path but its
    // own file in PackageName. Query the physical package and group by map.
    TMap<FName, FName> Sources;
    Registry.EnumerateAssets(Filter, [&](const FAssetData& Asset)
    {
        const FString Owner = Asset.GetSoftObjectPath().GetLongPackageName();
        if (WithinReferencePath(Owner, Folder)) { Sources.Add(Asset.PackageName, FName(*Owner)); }
        return true;
    }, false);

    struct FMatch
    {
        TSet<FName> References;
        TSet<FName> Sources;
    };
    TMap<FName, FMatch> Matches;
    for (const auto& Source : Sources)
    {
        for (FName Target : PackageReferences(Registry, Source.Key, false, IncludeSoft))
        {
            if (Target == Source.Key || !WithinReferencePath(Target.ToString(), Prefix)) { continue; }
            FMatch& Match = Matches.FindOrAdd(Source.Value);
            Match.References.Add(Target);
            Match.Sources.Add(Source.Key);
        }
    }
    TArray<FName> Owners;
    Matches.GetKeys(Owners);
    Owners.Sort(FNameLexicalLess());
    TArray<TSharedPtr<FJsonValue>> Records;
    for (FName Owner : Owners)
    {
        const FMatch& Match = Matches.FindChecked(Owner);
        FJson Record = MakeShared<FJsonObject>();
        Record->SetStringField(TEXT("asset"), Owner.ToString());
        Record->SetArrayField(TEXT("references"), SortedPackageNames(Match.References));
        Record->SetArrayField(TEXT("source_packages"), SortedPackageNames(Match.Sources));
        Records.Add(JsonValue(Record));
    }
    Response->SetStringField(TEXT("folder"), Folder);
    Response->SetStringField(TEXT("path_prefix"), Prefix);
    Response->SetNumberField(TEXT("scanned_packages"), Sources.Num());
    Response->SetArrayField(TEXT("matches"), Records);
}

static void ReferenceAction(const FJson& Request, const FJson& Response, bool FindPath)
{
    const bool Soft = Boolean(Request, TEXT("include_soft"), true);
    Response->SetBoolField(TEXT("include_soft"), Soft);
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    if (FindPath)
    {
        const FString Prefix = ReferencePath(Request, TEXT("path_prefix"), true);
        const FString Folder = ReferencePath(Request, TEXT("folder"), true, TEXT("/Game"));
        Registry.SearchAllAssets(true);
        FindReferencePath(Registry, Prefix, Folder, Soft, Response);
        return;
    }
    const FString Asset = ReferencePath(Request, TEXT("asset"), false);
    const FString Direction = String(Request, TEXT("direction"), TEXT("referencers"), false);
    Require(Direction == TEXT("referencers") || Direction == TEXT("dependencies"),
        TEXT("direction must be referencers or dependencies"));
    const bool Recursive = Boolean(Request, TEXT("recursive"));
    Registry.SearchAllAssets(true);
    const FName Root(*Asset);
    TSet<FName> Visited{Root};
    TArray<FName> Pending{Root};
    for (int32 Index = 0; Index < Pending.Num(); ++Index)
    {
        for (FName Package : PackageReferences(Registry, Pending[Index], Direction == TEXT("referencers"), Soft))
        {
            bool AlreadyVisited = false;
            Visited.Add(Package, &AlreadyVisited);
            if (Recursive && !AlreadyVisited) { Pending.Add(Package); }
        }
    }
    Visited.Remove(Root);
    Response->SetStringField(TEXT("asset"), Asset);
    Response->SetStringField(TEXT("direction"), Direction);
    Response->SetBoolField(TEXT("recursive"), Recursive);
    Response->SetArrayField(TEXT("references"), SortedPackageNames(Visited));
}
