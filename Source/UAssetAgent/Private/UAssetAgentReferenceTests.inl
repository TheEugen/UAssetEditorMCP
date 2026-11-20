#include "Engine/LevelStreamingDynamic.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentReferenceQueryTest, "UAssetAgent.AssetReferences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUAssetAgentReferenceQueryTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Root = TestRoot();
        const FString Prefix = Root + TEXT("/Waterline");
        const FString Folder = Root + TEXT("/Maps");
        const FString Map = Folder + TEXT("/Harbor");
        const FString SoftMap = Prefix + TEXT("/OptionalMap");
        UBlueprint* Base = Blueprint(Prefix + TEXT("/BP_Base"));
        UBlueprint* Boat = Blueprint(Prefix + TEXT("/BP_Boat"), Base->GeneratedClass);
        UBlueprint* Other = Blueprint(Root + TEXT("/WaterlineBackup/BP_Boat"));
        Save(Base);
        Save(Boat);
        Save(Other);

        UPackage* Package = CreatePackage(*Map);
        const UWorld::InitializationValues Init = UWorld::InitializationValues()
            .CreateNavigation(false).CreateAISystem(false).AllowAudioPlayback(false);
        UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("Harbor"), Package,
            false, ERHIFeatureLevel::Num, &Init);
        Require(World != nullptr, TEXT("Could not create map fixture"));
        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Require(World->SpawnActor<AActor>(Boat->GeneratedClass, Spawn) &&
            World->SpawnActor<AActor>(Other->GeneratedClass, Spawn), TEXT("Could not spawn reference fixtures"));
        ULevelStreamingDynamic* Streaming = NewObject<ULevelStreamingDynamic>(World, NAME_None);
        Streaming->SetWorldAssetByPackageName(FName(*SoftMap));
        World->AddStreamingLevel(Streaming);
        World->SetFlags(RF_Public | RF_Standalone);
        const FString File = LevelFilename(Map);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        Require(UPackage::SavePackage(Package, World, *File, SaveArgs), TEXT("Could not save map fixture"));
        World->DestroyWorld(false);
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous({File}, true);

        const auto Query = [](const FJson& Request)
        {
            FJson Response = MakeShared<FJsonObject>();
            Execute(Request, Response);
            Require(!Response->GetBoolField(TEXT("saved")), TEXT("Reference query wrote asset data"));
            return Response;
        };
        const auto Contains = [](const FJson& Result, const FString& Path)
        {
            return Array(Result, TEXT("references"), true).ContainsByPredicate([&](const auto& Value)
            {
                return Value->AsString() == Path;
            });
        };

        FJson References = Json(TEXT("{\"action\":\"references\",\"direction\":\"dependencies\"}"));
        References->SetStringField(TEXT("asset"), Map);
        const FJson Direct = Query(References);
        TestTrue(TEXT("Map imports Blueprint package"), Contains(Direct, Boat->GetOutermost()->GetName()));
        TestTrue(TEXT("Map records soft level reference"), Contains(Direct, SoftMap));
        References->SetBoolField(TEXT("include_soft"), false);
        TestFalse(TEXT("Hard-only query omits soft map dependency"), Contains(Query(References), SoftMap));
        References->SetBoolField(TEXT("recursive"), true);
        TestTrue(TEXT("Recursive dependencies reach Blueprint parent"), Contains(Query(References), Base->GetOutermost()->GetName()));

        References->SetStringField(TEXT("asset"), Boat->GetOutermost()->GetName());
        References->SetStringField(TEXT("direction"), TEXT("referencers"));
        References->SetBoolField(TEXT("recursive"), false);
        TestTrue(TEXT("Incoming references include map"), Contains(Query(References), Map));

        FJson Find = Json(TEXT("{\"action\":\"find_path\",\"include_soft\":false}"));
        Find->SetStringField(TEXT("path_prefix"), Prefix + TEXT("/"));
        Find->SetStringField(TEXT("folder"), Folder);
        const auto Matches = Array(Query(Find), TEXT("matches"), true);
        Require(Matches.Num() == 1, TEXT("Expected one matching map"));
        const FJson Found = AsObject(Matches[0]);
        TestEqual(TEXT("Prefix scan reports map package"), String(Found, TEXT("asset")), Map);
        TestTrue(TEXT("Prefix scan includes map import"), Contains(Found, Boat->GetOutermost()->GetName()));
        TestFalse(TEXT("Prefix scan excludes sibling folder"), Contains(Found, Other->GetOutermost()->GetName()));
    }
    catch (const UAssetAgent::FFailure& Failure) { AddError(Failure.Message); }
    return true;
}
