static void ValidateLibraryOperation(const FJson& Op)
{
    const FString Kind = String(Op, TEXT("op"));
    Require(Kind == TEXT("add_function") || Kind == TEXT("edit_function") || Kind == TEXT("delete_function") ||
        Kind == TEXT("add_node") || Kind == TEXT("retarget_call") || Kind == TEXT("connect") || Kind == TEXT("disconnect") ||
        Kind == TEXT("set_pin_default") || Kind == TEXT("move_node") || Kind == TEXT("delete_node"),
        TEXT("Operation is not supported in function libraries: ") + Kind);
    if (Kind == TEXT("add_node"))
    {
        const FString NodeKind = String(Op, TEXT("kind"));
        Require(NodeKind == TEXT("call") || NodeKind == TEXT("branch") || NodeKind == TEXT("sequence") ||
            NodeKind == TEXT("reroute") || NodeKind == TEXT("cast") || NodeKind == TEXT("make_struct") ||
            NodeKind == TEXT("break_struct") || NodeKind == TEXT("select") || NodeKind == TEXT("foreach") || NodeKind == TEXT("macro"), TEXT("Node kind is not supported in function libraries: ") + NodeKind);
    }
}

// Library signature changes are checked against on-disk Blueprint callers.
struct FLibraryParameter
{
    FName Name;
    FEdGraphPinType Type;
    bool bOutput;

    bool operator==(const FLibraryParameter& Other) const
    {
        return Name == Other.Name && Type == Other.Type && bOutput == Other.bOutput;
    }
};

struct FLibrarySignature
{
    bool bPure;
    TArray<FLibraryParameter> Parameters;

    bool operator==(const FLibrarySignature& Other) const
    {
        return bPure == Other.bPure && Parameters == Other.Parameters;
    }
};

static const UFunction* FindLibraryFunction(const UBlueprint* Library, FName Name)
{
    return Library->GeneratedClass ? Library->GeneratedClass->FindFunctionByName(Name, EIncludeSuperFlag::ExcludeSuper) : nullptr;
}

static FLibrarySignature LibrarySignature(const UFunction* Function)
{
    FLibrarySignature Signature{Function->HasAnyFunctionFlags(FUNC_BlueprintPure), {}};
    for (TFieldIterator<FProperty> P(Function); P && P->HasAnyPropertyFlags(CPF_Parm); ++P)
    {
        FLibraryParameter Parameter{P->GetFName(), {}, IsFunctionOutput(*P)};
        GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(*P, Parameter.Type);
        Signature.Parameters.Add(MoveTemp(Parameter));
    }
    return Signature;
}

static TMap<FName, FLibrarySignature> CaptureLibrarySignatures(const UBlueprint* Library,
    const TArray<TSharedPtr<FJsonValue>>& Operations)
{
    TMap<FName, FLibrarySignature> Signatures;
    for (const auto& Value : Operations)
    {
        const FJson Op = AsObject(Value);
        const FString Kind = String(Op, TEXT("op"));
        if (Kind != TEXT("edit_function") && Kind != TEXT("delete_function")) { continue; }
        const FName Name(*String(Op, TEXT("name")));
        if (const UFunction* Function = FindLibraryFunction(Library, Name))
        { Signatures.Add(Name, LibrarySignature(Function)); }
    }
    return Signatures;
}

struct FLibraryCaller
{
    UBlueprint* Blueprint;
    TArray<FName> Functions;
};

static TArray<FLibraryCaller> FindLibraryCallers(UBlueprint* Library, const TMap<FName, FLibrarySignature>& Signatures)
{
    TArray<FLibraryCaller> Callers;
    for (UBlueprint* Blueprint : ReferencingBlueprints(Library))
    {
        FLibraryCaller Caller{Blueprint, {}};
        TArray<UK2Node_CallFunction*> Calls;
        FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Calls);
        for (const UK2Node_CallFunction* Call : Calls)
        {
            const FName Name = Call->FunctionReference.GetMemberName();
            const UClass* Owner = Call->FunctionReference.GetMemberParentClass();
            if (Signatures.Contains(Name) && Owner && Owner->ClassGeneratedBy == Library)
            { Caller.Functions.AddUnique(Name); }
        }
        if (!Caller.Functions.IsEmpty())
        {
            Caller.Functions.Sort(FNameLexicalLess());
            Callers.Add(MoveTemp(Caller));
        }
    }
    return Callers;
}

static void CheckLibraryCallers(const UBlueprint* Library, const TMap<FName, FLibrarySignature>& Before,
    const TArray<FLibraryCaller>& Callers, const FJson& Response)
{
    TMap<FName, bool> Changes;
    for (const auto& Pair : Before)
    {
        const UFunction* Function = FindLibraryFunction(Library, Pair.Key);
        if (!Function)
        {
            Changes.Add(Pair.Key, false);
            continue;
        }
        const FLibrarySignature After = LibrarySignature(Function);
        if (After == Pair.Value) { continue; }
        // Compilation can reconstruct stale nodes by dropping pins. Preserve the
        // existing external contract even if such a reconstruction compiles.
        bool Compatible = After.bPure == Pair.Value.bPure;
        for (const FLibraryParameter& Parameter : Pair.Value.Parameters)
        { Compatible &= After.Parameters.Contains(Parameter); }
        Changes.Add(Pair.Key, Compatible);
    }

    bool Passed = true;
    TArray<TSharedPtr<FJsonValue>> Results;
    for (const FLibraryCaller& Caller : Callers)
    {
        TArray<TSharedPtr<FJsonValue>> Functions;
        TArray<TSharedPtr<FJsonValue>> Incompatible;
        for (const FName Name : Caller.Functions)
        {
            if (const bool* Compatible = Changes.Find(Name))
            {
                Functions.Add(MakeShared<FJsonValueString>(Name.ToString()));
                if (!*Compatible) { Incompatible.Add(MakeShared<FJsonValueString>(Name.ToString())); }
            }
        }
        if (Functions.IsEmpty()) { continue; }
        const FJson Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset"), Caller.Blueprint->GetOutermost()->GetName());
        Result->SetStringField(TEXT("blueprint"), Caller.Blueprint->GetPathName());
        Result->SetArrayField(TEXT("functions"), Functions);
        Result->SetArrayField(TEXT("incompatible_functions"), Incompatible);
        const bool Compiled = Compile(Caller.Blueprint, Result);
        Result->SetBoolField(TEXT("compiled"), Compiled);
        Result->SetBoolField(TEXT("ok"), Compiled && Incompatible.IsEmpty());
        Passed &= Compiled && Incompatible.IsEmpty();
        Results.Add(JsonValue(Result));
    }
    Response->SetArrayField(TEXT("affected_callers"), Results);
    Require(Passed, TEXT("Function library change would break callers; see affected_callers"));
}
