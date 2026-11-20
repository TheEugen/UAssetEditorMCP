IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentCompactResponsesTest, "UAssetAgent.CompactResponses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUAssetAgentCompactResponsesTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Root = TestRoot();
        UBlueprint* Target = Blueprint(Root + TEXT("/Waterline/BP_Ocean"));
        Save(Target);
        const FString Package = Root + TEXT("/Ships/BP_Ship");
        UBlueprint* Ship = Blueprint(Package);
        FEditor Editor(Ship, MakeShared<FJsonObject>());
        FJson Add = Json(TEXT(R"json({"op":"add_variable","name":"Ocean","type":{"category":"object"}})json"));
        Object(Add, TEXT("type"))->SetStringField(TEXT("path"), Target->GeneratedClass->GetPathName());
        Editor.Apply(Add);
        Require(Editor.CompileCurrent(), TEXT("Could not compile referenced property"));
        Editor.Apply(Json(TEXT(R"json({"op":"add_node","id":"ocean","kind":"get","variable":"Ocean"})json")));
        Editor.Apply(Json(TEXT(R"json({"op":"add_node","id":"call","kind":"call","class":"/Script/Engine.KismetSystemLibrary","function":"PrintString"})json")));
        Require(Editor.CompileCurrent(), TEXT("Could not compile compact view fixture"));
        Save(Ship);

        const auto Run = [&](const FJson& Request)
        {
            Request->SetStringField(TEXT("asset"), Package);
            FJson Response = MakeShared<FJsonObject>();
            Execute(Request, Response);
            return Response;
        };
        const auto Nodes = [](const FJson& Result)
        {
            const auto Graphs = Array(Object(Result, TEXT("blueprint")), TEXT("graphs"), true);
            return Graphs.IsEmpty() ? TArray<TSharedPtr<FJsonValue>>() : Array(AsObject(Graphs[0]), TEXT("nodes"), true);
        };
        FJson Request = Json(TEXT(R"json({"action":"inspect","node_filter":{"class":"K2Node_VariableGet","variable":"Ocean"}})json"));
        Object(Request, TEXT("node_filter"))->SetStringField(TEXT("references_path"), Root + TEXT("/Waterline/"));
        FJson Filtered = Run(Request);
        const auto Matched = Nodes(Filtered);
        Require(Matched.Num() == 1, TEXT("Combined node filters did not find the getter"));
        TestEqual(TEXT("Variable metadata identifies the getter"), String(AsObject(Matched[0]), TEXT("variable")), FString(TEXT("Ocean")));
        TestFalse(TEXT("Filtered response omits components"), Object(Filtered, TEXT("blueprint"))->HasField(TEXT("components")));
        Object(Request, TEXT("node_filter"))->SetStringField(TEXT("references_path"), Root + TEXT("/Water"));
        TestEqual(TEXT("Path filtering respects segment boundaries"), Nodes(Run(Request)).Num(), 0);
        FJson Call = Run(Json(TEXT(R"json({"action":"inspect","response_mode":"graph","graph":"EventGraph","node_filter":{"function":"PrintString"}})json")));
        TestEqual(TEXT("Graph view matches call target"), Nodes(Call).Num(), 1);
        FJson Summary = Run(Json(TEXT(R"json({"action":"inspect","response_mode":"summary","include_components":false})json")));
        TestFalse(TEXT("Summary can omit components"), Object(Summary, TEXT("blueprint"))->HasField(TEXT("components")));

        FJson Change = Json(TEXT(R"json({"action":"apply","response_mode":"minimal","dry_run":true,
            "operations":[{"op":"add_node","id":"status","kind":"branch"}]})json"));
        Change->SetStringField(TEXT("expected_revision"), String(Summary, TEXT("revision")));
        Change->SetBoolField(TEXT("repair"), true);
        FJson Applied = Run(Change);
        TestTrue(TEXT("Repair reports initial compile status"), Boolean(Applied, TEXT("initial_compile_ok")));
        TestTrue(TEXT("Repair includes initial diagnostics separately"), Applied->HasField(TEXT("initial_diagnostics")));
        TestFalse(TEXT("Minimal response has no graph snapshot"), Applied->HasField(TEXT("blueprint")));
        TestTrue(TEXT("Minimal response retains node aliases"), Object(Applied, TEXT("node_ids"))->HasField(TEXT("status")));
        TestTrue(TEXT("Minimal response retains compile diagnostics"), Applied->HasField(TEXT("diagnostics")));
        TestEqual(TEXT("Minimal preview returns revision"), String(Applied, TEXT("revision")), String(Summary, TEXT("revision")));
        Ship->ParentClass = nullptr;
        FJson Repair = Json(TEXT(R"json({"action":"apply","response_mode":"minimal","dry_run":true,
            "repair":true,"operations":[{"op":"reparent","parent_class":"/Script/Engine.Actor"}]})json"));
        Repair->SetStringField(TEXT("expected_revision"), String(Summary, TEXT("revision")));
        FJson Repaired = Run(Repair);
        TestFalse(TEXT("Missing parent is reported before reparent"), Boolean(Repaired, TEXT("initial_compile_ok")));
        TestTrue(TEXT("Initial missing-parent diagnostic survives repair"), !Array(Repaired, TEXT("initial_diagnostics")).IsEmpty());
        TestTrue(TEXT("Restored actor parent compiles"), Ship->ParentClass == AActor::StaticClass() && Ship->Status != BS_Error);
        TestEqual(TEXT("Repair preview preserves disk revision"), Revision(AssetFilename(Package)), String(Summary, TEXT("revision")));
    }
    catch (const UAssetAgent::FFailure& Failure) { AddError(Failure.Message); }
    return true;
}
