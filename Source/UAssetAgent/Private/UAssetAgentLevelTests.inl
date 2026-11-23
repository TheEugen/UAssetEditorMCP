IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentPlacedActorsTest, "UAssetAgent.PlacedActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUAssetAgentPlacedActorsTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        for (bool Partitioned : {false, true})
        {
            const FString Package = TestRoot() + TEXT("/Actors");
            UPackage* MapPackage = CreatePackage(*Package);
            const UWorld::InitializationValues Init = UWorld::InitializationValues()
                .CreateWorldPartition(Partitioned).EnableWorldPartitionStreaming(Partitioned)
                .CreateNavigation(false).CreateAISystem(false).AllowAudioPlayback(false);
            UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("Actors"), MapPackage, false,
                ERHIFeatureLevel::Num, &Init);
            FSavePackageArgs Save;
            Save.TopLevelFlags = RF_Public | RF_Standalone;
            const FString Filename = LevelFilename(Package);
            Require(World != nullptr, TEXT("Cannot create test map"));
            World->SetFlags(RF_Public | RF_Standalone);
            IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
            for (AActor* Actor : World->PersistentLevel->Actors)
            {
                if (Actor && Actor->GetExternalPackage())
                {
                    const FString External = AssetFilename(Actor->GetPackage()->GetName());
                    FSavePackageArgs ExternalSave;
                    ExternalSave.TopLevelFlags = RF_NoFlags;
                    Require(UPackage::SavePackage(Actor->GetPackage(), Actor, *External, ExternalSave),
                        TEXT("Cannot save test map's external actor"));
                    Registry.ScanFilesSynchronous({External}, true);
                }
            }
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
            Require(UPackage::SavePackage(MapPackage, World, *Filename, Save), TEXT("Cannot save test map"));
            World->DestroyWorld(false);
            // Purge the destroyed world while preserving standalone editor assets from other tests.
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

            const auto Request = [&](const TCHAR* Body)
            {
                FJson Result = Json(Body);
                Result->SetStringField(TEXT("asset"), Package);
                return Result;
            };
            const auto Inspect = [&](const TCHAR* Search)
            {
                FJson Response = MakeShared<FJsonObject>();
                FJson Command = Request(TEXT("{\"action\":\"level_inspect\"}"));
                Command->SetStringField(TEXT("search"), Search);
                Execute(Command, Response);
                CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
                return Response;
            };
            const auto Apply = [&](const TCHAR* Body, const FString& Revision)
            {
                FJson Response = MakeShared<FJsonObject>();
                FJson Command = Request(Body);
                Command->SetStringField(TEXT("expected_revision"), Revision);
                Execute(Command, Response);
                // In production each request uses a fresh registry. Refresh the
                // editor's shared registry before reopening the fixture here.
                for (const auto& Changed : Array(Response, TEXT("changed_packages")))
                {
                    const FString ChangedPackage = Changed->AsString();
                    if (ChangedPackage == Package) { continue; }
                    const FString External = AssetFilename(ChangedPackage);
                    if (IFileManager::Get().FileExists(*External)) { Registry.ScanFilesSynchronous({External}, true); }
                    else if (UPackage* Removed = FindPackage(nullptr, *ChangedPackage)) { Registry.PackageDeleted(Removed); }
                }
                CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
                return Response;
            };
            FJson Before = Inspect(TEXT("DockStart"));
            TestEqual(TEXT("Map partition flag"), Before->GetBoolField(TEXT("world_partition")), Partitioned);
            TestEqual(TEXT("Empty fixture"), Array(Before, TEXT("actors"), true).Num(), 0);
            const TCHAR* Spawn = TEXT(R"json({"action":"level_apply","operations":[
                {"op":"spawn_actor","class":"/Script/Engine.PlayerStart","name":"DockStart",
                 "transform":{"location":[10,20,30],"rotation":[0,90,0],"scale":[1,1,1]}},
                {"op":"set_actor_property","actor":"DockStart","property":"PlayerStartTag","value":"Dock"},
                {"op":"set_tags","actor":"DockStart","tags":["Dock","Start"]},
                {"op":"spawn_actor","class":"/Script/Engine.TargetPoint","name":"OceanSolver"}
            ]})json");
            const FString StartRevision = String(Before, TEXT("revision"));
            TestFalse(TEXT("Default dry run leaves disk untouched"), Apply(Spawn, StartRevision)->GetBoolField(TEXT("saved")));
            TestEqual(TEXT("Dry-run actor absent"), Array(Inspect(TEXT("DockStart")), TEXT("actors"), true).Num(), 0);
            FString SaveSpawn(Spawn);
            SaveSpawn.InsertAt(1, TEXT("\"dry_run\":false,"));
            FJson Spawned = Apply(*SaveSpawn, StartRevision);
            Require(Spawned->GetBoolField(TEXT("saved")), TEXT("Spawn did not save"));
            FJson ReadSpawn = Inspect(TEXT("DockStart"));
            const auto SpawnActors = Array(ReadSpawn, TEXT("actors"), true);
            Require(SpawnActors.Num() == 1, TEXT("Saved actor did not reload"));
            FJson First = AsObject(SpawnActors[0]);
            const FString FirstRevision = String(ReadSpawn, TEXT("revision"));
            TestTrue(TEXT("Saved spawn changes revision"), FirstRevision != StartRevision);
            TestEqual(TEXT("Saved tag"), String(Object(First, TEXT("instance_properties")), TEXT("PlayerStartTag")), FString(TEXT("Dock")));
            TestEqual(TEXT("Saved tags"), Array(First, TEXT("tags"), true).Num(), 2);
            const FString ActorPackage = String(First, TEXT("package"));
            TestEqual(TEXT("Actor has expected storage"), ActorPackage != Package, Partitioned);
            bool StaleRejected = false;
            try { Apply(Spawn, StartRevision); }
            catch (const FFailure& Error) { StaleRejected = Error.Message == TEXT("Revision conflict"); }
            TestTrue(TEXT("Stale map revision rejected"), StaleRejected);

            const TCHAR* Move = TEXT(R"json({"action":"level_apply","operations":[
                {"op":"set_actor_transform","actor":"DockStart","location":[10,20,1750],
                 "rerun_construction":["OceanSolver"]}
            ]})json");
            const FJson MovePreview = Apply(Move, FirstRevision);
            TestFalse(TEXT("Transform preview leaves disk untouched"), MovePreview->GetBoolField(TEXT("saved")));
            TestEqual(TEXT("Transform preview preserves revision"), String(Inspect(TEXT("DockStart")), TEXT("revision")), FirstRevision);
            FString SaveMove(Move);
            SaveMove.InsertAt(1, TEXT("\"dry_run\":false,"));
            const FJson Moved = Apply(*SaveMove, FirstRevision);
            Require(Moved->GetBoolField(TEXT("saved")), TEXT("Transform did not save"));
            const FJson ReadMoved = Inspect(TEXT("DockStart"));
            const auto MovedActors = Array(ReadMoved, TEXT("actors"), true);
            Require(MovedActors.Num() == 1, TEXT("Moved actor did not reload"));
            const FJson MovedActor = AsObject(MovedActors[0]);
            const FJson Transform = Object(MovedActor, TEXT("transform"));
            TestEqual(TEXT("Moved actor retains its GUID"), String(MovedActor, TEXT("guid")), String(First, TEXT("guid")));
            TestEqual(TEXT("Moved actor retains its spawn tag"), String(Object(MovedActor, TEXT("instance_properties")), TEXT("PlayerStartTag")), FString(TEXT("Dock")));
            TestEqual(TEXT("Moved actor retains actor tags"), Array(MovedActor, TEXT("tags"), true).Num(), 2);
            TestTrue(TEXT("Moved actor retains its yaw"), FMath::IsNearlyEqual(Array(Transform, TEXT("rotation"), true)[1]->AsNumber(), 90.0));
            TestEqual(TEXT("Moved actor retains its scale"), Array(Transform, TEXT("scale"), true)[2]->AsNumber(), 1.0);
            TestEqual(TEXT("Moved actor's world Z persists"), Array(Transform, TEXT("location"), true)[2]->AsNumber(), 1750.0);
            TestTrue(TEXT("Transform save backs up the edited actor"), Object(Moved, TEXT("backups"))->HasField(ActorPackage));
            const auto Solvers = Array(Inspect(TEXT("OceanSolver")), TEXT("actors"), true);
            Require(Solvers.Num() == 1, TEXT("Dependent actor did not reload"));
            const FJson Solver = AsObject(Solvers[0]);
            const FJson MoveResult = AsObject(Array(Moved, TEXT("operations"), true)[0]);
            const auto Reruns = Array(MoveResult, TEXT("rerun_construction"), true);
            Require(Reruns.Num() == 1, TEXT("Expected one dependency to rerun construction"));
            const FJson Rerun = AsObject(Reruns[0]);
            TestEqual(TEXT("Requested construction rerun identifies the dependency"), String(Rerun, TEXT("guid")), String(Solver, TEXT("guid")));
            TestTrue(TEXT("Transform save backs up the rerun dependency"),
                Object(Moved, TEXT("backups"))->HasField(String(Solver, TEXT("package"))));

            FJson Replacement = Apply(TEXT(R"json({"action":"level_apply","dry_run":false,"operations":[
                {"op":"replace_actor_class","actor":"DockStart","class":"/Script/Engine.TargetPoint"}
            ]})json"), String(ReadMoved, TEXT("revision")));
            Require(Replacement->GetBoolField(TEXT("saved")), TEXT("Replacement did not save"));
            FJson ReadReplacement = Inspect(TEXT("DockStart"));
            const auto ReplacementActors = Array(ReadReplacement, TEXT("actors"), true);
            Require(ReplacementActors.Num() == 1, TEXT("Replacement actor did not reload"));
            FJson Second = AsObject(ReplacementActors[0]);
            TestEqual(TEXT("Replacement class"), String(Second, TEXT("class")), FString(TEXT("/Script/Engine.TargetPoint")));
            TestEqual(TEXT("Replacement preserves actor GUID"), String(Second, TEXT("guid")), String(First, TEXT("guid")));
            TestEqual(TEXT("Replacement preserves actor package"), String(Second, TEXT("package")), ActorPackage);
            TestEqual(TEXT("Replacement preserves actor tags"), Array(Second, TEXT("tags"), true).Num(), 2);
            TestEqual(TEXT("Replacement preserves position"), Array(Object(Second, TEXT("transform")), TEXT("location"), true)[1]->AsNumber(), 20.0);

            FJson Deleted = Apply(TEXT(R"json({"action":"level_apply","dry_run":false,"operations":[
                {"op":"delete_actor","actor":"DockStart"}
            ]})json"), String(ReadReplacement, TEXT("revision")));
            TestTrue(TEXT("Delete backs up previous package"), Object(Deleted, TEXT("backups"))->HasField(ActorPackage));
            TestEqual(TEXT("Deleted actor stays removed"), Array(Inspect(TEXT("DockStart")), TEXT("actors"), true).Num(), 0);
            if (Partitioned) { TestFalse(TEXT("External actor file removed"), IFileManager::Get().FileExists(*AssetFilename(ActorPackage))); }
        }
    }
    catch (const UAssetAgent::FFailure& Failure) { AddError(Failure.Message); }
    return true;
}
