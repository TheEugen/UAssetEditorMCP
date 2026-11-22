// Native regression tests. Run only in a disposable project; saved fixtures are
// retained under a unique /Game/UAssetAgentTests folder for inspection.
#include "Misc/AutomationTest.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/DataAsset.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"

namespace UAssetAgentTests
{
using namespace UAssetAgent;

static FJson Json(const TCHAR* Text)
{
    FJson Result;
    Require(FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result), TEXT("Invalid test JSON"));
    return Result;
}

static FString TestRoot()
{
    return TEXT("/Game/UAssetAgentTests/Native_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

static UBlueprint* Blueprint(const FString& Package, UClass* Parent = AActor::StaticClass())
{
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(*Package),
        FName(*FPackageName::GetLongPackageAssetName(Package)), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    Require(BP && Compile(BP, MakeShared<FJsonObject>()), TEXT("Test Blueprint creation failed"));
    return BP;
}

static void Save(UObject* Asset)
{
    const FString Filename = AssetFilename(Asset->GetOutermost()->GetName());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    Require(UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, Args), TEXT("Cannot save test fixture"));
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous({Filename}, true);
}

static bool Rejected(FEditor& Editor, const FJson& Operation)
{
    try { Editor.Apply(Operation); }
    catch (const FFailure&) { return true; }
    return false;
}

static FJson Reparent(UClass* Class)
{
    FJson Operation = Json(TEXT("{\"op\":\"reparent\"}"));
    Operation->SetStringField(TEXT("parent_class"), Class->GetPathName());
    return Operation;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentReparentTest, "UAssetAgent.Reparent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentReparentTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Root = TestRoot();
        UBlueprint* BP = Blueprint(Root + TEXT("/BP_Actor"));
        FEditor Editor(BP, MakeShared<FJsonObject>());
        UBlueprint* Child = Blueprint(Root + TEXT("/BP_Child"), BP->GeneratedClass);
        TestTrue(TEXT("Reject descendant parent"), Rejected(Editor, Reparent(Child->GeneratedClass)));
        TestTrue(TEXT("Reject self parent"), Rejected(Editor, Reparent(BP->GeneratedClass)));
        TestTrue(TEXT("Reject family change"), Rejected(Editor, Reparent(UActorComponent::StaticClass())));
        Editor.Apply(Reparent(APawn::StaticClass()));
        TestTrue(TEXT("Reparent compiles"), Editor.CompileCurrent());
        TestEqual(TEXT("New parent"), BP->ParentClass.Get(), APawn::StaticClass());
        TestTrue(TEXT("Generated class uses new parent"), BP->GeneratedClass->IsChildOf(APawn::StaticClass()));
        Editor.Apply(Reparent(AActor::StaticClass()));
        Require(Editor.CompileCurrent(), TEXT("Cannot restore parent for collision check"));
        Editor.Apply(Json(TEXT("{\"op\":\"add_variable\",\"name\":\"Controller\",\"type\":{\"category\":\"int\"}}")));
        TestTrue(TEXT("Reject inherited variable collision"), Rejected(Editor, Reparent(APawn::StaticClass())));
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentComponentTest, "UAssetAgent.ComponentDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentComponentTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Root = TestRoot();
        UBlueprint* Parent = Blueprint(Root + TEXT("/BP_Parent"));
        USCS_Node* Hull = Parent->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("Hull"));
        Parent->SimpleConstructionScript->AddNode(Hull);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Parent);
        FEditor ParentEditor(Parent, MakeShared<FJsonObject>());
        ParentEditor.Apply(Json(TEXT("{\"op\":\"set_component_default\",\"component\":\"Hull\",\"property\":\"RelativeLocation\",\"value\":\"(X=12,Y=2,Z=3)\"}")));
        Require(ParentEditor.CompileCurrent(), TEXT("Local component edit did not compile"));
        TestEqual(TEXT("Local component value"), CastChecked<USceneComponent>(Hull->ComponentTemplate)->GetRelativeLocation(), FVector(12, 2, 3));
        Save(Parent);
        const FString ParentRevision = Revision(AssetFilename(Parent->GetOutermost()->GetName()));
        UBlueprint* Child = Blueprint(Root + TEXT("/BP_Child"), Parent->GeneratedClass);
        FEditor ChildEditor(Child, MakeShared<FJsonObject>());
        ChildEditor.Apply(Json(TEXT("{\"op\":\"set_component_default\",\"component\":\"Hull\",\"property\":\"RelativeLocation\",\"value\":\"(X=100,Y=0,Z=0)\"}")));
        Require(ChildEditor.CompileCurrent(), TEXT("Inherited component edit did not compile"));
        UActorComponent* Override = Hull->GetActualComponentTemplate(CastChecked<UBlueprintGeneratedClass>(Child->GeneratedClass));
        TestEqual(TEXT("Override belongs to child"), Override->GetOutermost(), Child->GetOutermost());
        TestEqual(TEXT("Child value"), CastChecked<USceneComponent>(Override)->GetRelativeLocation(), FVector(100, 0, 0));
        TestEqual(TEXT("Parent memory unchanged"), CastChecked<USceneComponent>(Hull->ComponentTemplate)->GetRelativeLocation(), FVector(12, 2, 3));
        Save(Child);
        TestEqual(TEXT("Parent file unchanged"), Revision(AssetFilename(Parent->GetOutermost()->GetName())), ParentRevision);
        TestTrue(TEXT("Reject missing property"), Rejected(ChildEditor, Json(TEXT("{\"op\":\"set_component_default\",\"component\":\"Hull\",\"property\":\"NoSuchProperty\",\"value\":\"1\"}"))));
        UBlueprint* Native = Blueprint(Root + TEXT("/BP_Native"), ACharacter::StaticClass());
        FEditor NativeEditor(Native, MakeShared<FJsonObject>());
        FJson Edit = Json(TEXT(R"({"op":"set_component_default","property":"CapsuleRadius","value":"55"})"));
        Edit->SetStringField(TEXT("component"), TEXT("native:") + ACharacter::CapsuleComponentName.ToString());
        NativeEditor.Apply(Edit);
        Require(NativeEditor.CompileCurrent(), TEXT("Native component edit did not compile"));
        TestEqual(TEXT("Native component value survives compile"), CastChecked<ACharacter>(Native->GeneratedClass->GetDefaultObject())->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), 55.0f);
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentMemberTest, "UAssetAgent.VariablesAndEvents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentMemberTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        UBlueprint* BP = Blueprint(TestRoot() + TEXT("/BP_Members"));
        FEditor Editor(BP, MakeShared<FJsonObject>());
        Editor.Apply(Json(TEXT("{\"op\":\"add_variable\",\"name\":\"Used\",\"type\":{\"category\":\"int\"},\"replicated\":true}")));
        Editor.Apply(Json(TEXT("{\"op\":\"add_variable\",\"name\":\"Unused\",\"type\":{\"category\":\"int\"},\"replicated\":true,\"rep_notify\":\"OnRep_Unused\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"add_node\",\"id\":\"event\",\"kind\":\"custom_event\",\"name\":\"Message\",\"replication\":\"multicast\",\"reliable\":true,\"inputs\":[{\"name\":\"Count\",\"type\":{\"category\":\"int\"}}]}")));
        Editor.Apply(Json(TEXT("{\"op\":\"add_node\",\"id\":\"set\",\"kind\":\"set\",\"variable\":\"Used\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"connect\",\"from\":{\"node\":\"event\",\"pin\":\"Count\"},\"to\":{\"node\":\"set\",\"pin\":\"Used\"}}")));
        Require(Editor.CompileCurrent(), TEXT("Member fixture did not compile"));
        Editor.Apply(Json(TEXT("{\"op\":\"add_node\",\"id\":\"call\",\"kind\":\"call\",\"function\":\"Message\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"edit_custom_event\",\"node\":\"event\",\"replication\":\"none\",\"inputs\":[{\"name\":\"Count\",\"type\":{\"category\":\"int\"}},{\"name\":\"Label\",\"type\":{\"category\":\"string\"}}]}")));
        UEdGraph* Graph = Editor.Graph(Json(TEXT("{}")));
        UK2Node_CustomEvent* Event = CastChecked<UK2Node_CustomEvent>(Editor.Node(Graph, TEXT("event")));
        TestTrue(TEXT("Clear network and reliability flags"), !(Event->FunctionFlags & (FUNC_Net | FUNC_NetReliable | FUNC_NetMulticast)));
        TestEqual(TEXT("Preserve unchanged parameter wiring"), Event->FindPin(TEXT("Count"))->LinkedTo.Num(), 1);
        TestNotNull(TEXT("Update local caller signature"), Editor.Node(Graph, TEXT("call"))->FindPin(TEXT("Label")));
        TestTrue(TEXT("Reject implicit link loss"), Rejected(Editor, Json(TEXT("{\"op\":\"edit_custom_event\",\"node\":\"event\",\"inputs\":[]}"))));
        Editor.Apply(Json(TEXT("{\"op\":\"edit_custom_event\",\"node\":\"event\",\"inputs\":[],\"allow_break_links\":true}")));
        TestNull(TEXT("Removed parameter"), Event->FindPin(TEXT("Count")));
        TestTrue(TEXT("Reject reliable non-RPC"), Rejected(Editor, Json(TEXT("{\"op\":\"edit_custom_event\",\"node\":\"event\",\"reliable\":true}"))));
        TestTrue(TEXT("Reject referenced variable deletion"), Rejected(Editor, Json(TEXT("{\"op\":\"delete_variable\",\"name\":\"Used\"}"))));
        Editor.Apply(Json(TEXT("{\"op\":\"delete_variable\",\"name\":\"Unused\"}")));
        TestEqual(TEXT("Unused replicated variable removed"), FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("Unused")), INDEX_NONE);
        Editor.Apply(Json(TEXT("{\"op\":\"delete_node\",\"node\":\"set\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"delete_variable\",\"name\":\"Used\"}")));
        TestTrue(TEXT("Edited Blueprint compiles"), Editor.CompileCurrent());
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentMemberTypeTest, "UAssetAgent.MemberTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentMemberTypeTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Root = TestRoot();
        UBlueprint* BP = Blueprint(Root + TEXT("/BP_TypeChanges"));
        const FString Asset = BP->GetOutermost()->GetName(), Filename = AssetFilename(Asset);
        FEditor Editor(BP, MakeShared<FJsonObject>());
        Editor.Apply(Json(TEXT(R"({"op":"add_variable","name":"Target","type":{"category":"object","path":"/Script/Engine.Pawn"}})")));
        Editor.Apply(Json(TEXT(R"({"op":"add_variable","name":"Count","type":{"category":"int"}})")));
        const FGuid VariableId = BP->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("Target"))].VarGuid;

        USCS_Node* Hull = BP->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("Hull"));
        USCS_Node* Detail = BP->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("Detail"));
        BP->SimpleConstructionScript->AddNode(Hull);
        Hull->AddChildNode(Detail);
        const FGuid ComponentId = Hull->VariableGuid;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        Editor.Apply(Json(TEXT(R"json({"op":"set_component_default","component":"Hull","property":"RelativeLocation","value":"(X=12,Y=2,Z=3)"})json")));
        Editor.Apply(Json(TEXT(R"({"op":"add_function","name":"ReadHull","outputs":[{"name":"Value","type":{"category":"object","path":"/Script/Engine.SceneComponent"}}],"result_id":"out"})")));
        Editor.Apply(Json(TEXT(R"({"op":"add_node","graph":"ReadHull","id":"get","kind":"get","variable":"Hull"})")));
        Editor.Apply(Json(TEXT(R"({"op":"connect","graph":"ReadHull","from":{"node":"get","pin":"Hull"},"to":{"node":"out","pin":"Value"}})")));
        Require(Editor.CompileCurrent(), TEXT("Original member type fixture must compile"));
        Save(BP);

        UBlueprint* Child = Blueprint(Root + TEXT("/BP_TypeChild"), BP->GeneratedClass);
        FEditor ChildEditor(Child, MakeShared<FJsonObject>());
        ChildEditor.Apply(Json(TEXT(R"({"op":"add_function","name":"ReadPawn","outputs":[{"name":"Value","type":{"category":"object","path":"/Script/Engine.Pawn"}}],"result_id":"out"})")));
        ChildEditor.Apply(Json(TEXT(R"({"op":"add_node","graph":"ReadPawn","id":"get","kind":"get","variable":"Target"})")));
        ChildEditor.Apply(Json(TEXT(R"({"op":"connect","graph":"ReadPawn","from":{"node":"get","pin":"Target"},"to":{"node":"out","pin":"Value"}})")));
        Require(ChildEditor.CompileCurrent(), TEXT("Original child must compile"));
        Save(Child);
        const FString ChildFilename = AssetFilename(Child->GetOutermost()->GetName());
        const FString ChildRevision = Revision(ChildFilename);

        const FJson Request = Json(TEXT(R"({"action":"apply","response_mode":"summary","operations":[
            {"op":"change_variable_type","name":"Target","type":{"category":"object","path":"/Script/Engine.Character"}},
            {"op":"change_variable_type","name":"Count","type":{"category":"string"},"default":"captain"},
            {"op":"replace_component_class","component":"Hull","class":"/Script/Engine.SphereComponent"}
        ]})"));
        Request->SetStringField(TEXT("asset"), Asset);
        Request->SetStringField(TEXT("expected_revision"), Revision(Filename));
        const FJson Result = MakeShared<FJsonObject>();
        Execute(Request, Result);
        TestTrue(TEXT("Changed Blueprint saved"), Result->GetBoolField(TEXT("saved")));
        TestEqual(TEXT("Child Blueprint validated"), Array(Result, TEXT("affected_blueprints")).Num(), 1);
        TestEqual(TEXT("Child Blueprint file retained"), Revision(ChildFilename), ChildRevision);
        const FBPVariableDescription& Target = BP->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("Target"))];
        TestEqual(TEXT("Variable GUID retained"), Target.VarGuid, VariableId);
        TestTrue(TEXT("Variable type changed to Character"), Target.VarType.PinSubCategoryObject.Get() == ACharacter::StaticClass());
        TestTrue(TEXT("Local component changed to a sphere"), Hull->ComponentTemplate->IsA<USphereComponent>());
        TestEqual(TEXT("Component GUID retained"), Hull->VariableGuid, ComponentId);
        TestTrue(TEXT("Child component attachment retained"), Hull->GetChildNodes().Contains(Detail));
        TestEqual(TEXT("Compatible component default retained"), CastChecked<USceneComponent>(Hull->ComponentTemplate)->GetRelativeLocation(), FVector(12, 2, 3));
        UEdGraph* ReadHull = Editor.Graph(Json(TEXT(R"({"graph":"ReadHull"})")));
        UEdGraphPin* HullPin = Editor.Node(ReadHull, TEXT("get"))->FindPin(TEXT("Hull"), EGPD_Output);
        TestTrue(TEXT("Component graph connection retained"), HullPin && HullPin->LinkedTo.Num() == 1);
        const FStrProperty* Count = FindFProperty<FStrProperty>(BP->GeneratedClass, TEXT("Count"));
        TestTrue(TEXT("Changed variable default retained"), Count &&
            Count->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) == TEXT("captain"));

        const FString SavedRevision = Revision(Filename);
        const FJson Incompatible = Json(TEXT(R"({"action":"apply","operations":[
            {"op":"change_variable_type","name":"Target","type":{"category":"object","path":"/Script/Engine.Actor"}}
        ]})"));
        Incompatible->SetStringField(TEXT("asset"), Asset);
        Incompatible->SetStringField(TEXT("expected_revision"), SavedRevision);
        const FJson Blocked = MakeShared<FJsonObject>();
        bool Rejected = false;
        AddExpectedErrorPlain(TEXT("BP_TypeChild.uasset: [Compiler]"), EAutomationExpectedErrorFlags::Contains, 0);
        try { Execute(Incompatible, Blocked); }
        catch (const FFailure& Failure)
        {
            Rejected = true;
            TestTrue(TEXT("Rejection identifies the dependent Blueprint"), Failure.Message.Contains(TEXT("dependent Blueprints")));
        }
        TestTrue(TEXT("Incompatible dependent wiring blocks the save"), Rejected);
        TestEqual(TEXT("Rejected edit preserves the file"), Revision(Filename), SavedRevision);
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentRedirectorTest, "UAssetAgent.Redirectors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentRedirectorTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Root = TestRoot();
        auto Data = [&](const TCHAR* Name) { return NewObject<UCurveFloat>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone); };
        auto Redirector = [&](const TCHAR* Name) { return NewObject<UObjectRedirector>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone); };
        UCurveFloat* Target = Data(TEXT("Target")); Save(Target);
        UObjectRedirector* Good = Redirector(TEXT("Good")); Good->DestinationObject = Target; Save(Good);
        UObjectRedirector* Chain = Redirector(TEXT("Chain")); Chain->DestinationObject = Good; Save(Chain);
        UObjectRedirector* Broken = Redirector(TEXT("Broken")); Broken->DestinationObject = Data(TEXT("UnsavedTarget")); Save(Broken);
        UObjectRedirector* CycleA = Redirector(TEXT("CycleA"));
        UObjectRedirector* CycleB = Redirector(TEXT("CycleB"));
        CycleA->DestinationObject = CycleB; CycleB->DestinationObject = CycleA; Save(CycleA); Save(CycleB);
        FJson Request = Json(TEXT("{\"action\":\"redirectors\"}")); Request->SetStringField(TEXT("path"), Root);
        FJson Response = MakeShared<FJsonObject>(); Execute(Request, Response);
        TMap<FString, FString> Statuses;
        for (const TSharedPtr<FJsonValue>& Value : Array(Response, TEXT("redirectors")))
        {
            FJson Item = AsObject(Value);
            Statuses.Add(FPackageName::GetLongPackageAssetName(String(Item, TEXT("asset"))), String(Item, TEXT("status")));
        }
        TestEqual(TEXT("All redirectors listed"), Statuses.Num(), 5);
        TestEqual(TEXT("Valid target"), Statuses.FindRef(TEXT("Good")), FString(TEXT("resolved")));
        TestEqual(TEXT("Chained target"), Statuses.FindRef(TEXT("Chain")), FString(TEXT("resolved")));
        TestEqual(TEXT("Missing target"), Statuses.FindRef(TEXT("Broken")), FString(TEXT("missing")));
        TestEqual(TEXT("Cycle detected"), Statuses.FindRef(TEXT("CycleA")), FString(TEXT("cycle")));
        UBlueprint* BP = Blueprint(Root + TEXT("/BP_Target")); Save(BP);
        UObjectRedirector* Alias = Redirector(TEXT("BP_Alias")); Alias->DestinationObject = BP; Save(Alias);
        FJson Apply = Json(TEXT("{\"action\":\"apply\",\"operations\":[]}"));
        Apply->SetStringField(TEXT("asset"), Alias->GetOutermost()->GetName());
        const FString Before = Revision(AssetFilename(Alias->GetOutermost()->GetName()));
        bool AliasRejected = false;
        try { Execute(Apply, MakeShared<FJsonObject>()); }
        catch (const FFailure&) { AliasRejected = true; }
        TestTrue(TEXT("Reject edits through redirectors"), AliasRejected);
        TestEqual(TEXT("Redirector file unchanged"), Revision(AssetFilename(Alias->GetOutermost()->GetName())), Before);
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}
