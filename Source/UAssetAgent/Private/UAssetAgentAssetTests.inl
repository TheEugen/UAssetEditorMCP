IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentReflectedAssetsTest, "UAssetAgent.ReflectedAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUAssetAgentReflectedAssetsTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Package = TestRoot() + TEXT("/Label");
        const auto Run = [&](const FJson& Request)
        {
            Request->SetStringField(TEXT("asset"), Package);
            FJson Response = MakeShared<FJsonObject>();
            Execute(Request, Response);
            return Response;
        };
        FJson Create = Json(TEXT(R"json({"action":"asset_create","class":"/Script/Engine.PrimaryAssetLabel",
            "properties":{"Rules.Priority":"5","bIsRuntimeLabel":"True"},"dry_run":false})json"));
        FJson Created = Run(Create);
        FJson Inspect = Json(TEXT(R"({"action":"asset_inspect"})"));
        FJson Initial = Run(Inspect);
        const FString Before = String(Created, TEXT("revision"));
        TestEqual(TEXT("Inspection checks disk revision"), String(Initial, TEXT("revision")), Before);
        TestEqual(TEXT("Reflected class"), String(Initial, TEXT("class")), FString(TEXT("/Script/Engine.PrimaryAssetLabel")));
        auto Value = [](const FJson& Result, const TCHAR* Property)
        { return ValueString(Object(Object(Result, TEXT("properties")), Property), TEXT("value")); };
        TestTrue(TEXT("Struct dot path persisted in initial properties"), Value(Initial, TEXT("Rules")).Contains(TEXT("Priority=5")));
        FJson Edit = Json(TEXT(R"json({"action":"asset_apply","operations":[
            {"op":"array_add","property":"ExplicitAssets","value":"/Game/Test/A.A"},
            {"op":"array_add","property":"ExplicitAssets","value":"/Game/Test/B.B"},
            {"op":"array_remove","property":"ExplicitAssets","index":0},
            {"op":"array_set","property":"ExplicitAssets","index":0,"value":"/Game/Test/C.C"}
        ]})json"));
        Edit->SetStringField(TEXT("expected_revision"), Before);
        FJson Preview = Run(Edit);
        TestTrue(TEXT("Array preview reports final element"), Value(Preview, TEXT("ExplicitAssets")).Contains(TEXT("/Game/Test/C.C")));
        TestEqual(TEXT("Preview leaves package bytes unchanged"), Revision(AssetFilename(Package)), Before);
        FJson Save = Json(TEXT(R"json({"action":"asset_apply","dry_run":false,"operations":[
            {"op":"set_property","property":"Rules.Priority","value":"7"}]})json"));
        Save->SetStringField(TEXT("expected_revision"), Before);
        FJson Saved = Run(Save);
        TestTrue(TEXT("Apply creates a new disk revision"), String(Saved, TEXT("revision")) != Before);
        TestEqual(TEXT("Backup matches original file"), Revision(String(Object(Saved, TEXT("backups")), *Package)), Before);
        bool StaleRejected = false;
        try { Run(Save); } catch (const FFailure&) { StaleRejected = true; }
        TestTrue(TEXT("Stale edit rejected"), StaleRejected);
        bool ExistingRejected = false;
        try { Run(Create); } catch (const FFailure&) { ExistingRejected = true; }
        TestTrue(TEXT("Existing package protected"), ExistingRejected);
    }
    catch (const UAssetAgent::FFailure& Failure) { AddError(Failure.Message); }
    return true;
}
