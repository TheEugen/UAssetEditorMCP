// Authored Blueprint members. External graph references to a renamed variable
// can follow Unreal's persistent member GUID when their package is next loaded.
static void RenameAuthoredVariable(UBlueprint* BP, const FJson& Op, const FJson& Response)
{
    const FName Old(*String(Op, TEXT("name")));
    const FString New = String(Op, TEXT("new_name"));
    const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Old);
    Require(Index != INDEX_NONE, TEXT("Local member variable not found"));
    AvailableName(BP, New);
    const FGuid Id = BP->NewVariables[Index].VarGuid;
    for (UBlueprint* Other : ReferencingBlueprints(BP, Response, true))
    {
        Require(!Other->ParentClass || !Other->ParentClass->IsChildOf(BP->GeneratedClass) ||
            FBlueprintEditorUtils::FindNewVariableIndex(Other, FName(*New)) == INDEX_NONE,
            TEXT("A child Blueprint already declares the new name: ") + Other->GetPathName());
        for (UK2Node* Node : MemberNodes(Other, BP, {Old}))
        {
            const auto* Variable = Cast<UK2Node_Variable>(Node);
            Require(Variable && Id.IsValid() && Variable->VariableReference.GetMemberGuid() == Id,
                TEXT("External reference cannot follow the variable GUID: ") + Node->GetPathName());
        }
    }
    // RenameMemberVariable otherwise asks an interactive question and clears
    // the RepNotify association. Its function may retain its existing name.
    const FName Notify = BP->NewVariables[Index].RepNotifyFunc;
    BP->Modify();
    BP->NewVariables[Index].RepNotifyFunc = NAME_None;
    FBlueprintEditorUtils::RenameMemberVariable(BP, Old, FName(*New));
    const int32 Renamed = FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*New));
    Require(Renamed != INDEX_NONE && BP->NewVariables[Renamed].VarGuid == Id, TEXT("Variable rename failed"));
    BP->NewVariables[Renamed].RepNotifyFunc = Notify;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
}

static void EditAuthoredComponent(UBlueprint* BP, const FJson& Op, bool Remove, const FJson& Response)
{
    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    Require(SCS && BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()), TEXT("Expected an Actor Blueprint"));
    BP->Modify();
    SCS->Modify();
    if (Remove)
    {
        const FComponentTemplate Component = SelectComponent(BP, String(Op, TEXT("component")));
        Require(Component.Node && !Component.Inherited && Component.Node != SCS->GetDefaultSceneRootNode(),
            TEXT("Only locally authored SCS components may be removed"));
        const FName Name = Component.Node->GetVariableName();
        Require(MemberNodes(BP, BP, {Name}).IsEmpty() && !FBlueprintEditorUtils::IsVariableUsed(BP, Name),
            TEXT("Remove component references before removing the component"));
        for (UBlueprint* Other : ReferencingBlueprints(BP, Response, true))
        {
            auto* Overrides = Other->GetInheritableComponentHandler(false);
            Require(MemberNodes(Other, BP, {Name}).IsEmpty() &&
                (!Overrides || !Overrides->GetOverridenComponentTemplate(FComponentKey(Component.Node))),
                TEXT("Component is used or overridden in ") + Other->GetPathName());
            if (Other->SimpleConstructionScript)
            {
                for (const USCS_Node* Child : Other->SimpleConstructionScript->GetRootNodes())
                { Require(Child->ParentComponentOrVariableName != Name, TEXT("Child Blueprint attaches to this component: ") + Other->GetPathName()); }
            }
        }
        SCS->RemoveNodeAndPromoteChildren(Component.Node);
    }
    else
    {
        const FString Name = String(Op, TEXT("name"));
        AvailableName(BP, Name);
        UClass* Class = LoadObject<UClass>(nullptr, *String(Op, TEXT("class")));
        Require(Class && Class->IsChildOf(UActorComponent::StaticClass()) &&
            !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists), TEXT("Expected a concrete ActorComponent class"));
        USCS_Node* ParentNode = nullptr;
        USceneComponent* Parent = nullptr;
        if (Op->HasField(TEXT("parent")))
        {
            const FComponentTemplate Selected = SelectComponent(BP, String(Op, TEXT("parent")));
            ParentNode = Selected.Node;
            Parent = Cast<USceneComponent>(Selected.Template);
            Require(Parent && Class->IsChildOf(USceneComponent::StaticClass()), TEXT("Attachments require scene components"));
        }
        else if (Class->IsChildOf(USceneComponent::StaticClass()))
        { Parent = SCS->GetSceneRootComponentTemplate(false, &ParentNode); }
        USCS_Node* Node = SCS->CreateNode(Class, FName(*Name));
        Require(Node && Node->GetVariableName() == FName(*Name), TEXT("Cannot create component with requested name"));
        if (ParentNode && ParentNode->GetSCS() == SCS) { ParentNode->AddChildNode(Node); }
        else
        {
            if (ParentNode) { Node->SetParent(ParentNode); }
            else if (Parent) { Node->SetParent(Parent); }
            SCS->AddNode(Node);
        }
    }
    SCS->ValidateSceneRootNodes();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
}
