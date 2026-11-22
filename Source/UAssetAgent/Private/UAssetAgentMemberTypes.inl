// Change the member declaration before reconstructing its authored K2 nodes.
static TArray<UK2Node*> MemberNodes(UBlueprint* Blueprint, const UBlueprint* Owner, const TSet<FName>& Names)
{
    TArray<UK2Node*> Result;
    TArray<UK2Node*> Nodes;
    FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Nodes);
    for (UK2Node* Node : Nodes)
    {
        UClass* Context = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
        if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
        {
            const FMemberReference& Reference = Variable->VariableReference;
            const FName Name = Reference.GetMemberName();
            if (Reference.IsLocalScope() || !Names.Contains(Name)) { continue; }
            const UClass* Scope = Reference.GetMemberParentClass(Context);
            const FProperty* Property = Scope ? FindFProperty<FProperty>(Scope, Name) : nullptr;
            const UClass* DeclaringClass = Property ? Cast<UClass>(Property->GetOwnerStruct()) : Scope;
            if (DeclaringClass && DeclaringClass->ClassGeneratedBy == Owner) { Result.Add(Node); }
        }
        else if (const UK2Node_ComponentBoundEvent* Event = Cast<UK2Node_ComponentBoundEvent>(Node))
        {
            if (!Names.Contains(Event->ComponentPropertyName) || !Context) { continue; }
            const FProperty* Property = FindFProperty<FProperty>(Context, Event->ComponentPropertyName);
            const UClass* DeclaringClass = Property ? Cast<UClass>(Property->GetOwnerStruct()) : nullptr;
            if (DeclaringClass && DeclaringClass->ClassGeneratedBy == Owner) { Result.Add(Node); }
        }
    }
    return Result;
}

struct FMemberLink
{
    UEdGraphNode* From;
    FName Output;
    UEdGraphNode* To;
    FName Input;
};

struct FMemberNodeSnapshot
{
    TArray<UK2Node*> Nodes;
    TArray<FMemberLink> Links;
};

static FMemberNodeSnapshot CaptureMemberNodes(UBlueprint* Blueprint, const UBlueprint* Owner, const TSet<FName>& Names)
{
    FMemberNodeSnapshot Snapshot{MemberNodes(Blueprint, Owner, Names), {}};
    for (const UK2Node* Node : Snapshot.Nodes)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bOrphanedPin) { continue; }
            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                const UEdGraphPin* From = Pin->Direction == EGPD_Output ? Pin : Linked;
                const UEdGraphPin* To = Pin->Direction == EGPD_Input ? Pin : Linked;
                Snapshot.Links.Add({From->GetOwningNode(), From->PinName, To->GetOwningNode(), To->PinName});
            }
        }
    }
    return Snapshot;
}

static void ReconstructMemberNodes(UBlueprint* Blueprint, const FMemberNodeSnapshot& Snapshot)
{
    for (UK2Node* Node : Snapshot.Nodes)
    {
        const bool Previous = Node->bDisableOrphanPinSaving;
        Node->bDisableOrphanPinSaving = true;
        Node->ReconstructNode();
        Node->bDisableOrphanPinSaving = Previous;
    }
    for (const FMemberLink& Link : Snapshot.Links)
    {
        UEdGraphPin* From = Link.From->FindPin(Link.Output, EGPD_Output);
        UEdGraphPin* To = Link.To->FindPin(Link.Input, EGPD_Input);
        const auto* Schema = CastChecked<UEdGraphSchema_K2>(Link.From->GetSchema());
        const FString Error = FString::Printf(TEXT("Type change would break %s -> %s in %s. Disconnect or replace the old use before changing the type."),
            *Link.Output.ToString(), *Link.Input.ToString(), *Link.From->GetGraph()->GetName());
        Require(From && To && !From->bOrphanedPin && !To->bOrphanedPin &&
            Schema->ArePinsCompatible(From, To, Blueprint->SkeletonGeneratedClass), Error);
        if (!From->LinkedTo.Contains(To))
        {
            Require(Schema->CanCreateConnection(From, To).Response == CONNECT_RESPONSE_MAKE &&
                Schema->TryCreateConnection(From, To), Error);
        }
    }
}

static bool ChangeVariableType(UBlueprint* Blueprint, FName Name, const FEdGraphPinType& Type)
{
    const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
    Require(Index != INDEX_NONE, TEXT("Local member variable not found; edit its declaring Blueprint: ") + Name.ToString());
    FBPVariableDescription& Variable = Blueprint->NewVariables[Index];
    if (Variable.VarType == Type) { return false; }
    Require(!((Type.IsSet() || Type.IsMap()) && (Variable.PropertyFlags & CPF_Net)), TEXT("Replicated variables cannot be changed to sets or maps"));
    const FMemberNodeSnapshot Nodes = CaptureMemberNodes(Blueprint, Blueprint, {Name});
    Blueprint->Modify();
    const bool WasBool = Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_Boolean;
    const bool IsBool = Type.PinCategory == UEdGraphSchema_K2::PC_Boolean;
    Variable.VarType = Type;
    Variable.DefaultValue.Reset();
    if (WasBool != IsBool) { Variable.FriendlyName = FName::NameToDisplayString(Name.ToString(), IsBool); }
    const UClass* Class = Cast<UClass>(Type.PinSubCategoryObject.Get());
    if (Type.PinCategory == UEdGraphSchema_K2::PC_Object && Class && Class->IsChildOf(AActor::StaticClass()))
    { Variable.PropertyFlags |= CPF_DisableEditOnTemplate; }
    else { Variable.PropertyFlags &= ~CPF_DisableEditOnTemplate; }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    ReconstructMemberNodes(Blueprint, Nodes);
    return true;
}

static USCS_Node* ReplaceComponentClass(UBlueprint* Blueprint, const FJson& Op)
{
    const FComponentTemplate Selected = SelectComponent(Blueprint, String(Op, TEXT("component")));
    Require(Selected.Node && !Selected.Inherited, TEXT("Replace a component class on the Blueprint that declares the SCS component"));
    USCS_Node* Node = Selected.Node;
    Require(Node != Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode(),
        TEXT("Replace an authored component class; the default scene root is generated by Unreal"));
    UClass* Class = LoadObject<UClass>(nullptr, *String(Op, TEXT("class")));
    Require(Class && Class->IsChildOf(UActorComponent::StaticClass()) &&
        !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists),
        TEXT("Expected a concrete ActorComponent class"));
    UActorComponent* Old = Node->ComponentTemplate;
    if (Old->GetClass() == Class) { return nullptr; }
    Require(Old->IsA<USceneComponent>() == Class->IsChildOf(USceneComponent::StaticClass()),
        TEXT("The replacement must keep the component's scene or non-scene attachment type"));

    const FMemberNodeSnapshot Nodes = CaptureMemberNodes(Blueprint, Blueprint, {Node->GetVariableName()});
    Blueprint->Modify();
    Node->Modify();
    UObject* Outer = Old->GetOuter();
    const FName Name = Old->GetFName();
    UEngine::FCopyPropertiesForUnrelatedObjectsParams Copy;
    Copy.SourceObjectArchetype = Old->GetArchetype();
    Copy.bNotifyObjectReplacement = false;
    Require(Old->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_ForceNoResetLoaders | REN_NonTransactional),
        TEXT("Cannot detach the old component template"));
    UActorComponent* Replacement = NewObject<UActorComponent>(Outer, Class, Name, RF_ArchetypeObject | RF_Transactional | RF_Public);
    UEngine::CopyPropertiesForUnrelatedObjects(Old, Replacement, Copy);
    Node->ComponentClass = Class;
    Node->ComponentTemplate = Replacement;
    const TMap<UObject*, UObject*> Replacements{{Old, Replacement}};
    const auto ReplaceFlags = EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef;
    FArchiveReplaceObjectRef<UObject> BlueprintReferences(Blueprint, Replacements, ReplaceFlags);
    FArchiveReplaceObjectRef<UObject> GeneratedClassReferences(Blueprint->GeneratedClass, Replacements, ReplaceFlags);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    ReconstructMemberNodes(Blueprint, Nodes);
    return Node;
}

struct FMemberDependent
{
    UBlueprint* Blueprint;
    bool bChild;
    FMemberNodeSnapshot Nodes;
    TMap<const USCS_Node*, UClass*> Overrides;
};

static void CheckMemberDependents(const TArray<USCS_Node*>& Components,
    const TArray<FMemberDependent>& Dependents, const FJson& Response)
{
    TArray<TSharedPtr<FJsonValue>> Results;
    bool Passed = true;
    for (const FMemberDependent& Dependent : Dependents)
    {
        UBlueprint* Blueprint = Dependent.Blueprint;
        const FJson Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset"), Blueprint->GetOutermost()->GetName());
        Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
        bool Ok = false;
        try
        {
            for (const USCS_Node* Component : Components)
            {
                const UClass* Override = Dependent.Overrides.FindRef(Component);
                Require(!Override || Override == Component->ComponentClass,
                    TEXT("A child Blueprint has a component override of the old class: ") + Component->GetVariableName().ToString());
            }
            if (Dependent.bChild) { FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); }
            ReconstructMemberNodes(Blueprint, Dependent.Nodes);
            Ok = Compile(Blueprint, Result);
        }
        catch (const FFailure& Failure)
        { Result->SetStringField(TEXT("error"), Failure.Message); }
        Result->SetBoolField(TEXT("ok"), Ok);
        Passed &= Ok;
        Results.Add(JsonValue(Result));
    }
    Response->SetArrayField(TEXT("affected_blueprints"), Results);
    Require(Passed, TEXT("Member type change would break dependent Blueprints; see affected_blueprints"));
}
