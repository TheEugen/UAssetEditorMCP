IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentReplaceReferencesTest, "UAssetAgent.ScopedReplaceReferences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUAssetAgentReplaceReferencesTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Root = TestRoot();
        const FString Old = Root + TEXT("/BP_Ocean_Old");
        const FString New = Root + TEXT("/BP_Ocean_New");
        const FString Missing = Root + TEXT("/BP_Ocean_Missing");
        const FString Ship = Root + TEXT("/Ships/BP_Ship");
        const FString Outside = Root + TEXT("/Outside/BP_Ship");
        const FString Map = Root + TEXT("/Ships/Harbor");
        const auto Ocean = [](const FString& Package, bool Members)
        {
            UBlueprint* BP = Blueprint(Package);
            if (Members)
            {
                FEditor Editor(BP, MakeShared<FJsonObject>());
                Editor.Apply(Json(TEXT(R"json({"op":"add_variable","name":"OceanSpeed","type":{"category":"float"}})json")));
                Editor.Apply(Json(TEXT(R"json({"op":"add_function","name":"GetOceanSpeed","pure":true,"outputs":[{"name":"Speed","type":{"category":"float"}}]})json")));
                Require(Editor.CompileCurrent(), TEXT("Could not compile ocean fixture"));
            }
            Save(BP);
            return BP;
        };
        UBlueprint* OldBP = Ocean(Old, true);
        UBlueprint* NewBP = Ocean(New, true);
        Ocean(Missing, false);

        const auto BuildShip = [&](const FString& Package)
        {
            UBlueprint* BP = Blueprint(Package);
            FEditor Editor(BP, MakeShared<FJsonObject>());
            FJson Add = Json(TEXT(R"json({"op":"add_variable","name":"Ocean","type":{"category":"object"}})json"));
            Object(Add, TEXT("type"))->SetStringField(TEXT("path"), OldBP->GeneratedClass->GetPathName());
            Editor.Apply(Add);
            Require(Editor.CompileCurrent(), TEXT("Could not create ship property"));
            Editor.Apply(Json(TEXT(R"json({"op":"add_node","id":"ocean","kind":"get","variable":"Ocean"})json")));
            UEdGraph* Graph = BP->UbergraphPages[0];
            UK2Node_VariableGet* Access = NewObject<UK2Node_VariableGet>(Graph);
            Graph->AddNode(Access, false, false);
            Access->CreateNewGuid();
            Access->VariableReference.SetExternalMember(TEXT("OceanSpeed"), OldBP->GeneratedClass,
                FBlueprintEditorUtils::FindMemberVariableGuidByName(OldBP, TEXT("OceanSpeed")));
            Access->AllocateDefaultPins();
            FJson Call = Json(TEXT(R"json({"op":"add_node","id":"call","kind":"call","function":"GetOceanSpeed"})json"));
            Call->SetStringField(TEXT("class"), OldBP->GeneratedClass->GetPathName());
            Editor.Apply(Call);
            Editor.Apply(Json(TEXT(R"json({"op":"connect","from":{"node":"ocean","pin":"Ocean"},"to":{"node":"call","pin":"self"}})json")));
            UK2Node_VariableGet* OceanRef = nullptr;
            TArray<UK2Node_VariableGet*> Gets;
            FBlueprintEditorUtils::GetAllNodesOfClass(BP, Gets);
            for (UK2Node_VariableGet* Get : Gets)
            { if (Get->GetVarName() == TEXT("Ocean")) { OceanRef = Get; } }
            Require(OceanRef && Access->FindPin(UEdGraphSchema_K2::PN_Self) &&
                CastChecked<UEdGraphSchema_K2>(Graph->GetSchema())->TryCreateConnection(
                    OceanRef->FindPin(TEXT("Ocean")), Access->FindPin(UEdGraphSchema_K2::PN_Self)),
                TEXT("Could not wire external member fixture"));
            Require(Editor.CompileCurrent(), TEXT("Could not compile ship fixture"));
            Save(BP);
        };
        BuildShip(Ship);
        BuildShip(Outside);
        UPackage* MapPackage = CreatePackage(*Map);
        const auto Init = UWorld::InitializationValues().CreateNavigation(false).CreateAISystem(false).AllowAudioPlayback(false);
        UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("Harbor"), MapPackage, false, ERHIFeatureLevel::Num, &Init);
        Require(World != nullptr && World->SpawnActor<AActor>(OldBP->GeneratedClass) != nullptr, TEXT("Could not create map referrer"));
        World->SetFlags(RF_Public | RF_Standalone);
        FSavePackageArgs MapSave;
        MapSave.TopLevelFlags = RF_Standalone;
        Require(UPackage::SavePackage(MapPackage, World, *LevelFilename(Map), MapSave), TEXT("Could not save map referrer"));
        World->DestroyWorld(false);
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.ScanFilesSynchronous({LevelFilename(Map)}, true);
        const FString ExpectedClass = NewBP->GeneratedClass->GetPathName();
        const FString ShipRevision = Revision(AssetFilename(Ship));
        const FString OldRevision = Revision(AssetFilename(Old));
        const FString NewRevision = Revision(AssetFilename(New));
        const FString OutsideRevision = Revision(AssetFilename(Outside));
        CollectGarbage(RF_NoFlags);

        const auto RequestFor = [&](const FString& Target)
        {
            FJson Request = Json(TEXT("{\"action\":\"replace_references\"}"));
            Request->SetStringField(TEXT("from"), Old);
            Request->SetStringField(TEXT("to"), Target);
            Request->SetArrayField(TEXT("scope"), LevelStrings({Root + TEXT("/Ships")}));
            return Request;
        };
        FJson Reject = MakeShared<FJsonObject>();
        bool Rejected = false;
        try { Execute(RequestFor(Missing), Reject); }
        catch (const FFailure&) { Rejected = true; }
        TestTrue(TEXT("Missing target member or call rejects replacement"), Rejected);
        TestEqual(TEXT("Failure leaves ship bytes intact"), Revision(AssetFilename(Ship)), ShipRevision);
        CollectGarbage(RF_NoFlags);

        FJson Request = RequestFor(New);
        FJson Preview = MakeShared<FJsonObject>();
        try { Execute(Request, Preview); }
        catch (const FFailure& Error)
        {
            FString Report;
            FJsonSerializer::Serialize(Preview.ToSharedRef(), TJsonWriterFactory<>::Create(&Report));
            throw FFailure{Error.Message + TEXT(" ") + Report};
        }
        TestFalse(TEXT("Preview leaves disk untouched"), Preview->GetBoolField(TEXT("saved")));
        TestEqual(TEXT("Preview preserves ship bytes"), Revision(AssetFilename(Ship)), ShipRevision);
        const auto Reports = Array(Preview, TEXT("packages"), true);
        Require(Reports.Num() == 1, TEXT("Expected exactly one scoped referencer"));
        const auto Excluded = Array(Preview, TEXT("excluded_maps"), true);
        Require(Excluded.Num() == 1 && Excluded[0]->AsString() == Map, TEXT("Expected the map to be excluded"));
        const auto Compiles = Array(AsObject(Reports[0]), TEXT("compiles"), true);
        Require(Compiles.Num() == 1 && AsObject(Compiles[0])->GetBoolField(TEXT("ok")), TEXT("Scoped Blueprint compile did not pass"));
        CollectGarbage(RF_NoFlags);

        Request->SetBoolField(TEXT("dry_run"), false);
        Request->SetObjectField(TEXT("expected_revisions"), Object(Preview, TEXT("revisions")));
        FJson Saved = MakeShared<FJsonObject>();
        Execute(Request, Saved);
        Require(Saved->GetBoolField(TEXT("saved")), TEXT("Scoped replacement did not save"));
        TestTrue(TEXT("Previous ship package backed up"), Object(Saved, TEXT("backups"))->HasField(Ship));
        TestEqual(TEXT("Source asset retained"), Revision(AssetFilename(Old)), OldRevision);
        TestEqual(TEXT("Destination asset retained"), Revision(AssetFilename(New)), NewRevision);
        TestEqual(TEXT("Out-of-scope asset retained"), Revision(AssetFilename(Outside)), OutsideRevision);
        CollectGarbage(RF_NoFlags);
        Registry.ScanFilesSynchronous({AssetFilename(Ship)}, true);
        const auto Dependencies = PackageReferences(Registry, FName(*Ship), false, true);
        TestTrue(TEXT("Saved ship depends on the replacement"), Dependencies.Contains(FName(*New)));
        TestFalse(TEXT("Saved ship has no old package dependency"), Dependencies.Contains(FName(*Old)));
        UBlueprint* ShipBP = CastChecked<UBlueprint>(LoadExactAsset(Ship));
        Require(ShipBP->NewVariables.Num() == 1 && ShipBP->NewVariables[0].VarType.PinSubCategoryObject.IsValid(),
            TEXT("Could not reload ship property"));
        TestEqual(TEXT("Variable type references replacement"), ShipBP->NewVariables[0].VarType.PinSubCategoryObject->GetPathName(), ExpectedClass);
        TArray<UK2Node_VariableGet*> Gets;
        FBlueprintEditorUtils::GetAllNodesOfClass(ShipBP, Gets);
        int32 RetargetedGets = 0;
        for (const UK2Node_VariableGet* Get : Gets)
        {
            if (Get->GetVarName() != TEXT("OceanSpeed")) { continue; }
            ++RetargetedGets;
            Require(Get->VariableReference.GetMemberParentClass() != nullptr, TEXT("Reloaded external member has no owner"));
            TestEqual(TEXT("External member retargeted"), Get->VariableReference.GetMemberParentClass()->GetPathName(), ExpectedClass);
            TestEqual(TEXT("External member wiring retained"), Get->FindPin(UEdGraphSchema_K2::PN_Self)->LinkedTo.Num(), 1);
        }
        TestEqual(TEXT("Exactly one external getter reloaded"), RetargetedGets, 1);
        TArray<UK2Node_CallFunction*> Calls;
        FBlueprintEditorUtils::GetAllNodesOfClass(ShipBP, Calls);
        Require(Calls.Num() == 1, TEXT("Expected one reloaded call node"));
        Require(Calls[0]->GetTargetFunction() != nullptr, TEXT("Reloaded call has no target"));
        TestEqual(TEXT("Call retargeted"), Calls[0]->GetTargetFunction()->GetOutermost()->GetName(), New);
        TestEqual(TEXT("Call wiring retained"), Calls[0]->FindPin(UEdGraphSchema_K2::PN_Self)->LinkedTo.Num(), 1);
    }
    catch (const UAssetAgent::FFailure& Failure) { AddError(Failure.Message); }
    return true;
}
