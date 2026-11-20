// Compiled into the same translation unit so tests exercise the actual handlers.
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Root.h"
#include "Components/Button.h"
#include "Components/Overlay.h"
#include "Components/TextBlock.h"
#include "UObject/UObjectIterator.h"

namespace UAssetAgentV2Tests
{
using namespace UAssetAgentTests;

struct FFixtures
{
    FString Root = TEXT("/Game/__UAssetAgentTests/Native_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ~FFixtures()
    {
        TArray<UObject*> Assets;
        for (TObjectIterator<UObject> It; It; ++It)
        {
            if (It->IsAsset() && It->GetOutermost()->GetName().StartsWith(Root + TEXT("/"))) { Assets.Add(*It); }
        }
        if (!Assets.IsEmpty()) { ObjectTools::DeleteObjectsUnchecked(Assets); }
    }
    UBlueprint* Create(const FString& Name, const FString& Kind)
    {
        FJson Request = Json(TEXT("{\"action\":\"create\",\"operations\":[],\"response_mode\":\"summary\"}"));
        Request->SetStringField(TEXT("asset"), Root + TEXT("/") + Name);
        Request->SetStringField(TEXT("kind"), Kind);
        if (Kind == TEXT("animation"))
        {
            Request->SetStringField(TEXT("parent_class"), TEXT("/Script/Engine.AnimInstance"));
            Request->SetBoolField(TEXT("template"), true);
        }
        if (Kind == TEXT("widget")) { Request->SetStringField(TEXT("parent_class"), TEXT("/Script/UMG.UserWidget")); }
        Execute(Request, MakeShared<FJsonObject>());
        return CastChecked<UBlueprint>(LoadExactAsset(Root + TEXT("/") + Name));
    }
};

static void Finish(FEditor& Editor)
{
    if (Editor.bRefreshAfterBatch) { FBlueprintEditorUtils::RefreshAllNodes(Editor.BP); Editor.bRefreshAfterBatch = false; }
    Require(Editor.CompileCurrent(), TEXT("Fixture did not compile"));
    for (FJson Op : Editor.DeferredDefaults) { Editor.SetDefault(String(Op, TEXT("property")), ValueString(Op, TEXT("value"))); }
    Editor.DeferredDefaults.Empty();
    Require(Editor.CompileCurrent(), TEXT("Fixture defaults did not compile"));
    Save(Editor.BP);
}

static FJson AnimOp(const TCHAR* Kind, UEdGraphNode* Node)
{
    FJson Op = MakeShared<FJsonObject>();
    Op->SetStringField(TEXT("op"), Kind);
    Op->SetStringField(TEXT("graph"), Node->GetGraph()->GetPathName());
    Op->SetStringField(TEXT("node"), Guid(Node->NodeGuid));
    return Op;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentRetargetCallTest, "UAssetAgent.V2.RetargetCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentRetargetCallTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentV2Tests;
    FFixtures Fixtures;
    try
    {
        UBlueprint* Original = Fixtures.Create(TEXT("BFL_Original"), TEXT("function_library"));
        UBlueprint* Replacement = Fixtures.Create(TEXT("BFL_Replacement"), TEXT("function_library"));
        FEditor OriginalEditor(Original, MakeShared<FJsonObject>());
        OriginalEditor.Apply(Json(TEXT(R"json({"op":"add_function","name":"Read","inputs":[
            {"name":"Amount","type":{"category":"int"}},{"name":"Adjust","type":{"category":"int"}}],
            "outputs":[{"name":"Value","type":{"category":"int"}}]})json")));
        OriginalEditor.Apply(Json(TEXT(R"json({"op":"add_function","name":"PureRead","pure":true})json")));
        Finish(OriginalEditor);
        FEditor ReplacementEditor(Replacement, MakeShared<FJsonObject>());
        ReplacementEditor.Apply(Json(TEXT(R"json({"op":"add_function","name":"NewRead","inputs":[
            {"name":"Quantity","type":{"category":"int"}},{"name":"Offset","type":{"category":"int"}}],
            "outputs":[{"name":"Result","type":{"category":"int"}}]})json")));
        Finish(ReplacementEditor);

        UBlueprint* Caller = Fixtures.Create(TEXT("BP_Caller"), TEXT("normal"));
        FEditor CallerEditor(Caller, MakeShared<FJsonObject>());
        CallerEditor.Apply(Json(TEXT(R"json({"op":"add_function","name":"ReadCaller","entry_id":"entry","result_id":"out",
            "inputs":[{"name":"Incoming","type":{"category":"int"}}],"outputs":[{"name":"Outgoing","type":{"category":"int"}}]})json")));
        FJson Add = Json(TEXT(R"json({"op":"add_node","kind":"call","graph":"ReadCaller","function":"Read","id":"call",
            "x":200,"y":75,"comment":"Keep this layout"})json"));
        Add->SetStringField(TEXT("class"), Original->GeneratedClass->GetPathName());
        CallerEditor.Apply(Add);
        for (const TCHAR* Operation : {
            TEXT(R"json({"op":"connect","graph":"ReadCaller","from":{"node":"entry","pin":"Incoming"},"to":{"node":"call","pin":"Amount"}})json"),
            TEXT(R"json({"op":"connect","graph":"ReadCaller","from":{"node":"call","pin":"Value"},"to":{"node":"out","pin":"Outgoing"}})json"),
            TEXT(R"json({"op":"connect","graph":"ReadCaller","from":{"node":"entry","pin":"then"},"to":{"node":"call","pin":"execute"},"replace":true})json"),
            TEXT(R"json({"op":"connect","graph":"ReadCaller","from":{"node":"call","pin":"then"},"to":{"node":"out","pin":"execute"}})json"),
            TEXT(R"json({"op":"set_pin_default","graph":"ReadCaller","node":"call","pin":"Adjust","value":27})json")})
        { CallerEditor.Apply(Json(Operation)); }
        Finish(CallerEditor);

        UEdGraph* Graph = CallerEditor.Graph(Json(TEXT(R"json({"graph":"ReadCaller"})json")));
        UK2Node_CallFunction* Call = CastChecked<UK2Node_CallFunction>(CallerEditor.Node(Graph, TEXT("call")));
        const FGuid Id = Call->NodeGuid;
        const FString Filename = AssetFilename(Caller->GetOutermost()->GetName());
        FJson Request = Json(TEXT(R"json({"action":"apply","response_mode":"summary","operations":[]})json"));
        Request->SetStringField(TEXT("asset"), Caller->GetOutermost()->GetName());
        Request->SetStringField(TEXT("expected_revision"), Revision(Filename));
        FJson Retarget = Json(TEXT(R"json({"op":"retarget_call","graph":"ReadCaller","function":"NewRead",
            "pin_map":{"Amount":"Quantity","Value":"Result"}})json"));
        Retarget->SetStringField(TEXT("node"), Guid(Id));
        Retarget->SetStringField(TEXT("class"), Replacement->GeneratedClass->GetPathName());
        Object(Retarget, TEXT("pin_map"))->SetStringField(Guid(Call->FindPinChecked(TEXT("Adjust"))->PinId), TEXT("Offset"));
        Request->SetArrayField(TEXT("operations"), {JsonValue(Retarget)});
        const FJson Result = MakeShared<FJsonObject>();
        Execute(Request, Result);
        TestTrue(TEXT("Retargeted graph saves"), Result->GetBoolField(TEXT("saved")));
        TestEqual(TEXT("Retarget keeps the same node"), CallerEditor.Node(Graph, TEXT("call")), static_cast<UEdGraphNode*>(Call));
        TestEqual(TEXT("Retarget keeps the GUID"), Call->NodeGuid, Id);
        TestTrue(TEXT("Retarget keeps position and comment"), Call->NodePosX == 200 && Call->NodePosY == 75 && Call->NodeComment == TEXT("Keep this layout"));
        const FJson NodeInfo = DescribeNode(Call);
        TestEqual(TEXT("Inspection reports the new library class"), String(NodeInfo, TEXT("function_class")), Replacement->GeneratedClass->GetPathName());
        TestEqual(TEXT("Inspection reports the new function"), String(NodeInfo, TEXT("function")), FString(TEXT("NewRead")));
        for (const TCHAR* Pin : {TEXT("Quantity"), TEXT("Result"), TEXT("execute"), TEXT("then")})
        { TestTrue(Pin, !Call->FindPinChecked(Pin)->LinkedTo.IsEmpty()); }
        TestEqual(TEXT("Literal moves with its mapped pin"), Call->FindPinChecked(TEXT("Offset"))->DefaultValue, FString(TEXT("27")));

        const FString RevisionBeforeReject = Revision(Filename);
        Request->SetStringField(TEXT("expected_revision"), RevisionBeforeReject);
        Retarget->SetStringField(TEXT("class"), Original->GeneratedClass->GetPathName());
        Retarget->SetStringField(TEXT("function"), TEXT("PureRead"));
        Retarget->RemoveField(TEXT("pin_map"));
        bool Rejected = false;
        try { Execute(Request, MakeShared<FJsonObject>()); }
        catch (const FFailure& Failure)
        {
            Rejected = true;
            TestTrue(TEXT("Rejection identifies the missing pin"), Failure.Message.Contains(TEXT("Replacement pin missing")));
        }
        TestTrue(TEXT("Retarget with missing linked pins fails"), Rejected);
        TestEqual(TEXT("Rejected retarget does not save"), Revision(Filename), RevisionBeforeReject);
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentV2AnimationTest, "UAssetAgent.V2.Animation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentV2AnimationTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentV2Tests;
    FFixtures Fixtures;
    try
    {
        UBlueprint* BP = Fixtures.Create(TEXT("ABP_Child"), TEXT("animation"));
        UBlueprint* Parent = Fixtures.Create(TEXT("ABP_Parent"), TEXT("animation"));
        FEditor ParentEditor(Parent, MakeShared<FJsonObject>()), Editor(BP, MakeShared<FJsonObject>());
        FJson Variable = Json(TEXT("{\"op\":\"add_variable\",\"name\":\"SharedValue\",\"type\":{\"category\":\"int\"}}"));
        ParentEditor.Apply(Variable);
        ParentEditor.Apply(Json(TEXT("{\"op\":\"add_function\",\"name\":\"DuplicateFunction\"}")));
        Finish(ParentEditor);
        Editor.Apply(Variable);
        Editor.Apply(Json(TEXT("{\"op\":\"add_function\",\"name\":\"DuplicateFunction\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"add_node\",\"kind\":\"get\",\"variable\":\"SharedValue\",\"id\":\"getter\"}")));
        const FGuid GetterId = Editor.Aliases[TEXT("getter")];
        Editor.PlannedRetargets.Add(TEXT("SharedValue"));
        Editor.PlannedFunctionDeletes.Add(TEXT("DuplicateFunction"));
        TestTrue(TEXT("Reject non-animation parent"), Rejected(Editor, Reparent(AActor::StaticClass())));
        Editor.Apply(Reparent(Parent->GeneratedClass));
        TestFalse(TEXT("Duplicate function prepared before parent refresh"), BP->FunctionGraphs.ContainsByPredicate([](UEdGraph* G) { return G->GetName() == TEXT("DuplicateFunction"); }));
        Editor.Apply(Json(TEXT("{\"op\":\"delete_function\",\"name\":\"DuplicateFunction\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"delete_variable\",\"name\":\"SharedValue\",\"retarget_references\":true}")));
        TestEqual(TEXT("Local descriptor removed"), FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("SharedValue")), INDEX_NONE);
        TestEqual(TEXT("Getter GUID retained"), Editor.Node(Editor.Graph(Json(TEXT("{}"))), TEXT("getter"))->NodeGuid, GetterId);
        Editor.Apply(Json(TEXT("{\"op\":\"add_function\",\"name\":\"UnusedFunction\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"delete_function\",\"name\":\"UnusedFunction\"}")));
        TestFalse(TEXT("Function removed"), BP->FunctionGraphs.ContainsByPredicate([](UEdGraph* G) { return G->GetName() == TEXT("UnusedFunction"); }));
        Editor.Apply(Json(TEXT("{\"op\":\"set_default\",\"property\":\"bReceiveNotifiesFromLinkedInstances\",\"value\":\"True\"}")));
        Finish(Editor);
        TestTrue(TEXT("Generated parent changed"), BP->GeneratedClass->IsChildOf(Parent->GeneratedClass));
        TestTrue(TEXT("Inherited CDO default"), CastChecked<UAnimInstance>(BP->GeneratedClass->GetDefaultObject())->bReceiveNotifiesFromLinkedInstances != 0);

        // Build a real thread-safe Blueprint function from UE's exact prototype.
        BP = Fixtures.Create(TEXT("ABP_Nodes"), TEXT("animation"));
        Editor = FEditor(BP, MakeShared<FJsonObject>());
        UFunction* Prototype = LoadObject<UFunction>(nullptr, TEXT("/Script/AnimGraphRuntime.AnimExecutionContextLibrary.Prototype_ThreadSafeAnimUpdateCall"));
        Require(Prototype != nullptr, TEXT("Missing animation update prototype"));
        UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(BP, TEXT("TestUpdate"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UFunction>(BP, Function, true, Prototype);
        TArray<UK2Node_FunctionEntry*> Entries; Function->GetNodesOfClass(Entries);
        Require(Entries.Num() == 1, TEXT("No function entry"));
        Entries[0]->MetaData.bThreadSafe = true;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        Require(Editor.CompileCurrent(), TEXT("Thread-safe fixture function did not compile"));
        UEdGraph* Graph = Editor.Graph(Json(TEXT("{\"graph\":\"AnimGraph\"}")));
        UAnimGraphNode_SequencePlayer* Player = NewObject<UAnimGraphNode_SequencePlayer>(Graph);
        Graph->AddNode(Player); Player->CreateNewGuid(); Player->PostPlacedNewNode(); Player->AllocateDefaultPins();
        TArray<UAnimGraphNode_Root*> Roots; Graph->GetNodesOfClass(Roots);
        Require(Roots.Num() == 1, TEXT("Animation fixture needs one root"));
        Editor.Remember(TEXT("pose"), Player); Editor.Remember(TEXT("result"), Roots[0]);
        Editor.Apply(Json(TEXT("{\"op\":\"connect\",\"graph\":\"AnimGraph\",\"from\":{\"node\":\"pose\",\"pin\":\"Pose\"},\"to\":{\"node\":\"result\",\"pin\":\"Result\"}}")));
        FJson Bind = AnimOp(TEXT("set_anim_node_function"), Player);
        Bind->SetStringField(TEXT("slot"), TEXT("UpdateFunction")); Bind->SetStringField(TEXT("function"), TEXT("TestUpdate"));
        Editor.Apply(Bind);
        TestEqual(TEXT("Function bound"), Player->UpdateFunction.GetMemberName(), FName(TEXT("TestUpdate")));
        Require(Editor.CompileCurrent(), TEXT("Bound animation function did not compile"));
        Bind->SetStringField(TEXT("function"), TEXT("MissingFunction"));
        TestTrue(TEXT("Unknown function rejected"), Rejected(Editor, Bind));
        Bind->SetField(TEXT("function"), MakeShared<FJsonValueNull>()); Editor.Apply(Bind);
        TestTrue(TEXT("Function cleared"), Player->UpdateFunction.GetMemberName().IsNone());
        FJson Property = AnimOp(TEXT("set_anim_node_property"), Player);
        Property->SetStringField(TEXT("property"), TEXT("PlayRate")); Property->SetStringField(TEXT("value"), TEXT("0.5"));
        Editor.Apply(Property);
        FJson NodeJson = DescribeNode(Player);
        TestTrue(TEXT("Runtime properties inspected"), NodeJson->GetObjectField(TEXT("properties"))->HasField(TEXT("Node.PlayRate")));
        TestEqual(TEXT("Runtime property changed"), FCString::Atof(*String(Object(NodeJson, TEXT("properties")), TEXT("Node.PlayRate"))), 0.5f);
        Property->SetStringField(TEXT("property"), TEXT("NotAProperty"));
        TestTrue(TEXT("Invalid anim property rejected"), Rejected(Editor, Property));
        Finish(Editor);
        // A template's sequence node needs no concrete animation asset.
        // Remove it afterward to exercise pose disconnection and node deletion.
        FJson Delete = AnimOp(TEXT("delete_node"), Player); Editor.Apply(Delete);
        Finish(Editor);
        FJson Summary = DescribeBlueprint(BP, Json(TEXT("{\"response_mode\":\"summary\"}")));
        for (const auto& Value : Array(Summary, TEXT("graphs"))) { TestFalse(TEXT("Summary omits nodes"), AsObject(Value)->HasField(TEXT("nodes"))); }
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentV2WidgetTest, "UAssetAgent.V2.Widget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentV2WidgetTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentV2Tests;
    FFixtures Fixtures;
    try
    {
        UBlueprint* BP = Fixtures.Create(TEXT("W_Test"), TEXT("widget"));
        FEditor Editor(BP, MakeShared<FJsonObject>());
        Editor.Apply(Json(TEXT("{\"op\":\"add_widget\",\"id\":\"root\",\"name\":\"Root\",\"class\":\"/Script/UMG.Overlay\"}")));
        for (int32 Index = 0; Index < 6; ++Index)
        {
            FJson Add = Json(TEXT("{\"op\":\"add_widget\",\"class\":\"/Script/UMG.Button\",\"parent\":\"root\",\"slot\":{\"Padding\":\"(Left=4,Top=0,Right=4,Bottom=0)\"}}"));
            Add->SetStringField(TEXT("id"), FString::Printf(TEXT("entry%d"), Index));
            Add->SetStringField(TEXT("name"), FString::Printf(TEXT("Entry%d"), Index)); Add->SetNumberField(TEXT("index"), Index);
            Editor.Apply(Add);
        }
        UPanelWidget* Root = CastChecked<UPanelWidget>(Editor.Widget(TEXT("root")));
        TestEqual(TEXT("Entry4 index"), Root->GetChildIndex(Editor.Widget(TEXT("entry4"))), 4);
        TestEqual(TEXT("Entry5 index"), Root->GetChildIndex(Editor.Widget(TEXT("entry5"))), 5);
        Editor.Apply(Json(TEXT("{\"op\":\"move_widget\",\"widget\":\"entry5\",\"parent\":\"root\",\"index\":4}")));
        TestEqual(TEXT("Move order"), Root->GetChildIndex(Editor.Widget(TEXT("entry5"))), 4);
        Editor.Apply(Json(TEXT("{\"op\":\"set_widget_property\",\"widget\":\"entry4\",\"property\":\"Visibility\",\"value\":\"Hidden\"}")));
        TestTrue(TEXT("Visibility set"), Editor.Widget(TEXT("entry4"))->GetVisibility() == ESlateVisibility::Hidden);
        Editor.Apply(Json(TEXT("{\"op\":\"set_slot_property\",\"widget\":\"entry4\",\"property\":\"Padding\",\"value\":\"(Left=8,Top=0,Right=8,Bottom=0)\"}")));
        TestTrue(TEXT("Invalid property rejected"), Rejected(Editor, Json(TEXT("{\"op\":\"set_widget_property\",\"widget\":\"entry4\",\"property\":\"NoSuchProperty\",\"value\":\"1\"}"))));
        Editor.Apply(Json(TEXT("{\"op\":\"rename_widget\",\"widget\":\"entry4\",\"name\":\"Chainshot\"}")));
        TestEqual(TEXT("Alias follows rename"), Editor.Widget(TEXT("entry4"))->GetName(), FString(TEXT("Chainshot")));
        Editor.Apply(Json(TEXT("{\"op\":\"bind_widget_event\",\"widget\":\"entry4\",\"event\":\"OnClicked\",\"id\":\"clicked\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"bind_widget_event\",\"widget\":\"entry4\",\"event\":\"OnClicked\",\"id\":\"clickedAgain\"}")));
        TestEqual(TEXT("Existing binding reused"), Editor.Aliases[TEXT("clicked")], Editor.Aliases[TEXT("clickedAgain")]);
        TestTrue(TEXT("Referenced widget removal rejected"), Rejected(Editor, Json(TEXT("{\"op\":\"remove_widget\",\"widget\":\"entry4\"}"))));
        Editor.Apply(Json(TEXT("{\"op\":\"add_node\",\"kind\":\"branch\",\"id\":\"branch\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"connect\",\"from\":{\"node\":\"clicked\",\"pin\":\"then\"},\"to\":{\"node\":\"branch\",\"pin\":\"execute\"}}")));
        Editor.Apply(Json(TEXT("{\"op\":\"delete_node\",\"node\":\"clicked\"}")));
        Editor.Apply(Json(TEXT("{\"op\":\"remove_widget\",\"widget\":\"entry4\"}")));
        TestEqual(TEXT("Widget removed"), Root->GetChildrenCount(), 5);
        TestTrue(TEXT("Reject animation parent"), Rejected(Editor, Reparent(UAnimInstance::StaticClass())));
        UBlueprint* Parent = Fixtures.Create(TEXT("W_Parent"), TEXT("widget"));
        Editor.Apply(Reparent(Parent->GeneratedClass)); Finish(Editor);
        FJson Full = DescribeBlueprint(BP, Json(TEXT("{}")));
        TestEqual(TEXT("Tree inspected"), Array(Full, TEXT("widget_tree")).Num(), 6);
        TestTrue(TEXT("Widget metadata inspected"), Full->HasField(TEXT("animations")) && Full->HasField(TEXT("bindings")));

        // All documented native panel families use the same insertion path.
        for (const TCHAR* Panel : {TEXT("CanvasPanel"), TEXT("HorizontalBox"), TEXT("VerticalBox"), TEXT("GridPanel"), TEXT("ScrollBox"), TEXT("SizeBox"), TEXT("Border")})
        {
            FJson Add = Json(TEXT("{\"op\":\"add_widget\",\"parent\":\"Root\"}"));
            Add->SetStringField(TEXT("id"), Panel); Add->SetStringField(TEXT("name"), Panel);
            Add->SetStringField(TEXT("class"), FString(TEXT("/Script/UMG.")) + Panel); Editor.Apply(Add);
            FJson Child = Json(TEXT("{\"op\":\"add_widget\",\"class\":\"/Script/UMG.TextBlock\"}"));
            Child->SetStringField(TEXT("id"), FString(Panel) + TEXT("Label")); Child->SetStringField(TEXT("name"), FString(Panel) + TEXT("Label"));
            Child->SetStringField(TEXT("parent"), Panel); Editor.Apply(Child);
            if (FString(Panel) == TEXT("SizeBox") || FString(Panel) == TEXT("Border"))
            {
                Child->SetStringField(TEXT("id"), FString(Panel) + TEXT("Overflow")); Child->SetStringField(TEXT("name"), FString(Panel) + TEXT("Overflow"));
                TestTrue(TEXT("Single-child panel capacity enforced"), Rejected(Editor, Child));
            }
        }
        Editor.Apply(Json(TEXT("{\"op\":\"move_widget\",\"widget\":\"HorizontalBoxLabel\",\"parent\":\"VerticalBox\",\"index\":0}")));
        TestEqual(TEXT("Move across panels"), Editor.Widget(TEXT("HorizontalBoxLabel"))->GetParent()->GetName(), FString(TEXT("VerticalBox")));
        FJson Nested = Json(TEXT("{\"op\":\"add_widget\",\"parent\":\"Root\",\"id\":\"nested\",\"name\":\"NestedWidget\"}"));
        Nested->SetStringField(TEXT("class"), Parent->GeneratedClass->GetPathName()); Editor.Apply(Nested);
        Finish(Editor);
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentV2MaintenanceTest, "UAssetAgent.V2.Maintenance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentV2MaintenanceTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentV2Tests;
    FFixtures Fixtures;
    try
    {
        UBlueprint* BP = Fixtures.Create(TEXT("W_Delete"), TEXT("widget"));
        const FString Package = BP->GetOutermost()->GetName(), Filename = AssetFilename(Package);
        FJson Delete = Json(TEXT("{\"action\":\"delete_asset\",\"dry_run\":true}"));
        Delete->SetStringField(TEXT("asset"), Package);
        FJson Dry = MakeShared<FJsonObject>(); Execute(Delete, Dry);
        TestTrue(TEXT("Dry deletion retains file"), IFileManager::Get().FileExists(*Filename));
        Delete->SetStringField(TEXT("expected_revision"), String(Dry, TEXT("revision")));
        Delete->SetBoolField(TEXT("dry_run"), false);
        FJson Deleted = MakeShared<FJsonObject>(); Execute(Delete, Deleted);
        TestTrue(TEXT("Deletion reported"), Boolean(Deleted, TEXT("deleted")));
        TestFalse(TEXT("File removed"), IFileManager::Get().FileExists(*Filename));
        TestTrue(TEXT("Backup returned"), Deleted->HasField(TEXT("backups")));

        UBlueprint* Target = Fixtures.Create(TEXT("W_Target"), TEXT("widget"));
        const FString RedirectPackage = Fixtures.Root + TEXT("/W_Old");
        UObjectRedirector* Redirector = NewObject<UObjectRedirector>(CreatePackage(*RedirectPackage), TEXT("W_Old"), RF_Public | RF_Standalone);
        Redirector->DestinationObject = Target; Save(Redirector);
        UBlueprint* Referencing = Fixtures.Create(TEXT("W_Referencing"), TEXT("widget"));
        FEdGraphPinType SoftType;
        SoftType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
        SoftType.PinSubCategoryObject = UUserWidget::StaticClass();
        Require(FBlueprintEditorUtils::AddMemberVariable(Referencing, TEXT("TargetClass"), SoftType), TEXT("Cannot create soft class fixture variable"));
        FEditor RefEditor(Referencing, MakeShared<FJsonObject>());
        Require(RefEditor.CompileCurrent(), TEXT("Cannot compile soft class fixture"));
        RefEditor.SetDefault(TEXT("TargetClass"), RedirectPackage + TEXT(".W_Old_C"));
        Save(Referencing);
        FJson Fixup = Json(TEXT("{\"action\":\"fixup_redirectors\",\"dry_run\":true}")); Fixup->SetStringField(TEXT("path"), Fixtures.Root);
        FJson Plan = MakeShared<FJsonObject>(); Execute(Fixup, Plan);
        TestTrue(TEXT("Fixup dry run retains redirector"), IFileManager::Get().FileExists(*AssetFilename(RedirectPackage)));
        Fixup->SetObjectField(TEXT("expected_revisions"), Object(Plan, TEXT("revisions"))); Fixup->SetBoolField(TEXT("dry_run"), false);
        FJson Fixed = MakeShared<FJsonObject>(); Execute(Fixup, Fixed);
        TestEqual(TEXT("Redirector deleted"), Integer(Fixed, TEXT("deleted_redirectors")), 1);
        TestFalse(TEXT("Redirector file removed"), IFileManager::Get().FileExists(*AssetFilename(RedirectPackage)));
        TestTrue(TEXT("Referencer was saved"), Integer(Fixed, TEXT("packages_to_save")) >= 1);
        FSoftClassProperty* RefProperty = FindFProperty<FSoftClassProperty>(Referencing->GeneratedClass, TEXT("TargetClass"));
        Require(RefProperty != nullptr, TEXT("Soft class property missing"));
        const FSoftObjectPtr Value = RefProperty->GetPropertyValue_InContainer(Referencing->GeneratedClass->GetDefaultObject());
        TestEqual(TEXT("Soft generated-class reference rewritten"), Value.ToSoftObjectPath().ToString(), Target->GeneratedClass->GetPathName());
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}
