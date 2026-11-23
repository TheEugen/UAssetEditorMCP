static UK2Node* SpawnAsyncNode(UBlueprint* Blueprint, UEdGraph* Graph, const FJson& Op, bool GameplayMessage)
{
    const FString Owner = GameplayMessage ? TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage")
                                          : String(Op, TEXT("class"));
    const FString Function = GameplayMessage ? TEXT("ListenForGameplayMessages") : String(Op, TEXT("function"));
    const UFunction* Factory = ResolveCallFunction(Blueprint, Owner, Function);
    Require(Factory->HasAnyFunctionFlags(FUNC_Static), TEXT("Expected a static async factory: ") + Function);
    const FString NodeClass = String(Op, TEXT("node_class"), FString(), false);
    const auto& Registry = FBlueprintActionDatabase::Get().GetAllActions();
    const auto* Actions = Registry.Find(FObjectKey(Factory->GetOwnerClass()));
    const UBlueprintFunctionNodeSpawner* Spawner = nullptr;
    if (Actions)
    {
        for (const UBlueprintNodeSpawner* Action : *Actions)
        {
            const auto* Candidate = Cast<UBlueprintFunctionNodeSpawner>(Action);
            UClass* Class = Candidate ? Candidate->NodeClass.Get() : nullptr;
            if (!Class || !Class->IsChildOf(UK2Node_BaseAsyncTask::StaticClass()) || Candidate->GetFunction() != Factory ||
                (!NodeClass.IsEmpty() && Class->GetPathName() != NodeClass)) { continue; }
            Require(!Spawner, TEXT("Multiple async node types are registered for the factory. Specify node_class: ") + Function);
            Spawner = Candidate;
        }
    }
    Require(Spawner != nullptr, TEXT("No async Blueprint node is registered for ") + Owner + TEXT(".") + Function);
    const auto* Template = Cast<UK2Node>(Spawner->NodeClass->GetDefaultObject());
    Require(Template && Template->IsCompatibleWithGraph(Graph), TEXT("Async node requires a compatible event graph"));
    const FVector2D Location(Integer(Op, TEXT("x")), Integer(Op, TEXT("y")));
    UK2Node* Node = Cast<UK2Node>(Spawner->Invoke(Graph, {}, Location));
    Require(Node != nullptr, TEXT("Could not spawn the registered async node"));
    if (!GameplayMessage) { return Node; }

    const FString ChannelName = String(Op, TEXT("channel"));
    const FGameplayTag Channel = FGameplayTag::RequestGameplayTag(FName(*ChannelName), false);
    Require(Channel.IsValid(), TEXT("Gameplay message channel is not a registered tag: ") + ChannelName);
    const FString TypePath = String(Op, TEXT("payload_type"));
    UScriptStruct* PayloadType = LoadObject<UScriptStruct>(nullptr, *TypePath);
    Require(PayloadType && UEdGraphSchema_K2::IsAllowableBlueprintVariableType(PayloadType),
        TEXT("Expected a Blueprint-visible gameplay message payload struct: ") + TypePath);
    UEdGraphPin* ChannelPin = Node->FindPin(TEXT("Channel"), EGPD_Input);
    UEdGraphPin* TypePin = Node->FindPin(TEXT("PayloadType"), EGPD_Input);
    UEdGraphPin* PayloadPin = Node->FindPin(TEXT("Payload"), EGPD_Output);
    Require(ChannelPin && TypePin && PayloadPin, TEXT("The registered node does not expose typed Gameplay Message Router pins"));
    const auto* Schema = CastChecked<UEdGraphSchema_K2>(Graph->GetSchema());
    const FString ChannelValue = FString::Printf(TEXT("(TagName=\"%s\")"), *Channel.ToString());
    Schema->TrySetDefaultValue(*ChannelPin, ChannelValue);
    Schema->TrySetDefaultObject(*TypePin, PayloadType);
    Require(ChannelPin->DefaultValue.Contains(Channel.ToString()) && TypePin->DefaultObject == PayloadType &&
        PayloadPin->PinType.PinSubCategoryObject == PayloadType,
        TEXT("The gameplay message node rejected its channel or payload type"));
    return Node;
}

static UK2Node* NewDelegateNode(UBlueprint* Blueprint, UEdGraph* Graph, const FString& Kind, const FJson& Op)
{
    if (Kind == TEXT("create_delegate"))
    { return NewObject<UK2Node_CreateDelegate>(Graph); }
    const FString Owner = String(Op, TEXT("class"), TEXT("self"), false);
    const bool Self = Owner == TEXT("self");
    UClass* Class = Self ? Blueprint->SkeletonGeneratedClass.Get() : LoadObject<UClass>(nullptr, *Owner);
    Require(Class != nullptr, TEXT("Delegate class not found: ") + Owner);
    const FString Name = String(Op, TEXT("delegate"));
    const auto* Property = FindFProperty<FMulticastDelegateProperty>(Class, FName(*Name));
    Require(Property && Property->HasAnyPropertyFlags(CPF_BlueprintAssignable),
        TEXT("Blueprint-assignable multicast delegate not found: ") + Name);
    UK2Node_BaseMCDelegate* Node = Kind == TEXT("bind_delegate")
        ? static_cast<UK2Node_BaseMCDelegate*>(NewObject<UK2Node_AddDelegate>(Graph))
        : NewObject<UK2Node_RemoveDelegate>(Graph);
    Node->SetFromProperty(Property, Self, Property->GetOwnerClass());
    return Node;
}
