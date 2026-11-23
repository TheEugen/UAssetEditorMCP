#include "IUAssetAgentNiagara.h"
#include "NiagaraDataInterface.h"
#include "NiagaraSystem.h"
#include "Modules/ModuleManager.h"

class FUAssetAgentNiagara : public IUAssetAgentNiagara
{
public:
    void Describe(UObject* Asset, FJsonObject& Response) override
    {
        const auto& Store = CastChecked<UNiagaraSystem>(Asset)->GetExposedParameters();
        TArray<TSharedPtr<FJsonValue>> Parameters;
        for (const auto& Entry : Store.ReadParameterVariables())
        {
            FNiagaraVariable Var(Entry);
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), Var.GetName().ToString());
            Item->SetStringField(TEXT("type"), Var.GetType().GetName());
            if (Var.IsValid() && Var.IsDataInterface())
            {
                UNiagaraDataInterface* Interface = Store.GetDataInterface(Var);
                Item->SetStringField(TEXT("interface_class"), Var.GetType().GetClass()->GetPathName());
                Item->SetBoolField(TEXT("missing"), !Interface);
                if (Interface)
                {
                    Item->SetStringField(TEXT("object"), Interface->GetPathName());
                    auto Properties = MakeShared<FJsonObject>();
                    for (TFieldIterator<FProperty> It(Interface->GetClass()); It; ++It)
                    {
                        if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Transient)) { continue; }
                        FString Value;
                        It->ExportText_InContainer(0, Value, Interface, nullptr, Interface, PPF_None);
                        Properties->SetStringField(It->GetName(), Value);
                    }
                    Item->SetObjectField(TEXT("properties"), Properties);
                }
            }
            else if (Var.IsValid() && Var.IsUObject())
            { Item->SetStringField(TEXT("value"), GetPathNameSafe(Store.GetUObject(Var))); }
            else if (Var.IsValid() && Var.GetSizeInBytes() > 0)
            {
                Var.AllocateData();
                if (Store.CopyParameterData(Var, Var.GetData()))
                { Item->SetStringField(TEXT("value"), Var.GetType().ToString(Var.GetData())); }
            }
            Parameters.Add(MakeShared<FJsonValueObject>(Item));
        }
        Response.SetArrayField(TEXT("niagara_user_parameters"), Parameters);
    }

    UObject* RepairInterface(UObject* Asset, FName Parameter, FString& Error) override
    {
        auto& Store = CastChecked<UNiagaraSystem>(Asset)->GetExposedParameters();
        for (const auto& Entry : Store.ReadParameterVariables())
        {
            if (Entry.GetName() != Parameter) { continue; }
            UClass* Type = Entry.GetType().GetClass();
            if (!Type || !Entry.IsDataInterface() || Type->HasAnyClassFlags(CLASS_Abstract) ||
                !Store.GetDataInterfaces().IsValidIndex(Entry.Offset))
            { Error = TEXT("Expected a concrete data-interface parameter with a valid store offset"); return nullptr; }
            auto* Interface = Store.GetDataInterface(Entry.Offset);
            if (!Interface) { Interface = NewObject<UNiagaraDataInterface>(Asset, Type, NAME_None, RF_Transactional); }
            if (!Interface->IsA(Type) || !Interface->IsIn(Asset))
            { Error = TEXT("Existing interface has an incompatible type or owner"); return nullptr; }
            Store.SetDataInterface(Interface, Entry.Offset);
            return Interface;
        }
        Error = TEXT("Unknown exposed Niagara parameter: ") + Parameter.ToString();
        return nullptr;
    }

    bool CompileSystem(UObject* Asset) override
    {
        auto* System = CastChecked<UNiagaraSystem>(Asset);
        System->RequestCompile(true);
        System->WaitForCompilationComplete(true, false);
        return System->IsValid();
    }
};

IMPLEMENT_MODULE(FUAssetAgentNiagara, UAssetAgentNiagara)

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "NiagaraDataInterfaceTexture.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUAssetAgentNiagaraRepairTest, "UAssetAgent.Niagara.Repair",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUAssetAgentNiagaraRepairTest::RunTest(const FString& Parameters)
{
    auto* Template = LoadObject<UNiagaraSystem>(nullptr, TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst"));
    if (!TestNotNull(TEXT("Engine Niagara template"), Template)) { return false; }
    auto* System = DuplicateObject<UNiagaraSystem>(Template, GetTransientPackage());
    FNiagaraParameterStore& Store = System->GetExposedParameters();
    FNiagaraVariable Texture(FNiagaraTypeDefinition(UNiagaraDataInterfaceTexture::StaticClass()), TEXT("User.TestTexture"));
    int32 Offset = INDEX_NONE;
    if (!TestTrue(TEXT("Texture parameter added"), Store.AddParameter(Texture, true, true, &Offset))) { return false; }
    Store.SetDataInterface(nullptr, Offset);
    FUAssetAgentNiagara Module;
    FString Error;
    UObject* Interface = Module.RepairInterface(System, Texture.GetName(), Error);
    if (!TestNotNull(TEXT("Missing interface recreated"), Interface)) { return false; }
    TestTrue(TEXT("Interface has the parameter type and belongs to the system"),
        Interface->IsA<UNiagaraDataInterfaceTexture>() && Interface->GetOuter() == System);
    TestTrue(TEXT("Repeated repair preserves the instance"), Module.RepairInterface(System, Texture.GetName(), Error) == Interface);
    TestTrue(TEXT("Repaired system compiles"), Module.CompileSystem(System));
    FJsonObject Inspection;
    Module.Describe(System, Inspection);
    TestTrue(TEXT("Repaired interface visible in inspection"), Inspection.GetArrayField(TEXT("niagara_user_parameters")).ContainsByPredicate(
        [&](const TSharedPtr<FJsonValue>& V) { return V->AsObject()->GetStringField(TEXT("name")) == Texture.GetName().ToString() &&
            !V->AsObject()->GetBoolField(TEXT("missing")); }));
    return true;
}
#endif
