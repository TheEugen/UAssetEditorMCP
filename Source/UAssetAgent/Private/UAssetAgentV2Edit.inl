// Included in FEditor. All operations remain inside its single-asset transaction.
void RetargetVariable(FName Name, UClass* Parent)
{
    const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Name);
    Require(Index != INDEX_NONE, TEXT("Local variable not found for retargeting"));
    FProperty* Inherited = Parent ? FindFProperty<FProperty>(Parent, Name) : nullptr;
    FEdGraphPinType InheritedType;
    Require(Inherited && GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Inherited, InheritedType) &&
        InheritedType == BP->NewVariables[Index].VarType, TEXT("Retargeting requires an inherited property with the same name and type"));
    TArray<UEdGraph*> Graphs;
    BP->GetAllGraphs(Graphs);
    for (UEdGraph* G : Graphs)
    {
        for (UEdGraphNode* N : G->Nodes)
        {
            if (UK2Node_Variable* V = Cast<UK2Node_Variable>(N))
            {
                UClass* Owner = V->VariableReference.GetMemberParentClass();
                if (V->VariableReference.GetMemberName() == Name &&
                    (V->VariableReference.IsSelfContext() || (Owner && Owner->ClassGeneratedBy == BP)))
                {
                    // Drop the local member GUID. The inherited property now
                    // resolves by name against the newly compiled self class.
                    V->VariableReference.SetSelfMember(Name);
                }
            }
        }
    }
    // RemoveMemberVariable also deletes nodes by name, which would destroy the
    // getters/setters we just retained. Remove only the local descriptor here.
    BP->NewVariables.RemoveAt(Index);
}

UWidgetBlueprint* WidgetBlueprint()
{
    UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP);
    Require(WidgetBP && WidgetBP->WidgetTree, TEXT("This operation requires a Widget Blueprint with a local widget tree"));
    return WidgetBP;
}

UWidget* Widget(const FString& Selector)
{
    FName Name(*Selector);
    if (const FName* Alias = WidgetAliases.Find(Selector)) { Name = *Alias; }
    UWidget* Found = WidgetBlueprint()->WidgetTree->FindWidget(Name);
    Require(Found && Found->GetOuter() == WidgetBlueprint()->WidgetTree, TEXT("Local widget not found: ") + Selector);
    return Found;
}

void RequireUnreferencedWidget(UWidget* W)
{
    UWidgetBlueprint* WBP = WidgetBlueprint();
    TArray<UEdGraph*> Graphs;
    BP->GetAllGraphs(Graphs);
    for (UEdGraph* G : Graphs)
    {
        for (UEdGraphNode* N : G->Nodes)
        {
            if (UK2Node_ComponentBoundEvent* E = Cast<UK2Node_ComponentBoundEvent>(N))
            { Require(E->ComponentPropertyName != W->GetFName(), TEXT("Widget has a bound event; delete that node first")); }
            if (UK2Node* K2 = Cast<UK2Node>(N))
            { Require(!K2->ReferencesVariable(W->GetFName(), BP->SkeletonGeneratedClass), TEXT("Widget is referenced in a graph; remove those references first")); }
        }
    }
    for (const FDelegateEditorBinding& B : WBP->Bindings)
    { Require(B.ObjectName != W->GetName() && B.SourceProperty != W->GetFName() && B.SourcePath.IsEmpty(), TEXT("Widget Blueprint has property bindings that prevent safe removal/rename")); }
    for (UWidgetAnimation* A : WBP->Animations)
    {
        if (!A) { continue; }
        for (const FWidgetAnimationBinding& B : A->AnimationBindings)
        { Require(B.WidgetName != W->GetFName() && B.SlotWidgetName != W->GetFName(), TEXT("Widget is referenced by an animation")); }
    }
    WBP->WidgetTree->ForEachWidget([&](UWidget* Other)
    { Require(!Other->Navigation, TEXT("Explicit navigation is present; removal/rename is not supported until navigation is cleared")); });
    if (WBP->GeneratedClass)
    {
        const UUserWidget* Defaults = Cast<UUserWidget>(WBP->GeneratedClass->GetDefaultObject());
        Require(!Defaults || Defaults->GetDesiredFocusWidgetName() != W->GetFName(), TEXT("Widget is the desired focus target; clear that reference first"));
    }
    if (BP->ParentClass)
    { Require(!FindFProperty<FProperty>(BP->ParentClass, W->GetFName()), TEXT("Widget matches an inherited member; cannot safely remove/rename it")); }
}

void SetSlot(UWidget* W, const FJson& Op)
{
    if (!Op->HasField(TEXT("slot"))) { return; }
    Require(W->Slot != nullptr, TEXT("Root widgets have no panel slot"));
    for (const auto& Pair : Object(Op, TEXT("slot"))->Values)
    {
        FString Value;
        Require(Pair.Value->TryGetString(Value), TEXT("slot values must be Unreal text strings"));
        SetNestedDefault(W->Slot, Pair.Key, Value);
    }
}

bool ApplyV2(const FJson& Op)
{
    const FString Kind = String(Op, TEXT("op"));
    if (Kind == TEXT("set_anim_node_function") || Kind == TEXT("set_anim_node_property"))
    {
        Require(BP->IsA<UAnimBlueprint>(), TEXT("Animation node operations require an AnimBP"));
        UAnimGraphNode_Base* Anim = Cast<UAnimGraphNode_Base>(Node(Graph(Op), String(Op, TEXT("node"))));
        Require(Anim != nullptr, TEXT("Node is not an animation graph node"));
        if (Kind == TEXT("set_anim_node_function"))
        {
            const FString Slot = String(Op, TEXT("slot"));
            FMemberReference* Reference = AnimFunctionSlot(Anim, Slot);
            Require(Op->HasField(TEXT("function")), TEXT("function must be a name or null"));
            if (Op->HasTypedField<EJson::Null>(TEXT("function"))) { *Reference = FMemberReference(); }
            else
            {
                const FName Name(*String(Op, TEXT("function")));
                UFunction* Function = BP->ParentClass ? BP->ParentClass->FindFunctionByName(Name) : nullptr;
                if (!Function && BP->SkeletonGeneratedClass) { Function = BP->SkeletonGeneratedClass->FindFunctionByName(Name); }
                Require(Function && FBlueprintEditorUtils::HasFunctionBlueprintThreadSafeMetaData(Function), TEXT("Function must exist and be BlueprintThreadSafe"));
                FProperty* BindingProperty = FindFProperty<FProperty>(Anim->GetClass(), FName(*Slot));
                const FString PrototypePath = BindingProperty->GetMetaData(TEXT("PrototypeFunction"));
                UFunction* Prototype = LoadObject<UFunction>(nullptr, *PrototypePath);
                Require(Prototype && Prototype->IsSignatureCompatibleWith(Function), TEXT("Function signature must match the animation update prototype (context and node reference)"));
                Reference->SetFromField<UFunction>(Function, true);
            }
            FPropertyChangedEvent Changed(FindFProperty<FProperty>(Anim->GetClass(), FName(*Slot)));
            Anim->PostEditChangeProperty(Changed);
        }
        else
        {
            FString Property = String(Op, TEXT("property"));
            FStructProperty* Runtime = Anim->GetFNodeProperty();
            Require(!Property.Contains(TEXT("Function")) && (!Runtime || FName(*Property) != Runtime->GetFName()), TEXT("Use set_anim_node_function for bindings; set individual runtime fields"));
            if (!FindFProperty<FProperty>(Anim->GetClass(), FName(*Property)) && !Property.Contains(TEXT(".")))
            {
                Require(Runtime != nullptr, TEXT("Animation node has no runtime struct"));
                Property = Runtime->GetName() + TEXT(".") + Property;
            }
            SetNestedDefault(Anim, Property, ValueString(Op, TEXT("value")));
        }
        FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
        return true;
    }
    if (Kind == TEXT("delete_function"))
    {
        const bool Library = BP->BlueprintType == BPTYPE_FunctionLibrary;
        Require(Library || BP->IsA<UAnimBlueprint>() || BP->IsA<UWidgetBlueprint>(),
            TEXT("delete_function requires an animation, widget or function library Blueprint"));
        const FString Name = String(Op, TEXT("name"));
        if (CompletedFunctionDeletes.Remove(FName(*Name))) { return true; }
        UEdGraph* Found = nullptr;
        for (UEdGraph* G : BP->FunctionGraphs)
        { if (G->GetName() == Name) { Require(!Found, TEXT("Ambiguous function graph")); Found = G; } }
        Require(Found && Found->GetSchema()->GetGraphType(Found) == GT_Function &&
            Found->GetSchema()->IsA<UEdGraphSchema_K2>() && !Found->GetSchema()->GetClass()->GetName().Contains(TEXT("Animation")),
            TEXT("Only ordinary K2 function graphs may be deleted; animation layers/state machines are excluded"));
        FBlueprintEditorUtils::RemoveGraph(BP, Found, EGraphRemoveFlags::MarkTransient);
        if (Library) { FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP); }
        else { FBlueprintEditorUtils::MarkBlueprintAsModified(BP); }
        return true;
    }
    if (Kind == TEXT("add_widget"))
    {
        UWidgetBlueprint* WBP = WidgetBlueprint();
        const FString Name = String(Op, TEXT("name"));
        const FString Alias = String(Op, TEXT("id"));
        ValidName(Name);
        Require(!WidgetAliases.Contains(Alias) && !WBP->WidgetTree->FindWidget(FName(*Name)) &&
            !FindObject<UObject>(WBP->WidgetTree, *Name), TEXT("Widget name or alias already exists"));
        UClass* Class = LoadObject<UClass>(nullptr, *String(Op, TEXT("class")));
        Require(Class && Class->IsChildOf(UWidget::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists), TEXT("Invalid widget class"));
        Require(Class->ClassGeneratedBy != BP && (!BP->GeneratedClass || !Class->IsChildOf(BP->GeneratedClass)), TEXT("Recursive user widget nesting is not supported"));
        UPanelWidget* Parent = nullptr;
        const FString ParentName = String(Op, TEXT("parent"), FString(), false);
        if (!ParentName.IsEmpty()) { Parent = Cast<UPanelWidget>(Widget(ParentName)); Require(Parent && Parent->CanAddMoreChildren(), TEXT("Parent is not a panel or is full")); }
        else { Require(!WBP->WidgetTree->RootWidget, TEXT("Root already exists; supply a panel parent")); }
        const int32 Index = Integer(Op, TEXT("index"), Parent ? Parent->GetChildrenCount() : 0);
        Require(Index >= 0 && Index <= (Parent ? Parent->GetChildrenCount() : 0), TEXT("Widget index is out of range"));
        UWidget* W = WBP->WidgetTree->ConstructWidget<UWidget>(Class, FName(*Name));
        Require(W != nullptr, TEXT("Cannot construct widget"));
        W->bIsVariable = Boolean(Op, TEXT("is_variable"), true);
        if (Parent) { Require(Parent->InsertChildAt(Index, W) != nullptr, TEXT("Cannot insert widget")); }
        else { WBP->WidgetTree->RootWidget = W; }
        SetSlot(W, Op);
        WidgetAliases.Add(Alias, W->GetFName());
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        return true;
    }
    if (Kind == TEXT("set_widget_property") || Kind == TEXT("set_slot_property"))
    {
        UWidget* W = Widget(String(Op, TEXT("widget")));
        UObject* Target = Kind == TEXT("set_slot_property") ? static_cast<UObject*>(W->Slot) : W;
        Require(Target != nullptr, TEXT("Widget has no panel slot"));
        const FString Property = String(Op, TEXT("property"));
        Require(Property != TEXT("Slot") && Property != TEXT("Content") && Property != TEXT("Parent") && Property != TEXT("bIsVariable"), TEXT("Structural widget fields require tree operations"));
        SetNestedDefault(Target, Property, ValueString(Op, TEXT("value")));
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        return true;
    }
    if (Kind == TEXT("move_widget"))
    {
        UWidget* W = Widget(String(Op, TEXT("widget")));
        UPanelWidget* Parent = Cast<UPanelWidget>(Widget(String(Op, TEXT("parent"))));
        Require(Parent && W != Parent, TEXT("New parent must be a different panel widget"));
        for (UWidget* Ancestor = Parent; Ancestor; Ancestor = Ancestor->GetParent()) { Require(Ancestor != W, TEXT("Widget move would create a tree cycle")); }
        UPanelWidget* OldParent = W->GetParent();
        Require(OldParent != nullptr, TEXT("Moving the root is not supported"));
        const int32 Last = Parent->GetChildrenCount() - (OldParent == Parent ? 1 : 0);
        const int32 Index = Integer(Op, TEXT("index"), Last);
        Require(Index >= 0 && Index <= Last && (OldParent == Parent || Parent->CanAddMoreChildren()), TEXT("Panel is full or index is out of range"));
        if (OldParent == Parent) { Parent->ShiftChild(Index, W); }
        else
        {
            UPanelSlot* OldSlot = W->Slot;
            OldParent->RemoveChild(W);
            UPanelSlot* NewSlot = Parent->InsertChildAt(Index, W);
            Require(NewSlot != nullptr, TEXT("Cannot insert widget into new parent"));
            if (OldSlot && OldSlot->GetClass() == NewSlot->GetClass())
            {
                for (const auto& Pair : DescribeDefaults(OldSlot)->Values)
                {
                    FProperty* P = FindFProperty<FProperty>(NewSlot->GetClass(), FName(*Pair.Key));
                    FString Value;
                    if (P && !P->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_DisableEditOnTemplate) && Pair.Value->TryGetString(Value))
                    { SetNestedDefault(NewSlot, Pair.Key, Value); }
                }
            }
        }
        SetSlot(W, Op);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        return true;
    }
    if (Kind == TEXT("remove_widget") || Kind == TEXT("rename_widget"))
    {
        UWidgetBlueprint* WBP = WidgetBlueprint();
        UWidget* W = Widget(String(Op, TEXT("widget")));
        TArray<UWidget*> Affected;
        Affected.Add(W);
        if (Kind == TEXT("remove_widget")) { UWidgetTree::GetChildWidgets(W, Affected); }
        for (UWidget* Item : Affected) { RequireUnreferencedWidget(Item); }
        if (Kind == TEXT("rename_widget"))
        {
            const FString NewName = String(Op, TEXT("name"));
            ValidName(NewName);
            AvailableName(BP, NewName);
            Require(!FindObject<UObject>(WBP->WidgetTree, *NewName), TEXT("Widget name already exists"));
            const FName OldName = W->GetFName();
            Require(W->Rename(*NewName, nullptr, REN_DontCreateRedirectors), TEXT("Widget rename failed"));
            for (auto& Pair : WidgetAliases) { if (Pair.Value == OldName) { Pair.Value = W->GetFName(); } }
        }
        else
        {
            Require(WBP->WidgetTree->RemoveWidget(W), TEXT("Widget could not be detached"));
            for (UWidget* Item : Affected)
            {
                const FName OldName = Item->GetFName();
                for (auto It = WidgetAliases.CreateIterator(); It; ++It) { if (It.Value() == OldName) { It.RemoveCurrent(); } }
                Item->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
            }
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        return true;
    }
    if (Kind == TEXT("bind_widget_event"))
    {
        UWidget* W = Widget(String(Op, TEXT("widget")));
        Require(W->bIsVariable, TEXT("Widget must have is_variable:true to bind an event"));
        FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(W->GetClass(), FName(*String(Op, TEXT("event"))));
        Require(Delegate && Delegate->HasAnyPropertyFlags(CPF_BlueprintAssignable), TEXT("Blueprint-assignable widget delegate not found"));
        UEdGraph* G = Graph(Op);
        Require(G->GetSchema()->GetGraphType(G) == GT_Ubergraph, TEXT("Widget events require an event graph"));
        TArray<UEdGraph*> Graphs;
        BP->GetAllGraphs(Graphs);
        for (UEdGraph* ExistingGraph : Graphs)
        {
            for (UEdGraphNode* N : ExistingGraph->Nodes)
            {
                if (UK2Node_ComponentBoundEvent* E = Cast<UK2Node_ComponentBoundEvent>(N))
                {
                    if (E->ComponentPropertyName == W->GetFName() && E->DelegatePropertyName == Delegate->GetFName())
                    { Require(ExistingGraph == G, TEXT("Existing delegate event is in a different graph")); Remember(String(Op, TEXT("id")), E); return true; }
                }
            }
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        FObjectProperty* Member = FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, W->GetFName());
        Require(Member != nullptr, TEXT("Widget member was not generated"));
        UK2Node_ComponentBoundEvent* Event = NewObject<UK2Node_ComponentBoundEvent>(G);
        Event->InitializeComponentBoundEventParams(Member, Delegate);
        G->AddNode(Event, false, false);
        Event->CreateNewGuid(); Event->PostPlacedNewNode(); Event->AllocateDefaultPins();
        Event->NodePosX = Integer(Op, TEXT("x")); Event->NodePosY = Integer(Op, TEXT("y"));
        Remember(String(Op, TEXT("id")), Event);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        return true;
    }
    return false;
}
