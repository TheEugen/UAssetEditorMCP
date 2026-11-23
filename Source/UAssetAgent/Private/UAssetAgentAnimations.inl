static void DescribeSequence(UAnimSequence* Sequence, const FJson& Response)
{
    FJson Animation = MakeShared<FJsonObject>();
    Animation->SetStringField(TEXT("skeleton"), GetPathNameSafe(Sequence->GetSkeleton()));
    Animation->SetNumberField(TEXT("length"), Sequence->GetPlayLength());
    Animation->SetBoolField(TEXT("root_motion"), Sequence->HasRootMotion());
    auto* Settings = Sequence->CurveCompressionSettings.Get();
    Animation->SetStringField(TEXT("compression_settings"), GetPathNameSafe(Settings));
    Animation->SetStringField(TEXT("codec"), Settings && Settings->Codec ? Settings->Codec->GetClass()->GetPathName() : FString());
    FJson Curves = MakeShared<FJsonObject>();
    if (const IAnimationDataModel* Model = Sequence->GetDataModel())
    {
        for (const FFloatCurve& Curve : Model->GetFloatCurves())
        {
            TArray<TSharedPtr<FJsonValue>> Keys;
            for (const FRichCurveKey& Source : Curve.FloatCurve.GetConstRefOfKeys())
            {
                FJson Key = MakeShared<FJsonObject>();
                Key->SetNumberField(TEXT("time"), Source.Time);
                Key->SetNumberField(TEXT("value"), Source.Value);
                Keys.Add(JsonValue(Key));
            }
            Curves->SetArrayField(Curve.GetName().ToString(), Keys);
        }
    }
    Animation->SetObjectField(TEXT("float_curves"), Curves);
    Response->SetObjectField(TEXT("animation"), Animation);
}

static bool EditAnimation(UObject* Asset, const FJson& Op, const FJson& Response)
{
    const FString Kind = String(Op, TEXT("op"));
    if (Kind != TEXT("set_float_curve") && Kind != TEXT("apply_distance_curve") && Kind != TEXT("set_curve_compression")) { return false; }
    UAnimSequence* Sequence = Cast<UAnimSequence>(Asset);
    Require(Sequence && Sequence->GetSkeleton() && Sequence->GetDataModel(), TEXT("Expected an animation sequence with a skeleton and editable data"));
    if (Kind == TEXT("set_curve_compression"))
    {
        auto* Settings = Cast<UAnimCurveCompressionSettings>(LoadExactAsset(String(Op, TEXT("settings"))));
        Require(Settings && Settings->AreSettingsValid(), TEXT("Expected valid animation curve compression settings"));
        Sequence->CurveCompressionSettings = Settings;
        const ITargetPlatform* Platform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
        Require(Platform != nullptr, TEXT("Missing running target platform"));
        FCompressedAnimSequence& Data = Sequence->CacheDerivedData(Platform);
        if (Op->HasField(TEXT("verify_indexed_curve")))
        {
            auto* Codec = Cast<UAnimCurveCompressionCodec_UniformIndexable>(Data.CurveCompressionCodec);
            const float* Buffer = nullptr;
            int32 Count = 0;
            float Rate = 0;
            Require(Codec && !Data.CompressedCurveByteStream.IsEmpty() && Codec->GetCurveBufferAndSamples(Data,
                FName(*String(Op, TEXT("verify_indexed_curve"))), Buffer, Count, Rate) && Buffer && Count > 0,
                TEXT("Curve has no UniformIndexable compressed samples"));
            Response->SetNumberField(TEXT("indexed_curve_samples"), Count);
        }
        return true;
    }
    const FName Name(*String(Op, TEXT("curve"), TEXT("Distance"), Kind == TEXT("set_float_curve")));
    Require(!Name.IsNone(), TEXT("Expected a curve name"));
    const FAnimationCurveIdentifier Id(Name, ERawCurveTrackTypes::RCT_Float);
    auto& Controller = Sequence->GetController();
    const bool Exists = Sequence->GetDataModel()->FindFloatCurve(Id) != nullptr;
    if (Kind == TEXT("set_float_curve"))
    {
        TArray<FRichCurveKey> Keys;
        for (const auto& Value : Array(Op, TEXT("keys"), true))
        {
            FJson Key = AsObject(Value);
            double Time = 0, Number = 0;
            Require(Key->TryGetNumberField(TEXT("time"), Time) && Time >= 0 && Time <= Sequence->GetPlayLength() &&
                (Keys.IsEmpty() || float(Time) > Keys.Last().Time) && Key->TryGetNumberField(TEXT("value"), Number) &&
                FMath::Abs(Number) <= MAX_flt, TEXT("Expected finite keys in strictly increasing sequence time"));
            Keys.Emplace(float(Time), float(Number));
        }
        Require(!Keys.IsEmpty() && (Exists || Controller.AddCurve(Id)) && Controller.SetCurveKeys(Id, Keys), TEXT("Could not write float keys"));
        return true;
    }
    const int32 Rate = Integer(Op, TEXT("sample_rate"), 30);
    Require(Sequence->HasRootMotion() && Sequence->GetPlayLength() > 0, TEXT("DistanceCurveModifier requires root motion and a positive duration"));
    Require(Rate > 0 && double(Rate) * Sequence->GetPlayLength() < MAX_int32, TEXT("Invalid distance sample rate"));
    UClass* Class = LoadClass<UAnimationModifier>(nullptr, TEXT("/Script/AnimationLocomotionLibraryEditor.DistanceCurveModifier"));
    Require(Class != nullptr, TEXT("Enable the Animation Locomotion Library plugin for DistanceCurveModifier"));
    UAnimationModifier* Modifier = NewObject<UAnimationModifier>(GetTransientPackage(), Class);
    SetNestedDefault(Modifier, TEXT("CurveName"), Name.ToString());
    SetNestedDefault(Modifier, TEXT("SampleRate"), FString::FromInt(Rate));
    SetNestedDefault(Modifier, TEXT("bStopAtEnd"), Boolean(Op, TEXT("stop_at_end")) ? TEXT("True") : TEXT("False"));
    if (Exists)
    {
        Require(Boolean(Op, TEXT("replace_existing")), TEXT("Distance curve exists; set replace_existing to regenerate it"));
        Require(Controller.RemoveCurve(Id), TEXT("Could not remove previous distance curve"));
    }
    // Invoke the stock modifier once. Its generated keys belong to the sequence.
    Modifier->OnApply(Sequence);
    const FFloatCurve* Result = Sequence->GetDataModel()->FindFloatCurve(Id);
    Require(Result && Result->FloatCurve.GetNumKeys() > 0, TEXT("DistanceCurveModifier produced no keys"));
    return true;
}
