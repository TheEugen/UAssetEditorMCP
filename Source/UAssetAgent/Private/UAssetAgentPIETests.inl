// Opt-in integration test for a disposable Lyra project prepared by tests/prepare_lyra_pie.py.
#include "Tests/AutomationEditorCommon.h"
#include "Engine/SkeletalMesh.h"
#include "AnimationRuntime.h"
#include "GameFramework/PlayerController.h"
#include "Components/SkeletalMeshComponent.h"
#include "UObject/StructOnScope.h"
#include "UObject/StrongObjectPtr.h"

namespace UAssetAgentPIETests
{
using namespace UAssetAgent;

static void CheckCookedCurvesAndArmFilter(FAutomationTestBase& Test)
{
    const ITargetPlatform* Target = GetTargetPlatformManagerRef().FindTargetPlatform(TEXT("Windows"));
    Require(Target != nullptr, TEXT("Windows cook target unavailable"));
    const FString Prefix = TEXT("/Game/Polynesien/Characters/Animations/");
    TArray<FString> Packages{Prefix + TEXT("MeleeWeapon/MM_MeleeWeapon_Jog_Left_Start")};
    for (const TCHAR* Side : {TEXT("Fwd"), TEXT("Bwd"), TEXT("Left"), TEXT("Right")})
    { Packages.Add(Prefix + TEXT("RangeWeapon/Musket/MM_Musket_Jog_") + Side + TEXT("_Start")); }
    for (const FString& Package : Packages)
    {
        auto* Seq = Cast<UAnimSequence>(LoadExactAsset(Package));
        Require(Seq != nullptr, TEXT("Missing prepared jog-start sequence: ") + Package);
        FCompressedAnimSequence& Data = Seq->CacheDerivedData(Target);
        auto* Codec = Cast<UAnimCurveCompressionCodec_UniformIndexable>(Data.CurveCompressionCodec);
        const float* Buffer = nullptr;
        int32 Samples = 0;
        float Rate = 0;
        Require(Codec && !Data.CompressedCurveByteStream.IsEmpty() &&
            Codec->GetCurveBufferAndSamples(Data, TEXT("Distance"), Buffer, Samples, Rate) &&
            Buffer && Samples > 1 && Rate > 0 && !FMath::IsNearlyEqual(Buffer[0], Buffer[Samples - 1]),
            TEXT("Expected nonconstant distance samples from Windows cook-target data: ") + Package);
        Test.AddInfo(FString::Printf(TEXT("%s: %d Windows cook-target samples"), *Seq->GetName(), Samples));
    }
    auto* Blueprint = Cast<UAnimBlueprint>(LoadExactAsset(TEXT("/Game/__UAssetAgentTests/PIE/ABP_Emote")));
    auto* Class = Blueprint ? Cast<UAnimBlueprintGeneratedClass>(Blueprint->GeneratedClass) : nullptr;
    Require(Class && Blueprint->TargetSkeleton, TEXT("Expected a compiled emote AnimBP with a skeleton"));
    TArray<FPerBoneBlendWeight> Weights;
    for (FStructProperty* Prop : Class->GetAnimNodeProperties())
    {
        if (Prop->Struct == FAnimNode_LayeredBoneBlend::StaticStruct())
        {
            auto* Blend = Prop->ContainerPtrToValuePtr<FAnimNode_LayeredBoneBlend>(Class->GetDefaultObject());
            FAnimationRuntime::CreateMaskWeights(Weights, Blend->LayerSetup, Blueprint->TargetSkeleton);
        }
    }
    for (const auto& Bone : TMap<FName, float>{{TEXT("hand_l"), 1.f}, {TEXT("hand_r"), 0.f}, {TEXT("pelvis"), 0.f}})
    {
        int32 Index = Blueprint->TargetSkeleton->GetReferenceSkeleton().FindBoneIndex(Bone.Key);
        Require(Weights.IsValidIndex(Index) && FMath::IsNearlyEqual(Weights[Index].BlendWeight, Bone.Value),
            TEXT("Unexpected compiled emote mask for ") + Bone.Key.ToString());
    }
    Test.AddInfo(TEXT("Compiled mask weights: hand_l=1, hand_r=0, pelvis=0"));
}

class FCheckProjectPIE final : public IAutomationLatentCommand
{
    FAutomationTestBase& Test;
    TStrongObjectPtr<UUserWidget> Widget;
    TWeakObjectPtr<APlayerController> Sender;
    int32 Phase = 0;
    double Deadline = FPlatformTime::Seconds() + 90;

    void CheckMontages(UWorld* World)
    {
        auto* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/Heroes/Mannequin/Meshes/SKM_Manny.SKM_Manny"));
        Require(Mesh != nullptr, TEXT("Missing Manny mesh"));
        AActor* Owner = World->SpawnActor<AActor>();
        Require(Owner != nullptr, TEXT("Could not spawn montage test actor"));
        auto* Component = NewObject<USkeletalMeshComponent>(Owner);
        Owner->SetRootComponent(Component);
        Component->SetSkeletalMeshAsset(Mesh);
        Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
        Component->RegisterComponent();
        auto* Instance = Component->GetAnimInstance();
        Require(Instance != nullptr, TEXT("Could not initialize animation instance"));
        TArray<FVector> Motion;
        for (const FString& Side : {FString(TEXT("Left")), FString(TEXT("Right"))})
        {
            auto* Montage = LoadObject<UAnimMontage>(nullptr, *(TEXT("/Game/__UAssetAgentTests/PIE/AM_") + Side + TEXT(".AM_") + Side));
            Require(Montage && Montage->GetSkeleton() == Mesh->GetSkeleton() && Montage->HasRootMotion() &&
                Montage->SlotAnimTracks.Num() == 1 && Montage->SlotAnimTracks[0].SlotName == TEXT("DefaultSlot"),
                TEXT("Unexpected montage skeleton, root motion or slot: ") + Side);
            Require(Instance->Montage_Play(Montage) > 0 && Instance->Montage_IsPlaying(Montage), TEXT("Montage failed to play: ") + Side);
            const FVector Delta = Montage->ExtractRootMotionFromTrackRange(0, Montage->GetPlayLength()).GetTranslation();
            Test.AddInfo(Side + TEXT(" montage root displacement: ") + Delta.ToString());
            Require(!Delta.IsNearlyZero(), TEXT("Expected travelling root motion"));
            Motion.Add(Delta);
        }
        Require(FVector::DotProduct(Motion[0], Motion[1]) < 0, TEXT("Expected opposite root-motion directions"));
        Owner->Destroy();
    }

    void Broadcast(int32 Expected)
    {
        UFunction* Function = Sender->FindFunction(TEXT("ClientReceiveQuickComm"));
        FStructProperty* Message = Function ? FindFProperty<FStructProperty>(Function, TEXT("Message")) : nullptr;
        Require(Message != nullptr, TEXT("Missing Lyra quick-comm receiver"));
        FStructOnScope Args(Function);
        FStructProperty* Location = FindFProperty<FStructProperty>(Message->Struct, TEXT("Location"));
        Require(Location && Location->Struct == TBaseStructure<FVector>::Get(), TEXT("Unexpected quick-comm message signature"));
        const FVector Value(Phase * 10, 20, 30);
        *Location->ContainerPtrToValuePtr<FVector>(Message->ContainerPtrToValuePtr<void>(Args.GetStructMemory())) = Value;
        Sender->ProcessEvent(Function, Args.GetStructMemory());
        auto* Count = FindFProperty<FIntProperty>(Widget->GetClass(), TEXT("TestCount"));
        auto* Received = FindFProperty<FStructProperty>(Widget->GetClass(), TEXT("TestMessage"));
        Require(Count && Received && Received->Struct == Message->Struct, TEXT("Run prepare_lyra_pie.py on the disposable project first"));
        const int32 Actual = Count->GetPropertyValue_InContainer(Widget.Get());
        Require(Actual == Expected, FString::Printf(TEXT("At phase %d expected %d callbacks, received %d"), Phase, Expected, Actual));
        if (Phase != 2)
        {
            const FVector Payload = *Location->ContainerPtrToValuePtr<FVector>(Received->ContainerPtrToValuePtr<void>(Widget.Get()));
            Require(Payload.Equals(Value), TEXT("Listener did not receive the typed payload"));
        }
        Test.AddInfo(FString::Printf(TEXT("QuickComm phase %d, total callbacks %d"), Phase, Actual));
    }

public:
    explicit FCheckProjectPIE(FAutomationTestBase& InTest) : Test(InTest) {}
    bool Update() override
    {
        try
        {
            Require(FPlatformTime::Seconds() < Deadline, TEXT("Timed out waiting for the PIE player or widget lifecycle"));
            UWorld* World = GEditor->PlayWorld;
            APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
            if (!Player || !Player->IsLocalPlayerController()) { return false; }
            if (Phase == 0)
            {
                CheckMontages(World);
                auto* Class = LoadClass<UUserWidget>(nullptr, TEXT("/Game/Polynesien/UI/W_QuickComm.W_QuickComm_C"));
                Require(Class != nullptr, TEXT("Missing project QuickComm widget"));
                Widget.Reset(CreateWidget<UUserWidget>(Player, Class));
                Require(Widget.IsValid(), TEXT("Could not construct QuickComm"));
                UClass* Controller = LoadClass<APlayerController>(nullptr, TEXT("/Script/LyraGame.PP_PlayerController"));
                Require(Controller != nullptr, TEXT("Missing Lyra quick-comm controller"));
                Sender = Player->IsA(Controller) ? Player : World->SpawnActor<APlayerController>(Controller);
                Require(Sender.IsValid(), TEXT("Could not instantiate Lyra quick-comm controller"));
                Widget->AddToViewport();
            }
            else if (Phase == 1) { Broadcast(1); Widget->RemoveFromParent(); }
            else if (Phase == 2) { Broadcast(1); Widget->AddToViewport(); }
            else { Broadcast(2); Widget->RemoveFromParent(); return true; }
            ++Phase;
            return false;
        }
        catch (const FFailure& Failure) { Test.AddError(Failure.Message); return true; }
    }
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FUAssetAgentProjectPIE, "UAssetAgent.ProjectPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FUAssetAgentProjectPIE::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    if (FParse::Param(FCommandLine::Get(), TEXT("UAssetAgentProjectPIE")))
    {
        Names.Add(TEXT("QuickCommAndMontages")); Commands.Add(TEXT("QuickCommAndMontages"));
        Names.Add(TEXT("CookedCurvesAndArmFilter")); Commands.Add(TEXT("CookedCurvesAndArmFilter"));
    }
}

bool FUAssetAgentProjectPIE::RunTest(const FString& Parameters)
{
    if (Parameters == TEXT("CookedCurvesAndArmFilter"))
    {
        try { UAssetAgentPIETests::CheckCookedCurvesAndArmFilter(*this); }
        catch (const UAssetAgent::FFailure& Failure) { AddError(Failure.Message); }
        return true;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(UAssetAgentPIETests::FCheckProjectPIE(*this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}
