// Native regression tests. Run only in a disposable project; saved fixtures are
// retained under a unique /Game/UAssetAgentTests folder for inspection.
#include "Misc/AutomationTest.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
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
        NativeEditor.Apply(Json(TEXT("{\"op\":\"set_component_default\",\"component\":\"native:CapsuleComponent\",\"property\":\"CapsuleRadius\",\"value\":\"55\"}")));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentRedirectorTest, "UAssetAgent.Redirectors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentRedirectorTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Root = TestRoot();
        auto Data = [&](const TCHAR* Name) { return NewObject<UDataAsset>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone); };
        auto Redirector = [&](const TCHAR* Name) { return NewObject<UObjectRedirector>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone); };
        UDataAsset* Target = Data(TEXT("Target")); Save(Target);
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
