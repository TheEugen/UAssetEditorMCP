#include "UAssetAgentCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectRedirector.h"

namespace UAssetAgent
{
using FJson = TSharedPtr<FJsonObject>;
struct FFailure { FString Message; };

static void Require(bool Condition, const FString& Message)
{
    if (!Condition) { throw FFailure{Message}; }
}

static FString String(const FJson& J, const TCHAR* Key, const FString& Default = FString(), bool Required = true)
{
    FString Value;
    if (!J->HasField(Key) && !Required) { return Default; }
    Require(J->TryGetStringField(Key, Value), FString::Printf(TEXT("'%s' must be a string"), Key));
    Require(!Required || !Value.IsEmpty(), FString::Printf(TEXT("'%s' must not be empty"), Key));
    return Value;
}

static bool Boolean(const FJson& J, const TCHAR* Key, bool Default = false)
{
    if (!J->HasField(Key)) { return Default; }
    bool Value = false;
    Require(J->TryGetBoolField(Key, Value), FString::Printf(TEXT("'%s' must be a boolean"), Key));
    return Value;
}

static FString ValueString(const FJson& J, const TCHAR* Key)
{
    FString Value;
    Require(J->TryGetStringField(Key, Value), FString::Printf(TEXT("'%s' must be a string (empty is allowed)"), Key));
    return Value;
}

static int32 Integer(const FJson& J, const TCHAR* Key, int32 Default = 0)
{
    if (!J->HasField(Key)) { return Default; }
    double Value = 0;
    Require(J->TryGetNumberField(Key, Value) && FMath::IsFinite(Value) &&
        Value >= MIN_int32 && Value <= MAX_int32 && FMath::FloorToDouble(Value) == Value,
        FString::Printf(TEXT("'%s' must be a 32-bit integer"), Key));
    return static_cast<int32>(Value);
}

static FJson Object(const FJson& J, const TCHAR* Key)
{
    const FJson* Value = nullptr;
    Require(J->TryGetObjectField(Key, Value), FString::Printf(TEXT("'%s' must be an object"), Key));
    return *Value;
}

static TArray<TSharedPtr<FJsonValue>> Array(const FJson& J, const TCHAR* Key, bool Required = false)
{
    if (!Required && !J->HasField(Key)) { return {}; }
    const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
    Require(J->TryGetArrayField(Key, Value), FString::Printf(TEXT("'%s' must be an array"), Key));
    return *Value;
}

static FJson AsObject(const TSharedPtr<FJsonValue>& Value)
{
    Require(Value.IsValid() && Value->Type == EJson::Object, TEXT("Array entries must be objects"));
    return Value->AsObject();
}

static TSharedPtr<FJsonValue> JsonValue(const FJson& J) { return MakeShared<FJsonValueObject>(J); }
static FString Guid(const FGuid& Value) { return Value.ToString(EGuidFormats::DigitsWithHyphens); }

static FJson DescribeDefaults(UObject* Object)
{
    FJson Defaults = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
    {
        if (!It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) || It->HasAnyPropertyFlags(CPF_Transient)) { continue; }
        FString Value;
        It->ExportText_InContainer(0, Value, Object, Object, Object, PPF_None);
        Defaults->SetStringField(It->GetName(), Value);
    }
    return Defaults;
}

// SCS components live on templates, while native default subobjects belong to
// the generated CDO. Never return a parent-owned object as an editable target.
struct FComponentTemplate
{
    FString Id;
    FString Name;
    FString Source;
    UActorComponent* Template = nullptr;
    USCS_Node* Node = nullptr;
    bool Inherited = false;
};

static TArray<FComponentTemplate> ComponentTemplates(UBlueprint* BP)
{
    TArray<FComponentTemplate> Result;
    UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
    TSet<UBlueprint*> Visited;
    for (UBlueprint* Owner = BP; Owner && !Visited.Contains(Owner);)
    {
        Visited.Add(Owner);
        if (Owner->SimpleConstructionScript)
        {
            for (USCS_Node* Node : Owner->SimpleConstructionScript->GetAllNodes())
            {
                if (!Node || !Node->ComponentTemplate) { continue; }
                FComponentTemplate Entry;
                Entry.Id = Owner->GetPathName() + TEXT(":") + Guid(Node->VariableGuid);
                Entry.Name = Node->GetVariableName().ToString();
                Entry.Source = Owner == BP ? TEXT("local") : TEXT("inherited");
                Entry.Node = Node;
                Entry.Inherited = Owner != BP;
                Entry.Template = Generated ? Node->GetActualComponentTemplate(Generated) : Node->ComponentTemplate.Get();
                Result.Add(Entry);
            }
        }
        Owner = Owner->ParentClass ? Cast<UBlueprint>(Owner->ParentClass->ClassGeneratedBy) : nullptr;
    }
    if (Generated)
    {
        if (AActor* CDO = Cast<AActor>(Generated->GetDefaultObject()))
        {
            TArray<UActorComponent*> Components;
            CDO->GetComponents(Components);
            for (UActorComponent* Component : Components)
            {
                if (!Component || Component->CreationMethod != EComponentCreationMethod::Native) { continue; }
                FComponentTemplate Entry;
                Entry.Id = TEXT("native:") + Component->GetName();
                Entry.Name = Component->GetName();
                Entry.Source = TEXT("native");
                Entry.Template = Component;
                Entry.Inherited = true;
                Result.Add(Entry);
            }
        }
    }
    Result.Sort([](const FComponentTemplate& A, const FComponentTemplate& B) { return A.Id < B.Id; });
    return Result;
}

static TArray<TSharedPtr<FJsonValue>> DescribeComponents(UBlueprint* BP, bool IncludeDefaults)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FComponentTemplate& Component : ComponentTemplates(BP))
    {
        FJson Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), Component.Id);
        Item->SetStringField(TEXT("name"), Component.Name);
        Item->SetStringField(TEXT("source"), Component.Source);
        Item->SetStringField(TEXT("class"), Component.Template->GetClass()->GetPathName());
        Item->SetBoolField(TEXT("inherited"), Component.Inherited);
        Item->SetBoolField(TEXT("editable"), !Component.Inherited || Component.Template->bEditableWhenInherited);
        if (IncludeDefaults) { Item->SetObjectField(TEXT("defaults"), DescribeDefaults(Component.Template)); }
        Values.Add(JsonValue(Item));
    }
    return Values;
}

static void SetPropertyDefault(UObject* Target, const FString& Name, const FString& Value)
{
    FProperty* Property = FindFProperty<FProperty>(Target->GetClass(), FName(*Name));
    Require(Property && Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) &&
        !Property->HasAnyPropertyFlags(CPF_Transient | CPF_DisableEditOnTemplate | CPF_EditConst), TEXT("Editable default not found: ") + Name);
    void* Temp = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
    Property->InitializeValue(Temp);
    Property->CopyCompleteValue(Temp, Property->ContainerPtrToValuePtr<void>(Target));
    const TCHAR* End = Property->ImportText_Direct(*Value, Temp, Target, PPF_None);
    bool Valid = End && FString(End).TrimStartAndEnd().IsEmpty();
    if (Valid)
    {
        Target->Modify();
        Target->PreEditChange(Property);
        Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Target), Temp);
    }
    Property->DestroyValue(Temp);
    FMemory::Free(Temp);
    Require(Valid, TEXT("Invalid property value: ") + Name);
    FPropertyChangedEvent Changed(Property, EPropertyChangeType::ValueSet);
    Target->PostEditChangeProperty(Changed);
}

static FString Revision(const FString& Filename)
{
    TArray<uint8> Bytes;
    Require(FFileHelper::LoadFileToArray(Bytes, *Filename), TEXT("Cannot read asset for revision: ") + Filename);
    uint8 Hash[20];
    FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash);
    return BytesToHex(Hash, 20).ToLower();
}

static FString PackagePath(const FJson& Request)
{
    FString Path = String(Request, TEXT("asset"));
    Require(Path.StartsWith(TEXT("/Game/")) && !Path.Contains(TEXT(".")) &&
        FPackageName::IsValidLongPackageName(Path),
        TEXT("asset must be a package path under /Game, e.g. /Game/Blueprints/BP_Player (no .uasset suffix)"));
    return Path;
}

static FString AssetFilename(const FString& Package)
{
    return FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension()));
}

static void ValidName(const FString& Name)
{
    Require(!Name.IsEmpty() && Name.Len() <= 128 && (FChar::IsAlpha(Name[0]) || Name[0] == '_'), TEXT("Name must be an identifier"));
    for (TCHAR C : Name) { Require(FChar::IsAlnum(C) || C == '_', TEXT("Name must contain only letters, numbers and underscores")); }
    Require(!FName(*Name).IsNone(), TEXT("None is not a valid identifier"));
}

static void AvailableName(UBlueprint* BP, const FString& Name)
{
    ValidName(Name);
    const FName N(*Name);
    Require(!FindObject<UObject>(BP, *Name), TEXT("Name already exists: ") + Name);
    for (const FBPVariableDescription& V : BP->NewVariables) { Require(V.VarName != N, TEXT("Variable already exists: ") + Name); }
    UClass* Class = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass : BP->ParentClass;
    Require(!Class || (!FindFProperty<FProperty>(Class, N) && !Class->FindFunctionByName(N)), TEXT("Member already exists: ") + Name);
    TArray<UEdGraph*> Graphs;
    BP->GetAllGraphs(Graphs);
    for (UEdGraph* G : Graphs)
    {
        for (UEdGraphNode* Node : G->Nodes)
        {
            if (UK2Node_CustomEvent* E = Cast<UK2Node_CustomEvent>(Node))
            { Require(E->CustomFunctionName != N, TEXT("Event already exists: ") + Name); }
        }
    }
}

static FEdGraphPinType PinType(const FJson& J)
{
    FEdGraphPinType Type;
    const FString Name = String(J, TEXT("category"));
    if (Name == TEXT("float") || Name == TEXT("double"))
    {
        Type.PinCategory = UEdGraphSchema_K2::PC_Real;
        Type.PinSubCategory = Name == TEXT("float") ? UEdGraphSchema_K2::PC_Float : UEdGraphSchema_K2::PC_Double;
    }
    else if (Name == TEXT("bool") || Name == TEXT("int") || Name == TEXT("int64") || Name == TEXT("byte") ||
        Name == TEXT("string") || Name == TEXT("name") || Name == TEXT("text"))
    { Type.PinCategory = FName(*Name); }
    else if (Name == TEXT("object") || Name == TEXT("class"))
    {
        UClass* Class = LoadObject<UClass>(nullptr, *String(J, TEXT("path")));
        Require(Class && UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Class), TEXT("Invalid Blueprint class type"));
        Type.PinCategory = FName(*Name);
        Type.PinSubCategoryObject = Class;
    }
    else if (Name == TEXT("struct"))
    {
        UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *String(J, TEXT("path")));
        Require(Struct && UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Struct), TEXT("Invalid Blueprint struct type"));
        Type.PinCategory = UEdGraphSchema_K2::PC_Struct;
        Type.PinSubCategoryObject = Struct;
    }
    else if (Name == TEXT("enum"))
    {
        UEnum* Enum = LoadObject<UEnum>(nullptr, *String(J, TEXT("path")));
        Require(Enum && UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Enum), TEXT("Invalid Blueprint enum type"));
        Type.PinCategory = UEdGraphSchema_K2::PC_Byte;
        Type.PinSubCategoryObject = Enum;
    }
    else { throw FFailure{TEXT("Unsupported type category: ") + Name}; }
    const FString Container = String(J, TEXT("container"), TEXT("none"), false);
    Require(Container == TEXT("none") || Container == TEXT("array") || Container == TEXT("set"), TEXT("container must be none, array or set"));
    Type.ContainerType = Container == TEXT("array") ? EPinContainerType::Array :
        Container == TEXT("set") ? EPinContainerType::Set : EPinContainerType::None;
    return Type;
}

static FJson DescribeType(const FEdGraphPinType& Type)
{
    FJson J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("category"), Type.PinCategory.ToString());
    J->SetStringField(TEXT("subcategory"), Type.PinSubCategory.ToString());
    if (Type.PinSubCategoryObject.IsValid()) { J->SetStringField(TEXT("path"), Type.PinSubCategoryObject->GetPathName()); }
    J->SetStringField(TEXT("container"), Type.IsArray() ? TEXT("array") : Type.IsSet() ? TEXT("set") : Type.IsMap() ? TEXT("map") : TEXT("none"));
    J->SetBoolField(TEXT("reference"), Type.bIsReference);
    J->SetBoolField(TEXT("const"), Type.bIsConst);
    return J;
}

static FJson DescribeNode(UEdGraphNode* Node)
{
    FJson J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("id"), Guid(Node->NodeGuid));
    J->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
    J->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    J->SetNumberField(TEXT("x"), Node->NodePosX);
    J->SetNumberField(TEXT("y"), Node->NodePosY);
    J->SetStringField(TEXT("comment"), Node->NodeComment);
    if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
    {
        J->SetStringField(TEXT("function"), Call->FunctionReference.GetMemberName().ToString());
    }
    if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
    {
        J->SetStringField(TEXT("event"), Event->bOverrideFunction ? Event->EventReference.GetMemberName().ToString() : Event->CustomFunctionName.ToString());
        J->SetStringField(TEXT("replication"), (Event->FunctionFlags & FUNC_NetServer) ? TEXT("server") :
            (Event->FunctionFlags & FUNC_NetClient) ? TEXT("client") : (Event->FunctionFlags & FUNC_NetMulticast) ? TEXT("multicast") : TEXT("none"));
        J->SetBoolField(TEXT("reliable"), (Event->FunctionFlags & FUNC_NetReliable) != 0);
    }
    TArray<TSharedPtr<FJsonValue>> Pins;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin) { continue; }
        FJson P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("id"), Guid(Pin->PinId));
        P->SetStringField(TEXT("name"), Pin->PinName.ToString());
        P->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        P->SetObjectField(TEXT("type"), DescribeType(Pin->PinType));
        P->SetStringField(TEXT("default"), Pin->GetDefaultAsString());
        P->SetBoolField(TEXT("hidden"), Pin->bHidden);
        TArray<TSharedPtr<FJsonValue>> Links;
        for (UEdGraphPin* Linked : Pin->LinkedTo)
        {
            FJson L = MakeShared<FJsonObject>();
            L->SetStringField(TEXT("node"), Guid(Linked->GetOwningNode()->NodeGuid));
            L->SetStringField(TEXT("pin"), Linked->PinName.ToString());
            L->SetStringField(TEXT("pin_id"), Guid(Linked->PinId));
            Links.Add(JsonValue(L));
        }
        P->SetArrayField(TEXT("links"), Links);
        Pins.Add(JsonValue(P));
    }
    J->SetArrayField(TEXT("pins"), Pins);
    return J;
}

static FJson DescribeBlueprint(UBlueprint* BP, const FJson& Request)
{
    const FString Mode = String(Request, TEXT("response_mode"), TEXT("full"), false);
    const FString Selected = Mode == TEXT("graph") ? String(Request, TEXT("graph")) : FString();
    FJson J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("asset"), BP->GetOutermost()->GetName());
    J->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetPathName() : FString());
    if (Mode != TEXT("graph"))
    {
        TArray<TSharedPtr<FJsonValue>> Variables;
        for (const FBPVariableDescription& V : BP->NewVariables)
        {
            FJson Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), V.VarName.ToString());
            Item->SetObjectField(TEXT("type"), DescribeType(V.VarType));
            Item->SetStringField(TEXT("default"), V.DefaultValue);
            Item->SetBoolField(TEXT("replicated"), (V.PropertyFlags & CPF_Net) != 0);
            Item->SetStringField(TEXT("rep_notify"), V.RepNotifyFunc.ToString());
            Variables.Add(JsonValue(Item));
        }
        J->SetArrayField(TEXT("variables"), Variables);
    }
    TArray<TSharedPtr<FJsonValue>> GraphValues;
    TArray<UEdGraph*> Graphs;
    BP->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (Mode == TEXT("graph") && Graph->GetName() != Selected && Graph->GetPathName() != Selected) { continue; }
        FJson G = MakeShared<FJsonObject>();
        G->SetStringField(TEXT("name"), Graph->GetName());
        G->SetStringField(TEXT("path"), Graph->GetPathName());
        TArray<TSharedPtr<FJsonValue>> Nodes;
        TArray<TSharedPtr<FJsonValue>> Events;
        int32 NodeCount = 0;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) { continue; }
            ++NodeCount;
            if (Mode != TEXT("summary")) { Nodes.Add(JsonValue(DescribeNode(Node))); }
            else if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
            {
                FJson E = MakeShared<FJsonObject>();
                E->SetStringField(TEXT("id"), Guid(Event->NodeGuid));
                E->SetStringField(TEXT("event"), Event->bOverrideFunction ? Event->EventReference.GetMemberName().ToString() : Event->CustomFunctionName.ToString());
                E->SetStringField(TEXT("replication"), (Event->FunctionFlags & FUNC_NetServer) ? TEXT("server") :
                    (Event->FunctionFlags & FUNC_NetClient) ? TEXT("client") : (Event->FunctionFlags & FUNC_NetMulticast) ? TEXT("multicast") : TEXT("none"));
                E->SetBoolField(TEXT("reliable"), (Event->FunctionFlags & FUNC_NetReliable) != 0);
                Events.Add(JsonValue(E));
            }
        }
        G->SetNumberField(TEXT("node_count"), NodeCount);
        if (Mode == TEXT("summary")) { G->SetArrayField(TEXT("events"), Events); }
        else { G->SetArrayField(TEXT("nodes"), Nodes); }
        GraphValues.Add(JsonValue(G));
    }
    if (Mode == TEXT("graph"))
    {
        Require(GraphValues.Num() != 0, TEXT("Graph not found: ") + Selected);
        Require(GraphValues.Num() == 1, TEXT("Ambiguous graph name; use its object path: ") + Selected);
    }
    J->SetArrayField(TEXT("graphs"), GraphValues);
    if (Mode != TEXT("graph")) { J->SetArrayField(TEXT("components"), DescribeComponents(BP, Mode == TEXT("full"))); }
    if (Mode != TEXT("full")) { return J; }
    FJson Defaults = MakeShared<FJsonObject>();
    if (BP->GeneratedClass)
    {
        UObject* CDO = BP->GeneratedClass->GetDefaultObject();
        for (TFieldIterator<FProperty> It(BP->GeneratedClass); It; ++It)
        {
            if (!It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) { continue; }
            FString Value;
            It->ExportText_InContainer(0, Value, CDO, CDO, CDO, PPF_None);
            Defaults->SetStringField(It->GetName(), Value);
        }
    }
    J->SetObjectField(TEXT("defaults"), Defaults);
    return J;
}

static bool Compile(UBlueprint* BP, const FJson& Response, EBlueprintCompileOptions ExtraOptions = EBlueprintCompileOptions::None)
{
    FCompilerResultsLog Log;
    Log.bSilentMode = true;
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection | EBlueprintCompileOptions::SkipSave | ExtraOptions, &Log);
    TArray<TSharedPtr<FJsonValue>> Messages;
    for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
    {
        FJson M = MakeShared<FJsonObject>();
        M->SetStringField(TEXT("severity"), Message->GetSeverity() == EMessageSeverity::Error ? TEXT("error") :
            Message->GetSeverity() == EMessageSeverity::Warning ? TEXT("warning") : TEXT("info"));
        M->SetStringField(TEXT("message"), Message->ToText().ToString());
        Messages.Add(JsonValue(M));
    }
    // Preserve diagnostics from intermediate compiles too.
    TArray<TSharedPtr<FJsonValue>> All = Array(Response, TEXT("diagnostics"));
    All.Append(Messages);
    Response->SetArrayField(TEXT("diagnostics"), All);
    return Log.NumErrors == 0 && BP->Status != BS_Error;
}

class FEditor
{
public:
    UBlueprint* BP;
    FJson Response;
    TMap<FString, FGuid> Aliases;
    EBlueprintCompileOptions CompileOptions = EBlueprintCompileOptions::None;
    explicit FEditor(UBlueprint* InBP, const FJson& InResponse) : BP(InBP), Response(InResponse) {}

    bool CompileCurrent() { return Compile(BP, Response, CompileOptions); }

    UEdGraph* Graph(const FJson& Op)
    {
        const FString Name = String(Op, TEXT("graph"), TEXT("EventGraph"), false);
        TArray<UEdGraph*> Graphs;
        BP->GetAllGraphs(Graphs);
        UEdGraph* Found = nullptr;
        for (UEdGraph* G : Graphs)
        {
            if (G->GetName() == Name || G->GetPathName() == Name)
            {
                Require(!Found, TEXT("Ambiguous graph name: ") + Name);
                Found = G;
            }
        }
        Require(Found && Found->GetSchema()->IsA<UEdGraphSchema_K2>(), TEXT("K2 graph not found: ") + Name);
        return Found;
    }

    UEdGraphNode* Node(UEdGraph* G, const FString& Id)
    {
        FGuid Wanted;
        if (const FGuid* Alias = Aliases.Find(Id)) { Wanted = *Alias; }
        else { Require(FGuid::Parse(Id, Wanted), TEXT("Node must be an existing GUID or a batch alias: ") + Id); }
        for (UEdGraphNode* N : G->Nodes) { if (N && N->NodeGuid == Wanted) { return N; } }
        throw FFailure{TEXT("Node not found in graph: ") + Id};
    }

    UEdGraphPin* Pin(UEdGraph* G, const FJson& Ref, EEdGraphPinDirection Direction)
    {
        UEdGraphNode* N = Node(G, String(Ref, TEXT("node")));
        FString Name = String(Ref, TEXT("pin"));
        FGuid Id;
        bool ById = FGuid::Parse(Name, Id);
        UEdGraphPin* Found = nullptr;
        for (UEdGraphPin* P : N->Pins)
        {
            if (P && P->Direction == Direction && (ById ? P->PinId == Id : P->PinName == FName(*Name)))
            {
                Require(!Found, TEXT("Ambiguous pin name; use pin GUID"));
                Found = P;
            }
        }
        Require(Found != nullptr, TEXT("Pin not found: ") + Name);
        return Found;
    }

    void Remember(const FString& Alias, UEdGraphNode* N)
    {
        Require(!Aliases.Contains(Alias), TEXT("Duplicate batch alias: ") + Alias);
        Aliases.Add(Alias, N->NodeGuid);
    }

    void Parameters(UK2Node_EditablePinBase* N, const FJson& Op, const TCHAR* Key, EEdGraphPinDirection Direction)
    {
        for (const TSharedPtr<FJsonValue>& Value : Array(Op, Key))
        {
            FJson P = AsObject(Value);
            FString Name = String(P, TEXT("name"));
            ValidName(Name);
            Require(!N->FindPin(FName(*Name)), TEXT("Parameter name already exists: ") + Name);
            FEdGraphPinType Type = PinType(Object(P, TEXT("type")));
            FText Error;
            Require(N->CanCreateUserDefinedPin(Type, Direction, Error), TEXT("Cannot create parameter: ") + Error.ToString());
            Require(N->CreateUserDefinedPin(FName(*Name), Type, Direction, false) != nullptr, TEXT("Cannot create parameter"));
        }
    }

    UEdGraph* NewFunction(const FString& Name)
    {
        AvailableName(BP, Name);
        UEdGraph* G = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, G, true, nullptr);
        return G;
    }

    void EditEventParameters(UK2Node_CustomEvent* Event, const FJson& Op)
    {
        TArray<TSharedPtr<FUserPinInfo>> Parameters;
        TSet<FName> Names;
        for (const TSharedPtr<FJsonValue>& Value : Array(Op, TEXT("inputs"), true))
        {
            FJson P = AsObject(Value);
            const FString Name = String(P, TEXT("name"));
            ValidName(Name);
            FName PinName(*Name);
            Require(!Names.Contains(PinName), TEXT("Duplicate parameter name: ") + Name);
            Names.Add(PinName);
            UEdGraphPin* Existing = Event->FindPin(PinName);
            Require(!Existing || Event->UserDefinedPinExists(PinName), TEXT("Parameter conflicts with a built-in pin: ") + Name);
            TSharedPtr<FUserPinInfo> Parameter = MakeShared<FUserPinInfo>();
            Parameter->PinName = PinName;
            Parameter->PinType = PinType(Object(P, TEXT("type")));
            Parameter->DesiredPinDirection = EGPD_Output;
            FText Error;
            Require(Event->CanCreateUserDefinedPin(Parameter->PinType, EGPD_Output, Error), TEXT("Invalid event parameter: ") + Error.ToString());
            for (const TSharedPtr<FUserPinInfo>& Old : Event->UserDefinedPins)
            {
                if (Old->PinName == PinName && Old->PinType == Parameter->PinType) { Parameter->PinDefaultValue = Old->PinDefaultValue; }
            }
            Parameters.Add(Parameter);
        }
        TArray<UK2Node*> Nodes;
        Nodes.Add(Event);
        TArray<UEdGraph*> Graphs;
        BP->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
                if (!Call || Call->FunctionReference.GetMemberName() != Event->CustomFunctionName) { continue; }
                UClass* Owner = Call->FunctionReference.GetMemberParentClass();
                if (Call->FunctionReference.IsSelfContext() || (Owner && Owner->ClassGeneratedBy == BP)) { Nodes.Add(Call); }
            }
        }
        // Check both the definition's output pins and this Blueprint's call-site
        // input pins before reconstruction can discard any user wiring.
        TSet<FName> Changed;
        for (const TSharedPtr<FUserPinInfo>& Old : Event->UserDefinedPins)
        {
            bool Unchanged = false;
            for (const TSharedPtr<FUserPinInfo>& New : Parameters)
            { if (Old->PinName == New->PinName && Old->PinType == New->PinType) { Unchanged = true; break; } }
            if (!Unchanged) { Changed.Add(Old->PinName); }
        }
        const bool AllowBreakLinks = Boolean(Op, TEXT("allow_break_links"));
        for (UK2Node* Node : Nodes)
        {
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Changed.Contains(Pin->PinName))
                { Require(AllowBreakLinks || Pin->LinkedTo.IsEmpty(), TEXT("Parameter change would break links; disconnect them first or set allow_break_links:true")); }
            }
        }
        for (UK2Node* Node : Nodes)
        {
            for (UEdGraphPin* Pin : Node->Pins)
            { if (Pin && Changed.Contains(Pin->PinName)) { Pin->BreakAllPinLinks(); } }
        }
        Event->UserDefinedPins = MoveTemp(Parameters);
        {
            const bool Previous = Event->bDisableOrphanPinSaving;
            Event->bDisableOrphanPinSaving = true;
            Event->ReconstructNode();
            Event->bDisableOrphanPinSaving = Previous;
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        for (int32 I = 1; I < Nodes.Num(); ++I)
        {
            const bool Previous = Nodes[I]->bDisableOrphanPinSaving;
            Nodes[I]->bDisableOrphanPinSaving = true;
            Nodes[I]->ReconstructNode();
            Nodes[I]->bDisableOrphanPinSaving = Previous;
        }
    }

    void Apply(const FJson& Op)
    {
        FString Kind = String(Op, TEXT("op"));
        if (Kind == TEXT("reparent"))
        {
            UClass* Parent = LoadObject<UClass>(nullptr, *String(Op, TEXT("parent_class")));
            Require(Parent && FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent) &&
                !Parent->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Interface), TEXT("Invalid Blueprint parent class"));
            Require(Parent->ClassGeneratedBy != BP && (!BP->GeneratedClass || !Parent->IsChildOf(BP->GeneratedClass)) &&
                (!BP->SkeletonGeneratedClass || !Parent->IsChildOf(BP->SkeletonGeneratedClass)), TEXT("Re-parenting would create an inheritance cycle"));
            Require(BP->ParentClass &&
                BP->ParentClass->IsChildOf(AActor::StaticClass()) == Parent->IsChildOf(AActor::StaticClass()) &&
                BP->ParentClass->IsChildOf(UActorComponent::StaticClass()) == Parent->IsChildOf(UActorComponent::StaticClass()),
                TEXT("Re-parenting must preserve the Actor, ActorComponent or UObject family"));
            if (Parent == BP->ParentClass) { return; }
            for (const FBPVariableDescription& Variable : BP->NewVariables)
            {
                Require(!FindFProperty<FProperty>(Parent, Variable.VarName) && !Parent->FindFunctionByName(Variable.VarName),
                    TEXT("New parent has a member conflicting with local variable: ") + Variable.VarName.ToString());
            }
            BP->Modify();
            BP->ParentClass = Parent;
            if (UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass))
            { Generated->PrepareToConformSparseClassData(Parent->GetSparseClassDataStruct()); }
            if (BP->SimpleConstructionScript)
            {
                BP->SimpleConstructionScript->FixupRootNodeParentReferences();
                BP->SimpleConstructionScript->ValidateSceneRootNodes();
            }
            CompileOptions |= EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing | EBlueprintCompileOptions::SkipNewVariableDefaultsDetection;
            FBlueprintEditorUtils::RefreshAllNodes(BP);
            FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
            return;
        }
        if (Kind == TEXT("delete_variable"))
        {
            const FName Name(*String(Op, TEXT("name")));
            Require(FBlueprintEditorUtils::FindNewVariableIndex(BP, Name) != INDEX_NONE, TEXT("Local member variable not found (inherited variables cannot be deleted)"));
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().SearchAllAssets(true);
            Require(!FBlueprintEditorUtils::IsVariableUsed(BP, Name), TEXT("Variable is referenced; remove its uses before deleting: ") + Name.ToString());
            FBlueprintEditorUtils::RemoveMemberVariable(BP, Name);
            return;
        }
        if (Kind == TEXT("set_component_default"))
        {
            Require(CompileCurrent(), TEXT("Compile failed before setting component default"));
            const FString Selector = String(Op, TEXT("component"));
            TArray<FComponentTemplate> Matches;
            for (const FComponentTemplate& Candidate : ComponentTemplates(BP))
            { if (Candidate.Id == Selector || Candidate.Name == Selector) { Matches.Add(Candidate); } }
            Require(Matches.Num() == 1, TEXT("Component not found or ambiguous; use an id from components inspection: ") + Selector);
            FComponentTemplate& Component = Matches[0];
            Require(!Component.Inherited || Component.Template->bEditableWhenInherited, TEXT("Component is not editable when inherited"));
            UActorComponent* Target = Component.Template;
            if (Component.Node && Component.Inherited)
            {
                UInheritableComponentHandler* Handler = BP->GetInheritableComponentHandler(true);
                Require(Handler != nullptr, TEXT("Inherited component overrides are unavailable"));
                const FComponentKey Key(Component.Node);
                Target = Handler->GetOverridenComponentTemplate(Key);
                if (!Target) { Target = Handler->CreateOverridenComponentTemplate(Key); }
            }
            Require(Target && Target->GetOutermost() == BP->GetOutermost(), TEXT("Cannot edit a parent-owned component template"));
            SetPropertyDefault(Target, String(Op, TEXT("property")), ValueString(Op, TEXT("value")));
            FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
            return;
        }
        if (Kind == TEXT("add_variable"))
        {
            FString Name = String(Op, TEXT("name"));
            AvailableName(BP, Name);
            Require(FBlueprintEditorUtils::AddMemberVariable(BP, FName(*Name), PinType(Object(Op, TEXT("type")))), TEXT("Cannot add variable"));
            FBPVariableDescription& V = BP->NewVariables.Last();
            V.PropertyFlags |= CPF_BlueprintVisible;
            if (Boolean(Op, TEXT("instance_editable"))) { V.PropertyFlags &= ~CPF_DisableEditOnInstance; }
            else { V.PropertyFlags |= CPF_DisableEditOnInstance; }
            if (Boolean(Op, TEXT("replicated"))) { V.PropertyFlags |= CPF_Net; }
            FString Notify = String(Op, TEXT("rep_notify"), FString(), false);
            if (!Notify.IsEmpty())
            {
                NewFunction(Notify);
                V.PropertyFlags |= CPF_Net | CPF_RepNotify;
                V.RepNotifyFunc = FName(*Notify);
            }
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
            if (Op->HasField(TEXT("default")))
            {
                Require(CompileCurrent(), TEXT("Compile failed after adding variable"));
                SetDefault(Name, String(Op, TEXT("default"), FString(), false));
            }
            return;
        }
        if (Kind == TEXT("set_default"))
        {
            Require(CompileCurrent(), TEXT("Compile failed before setting default"));
            SetDefault(String(Op, TEXT("property")), ValueString(Op, TEXT("value")));
            return;
        }
        if (Kind == TEXT("add_function"))
        {
            UEdGraph* G = NewFunction(String(Op, TEXT("name")));
            TArray<UK2Node_FunctionEntry*> Entries;
            G->GetNodesOfClass(Entries);
            Require(Entries.Num() == 1, TEXT("Function entry not found"));
            Parameters(Entries[0], Op, TEXT("inputs"), EGPD_Output);
            if (Op->HasField(TEXT("entry_id"))) { Remember(String(Op, TEXT("entry_id")), Entries[0]); }
            if (Array(Op, TEXT("outputs")).Num() > 0)
            {
                FGraphNodeCreator<UK2Node_FunctionResult> Creator(*G);
                UK2Node_FunctionResult* Result = Creator.CreateNode();
                Result->FunctionReference = Entries[0]->FunctionReference;
                Creator.Finalize();
                Result->NodePosX = 400;
                Parameters(Result, Op, TEXT("outputs"), EGPD_Input);
                if (Op->HasField(TEXT("result_id"))) { Remember(String(Op, TEXT("result_id")), Result); }
                Require(G->GetSchema()->TryCreateConnection(Entries[0]->FindPin(UEdGraphSchema_K2::PN_Then), Result->FindPin(UEdGraphSchema_K2::PN_Execute)), TEXT("Cannot connect function entry to result"));
            }
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
            return;
        }
        UEdGraph* G = Graph(Op);
        const UEdGraphSchema_K2* Schema = CastChecked<UEdGraphSchema_K2>(G->GetSchema());
        if (Kind == TEXT("edit_custom_event"))
        {
            UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node(G, String(Op, TEXT("node"))));
            Require(Event && Event->IsEditable() && !Event->bOverrideFunction &&
                (!BP->ParentClass || !BP->ParentClass->FindFunctionByName(Event->CustomFunctionName)),
                TEXT("Only locally declared custom events can be edited"));
            Require(Op->HasField(TEXT("inputs")) || Op->HasField(TEXT("replication")) || Op->HasField(TEXT("reliable")), TEXT("Specify inputs, replication or reliable"));
            if (Op->HasField(TEXT("replication")))
            {
                const FString Mode = String(Op, TEXT("replication"));
                Require(Mode == TEXT("none") || Mode == TEXT("server") || Mode == TEXT("client") || Mode == TEXT("multicast"), TEXT("Invalid replication mode"));
                Event->FunctionFlags &= ~(FUNC_Net | FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast);
                if (Mode == TEXT("none")) { Event->FunctionFlags &= ~FUNC_NetReliable; }
                else
                {
                    Event->FunctionFlags |= FUNC_Net;
                    Event->FunctionFlags |= Mode == TEXT("server") ? FUNC_NetServer : Mode == TEXT("client") ? FUNC_NetClient : FUNC_NetMulticast;
                }
            }
            if (Op->HasField(TEXT("reliable")))
            {
                bool Reliable = Boolean(Op, TEXT("reliable"));
                Require(!Reliable || (Event->FunctionFlags & FUNC_Net), TEXT("Reliable requires a replicated event"));
                if (Reliable) { Event->FunctionFlags |= FUNC_NetReliable; }
                else { Event->FunctionFlags &= ~FUNC_NetReliable; }
            }
            if (Op->HasField(TEXT("inputs"))) { EditEventParameters(Event, Op); }
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        }
        else if (Kind == TEXT("add_node"))
        {
            FString Alias = String(Op, TEXT("id"));
            Require(!Aliases.Contains(Alias), TEXT("Duplicate batch alias: ") + Alias);
            FString Type = String(Op, TEXT("kind"));
            UK2Node* N = nullptr;
            if (Type == TEXT("call"))
            {
                FString Owner = String(Op, TEXT("class"), TEXT("self"), false);
                UClass* Class = Owner == TEXT("self") ? BP->SkeletonGeneratedClass.Get() : LoadObject<UClass>(nullptr, *Owner);
                Require(Class != nullptr, TEXT("Function class not found"));
                UFunction* Function = Class->FindFunctionByName(FName(*String(Op, TEXT("function"))));
                Require(Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure), TEXT("Blueprint callable function not found"));
                UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(G);
                Call->SetFromFunction(Function);
                if (Owner == TEXT("self")) { Call->FunctionReference.SetSelfMember(Function->GetFName()); }
                N = Call;
            }
            else if (Type == TEXT("get") || Type == TEXT("set"))
            {
                FName Name(*String(Op, TEXT("variable")));
                Require(BP->SkeletonGeneratedClass && FindFProperty<FProperty>(BP->SkeletonGeneratedClass, Name), TEXT("Variable not found"));
                UK2Node_Variable* V = Type == TEXT("get") ? static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableGet>(G)) : NewObject<UK2Node_VariableSet>(G);
                V->VariableReference.SetSelfMember(Name);
                N = V;
            }
            else if (Type == TEXT("custom_event"))
            {
                Require(Schema->GetGraphType(G) == GT_Ubergraph, TEXT("Custom events require an event graph"));
                FString Name = String(Op, TEXT("name"));
                AvailableName(BP, Name);
                UK2Node_CustomEvent* E = NewObject<UK2Node_CustomEvent>(G);
                E->CustomFunctionName = FName(*Name);
                E->FunctionFlags = FUNC_Public | FUNC_BlueprintCallable | FUNC_BlueprintEvent;
                FString Net = String(Op, TEXT("replication"), TEXT("none"), false);
                Require(Net == TEXT("none") || Net == TEXT("server") || Net == TEXT("client") || Net == TEXT("multicast"), TEXT("replication must be none, server, client or multicast"));
                if (Net != TEXT("none"))
                {
                    E->FunctionFlags |= FUNC_Net;
                    E->FunctionFlags |= Net == TEXT("server") ? FUNC_NetServer : Net == TEXT("client") ? FUNC_NetClient : FUNC_NetMulticast;
                }
                if (Boolean(Op, TEXT("reliable")))
                {
                    Require(Net != TEXT("none"), TEXT("Reliable requires a replicated event"));
                    E->FunctionFlags |= FUNC_NetReliable;
                }
                N = E;
            }
            else if (Type == TEXT("event"))
            {
                Require(Schema->GetGraphType(G) == GT_Ubergraph, TEXT("Events require an event graph"));
                FName Name(*String(Op, TEXT("event")));
                UFunction* Function = BP->ParentClass->FindFunctionByName(Name);
                Require(Function && Function->HasAnyFunctionFlags(FUNC_BlueprintEvent), TEXT("Overridable event not found"));
                Require(!FBlueprintEditorUtils::FindOverrideForFunction(BP, Function->GetOwnerClass(), Name), TEXT("Event already exists; use its GUID from inspect"));
                UK2Node_Event* E = NewObject<UK2Node_Event>(G);
                E->EventReference.SetExternalMember(Name, Function->GetOwnerClass());
                E->bOverrideFunction = true;
                N = E;
            }
            else if (Type == TEXT("branch")) { N = NewObject<UK2Node_IfThenElse>(G); }
            else if (Type == TEXT("sequence")) { N = NewObject<UK2Node_ExecutionSequence>(G); }
            else if (Type == TEXT("self")) { N = NewObject<UK2Node_Self>(G); }
            else if (Type == TEXT("reroute")) { N = NewObject<UK2Node_Knot>(G); }
            else { throw FFailure{TEXT("Unsupported node kind: ") + Type}; }
            Require(N->IsCompatibleWithGraph(G), TEXT("Node is incompatible with this graph"));
            G->AddNode(N, false, false);
            N->CreateNewGuid();
            N->PostPlacedNewNode();
            N->AllocateDefaultPins();
            N->NodePosX = Integer(Op, TEXT("x"));
            N->NodePosY = Integer(Op, TEXT("y"));
            N->NodeComment = String(Op, TEXT("comment"), FString(), false);
            if (UK2Node_CustomEvent* E = Cast<UK2Node_CustomEvent>(N)) { Parameters(E, Op, TEXT("inputs"), EGPD_Output); }
            Remember(Alias, N);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        }
        else if (Kind == TEXT("connect") || Kind == TEXT("disconnect"))
        {
            UEdGraphPin* From = Pin(G, Object(Op, TEXT("from")), EGPD_Output);
            UEdGraphPin* To = Pin(G, Object(Op, TEXT("to")), EGPD_Input);
            if (Kind == TEXT("disconnect")) { Schema->BreakSinglePinLink(From, To); }
            else if (!From->LinkedTo.Contains(To))
            {
                FPinConnectionResponse Decision = Schema->CanCreateConnection(From, To);
                Require(Decision.Response != CONNECT_RESPONSE_DISALLOW, TEXT("Connection rejected: ") + Decision.Message.ToString());
                bool Breaks = Decision.Response == CONNECT_RESPONSE_BREAK_OTHERS_A || Decision.Response == CONNECT_RESPONSE_BREAK_OTHERS_B || Decision.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB;
                Require(!Breaks || Boolean(Op, TEXT("replace")), TEXT("Connection would break existing links; set replace:true explicitly"));
                Require(Decision.Response != CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE, TEXT("Add an explicit conversion node before connecting these types"));
                Require(Schema->TryCreateConnection(From, To), TEXT("Could not create connection"));
            }
        }
        else if (Kind == TEXT("set_pin_default"))
        {
            UEdGraphPin* P = Pin(G, Op, EGPD_Input);
            Require(P->LinkedTo.IsEmpty() && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && !P->bDefaultValueIsIgnored,
                TEXT("Pin is connected or does not accept a default"));
            FString Value = ValueString(Op, TEXT("value"));
            FString Default;
            TObjectPtr<UObject> DefaultObject = nullptr;
            FText DefaultText;
            Schema->GetPinDefaultValuesFromString(P->PinType, P->GetOwningNode(), Value, Default, DefaultObject, DefaultText);
            if (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Object || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
            {
                Require(Value.IsEmpty() || Value == TEXT("None") || DefaultObject != nullptr, TEXT("Pin object/class reference could not be resolved"));
            }
            FString Error = Schema->IsPinDefaultValid(P, Default, DefaultObject, DefaultText);
            Require(Error.IsEmpty(), TEXT("Invalid pin default: ") + Error);
            Schema->TrySetDefaultValue(*P, Value);
        }
        else if (Kind == TEXT("delete_node"))
        {
            UEdGraphNode* N = Node(G, String(Op, TEXT("node")));
            Require(N->CanUserDeleteNode(), TEXT("This node cannot be deleted"));
            FBlueprintEditorUtils::RemoveNode(BP, N, true);
        }
        else if (Kind == TEXT("move_node"))
        {
            UEdGraphNode* N = Node(G, String(Op, TEXT("node")));
            N->NodePosX = Integer(Op, TEXT("x"), N->NodePosX);
            N->NodePosY = Integer(Op, TEXT("y"), N->NodePosY);
            if (Op->HasField(TEXT("comment"))) { N->NodeComment = String(Op, TEXT("comment"), FString(), false); }
        }
        else { throw FFailure{TEXT("Unknown operation: ") + Kind}; }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    }

    void SetDefault(const FString& Name, const FString& Value)
    {
        Require(BP->GeneratedClass != nullptr, TEXT("Blueprint has no generated class"));
        UObject* CDO = BP->GeneratedClass->GetDefaultObject();
        SetPropertyDefault(CDO, Name, Value);
        FProperty* Property = FindFProperty<FProperty>(BP->GeneratedClass, FName(*Name));
        for (FBPVariableDescription& V : BP->NewVariables)
        {
            if (V.VarName == Property->GetFName())
            {
                Property->ExportText_InContainer(0, V.DefaultValue, CDO, CDO, CDO, PPF_None);
            }
        }
        BP->MarkPackageDirty();
    }
};

static FJson DescribeRedirector(IAssetRegistry& Registry, const FAssetData& Asset)
{
    FJson Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset"), Asset.PackageName.ToString());
    Result->SetStringField(TEXT("object_path"), Asset.GetSoftObjectPath().ToString());
    TArray<TSharedPtr<FJsonValue>> Chain;
    TSet<FString> Seen;
    FAssetData Current = Asset;
    FString Status = TEXT("unknown"), Resolved;
    for (int32 Depth = 0; Depth < 256; ++Depth)
    {
        const FString ObjectPath = Current.GetSoftObjectPath().ToString();
        if (Seen.Contains(ObjectPath)) { Status = TEXT("cycle"); break; }
        Seen.Add(ObjectPath);
        Chain.Add(MakeShared<FJsonValueString>(ObjectPath));
        if (!Current.IsRedirector()) { Status = TEXT("resolved"); Resolved = ObjectPath; break; }
        FString ExportPath;
        if (!Current.GetTagValue(FName(TEXT("DestinationObject")), ExportPath)) { break; }
        FString Destination = FPackageName::ExportTextPathToObjectPath(ExportPath);
        if (Depth == 0) { Result->SetStringField(TEXT("destination"), Destination); }
        if (Destination.IsEmpty() || Destination == TEXT("None")) { Status = TEXT("missing"); break; }
        FSoftObjectPath Target(Destination);
        const FString Package = Target.GetLongPackageName();
        if (!Target.IsValid() || Package.StartsWith(TEXT("/Script/"))) { break; }
        // Check the package before consulting the registry: a deleted package
        // may still have a cached registry entry. Do not load/follow redirectors.
        if (!FPackageName::DoesPackageExist(Package)) { Status = TEXT("missing"); Chain.Add(MakeShared<FJsonValueString>(Destination)); break; }
        Registry.ScanPathsSynchronous({FPackageName::GetLongPackagePath(Package)}, true);
        Current = Registry.GetAssetByObjectPath(Target, true);
        if (!Current.IsValid()) { Status = TEXT("missing"); Chain.Add(MakeShared<FJsonValueString>(Destination)); break; }
    }
    Result->SetStringField(TEXT("status"), Status);
    if (!Resolved.IsEmpty()) { Result->SetStringField(TEXT("resolved_destination"), Resolved); }
    Result->SetArrayField(TEXT("chain"), Chain);
    return Result;
}

static void Execute(const FJson& Request, const FJson& Response)
{
    FString Action = String(Request, TEXT("action"));
    Response->SetNumberField(TEXT("protocol_version"), 1);
    Response->SetBoolField(TEXT("saved"), false);
    if (Request->HasField(TEXT("response_mode")) || Request->HasField(TEXT("graph")))
    {
        Require(Action == TEXT("inspect") || Action == TEXT("apply") || Action == TEXT("create"), TEXT("response_mode and graph require inspect, apply or create"));
        const FString Mode = String(Request, TEXT("response_mode"), TEXT("full"), false);
        Require(Mode == TEXT("full") || Mode == TEXT("summary") || Mode == TEXT("graph"), TEXT("response_mode must be full, summary or graph"));
        if (Mode == TEXT("graph")) { Require(!String(Request, TEXT("graph")).TrimStartAndEnd().IsEmpty(), TEXT("graph must not be empty")); }
        else { Require(!Request->HasField(TEXT("graph")), TEXT("graph requires response_mode graph")); }
    }
    if (Action == TEXT("compile"))
    {
        const FString Root = String(Request, TEXT("path"), TEXT("/Game"), false);
        Require(Root == TEXT("/Game") || (Root.StartsWith(TEXT("/Game/")) && !Root.Contains(TEXT(".")) &&
            FPackageName::IsValidLongPackageName(Root)), TEXT("Compile path must be a package or folder under /Game"));
        const bool SingleAsset = FPackageName::DoesPackageExist(Root);
        FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        Registry.Get().ScanPathsSynchronous({SingleAsset ? FPackageName::GetLongPackagePath(Root) : Root}, true);
        FARFilter Filter;
        if (SingleAsset) { Filter.PackageNames.Add(FName(*Root)); }
        else { Filter.PackagePaths.Add(FName(*Root)); Filter.bRecursivePaths = true; }
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;
        TArray<FAssetData> Assets;
        Registry.Get().GetAssets(Filter, Assets);
        Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
        Response->SetStringField(TEXT("path"), Root);
        TArray<TSharedPtr<FJsonValue>> Failures;
        int32 Passed = 0;
        for (const FAssetData& Asset : Assets)
        {
            FJson Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("asset"), Asset.PackageName.ToString());
            try
            {
                UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
                Require(BP != nullptr, TEXT("Blueprint could not be loaded"));
                Require(Compile(BP, Result), TEXT("Blueprint compilation failed"));
                ++Passed;
            }
            catch (const FFailure& Failure)
            {
                Result->SetStringField(TEXT("error"), Failure.Message);
                Failures.Add(JsonValue(Result));
            }
            // Compile skips GC for edit batches. Release each asset here so a
            // project-wide check does not retain every compiled graph in memory.
            CollectGarbage(RF_NoFlags);
        }
        Response->SetNumberField(TEXT("checked"), Assets.Num());
        Response->SetNumberField(TEXT("passed"), Passed);
        Response->SetNumberField(TEXT("failed"), Failures.Num());
        Response->SetArrayField(TEXT("failures"), Failures);
        Require(!Assets.IsEmpty(), TEXT("No Blueprint assets found at path: ") + Root);
        Require(Failures.IsEmpty(), TEXT("One or more Blueprints failed to compile"));
        return;
    }
    if (Action == TEXT("list") || Action == TEXT("redirectors"))
    {
        FString Root = String(Request, TEXT("path"), TEXT("/Game"), false);
        Require(Root == TEXT("/Game") || (Root.StartsWith(TEXT("/Game/")) && FPackageName::IsValidLongPackageName(Root)), TEXT("List path must be under /Game"));
        FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        Registry.Get().ScanPathsSynchronous({Root}, true);
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*Root));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(Action == TEXT("redirectors") ? UObjectRedirector::StaticClass()->GetClassPathName() : UBlueprint::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;
        Filter.bIncludeOnlyOnDiskAssets = true;
        TArray<FAssetData> Assets;
        Registry.Get().GetAssets(Filter, Assets);
        Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FAssetData& A : Assets)
        {
            if (Action == TEXT("redirectors")) { Values.Add(JsonValue(DescribeRedirector(Registry.Get(), A))); }
            else { Values.Add(MakeShared<FJsonValueString>(A.PackageName.ToString())); }
        }
        Response->SetArrayField(Action == TEXT("redirectors") ? TEXT("redirectors") : TEXT("assets"), Values);
        return;
    }
    if (Action == TEXT("functions"))
    {
        UClass* Class = LoadObject<UClass>(nullptr, *String(Request, TEXT("class")));
        Require(Class != nullptr, TEXT("Class not found"));
        FString Search = String(Request, TEXT("search"), FString(), false);
        TArray<TSharedPtr<FJsonValue>> Functions;
        for (TFieldIterator<UFunction> It(Class); It; ++It)
        {
            if (!It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent) ||
                (!Search.IsEmpty() && !It->GetName().Contains(Search))) { continue; }
            FJson F = MakeShared<FJsonObject>();
            F->SetStringField(TEXT("name"), It->GetName());
            F->SetStringField(TEXT("class"), It->GetOwnerClass()->GetPathName());
            F->SetBoolField(TEXT("pure"), It->HasAnyFunctionFlags(FUNC_BlueprintPure));
            F->SetBoolField(TEXT("static"), It->HasAnyFunctionFlags(FUNC_Static));
            TArray<TSharedPtr<FJsonValue>> Parameters;
            for (TFieldIterator<FProperty> P(*It); P && P->HasAnyPropertyFlags(CPF_Parm); ++P)
            {
                FJson Param = MakeShared<FJsonObject>();
                Param->SetStringField(TEXT("name"), P->GetName());
                FEdGraphPinType Type;
                GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(*P, Type);
                Param->SetObjectField(TEXT("type"), DescribeType(Type));
                Param->SetBoolField(TEXT("output"), P->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm) && !P->HasAnyPropertyFlags(CPF_ReferenceParm));
                Parameters.Add(JsonValue(Param));
            }
            F->SetArrayField(TEXT("parameters"), Parameters);
            Functions.Add(JsonValue(F));
        }
        Response->SetArrayField(TEXT("functions"), Functions);
        return;
    }
    Require(Action == TEXT("inspect") || Action == TEXT("components") || Action == TEXT("apply") || Action == TEXT("create"), TEXT("Unknown action: ") + Action);
    FString Package = PackagePath(Request);
    FString Filename = AssetFilename(Package);
    Require(!IFileManager::Get().FileExists(*FPaths::ChangeExtension(Filename, TEXT("uexp"))) &&
        !IFileManager::Get().FileExists(*FPaths::ChangeExtension(Filename, TEXT("ubulk"))),
        TEXT("Split packages (.uexp/.ubulk) are not supported"));
    FString Before;
    UBlueprint* BP = nullptr;
    if (Action == TEXT("create"))
    {
        Require(!FPackageName::DoesPackageExist(Package), TEXT("Asset already exists"));
        UClass* Parent = LoadObject<UClass>(nullptr, *String(Request, TEXT("parent_class"), TEXT("/Script/Engine.Actor"), false));
        Require(Parent && FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent), TEXT("Invalid Blueprint parent class"));
        BP = FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(*Package), FName(*FPackageName::GetLongPackageAssetName(Package)), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    }
    else
    {
        Before = Revision(Filename);
        if (Request->HasField(TEXT("expected_revision")))
        { Require(String(Request, TEXT("expected_revision")) == Before, TEXT("Revision conflict: inspect the asset again before editing")); }
        BP = LoadObject<UBlueprint>(nullptr, *(Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package)));
    }
    Require(BP != nullptr, TEXT("Blueprint could not be loaded/created"));
    Require(BP->GetOutermost()->GetName() == Package,
        TEXT("Asset path resolves to another package through a redirector; use the destination Blueprint package path"));
    Require(BP->GetClass() == UBlueprint::StaticClass() && BP->BlueprintType == BPTYPE_Normal,
        TEXT("Only normal class Blueprints are supported; animation, widget, level, interface and macro-library Blueprints are excluded"));
    if (Action == TEXT("components"))
    {
        Response->SetArrayField(TEXT("components"), DescribeComponents(BP, true));
        Response->SetStringField(TEXT("revision"), Before);
        return;
    }
    if (Action == TEXT("inspect"))
    {
        Response->SetObjectField(TEXT("blueprint"), DescribeBlueprint(BP, Request));
        Response->SetStringField(TEXT("revision"), Before);
        return;
    }
    Require(Compile(BP, Response), TEXT("Existing Blueprint does not compile; fix its errors before editing"));
    FEditor Editor(BP, Response);
    TArray<TSharedPtr<FJsonValue>> Operations = Array(Request, TEXT("operations"), true);
    Require(Operations.Num() <= 1000, TEXT("At most 1000 operations per batch"));
    for (int32 I = 0; I < Operations.Num(); ++I)
    {
        Response->SetNumberField(TEXT("operation_index"), I);
        Editor.Apply(AsObject(Operations[I]));
    }
    Response->RemoveField(TEXT("operation_index"));
    Require(Editor.CompileCurrent(), TEXT("Blueprint compilation failed; no asset was saved"));
    FJson Aliases = MakeShared<FJsonObject>();
    for (const auto& Pair : Editor.Aliases) { Aliases->SetStringField(Pair.Key, Guid(Pair.Value)); }
    Response->SetObjectField(TEXT("node_ids"), Aliases);
    Response->SetObjectField(TEXT("blueprint"), DescribeBlueprint(BP, Request));
    Response->SetStringField(TEXT("previous_revision"), Before);
    bool DryRun = Boolean(Request, TEXT("dry_run"));
    Response->SetBoolField(TEXT("dry_run"), DryRun);
    if (DryRun) { return; }
    // Detect a file changed by another process while this commandlet was compiling.
    if (!Before.IsEmpty()) { Require(Revision(Filename) == Before, TEXT("Asset changed during the request; nothing saved")); }
    else { Require(!IFileManager::Get().FileExists(*Filename), TEXT("Asset appeared during the request; nothing saved")); }
    Require(!IFileManager::Get().IsReadOnly(*Filename), TEXT("Asset is read-only; check it out from source control first"));
    if (!Before.IsEmpty())
    {
        FString BackupDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UAssetAgent/Backups"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
        Require(IFileManager::Get().MakeDirectory(*BackupDir, true), TEXT("Cannot create backup directory"));
        FString Backup = FPaths::ConvertRelativePathToFull(FPaths::Combine(BackupDir, FPaths::GetCleanFilename(Filename)));
        Require(IFileManager::Get().Copy(*Backup, *Filename, false) == COPY_OK, TEXT("Cannot back up asset; nothing saved"));
        Response->SetStringField(TEXT("backup"), Backup);
    }
    Require(IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true), TEXT("Cannot create asset directory"));
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    Response->SetBoolField(TEXT("save_attempted"), true);
    Require(UPackage::SavePackage(BP->GetOutermost(), BP, *Filename, Args), TEXT("SavePackage failed; check log and backup before retrying"));
    Response->SetBoolField(TEXT("saved"), true);
    Response->SetStringField(TEXT("revision"), Revision(Filename));
}
} // namespace UAssetAgent

#if WITH_DEV_AUTOMATION_TESTS
#include "UAssetAgentTests.inl"
#endif

UUAssetAgentCommandlet::UUAssetAgentCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
    ShowErrorCount = false;
}

int32 UUAssetAgentCommandlet::Main(const FString& Params)
{
    using namespace UAssetAgent;
    FString RequestFile, ResponseFile;
    if (!FParse::Value(*Params, TEXT("Request="), RequestFile) || !FParse::Value(*Params, TEXT("Response="), ResponseFile))
    {
        UE_LOG(LogTemp, Error, TEXT("Usage: -run=UAssetAgent -Request=<json file> -Response=<json file>"));
        return 2;
    }
    FJson Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("saved"), false);
    int32 ExitCode = 0;
    try
    {
        FString Text;
        Require(FFileHelper::LoadFileToString(Text, *RequestFile), TEXT("Cannot read request file"));
        FJson Request;
        Require(FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Request) && Request.IsValid(), TEXT("Request must be a JSON object"));
        Execute(Request, Response);
        Response->SetBoolField(TEXT("ok"), true);
    }
    catch (const FFailure& Failure)
    {
        Response->SetBoolField(TEXT("ok"), false);
        Response->SetStringField(TEXT("error"), Failure.Message);
        ExitCode = 1;
    }
    FString Text;
    FJsonSerializer::Serialize(Response.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
    if (!FFileHelper::SaveStringToFile(Text, *ResponseFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogTemp, Error, TEXT("Cannot write response file: %s"), *ResponseFile);
        return 2;
    }
    return ExitCode;
}
