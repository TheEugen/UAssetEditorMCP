// Included inside UAssetAgent after the common JSON/reflection helpers.
static FString BlueprintKind(const UBlueprint* BP)
{
    if (BP->BlueprintType == BPTYPE_FunctionLibrary) { return TEXT("function_library"); }
    return BP->IsA<UAnimBlueprint>() ? TEXT("animation") : BP->IsA<UWidgetBlueprint>() ? TEXT("widget") : TEXT("normal");
}

static FMemberReference* AnimFunctionSlot(UAnimGraphNode_Base* Node, const FString& Slot)
{
    if (Slot == TEXT("InitialUpdateFunction")) { return &Node->InitialUpdateFunction; }
    if (Slot == TEXT("BecomeRelevantFunction")) { return &Node->BecomeRelevantFunction; }
    if (Slot == TEXT("UpdateFunction")) { return &Node->UpdateFunction; }
    throw FFailure{TEXT("slot must be InitialUpdateFunction, BecomeRelevantFunction or UpdateFunction")};
}

static void DescribeAnimNode(UAnimGraphNode_Base* Node, const FJson& J)
{
    FJson Functions = MakeShared<FJsonObject>();
    for (const TCHAR* Slot : {TEXT("InitialUpdateFunction"), TEXT("BecomeRelevantFunction"), TEXT("UpdateFunction")})
    {
        const FName Name = AnimFunctionSlot(Node, Slot)->GetMemberName();
        if (Name.IsNone()) { Functions->SetField(Slot, MakeShared<FJsonValueNull>()); }
        else { Functions->SetStringField(Slot, Name.ToString()); }
    }
    J->SetObjectField(TEXT("functions"), Functions);
    FJson Properties = DescribeDefaults(Node);
    if (FStructProperty* Runtime = Node->GetFNodeProperty())
    {
        void* Data = Runtime->ContainerPtrToValuePtr<void>(Node);
        for (TFieldIterator<FProperty> It(Runtime->Struct); It; ++It)
        {
            if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Transient | CPF_EditConst)) { continue; }
            FString Value;
            It->ExportText_InContainer(0, Value, Data, Data, Node, PPF_None);
            Properties->SetStringField(Runtime->GetName() + TEXT(".") + It->GetName(), Value);
        }
    }
    J->SetObjectField(TEXT("properties"), Properties);
}

static void DescribeWidgetBlueprint(UWidgetBlueprint* BP, const FJson& J, bool Full)
{
    TArray<TSharedPtr<FJsonValue>> Tree;
    if (BP->WidgetTree)
    {
        BP->WidgetTree->ForEachWidget([&](UWidget* Widget)
        {
            FJson W = MakeShared<FJsonObject>();
            W->SetStringField(TEXT("name"), Widget->GetName());
            W->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
            W->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
            UPanelWidget* Parent = Widget->GetParent();
            W->SetStringField(TEXT("parent"), Parent ? Parent->GetName() : FString());
            W->SetNumberField(TEXT("index"), Parent ? Parent->GetChildIndex(Widget) : 0);
            if (Full) { W->SetObjectField(TEXT("properties"), DescribeDefaults(Widget)); }
            if (Widget->Slot)
            {
                FJson Slot = MakeShared<FJsonObject>();
                Slot->SetStringField(TEXT("class"), Widget->Slot->GetClass()->GetPathName());
                if (Full) { Slot->SetObjectField(TEXT("properties"), DescribeDefaults(Widget->Slot)); }
                W->SetObjectField(TEXT("slot"), Slot);
            }
            Tree.Add(JsonValue(W));
        });
    }
    J->SetArrayField(TEXT("widget_tree"), Tree);
    TArray<TSharedPtr<FJsonValue>> Bindings;
    for (const FDelegateEditorBinding& Binding : BP->Bindings)
    {
        FJson B = MakeShared<FJsonObject>();
        B->SetStringField(TEXT("widget"), Binding.ObjectName);
        B->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
        B->SetStringField(TEXT("function"), Binding.FunctionName.ToString());
        B->SetStringField(TEXT("source_property"), Binding.SourceProperty.ToString());
        B->SetStringField(TEXT("source_path"), Binding.SourcePath.GetDisplayText().ToString());
        B->SetStringField(TEXT("kind"), Binding.Kind == EBindingKind::Function ? TEXT("function") : TEXT("property"));
        Bindings.Add(JsonValue(B));
    }
    J->SetArrayField(TEXT("bindings"), Bindings);
    TArray<TSharedPtr<FJsonValue>> Animations;
    for (UWidgetAnimation* Animation : BP->Animations)
    {
        if (!Animation) { continue; }
        FJson A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("name"), Animation->GetName());
        A->SetNumberField(TEXT("start"), Animation->GetStartTime());
        A->SetNumberField(TEXT("end"), Animation->GetEndTime());
        A->SetNumberField(TEXT("length"), Animation->GetEndTime() - Animation->GetStartTime());
        Animations.Add(JsonValue(A));
    }
    J->SetArrayField(TEXT("animations"), Animations);
}

// Dot paths traverse structs only, never arbitrary UObject references.
static void SetNestedDefault(UObject* Object, const FString& Path, const FString& Value)
{
    TArray<FString> Parts;
    Path.ParseIntoArray(Parts, TEXT("."), false);
    Require(!Parts.IsEmpty(), TEXT("property path is empty"));
    UStruct* Type = Object->GetClass();
    void* Container = Object;
    FProperty* Property = nullptr;
    FProperty* Top = nullptr;
    for (int32 I = 0; I < Parts.Num(); ++I)
    {
        Require(!Parts[I].IsEmpty(), TEXT("Invalid property path"));
        Property = FindFProperty<FProperty>(Type, FName(*Parts[I]));
        Require(Property && Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) &&
            !Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditConst | CPF_DisableEditOnTemplate), TEXT("Editable property not found: ") + Path);
        if (!Top) { Top = Property; }
        if (I + 1 < Parts.Num())
        {
            FStructProperty* Struct = CastField<FStructProperty>(Property);
            Require(Struct != nullptr, TEXT("Property paths may traverse only structs"));
            Container = Struct->ContainerPtrToValuePtr<void>(Container);
            Type = Struct->Struct;
        }
    }
    Require(Property->ArrayDim == 1, TEXT("Fixed-size C++ property arrays are not supported"));
    void* Temp = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
    Property->InitializeValue(Temp);
    Property->CopyCompleteValue(Temp, Property->ContainerPtrToValuePtr<void>(Container));
    const TCHAR* End = Property->ImportText_Direct(*Value, Temp, Object, PPF_None);
    const bool Valid = End && FString(End).TrimStartAndEnd().IsEmpty();
    if (Valid)
    {
        Object->Modify();
        Object->PreEditChange(Top);
        Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Container), Temp);
    }
    Property->DestroyValue(Temp);
    FMemory::Free(Temp);
    Require(Valid, TEXT("Invalid property text: ") + Path);
    FPropertyChangedEvent Changed(Property, EPropertyChangeType::ValueSet);
    Changed.SetActiveMemberProperty(Top);
    Object->PostEditChangeProperty(Changed);
}
