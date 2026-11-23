static UK2Node* NewGraphNode(UBlueprint* BP, UEdGraph* Graph, const FString& Kind, const FJson& Op)
{
    if (Kind == TEXT("layered_blend_per_bone"))
    {
        UAnimBlueprint* Anim = Cast<UAnimBlueprint>(BP);
        Require(Anim && Graph->GetSchema()->IsA<UAnimationGraphSchema>(), TEXT("Layered Blend per Bone requires an animation pose graph"));
        auto* Blend = NewObject<UAnimGraphNode_LayeredBoneBlend>(Graph);
        const auto Layers = Array(Op, TEXT("layers"), true);
        Require(!Layers.IsEmpty(), TEXT("Specify at least one blend layer"));
        for (int32 Index = 0; Index < Layers.Num(); ++Index)
        {
            if (Index > 0) { Blend->Node.AddPose(); }
            FJson Layer = AsObject(Layers[Index]);
            double Weight = 1;
            Require(!Layer->HasField(TEXT("weight")) || (Layer->TryGetNumberField(TEXT("weight"), Weight) &&
                Weight >= 0 && Weight <= 1), TEXT("Blend weight must be between zero and one"));
            Blend->Node.BlendWeights[Index] = float(Weight);
            const auto Filters = Array(Layer, TEXT("branches"), true);
            Require(!Filters.IsEmpty(), TEXT("Each layer requires at least one bone branch"));
            for (const auto& Entry : Filters)
            {
                FJson Branch = AsObject(Entry);
                auto& Filter = Blend->Node.LayerSetup[Index].BranchFilters.AddDefaulted_GetRef();
                Filter.BoneName = FName(*String(Branch, TEXT("bone")));
                Require(!Filter.BoneName.IsNone() && (!Anim->TargetSkeleton ||
                    Anim->TargetSkeleton->GetReferenceSkeleton().FindBoneIndex(Filter.BoneName) != INDEX_NONE),
                    TEXT("Unknown branch bone: ") + Filter.BoneName.ToString());
                Filter.BlendDepth = Integer(Branch, TEXT("depth"));
            }
        }
        return Blend;
    }
    if (Kind == TEXT("cast"))
    {
        UClass* Target = LoadObject<UClass>(nullptr, *String(Op, TEXT("class")));
        Require(Target && UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Target), TEXT("Invalid cast target class"));
        auto* Node = NewObject<UK2Node_DynamicCast>(Graph);
        Node->TargetType = Target;
        return Node;
    }
    if (Kind == TEXT("make_struct") || Kind == TEXT("break_struct"))
    {
        auto* Struct = LoadObject<UScriptStruct>(nullptr, *String(Op, TEXT("struct")));
        Require(Struct && UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Struct), TEXT("Invalid Blueprint struct type"));
        if (Kind == TEXT("make_struct"))
        {
            Require(UK2Node_MakeStruct::CanBeMade(Struct), TEXT("Struct requires a native make function or has no editable fields"));
            auto* Node = NewObject<UK2Node_MakeStruct>(Graph);
            Node->StructType = Struct;
            return Node;
        }
        Require(!Struct->HasMetaData(FBlueprintMetadata::MD_NativeBreakFunction), TEXT("Struct requires its native break function"));
        auto* Node = NewObject<UK2Node_BreakStruct>(Graph);
        Node->StructType = Struct;
        return Node;
    }
    if (Kind == TEXT("select")) { return NewObject<UK2Node_Select>(Graph); }
    if (Kind == TEXT("foreach") || Kind == TEXT("macro"))
    {
        UBlueprint* Library = BP;
        if (Kind == TEXT("foreach"))
        { Library = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros")); }
        else if (Op->HasField(TEXT("library")))
        { Library = LoadObject<UBlueprint>(nullptr, *String(Op, TEXT("library"))); }
        Require(Library && (Library == BP || Library->BlueprintType == BPTYPE_MacroLibrary), TEXT("Invalid macro library"));
        Require(Library == BP || (BP->ParentClass && Library->ParentClass && BP->ParentClass->IsChildOf(Library->ParentClass)),
            TEXT("Macro library parent class is incompatible with this Blueprint"));
        const FString Name = Kind == TEXT("foreach") ? TEXT("ForEachLoop") : String(Op, TEXT("macro"));
        UEdGraph* Macro = nullptr;
        for (UEdGraph* Candidate : Library->MacroGraphs)
        { if (Candidate->GetFName() == FName(*Name)) { Macro = Candidate; break; } }
        Require(Macro && Macro != Graph, TEXT("Macro not found or recursive: ") + Name);
        auto* Node = NewObject<UK2Node_MacroInstance>(Graph);
        Node->SetMacroGraph(Macro);
        return Node;
    }
    if (Kind == TEXT("timeline"))
    {
        auto* Node = NewObject<UK2Node_Timeline>(Graph);
        Require(BP->BlueprintType == BPTYPE_Normal && BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()) &&
            Node->IsCompatibleWithGraph(Graph), TEXT("Timeline requires an Actor event graph"));
        const FString Name = String(Op, TEXT("name"));
        AvailableName(BP, Name);
        Node->TimelineName = FName(*Name);
        UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(BP, Node->TimelineName);
        Require(Template != nullptr, TEXT("Unable to create timeline"));
        Node->TimelineGuid = Template->TimelineGuid;
        double Length = Template->TimelineLength;
        Require(!Op->HasField(TEXT("length")) || (Op->TryGetNumberField(TEXT("length"), Length) &&
            Length > 0 && Length <= MAX_flt), TEXT("Expected positive timeline length in seconds"));
        Template->TimelineLength = Length;
        Template->bLoop = Boolean(Op, TEXT("loop"));
        Template->bAutoPlay = Boolean(Op, TEXT("autoplay"));
        for (const auto& TrackValue : Array(Op, TEXT("float_tracks")))
        {
            const FJson Track = AsObject(TrackValue);
            const FString TrackName = String(Track, TEXT("name"));
            ValidName(TrackName);
            Require(!Template->FloatTracks.ContainsByPredicate([&](const FTTFloatTrack& Existing) { return Existing.GetTrackName() == FName(*TrackName); }), TEXT("Duplicate timeline track name"));
            auto& Float = Template->FloatTracks.AddDefaulted_GetRef();
            Float.SetTrackName(FName(*TrackName), Template);
            Float.CurveFloat = NewObject<UCurveFloat>(Template, NAME_None, RF_Transactional);
            for (const auto& KeyValue : Array(Track, TEXT("keys"), true))
            {
                const FJson Key = AsObject(KeyValue);
                double Time = 0, Value = 0;
                Require(Key->TryGetNumberField(TEXT("time"), Time) && Time >= 0 && Time <= Length &&
                    Key->TryGetNumberField(TEXT("value"), Value) && FMath::Abs(Value) <= MAX_flt, TEXT("Invalid timeline key"));
                Float.CurveFloat->FloatCurve.UpdateOrAddKey(Time, Value);
            }
        }
        return Node;
    }
    throw FFailure{TEXT("Unsupported node kind: ") + Kind};
}
