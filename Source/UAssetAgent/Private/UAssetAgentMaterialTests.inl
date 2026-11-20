#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentMaterialTest, "UAssetAgent.MaterialInstances",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUAssetAgentMaterialTest::RunTest(const FString& Parameters)
{
    using namespace UAssetAgentTests;
    try
    {
        const FString Root = TestRoot();
        auto* Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
        Require(Texture != nullptr, TEXT("Fixture texture could not be loaded"));
        const auto MakeMaterial = [&](const TCHAR* Name, float Speed)
        {
            UMaterial* Material = NewObject<UMaterial>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone);
            auto* Scalar = NewObject<UMaterialExpressionScalarParameter>(Material);
            Scalar->ParameterName = TEXT("Speed");
            Scalar->DefaultValue = Speed;
            auto* Vector = NewObject<UMaterialExpressionVectorParameter>(Material);
            Vector->ParameterName = TEXT("Tint");
            auto* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
            Sample->ParameterName = TEXT("Detail");
            Sample->Texture = Texture;
            auto* Switch = NewObject<UMaterialExpressionStaticSwitchParameter>(Material);
            Switch->ParameterName = TEXT("UseDetail");
            Switch->A.Connect(0, Sample);
            Switch->B.Connect(0, Vector);
            Material->GetExpressionCollection().AddExpression(Scalar);
            Material->GetExpressionCollection().AddExpression(Vector);
            Material->GetExpressionCollection().AddExpression(Sample);
            Material->GetExpressionCollection().AddExpression(Switch);
            Material->GetEditorOnlyData()->BaseColor.Connect(0, Switch);
            Material->GetEditorOnlyData()->Roughness.Connect(0, Scalar);
            Material->PostEditChange();
            Save(Material);
            return Material;
        };
        UMaterial* Parent = MakeMaterial(TEXT("M_Original"), 0.25f);
        UMaterial* NewParent = MakeMaterial(TEXT("M_Replacement"), 0.75f);
        const FString Asset = Root + TEXT("/MI_Fixture");
        const FString File = AssetFilename(Asset);
        auto* Instance = NewObject<UMaterialInstanceConstant>(CreatePackage(*Asset), TEXT("MI_Fixture"), RF_Public | RF_Standalone);
        Instance->SetParentEditorOnly(Parent, false);
        Instance->PostEditChange();
        Save(Instance);
        const FString Before = Revision(File);
        const FJson Request = Json(TEXT(R"json({"action":"material_apply","dry_run":false,"operations":[
            {"op":"set_parent"},
            {"op":"set_parameter","name":"Speed","type":"scalar","value":0.5},
            {"op":"set_parameter","name":"Tint","type":"vector","value":[0.1,0.2,0.3,1.0]},
            {"op":"set_parameter","name":"Detail","type":"texture"},
            {"op":"set_parameter","name":"UseDetail","type":"static_switch","value":true}
        ]})json"));
        Request->SetStringField(TEXT("asset"), Asset);
        Request->SetStringField(TEXT("expected_revision"), Before);
        Array(Request, TEXT("operations"))[0]->AsObject()->SetStringField(TEXT("parent"), NewParent->GetPathName());
        Array(Request, TEXT("operations"))[3]->AsObject()->SetStringField(TEXT("value"), Texture->GetPathName());
        const FJson Result = MakeShared<FJsonObject>();
        Execute(Request, Result);
        TestTrue(TEXT("Override batch saved"), Result->GetBoolField(TEXT("saved")));
        TestTrue(TEXT("Backup recorded"), Result->HasField(TEXT("backups")));
        TestEqual(TEXT("Replacement parent reported"), String(Result, TEXT("parent")), NewParent->GetPathName());
        TestEqual(TEXT("All four parameters inspected"), Array(Result, TEXT("parameters")).Num(), 4);
        for (const auto& Value : Array(Result, TEXT("parameters")))
        { TestTrue(TEXT("Each parameter is overridden"), Value->AsObject()->GetBoolField(TEXT("overridden"))); }

        const FString Saved = Revision(File);
        const FJson Clear = Json(TEXT(R"json({"action":"material_apply","operations":[
            {"op":"clear_parameter","name":"Speed","type":"scalar"},
            {"op":"clear_parameter","name":"UseDetail","type":"static_switch"}
        ]})json"));
        Clear->SetStringField(TEXT("asset"), Asset);
        Clear->SetStringField(TEXT("expected_revision"), Saved);
        const FJson Preview = MakeShared<FJsonObject>();
        Execute(Clear, Preview);
        TestFalse(TEXT("Dry run default prevents save"), Preview->GetBoolField(TEXT("saved")));
        TestEqual(TEXT("Dry run preserves saved revision"), Revision(File), Saved);
        for (const auto& Value : Array(Preview, TEXT("parameters")))
        {
            if (String(Value->AsObject(), TEXT("name")) == TEXT("Speed"))
            {
                TestFalse(TEXT("Clear restores inheritance"), Value->AsObject()->GetBoolField(TEXT("overridden")));
                TestEqual(TEXT("Preview shows parent value"), Value->AsObject()->GetNumberField(TEXT("value")), 0.75);
            }
        }
        const FJson Cycle = Json(TEXT(R"json({"action":"material_apply","operations":[{"op":"set_parent"}]})json"));
        Cycle->SetStringField(TEXT("asset"), Asset);
        Cycle->SetStringField(TEXT("expected_revision"), Saved);
        Array(Cycle, TEXT("operations"))[0]->AsObject()->SetStringField(TEXT("parent"), Instance->GetPathName());
        bool Rejected = false;
        try { Execute(Cycle, MakeShared<FJsonObject>()); }
        catch (const FFailure& Failure) { Rejected = Failure.Message.Contains(TEXT("cycle")); }
        TestTrue(TEXT("Parent cycle rejected"), Rejected);
        TestEqual(TEXT("Rejected cycle keeps file"), Revision(File), Saved);
    }
    catch (const FFailure& Failure) { AddError(Failure.Message); }
    return true;
}
