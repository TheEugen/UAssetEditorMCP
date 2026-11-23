static bool EditableAssetField(const FProperty* Field)
{
    return Field && Field->HasAnyPropertyFlags(CPF_Edit) &&
        !Field->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated) && Field->ArrayDim == 1;
}

static FJson DescribeEditableProperties(UObject* Asset)
{
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
    return Properties;
}

static void EditAssetProperty(UObject* Asset, const FJson& Op, bool AllowInstancedAssignment = true)
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
    Require(AllowInstancedAssignment || !Field->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference),
        TEXT("Use create_subobject or clone_subobject for an instanced slot: ") + Path);
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

static FObjectProperty* InstancedObjectProperty(FProperty* Field)
{
    auto* Object = CastField<FObjectProperty>(Field);
    return Object && Object->HasAnyPropertyFlags(CPF_InstancedReference) ? Object : nullptr;
}

static FJson DescribeOwnedSlots(UObject* Owner, const FString& ParentPath = FString())
{
    FJson Slots = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Owner->GetClass()); It; ++It)
    {
        FProperty* Field = *It;
        if (!EditableAssetField(Field)) { continue; }
        auto* ArrayField = CastField<FArrayProperty>(Field);
        FObjectProperty* ObjectField = InstancedObjectProperty(ArrayField ? ArrayField->Inner : Field);
        if (!ObjectField) { continue; }

        FJson Slot = MakeShared<FJsonObject>();
        Slot->SetStringField(TEXT("class"), ObjectField->PropertyClass->GetPathName());
        Slot->SetBoolField(TEXT("array"), ArrayField != nullptr);
        TArray<TSharedPtr<FJsonValue>> Items;
        const FString Path = ParentPath + Field->GetName();
        const auto AddItem = [&](void* Address, const FString& ItemPath)
        {
            UObject* Object = ObjectField->GetObjectPropertyValue(Address);
            if (!Object) { return; }
            FJson Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("path"), ItemPath);
            Item->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
            Item->SetStringField(TEXT("object"), Object->GetPathName());
            Item->SetStringField(TEXT("owner"), GetPathNameSafe(Object->GetOuter()));
            const bool Owned = Object->GetOuter() == Owner;
            Item->SetBoolField(TEXT("owned"), Owned);
            if (Owned)
            {
                Item->SetObjectField(TEXT("properties"), DescribeEditableProperties(Object));
                FJson Nested = DescribeOwnedSlots(Object, ItemPath + TEXT("."));
                if (!Nested->Values.IsEmpty()) { Item->SetObjectField(TEXT("subobjects"), Nested); }
            }
            Items.Add(JsonValue(Item));
        };
        void* Address = Field->ContainerPtrToValuePtr<void>(Owner);
        if (ArrayField)
        {
            FScriptArrayHelper Elements(ArrayField, Address);
            Slot->SetNumberField(TEXT("length"), Elements.Num());
            for (int32 Index = 0; Index < Elements.Num(); ++Index)
            { AddItem(Elements.GetRawPtr(Index), FString::Printf(TEXT("%s[%d]"), *Path, Index)); }
        }
        else { AddItem(Address, Path); }
        Slot->SetArrayField(TEXT("items"), Items);
        Slots->SetObjectField(Field->GetName(), Slot);
    }
    return Slots;
}

static UObject* ResolveOwnedObject(UObject* Owner, const FString& Path)
{
    TArray<FString> Parts;
    Path.ParseIntoArray(Parts, TEXT("."), false);
    Require(!Parts.IsEmpty(), TEXT("Expected a subobject path"));
    for (FString Part : Parts)
    {
        FString IndexText;
        int32 Index = INDEX_NONE;
        FString FieldName;
        const bool Indexed = Part.Split(TEXT("["), &FieldName, &IndexText);
        if (Indexed) { Part = MoveTemp(FieldName); }
        if (Indexed)
        {
            int64 ParsedIndex = INDEX_NONE;
            Require(IndexText.RemoveFromEnd(TEXT("]")) && !IndexText.IsEmpty() &&
                IndexText.IsNumeric() && FChar::IsDigit(IndexText[0]) && !IndexText.Contains(TEXT(".")) &&
                LexTryParseString(ParsedIndex, *IndexText) && ParsedIndex >= 0 && ParsedIndex <= MAX_int32,
                TEXT("Invalid array index in subobject path: ") + Path);
            Index = static_cast<int32>(ParsedIndex);
        }
        FProperty* Field = FindFProperty<FProperty>(Owner->GetClass(), FName(*Part));
        Require(EditableAssetField(Field), TEXT("Editable subobject slot not found: ") + Path);
        auto* ArrayField = CastField<FArrayProperty>(Field);
        FObjectProperty* ObjectField = InstancedObjectProperty(ArrayField ? ArrayField->Inner : Field);
        Require(ObjectField && Indexed == (ArrayField != nullptr), TEXT("Expected an instanced object slot or indexed slot array: ") + Path);
        void* Address = Field->ContainerPtrToValuePtr<void>(Owner);
        if (ArrayField)
        {
            FScriptArrayHelper Elements(ArrayField, Address);
            Require(Elements.IsValidIndex(Index), TEXT("Subobject index out of bounds: ") + Path);
            Address = Elements.GetRawPtr(Index);
        }
        UObject* Object = ObjectField->GetObjectPropertyValue(Address);
        Require(Object && Object->GetOuter() == Owner && !Object->HasAnyFlags(RF_Transient),
            TEXT("Subobject is null or is not owned by its containing object: ") + Path);
        Owner = Object;
    }
    return Owner;
}

static UObject* LoadExactAsset(const FString& Package);

static bool IsOwnedObjectOperation(const FString& Kind)
{
    return Kind == TEXT("create_subobject") || Kind == TEXT("clone_subobject") || Kind == TEXT("set_subobject_property");
}

static void EditOwnedObject(UObject* Root, const FJson& Op)
{
    const FString Kind = String(Op, TEXT("op"));
    if (Kind == TEXT("set_subobject_property"))
    {
        UObject* Target = ResolveOwnedObject(Root, String(Op, TEXT("target")));
        FJson Edit = MakeShared<FJsonObject>();
        Edit->SetStringField(TEXT("op"), TEXT("set_property"));
        Edit->SetStringField(TEXT("property"), String(Op, TEXT("property")));
        Edit->SetStringField(TEXT("value"), ValueString(Op, TEXT("value")));
        Target->Modify();
        EditAssetProperty(Target, Edit, false);
        return;
    }

    const FString OwnerPath = String(Op, TEXT("owner"), FString(), false);
    UObject* Owner = OwnerPath.IsEmpty() ? Root : ResolveOwnedObject(Root, OwnerPath);
    const FString Slot = String(Op, TEXT("property"));
    FProperty* Field = FindFProperty<FProperty>(Owner->GetClass(), FName(*Slot));
    Require(EditableAssetField(Field), TEXT("Editable subobject slot not found: ") + Slot);
    auto* ArrayField = CastField<FArrayProperty>(Field);
    FObjectProperty* ObjectField = InstancedObjectProperty(ArrayField ? ArrayField->Inner : Field);
    Require(ObjectField != nullptr, TEXT("Expected an instanced object property or array: ") + Slot);
    void* Address = Field->ContainerPtrToValuePtr<void>(Owner);
    Require(ArrayField || !ObjectField->GetObjectPropertyValue(Address), TEXT("Subobject slot is already occupied: ") + Slot);

    UObject* Source = nullptr;
    UClass* Class;
    if (Kind == TEXT("clone_subobject"))
    {
        FJson Selection = Object(Op, TEXT("source"));
        UObject* SourceRoot = Root;
        if (Selection->HasField(TEXT("asset")))
        {
            const FString SourcePackage = PackagePath(Selection);
            SourceRoot = LoadExactAsset(SourcePackage);
            Require(!SourceRoot->IsA<UObjectRedirector>() && !SourceRoot->IsA<UWorld>(),
                TEXT("Expected a source Blueprint or reflected asset: ") + SourcePackage);
            if (UBlueprint* SourceBlueprint = Cast<UBlueprint>(SourceRoot))
            {
                Require(SourceBlueprint->GeneratedClass != nullptr, TEXT("Source Blueprint has no generated class"));
                SourceRoot = SourceBlueprint->GeneratedClass->GetDefaultObject();
            }
        }
        Source = ResolveOwnedObject(SourceRoot, String(Selection, TEXT("path")));
        Class = Source->GetClass();
    }
    else { Class = LoadObject<UClass>(nullptr, *String(Op, TEXT("class"))); }
    Require(Class && Class->IsChildOf(ObjectField->PropertyClass) && Class->HasAnyClassFlags(CLASS_EditInlineNew) &&
        !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Transient) &&
        Owner->IsA(Class->ClassWithin), TEXT("Expected a concrete inline class compatible with the slot and its owner: ") + Slot);

    Owner->Modify();
    Owner->PreEditChange(Field);
    UObject* Instance = Source
        ? DuplicateObject<UObject>(Source, Owner, MakeUniqueObjectName(Owner, Class, Class->GetFName()))
        : NewObject<UObject>(Owner, Class, NAME_None, RF_Transactional);
    Require(Instance && Instance->GetOuter() == Owner, TEXT("Could not construct the owned subobject: ") + Slot);
    Instance->ClearFlags(RF_Public | RF_Standalone);
    Instance->SetFlags(RF_Transactional);
    if (ArrayField)
    {
        FScriptArrayHelper Elements(ArrayField, Address);
        Address = Elements.GetRawPtr(Elements.AddValue());
    }
    ObjectField->SetObjectPropertyValue(Address, Instance);
    FPropertyChangedEvent Changed(Field, ArrayField ? EPropertyChangeType::ArrayAdd : EPropertyChangeType::ValueSet);
    Owner->PostEditChangeProperty(Changed);
}

static void ValidateOwnedObjectEdits(const UObject* Root, const FJson& Response)
{
    FDataValidationContext Context;
    const EDataValidationResult Validity = Root->IsDataValid(Context);
    TArray<TSharedPtr<FJsonValue>> Issues;
    for (const auto& Issue : Context.GetIssues())
    {
        FJson Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("severity"), Issue.Severity == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
        Item->SetStringField(TEXT("message"), Issue.TokenizedMessage ? Issue.TokenizedMessage->ToText().ToString() : Issue.Message.ToString());
        Issues.Add(JsonValue(Item));
    }
    Response->SetArrayField(TEXT("validation_diagnostics"), Issues);
    Require(Validity != EDataValidationResult::Invalid && Context.GetNumErrors() == 0,
        TEXT("Asset validation failed after editing owned subobjects"));
}
