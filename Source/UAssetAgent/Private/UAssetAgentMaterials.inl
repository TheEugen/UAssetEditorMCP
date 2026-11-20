struct FMaterialParameterKind
{
    const TCHAR* Name;
    EMaterialParameterType Type;
};

static const FMaterialParameterKind MaterialParameterKinds[] = {
    {TEXT("scalar"), EMaterialParameterType::Scalar}, {TEXT("vector"), EMaterialParameterType::Vector},
    {TEXT("texture"), EMaterialParameterType::Texture}, {TEXT("static_switch"), EMaterialParameterType::StaticSwitch}
};

static const TCHAR* MaterialAssociationName(EMaterialParameterAssociation Association)
{
    if (Association == LayerParameter) { return TEXT("layer"); }
    if (Association == BlendParameter) { return TEXT("blend"); }
    return TEXT("global");
}

static FMaterialParameterInfo MaterialParameterSelector(const FJson& Op)
{
    const FString Association = String(Op, TEXT("association"), TEXT("global"), false);
    Require(Association == TEXT("global") || Association == TEXT("layer") || Association == TEXT("blend"),
        TEXT("association must be global, layer or blend"));
    const int32 Index = Integer(Op, TEXT("index"), INDEX_NONE);
    Require(Association == TEXT("global") ? Index == INDEX_NONE : Index >= 0,
        TEXT("Layer and blend selectors require a nonnegative index. Global selectors use no index"));
    const FName Name(*String(Op, TEXT("name")));
    Require(!Name.IsNone(), TEXT("Parameter name must be nonempty"));
    return FMaterialParameterInfo(Name, Association == TEXT("layer") ? LayerParameter :
        Association == TEXT("blend") ? BlendParameter : GlobalParameter, Index);
}

static TSharedPtr<FJsonValue> MaterialParameterValue(const FMaterialParameterValue& Value)
{
    if (Value.Type == EMaterialParameterType::Scalar) { return MakeShared<FJsonValueNumber>(Value.AsScalar()); }
    if (Value.Type == EMaterialParameterType::Vector)
    {
        const FLinearColor Color = Value.AsLinearColor();
        return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            MakeShared<FJsonValueNumber>(Color.R), MakeShared<FJsonValueNumber>(Color.G),
            MakeShared<FJsonValueNumber>(Color.B), MakeShared<FJsonValueNumber>(Color.A)});
    }
    if (Value.Type == EMaterialParameterType::StaticSwitch) { return MakeShared<FJsonValueBoolean>(Value.AsStaticSwitch()); }
    if (const UObject* Texture = Value.AsTextureObject()) { return MakeShared<FJsonValueString>(Texture->GetPathName()); }
    return MakeShared<FJsonValueNull>();
}

static void DescribeMaterialInstance(const UMaterialInstanceConstant* Instance, const FJson& Response)
{
    Response->SetStringField(TEXT("asset"), Instance->GetOutermost()->GetName());
    Response->SetStringField(TEXT("parent"), Instance->Parent ? Instance->Parent->GetPathName() : FString());
    TArray<TSharedPtr<FJsonValue>> Parameters;
    for (const FMaterialParameterKind& Kind : MaterialParameterKinds)
    {
        TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Values;
        Instance->GetAllParametersOfType(Kind.Type, Values);
        Values.KeySort([](const FMaterialParameterInfo& A, const FMaterialParameterInfo& B)
        {
            if (A.Association != B.Association) { return A.Association < B.Association; }
            if (A.Index != B.Index) { return A.Index < B.Index; }
            return A.Name.LexicalLess(B.Name);
        });
        for (const auto& Pair : Values)
        {
            FMaterialParameterMetadata Override;
            const bool Overridden = Instance->GetParameterOverrideValue(Kind.Type, Pair.Key, Override);
            const FJson Parameter = MakeShared<FJsonObject>();
            Parameter->SetStringField(TEXT("name"), Pair.Key.Name.ToString());
            Parameter->SetStringField(TEXT("type"), Kind.Name);
            Parameter->SetStringField(TEXT("association"), MaterialAssociationName(Pair.Key.Association));
            if (Pair.Key.Index != INDEX_NONE) { Parameter->SetNumberField(TEXT("index"), Pair.Key.Index); }
            Parameter->SetBoolField(TEXT("overridden"), Overridden);
            Parameter->SetField(TEXT("value"), MaterialParameterValue(Overridden ? Override.Value : Pair.Value.Value));
            Parameters.Add(JsonValue(Parameter));
        }
    }
    Response->SetArrayField(TEXT("parameters"), Parameters);
}

static float MaterialNumber(const TSharedPtr<FJsonValue>& Value)
{
    double Number = 0;
    Require(Value && Value->Type == EJson::Number && Value->TryGetNumber(Number) &&
        FMath::IsFinite(Number) && FMath::Abs(Number) <= TNumericLimits<float>::Max(), TEXT("Expected a finite float"));
    return static_cast<float>(Number);
}

static void EditMaterialInstance(UMaterialInstanceConstant* Instance, const FJson& Op)
{
    const FString Operation = String(Op, TEXT("op"));
    if (Operation == TEXT("set_parent"))
    {
        UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *String(Op, TEXT("parent")));
        Require(Parent != nullptr, TEXT("Material parent could not be loaded"));
        TSet<const UMaterialInterface*> Seen{Instance};
        for (const UMaterialInterface* Current = Parent; Current;)
        {
            Require(!Seen.Contains(Current), TEXT("Material parent would introduce a cycle"));
            Seen.Add(Current);
            const UMaterialInstance* ParentInstance = Cast<UMaterialInstance>(Current);
            Current = ParentInstance ? ParentInstance->Parent.Get() : nullptr;
        }
        Instance->SetParentEditorOnly(Parent, false);
        Require(Instance->Parent == Parent, TEXT("Unreal rejected the material parent"));
        return;
    }
    const bool Clear = Operation == TEXT("clear_parameter");
    Require(Clear || Operation == TEXT("set_parameter"), TEXT("Unsupported material instance operation: ") + Operation);
    const FString Type = String(Op, TEXT("type"));
    const FMaterialParameterKind* Kind = Algo::FindByPredicate(MaterialParameterKinds,
        [&](const FMaterialParameterKind& Candidate) { return Type == Candidate.Name; });
    Require(Kind != nullptr, TEXT("Parameter type must be scalar, vector, texture or static_switch"));
    const FMaterialParameterInfo Info = MaterialParameterSelector(Op);
    FGuid ExpressionGuid;
    if (!Clear)
    {
        Require(Op->HasField(TEXT("value")), TEXT("set_parameter requires a value"));
        TArray<FMaterialParameterInfo> Available;
        TArray<FGuid> Guids;
        Instance->GetAllParameterInfoOfType(Kind->Type, Available, Guids);
        const int32 Index = Available.Find(Info);
        Require(Index != INDEX_NONE, TEXT("Parameter does not exist on the material: ") + Info.Name.ToString());
        ExpressionGuid = Guids[Index];
    }
    const auto IsParameter = [&](const auto& Parameter) { return Parameter.ParameterInfo == Info; };
    if (Kind->Type == EMaterialParameterType::StaticSwitch)
    {
        FStaticParameterSet Static = Instance->GetStaticParameters();
        Static.StaticSwitchParameters.RemoveAll(IsParameter);
        if (!Clear) { Static.StaticSwitchParameters.Emplace(Info, Boolean(Op, TEXT("value")), true, ExpressionGuid); }
        Instance->UpdateStaticPermutation(Static);
    }
    else if (Kind->Type == EMaterialParameterType::Scalar)
    {
        if (Clear) { Instance->ScalarParameterValues.RemoveAll(IsParameter); }
        else { Instance->SetScalarParameterValueEditorOnly(Info, MaterialNumber(Op->TryGetField(TEXT("value")))); }
    }
    else if (Kind->Type == EMaterialParameterType::Vector)
    {
        if (Clear) { Instance->VectorParameterValues.RemoveAll(IsParameter); }
        else
        {
            const auto Components = Array(Op, TEXT("value"), true);
            Require(Components.Num() == 4, TEXT("Vector value requires [r, g, b, a]"));
            Instance->SetVectorParameterValueEditorOnly(Info, FLinearColor(MaterialNumber(Components[0]), MaterialNumber(Components[1]),
                MaterialNumber(Components[2]), MaterialNumber(Components[3])));
        }
    }
    else
    {
        if (Clear) { Instance->TextureParameterValues.RemoveAll(IsParameter); }
        else
        {
            UTexture* Texture = LoadObject<UTexture>(nullptr, *String(Op, TEXT("value")));
            Require(Texture != nullptr, TEXT("Texture parameter requires a valid texture object path"));
            Instance->SetTextureParameterValueEditorOnly(Info, Texture);
        }
    }
}

static void MaterialInstanceAction(const FJson& Request, const FJson& Response, bool Apply)
{
    const FString Package = PackagePath(Request);
    const FString Filename = AssetFilename(Package);
    const FString Before = Revision(Filename);
    auto* Instance = Cast<UMaterialInstanceConstant>(LoadExactAsset(Package));
    Require(Instance != nullptr, TEXT("Expected a saved Material Instance Constant"));
    Response->SetStringField(TEXT("revision"), Before);
    if (!Apply) { DescribeMaterialInstance(Instance, Response); return; }
    Require(String(Request, TEXT("expected_revision")) == Before, TEXT("Revision conflict"));
    const bool DryRun = Boolean(Request, TEXT("dry_run"), true);
    Response->SetBoolField(TEXT("dry_run"), DryRun);
    const auto Operations = Array(Request, TEXT("operations"), true);
    Require(Operations.Num() <= 1000, TEXT("At most 1000 material operations per batch"));
    Instance->Modify();
    for (int32 I = 0; I < Operations.Num(); ++I)
    {
        Response->SetNumberField(TEXT("operation_index"), I);
        EditMaterialInstance(Instance, AsObject(Operations[I]));
    }
    Response->RemoveField(TEXT("operation_index"));
    Require(Instance->Parent != nullptr, TEXT("Material instance requires a parent"));
    Instance->PostEditChange();
    // A standalone resource lets the commandlet validate shaders under NullRHI.
    FMaterialResource Resource;
    Resource.SetMaterial(Instance->GetMaterial(), Instance, ERHIFeatureLevel::SM5, EMaterialQualityLevel::High);
    const bool Compiled = Resource.CacheShaders(SP_PCD3D_SM5, EMaterialShaderPrecompileMode::Synchronous);
    TArray<TSharedPtr<FJsonValue>> Errors;
    for (const FString& Error : Resource.GetCompileErrors()) { Errors.Add(MakeShared<FJsonValueString>(Error)); }
    Response->SetArrayField(TEXT("shader_errors"), Errors);
    Require(Compiled && Errors.IsEmpty(), TEXT("Material shader compilation failed"));
    DescribeMaterialInstance(Instance, Response);
    if (DryRun) { return; }

    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.ScanFilesSynchronous({Filename}, true);
    MaintenanceFile(Registry, Package);
    BackupMaintenance({{Package, Before}}, Response);
    Require(Revision(Filename) == Before, TEXT("Material instance changed during the request"));
    FSavePackageArgs Save;
    Save.TopLevelFlags = RF_Public | RF_Standalone;
    Save.SaveFlags = SAVE_NoError;
    Response->SetBoolField(TEXT("save_attempted"), true);
    Require(UPackage::SavePackage(Instance->GetOutermost(), Instance, *Filename, Save), TEXT("Could not save the material instance"));
    Response->SetBoolField(TEXT("saved"), true);
    Response->SetStringField(TEXT("revision"), Revision(Filename));
}
