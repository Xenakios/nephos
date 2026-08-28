#include "granularsynth.h"

ToneGranulator::ToneGranulator() : m_sr(44100.0), modmatrix(44100.0)
{
    visualizer_fifo.reset(2048);
    trngFifo.reset(32);
    gevisfifo.reset(32);
    repeatsVisMessages.reset(32);
    shapeParToActualShape[0] = GranulatorModMatrix::lfo_t::SINE;
    shapeParToActualShape[1] = GranulatorModMatrix::lfo_t::PULSE;
    shapeParToActualShape[2] = GranulatorModMatrix::lfo_t::SAW_TRI_RAMP;
    shapeParToActualShape[3] = GranulatorModMatrix::lfo_t::SMOOTH_NOISE;
    shapeParToActualShape[4] = GranulatorModMatrix::lfo_t::SH_NOISE;
    for (int i = 0; i < 127; ++i)
    {
        midiCCMap[i + 1] = MIDICCSTART + i;
    }
    fifo.reset(2048);
    scheduledGrains.reserve(2048);
    for (auto &v : stepModValues)
        v = 0.0f;
    for (auto &v : randomModValues)
        v = 0.0f;
    // randomModSources[0] = TriggeredRandomSource{1001};
    // randomModSources[1].set_distribution(TriggeredRandomSource::D_CAUCHY);
    // randomModSources[1].parameter_values[1] = 0.02;
    // randomModSources[2].set_distribution(TriggeredRandomSource::D_UNIFORM);
    // randomModSources[3].set_distribution(TriggeredRandomSource::D_HYPCOS);
    auto initssfunc = [](StepModSource &sms, std::initializer_list<float> values) {
        for (int i = 0; i < values.size(); ++i)
        {
            sms.steps[i] = *(values.begin() + i);
        }
        sms.numactivesteps = StepModSource::maxSteps;
        sms.loopstartstep = 0;
        sms.looplen = values.size();
    };
    initssfunc(stepModSources[0], {-1.0f, 1.0f});
    initssfunc(stepModSources[1], {-1.0f, 0.0f, 1.0f});
    initssfunc(stepModSources[2], {-1.0f, -0.333f, 0.333f, 1.0f});
    initssfunc(stepModSources[3], {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f});
    for (size_t i = 0; i < 4; ++i)
    {
        stepModSources[4 + i].steps.resize(128);
        stepModSources[4 + i].numactivesteps = StepModSource::maxSteps;
        stepModSources[4 + i].loopstartstep = 0;
        stepModSources[4 + i].looplen = 128;
    }
    for (size_t i = 0; i < 128; ++i)
    {
        stepModSources[4].steps[i] = rng.nextFloatInRange(-1.0f, 1.0f);
        stepModSources[5].steps[i] = rng.nextFloatInRange(-1.0f, 1.0f);
        float z = rng.nextFloat();
        if (z < 0.5f)
            stepModSources[6].steps[i] = -1.0f;
        else
            stepModSources[6].steps[i] = 1.0f;
        z = rng.nextFloat();
        if (z < 0.5f)
            stepModSources[7].steps[i] = -1.0f;
        else
            stepModSources[7].steps[i] = 1.0f;
    }
    parmetadatas.reserve(128);
    parmetadatas.push_back(pmd()
                               .withRange(-24.0, 0.0)
                               .withDefault(-6.0)
                               .withLinearScaleFormatting("dB")
                               .withName("Main volume")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE)
                               .withGroupName("Main output")
                               .withID(PAR_MAINVOLUME));
    parmetadatas.push_back(pmd()
                               .withType(pmd::FLOAT)
                               .withRange(-48, 0)
                               .withDefault(-36)
                               .withSemitoneZeroAt440Formatting()
                               .withName("Main Highpass Cutoff")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE)
                               .withGroupName("Main output")
                               .withID(PAR_MASTERHIGHPASSCUTOFF));
    parmetadatas.push_back(pmd()
                               .withUnorderedMapFormatting({{0, "Ambisonic 1st Order"},
                                                            {1, "Ambisonic 2nd Order"},
                                                            {2, "Ambisonic 3rd Order"},
                                                            {3, "Ambisonic 4th Order"},
                                                            {4, "Ambisonic 5th Order"},
                                                            {5, "Ambisonic 6th Order"},
                                                            {6, "Ambisonic 7th Order"}},
                                                           true)
                               .withDefault(2)
                               .withName("Spatialization mode")
                               .withGroupName("Spatialization")
                               .withID(PAR_AMBORDER));
    parmetadatas.push_back(pmd()
                               .withUnorderedMapFormatting(oscTypeToStringMap, true)
                               .withDefault(0)
                               .withName("Oscillator type")
                               .withGroupName("Oscillator")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE)
                               .withID(PAR_OSCTYPE));
    parmetadatas.push_back(pmd()
                               .asBool()
                               .withDefault(false)
                               .withOnOffFormatting()
                               .withName("Pause Grain Generation")
                               .withGroupName("Time")
                               .withID(PAR_PAUSEAUTOGEN));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 7.0f)
                               .withDefault(4.0)
                               .withATwoToTheBFormatting(1.0f, 1.0, "Hz")
                               .withName("Density")
                               .withGroupName("Time")
                               .withID(PAR_DENSITY)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 1.0f)
                               .withDefault(0.75f)
                               .asCubicDecibelAttenuation()
                               .withName("Grain volume")
                               .withGroupName("Volume")
                               .withID(PAR_GRAINVOLUME)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 1.0f)
                               .withDefault(0.5f)
                               .withOffsetPowerFormatting("ms", 0.002f, 0.498f, 3.0f, 1000.0f)
                               .withDecimalPlaces(0)
                               .withName("Duration")
                               .withGroupName("Time")
                               .withID(PAR_DURATION)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 1.0f)
                               .withDefault(0.0f)
                               .withOffsetPowerFormatting("ms", 0.002f, 0.998f, 3.0f, 1000.0f)
                               .withDecimalPlaces(0)
                               .withName("Tail")
                               .withGroupName("Time")
                               .withID(PAR_GRAINTAIL));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 1.0f)
                               .withDefault(0.5f)
                               .withQuantizedStepCount(6)
                               .withLinearScaleFormatting("%", 100.0f)
                               .withName("Volume Envelope Morph")
                               .withGroupName("Volume")
                               .withID(PAR_ENVMORPH)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    std::unordered_map<int, std::string> easingCurveMap;
    for (int i = 0; i < 40; ++i)
    {
        if (easing_table[i].name && easing_table[i].function)
        {
            easingCurveMap[i] = easing_table[i].name;
        }
        else
            break;
    }
    parmetadatas.push_back(pmd()
                               .withUnorderedMapFormatting(easingCurveMap, true)
                               .withDefault(0.0)
                               .withName("Vol Env Start Curve")
                               .withGroupName("Volume")
                               .withID(PAR_VOLENVEASINGSTART));
    parmetadatas.push_back(pmd()
                               .withUnorderedMapFormatting(easingCurveMap, true)
                               .withDefault(0.0)
                               .withName("Vol Env End Curve")
                               .withGroupName("Volume")
                               .withID(PAR_VOLENVEASINGEND));
    for (int i = 0; i < numPitchBandAttens; ++i)
    {
        parmetadatas.push_back(pmd()
                                   .withRange(0.0, 1.0)
                                   .withDefault(1.0)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName(fmt::format("Pitch Gain {}", i + 1))
                                   .withGroupName("Volume")
                                   .withID(PAR_PITCHBANDGAIN0 + i * 10)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
    }

    parmetadatas.push_back(pmd()
                               .withRange(-48.0, 48.0)
                               .withDefault(0.0)
                               .withQuantizedInterval(1.0f)
                               .withLinearScaleFormatting("ST")
                               .withName("Pitch")
                               .withGroupName("Oscillator")
                               .withID(PAR_PITCH)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 1.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("%", 100.0f)
                               .withName("Quant Pitch")
                               .withGroupName("Oscillator")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE)
                               .withID(PAR_QUANTIZEPITCH));
    for (int i = 0; i < 4; ++i)
    {
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName(fmt::format("Mod Slot {} Depth", i + 1))
                                   .withGroupName("Oscillator")
                                   .withID(PAR_GRAINMODSLOTAMOUNT0 + i)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
    }
    for (int i = 0; i < GranulatorVoice::num_aux_envelopes; ++i)
    {
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("")
                                   .withName(fmt::format("Aux Env {} Time Warp", i + 1))
                                   .withGroupName("Oscillator")
                                   .withID(PAR_AUXENVTIMEWARP + i)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
    }
    for (int i = 0; i < GranulatorModConfig::FixedMatrixSize; ++i)
    {
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("")
                                   .withName(fmt::format("Mod Slot {} Depth", i + 1))
                                   .withGroupName("ModMatrix")
                                   .withID(PAR_MAINMODDEPTHSTART + i)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
    }
    for (int i = 0; i < GranulatorVoice::num_aux_envelopes; ++i)
    {
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("")
                                   .withName(fmt::format("Aux Env {} Time Shift", i + 1))
                                   .withGroupName("Oscillator")
                                   .withID(PAR_AUXENVTIMESHIFT + i)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
    }
    parmetadatas.push_back(pmd()
                               .withRange(0.0, 4.0)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("ST", 12.0f)
                               .withName("OSC Sync")
                               .withID(PAR_OSC_SYNC)
                               .withGroupName("Oscillator")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0, 1.0)
                               .withDefault(0.5)
                               .withLinearScaleFormatting("%", 100.0f)
                               .withName("OSC PW")
                               .withID(PAR_OSC_PW)
                               .withGroupName("Oscillator")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-48.0, 48.0)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("ST")
                               .withName("FM Pitch")
                               .withID(PAR_FMPITCH)
                               .withGroupName("Oscillator")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .asBool()
                               .withOnOffFormatting()
                               .withDefault(0.0)
                               .withName("Follow Main Pitch")
                               .withID(PAR_FMPITCHFOLLOWSMAINPITCH)
                               .withGroupName("Oscillator"));
    parmetadatas.push_back(pmd()
                               .withRange(0.0, 1.0)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("%", 100.0f)
                               .withName("FM Depth")
                               .withGroupName("Oscillator")
                               .withID(PAR_FMDEPTH)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-1.0, 1.0)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("%", 100.0f)
                               .withName("FM Feedback")
                               .withGroupName("Oscillator")
                               .withID(PAR_FMFEEDBACK)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-1.0, 1.0)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("%", 100.0f)
                               .withName("Noise Correlation")
                               .withGroupName("Oscillator")
                               .withID(PAR_NOISECORRELATION)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(
        pmd()
            .withUnorderedMapFormatting({{0, "Corr noise No interpolation"},
                                         {1, "Corr noise Linear interpolation"},
                                         {2, "Corr noise Corrupted output"},
                                         {3, "Corr noise BounceIn interpolation"},
                                         {4, "Logistic Chaos Linear interpolation"},
                                         {5, "Sinc/Pulse"}},
                                        true)
            .withDefault(1)
            .withName("Noise Mode")
            .withGroupName("Oscillator")
            .withID(PAR_NOISEMODE));
    for (size_t i = 0; i < GranulatorVoice::numInsertSlots; ++i)
    {
        auto groupname = fmt::format("Insert {}", char('A' + i));
        for (size_t j = 0; j < GranulatorVoice::maxParamsPerInsert; ++j)
        {
            size_t insparid = PAR_INSERTAFIRST + 32 * i + j;
            if (i == 3 && j == 0)
                assert(insparid == PAR_INSERTDFIRST);
            auto insparname = fmt::format("Ins {} Par {}", char('A' + i), j);
            parmetadatas.push_back(pmd()
                                       .withRange(0.0, 1.0)
                                       .withDefault(0.0)
                                       .withLinearScaleFormatting("")
                                       .withName(insparname)
                                       .withID(insparid)
                                       .withGroupName(groupname)
                                       .withFlags(CLAP_PARAM_IS_MODULATABLE));
        }
    }

    parmetadatas.push_back(pmd()
                               .withRange(-180.0f, 180.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("°")
                               .withName("Azimuth")
                               .withGroupName("Spatialization")
                               .withID(PAR_AZIMUTH)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-180.0f, 180.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("°")
                               .withName("Elevation")
                               .withGroupName("Spatialization")
                               .withID(PAR_ELEVATION)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-180.0f, 180.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("°")
                               .withName("Ambisonic Spread")
                               .withGroupName("Spatialization")
                               .withID(PAR_AMBSPREAD)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-180.0f, 180.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("°")
                               .withName("Ambisonic Rotation")
                               .withGroupName("Spatialization")
                               .withID(PAR_AMBROTATE)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 18.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("dB")
                               .withName("Omni Boost")
                               .withGroupName("Spatialization")
                               .withID(PAR_AMBOMNIBOOST)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .asInt()
                               .withRange(1.0f, 16.0f)
                               .withDefault(1.0)
                               .withIntegerQuantization()
                               .withName("Count")
                               .withGroupName("Repeats")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE)
                               .withID(PAR_STACKCOUNT));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 1.0f)
                               .withDefault(0.5)
                               .withOffsetPowerFormatting("s", 0.05f, 1.95, 2.0f, 1.0f)
                               .withName("Time Span")
                               .withGroupName("Repeats")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE)
                               .withID(PAR_STACKTIMESPAN));
    parmetadatas.push_back(pmd()
                               .withRange(-1.0f, 1.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("", 1.0f)
                               .withName("Time Curve")
                               .withGroupName("Repeats")
                               .withFlags(CLAP_PARAM_IS_MODULATABLE)
                               .withID(PAR_STACKTIMECURVE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 1.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("ST")
                               .withName("Pitch RW")
                               .withGroupName("Repeats")
                               .withID(PAR_STACKRANDOMPITCH)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 10.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("", 1.0f)
                               .withName("Spat RW")
                               .withGroupName("Repeats")
                               .withID(PAR_STACKRANDOMSPATIALIZATION)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 1.0f)
                               .withDefault(1.0)
                               .withLinearScaleFormatting("%", 100.0f)
                               .withName("End Volume")
                               .withGroupName("Repeats")
                               .withID(PAR_STACKENDVOLUME)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-7.0f, 7.0f)
                               .withDefault(0.0)
                               .withDecimalPlaces(3)
                               .withATwoToTheBFormatting(1.0f, 1.0f, "Hz")
                               .withName("LFO Master Rate")
                               .withGroupName("LFO Master")
                               .withID(PAR_MASTERLFORATE)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    for (int i = 0; i < GranulatorModMatrix::numLfos; ++i)
    {
        parmetadatas.push_back(pmd()
                                   .asOnOffBool()
                                   .withID(PAR_LFOUNIPOLARS + i)
                                   .withName(fmt::format("UNIPOLAR"))
                                   .withGroupName(fmt::format("LFO {}", i + 1)));
        parmetadatas.push_back(pmd()
                                   .asOnOffBool()
                                   .withID(PAR_LFOMASTERSYNCS + i)
                                   .withName(fmt::format("MST SYNC"))
                                   .withGroupName(fmt::format("LFO {}", i + 1)));
        parmetadatas.push_back(pmd()
                                   .withUnorderedMapFormatting({{0, "SIN"},
                                                                {1, "SIN<>SQR<>TRI"},
                                                                {2, "DOWN<>TRI<>UP"},
                                                                {3, "SMOOTH NOISE"},
                                                                {4, "S&H NOISE"}},
                                                               true)
                                   .withName(fmt::format("LFO {} SHAPE", i + 1))
                                   .withGroupName(fmt::format("LFO {}", i + 1))
                                   .withID(PAR_LFOSHAPES + i));
        parmetadatas.push_back(pmd()
                                   .withRange(-7.0, 7.0)
                                   .withDefault(0.0)
                                   .withDecimalPlaces(3)
                                   .withATwoToTheBFormatting(1.0f, 1.0f, "Hz")
                                   .withName(fmt::format("LFO {} RATE", i + 1))
                                   .withShortName("RATE")
                                   .withGroupName(fmt::format("LFO {}", i + 1))
                                   .withID(PAR_LFORATES + i)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName(fmt::format("LFO {} DEFORM", i + 1))
                                   .withShortName("DEFORM")
                                   .withGroupName(fmt::format("LFO {}", i + 1))
                                   .withID(PAR_LFODEFORMS + i)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName(fmt::format("LFO {} SHIFT", i + 1))
                                   .withGroupName(fmt::format("LFO {}", i + 1))
                                   .withID(PAR_LFOSHIFTS + i)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName(fmt::format("LFO {} WARP", i + 1))
                                   .withGroupName(fmt::format("LFO {}", i + 1))
                                   .withID(PAR_LFOWARPS + i)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
    }
    // std::cout << parmetadatas.size() << " parameters inited\n";
    paramvalues.resize(parmetadatas.size());
    for (int i = 0; i < parmetadatas.size(); ++i)
    {
        idtoparmetadata[parmetadatas[i].id] = &parmetadatas[i];
        paramvalues[i] = parmetadatas[i].defaultVal;
        idtoparvalptr[parmetadatas[i].id] = &paramvalues[i];
        if (parmetadatas[i].flags & CLAP_PARAM_IS_MODULATABLE)
        {
            // we might want to have custom ranges too, but these
            // autogenerated ones suffice for now
            float range = (parmetadatas[i].maxVal - parmetadatas[i].minVal);
            modRanges[parmetadatas[i].id] = range;
        }
    }

    create_voices();

    for (size_t i = 0; i < parmetadatas.size(); ++i)
    {
        const auto &md = parmetadatas[i];
        if (md.flags & CLAP_PARAM_IS_MODULATABLE)
        {
            modmatrix.m.bindTargetBaseValue(GranulatorModConfig::TargetIdentifier{(int)md.id},
                                            *idtoparvalptr[md.id]);
        }
    }
    modmatrix.m.bindTargetBaseValue(GranulatorModConfig::TargetIdentifier{(int)1},
                                    dummyTargetValue);

    modSourceInfos.reserve(256);
    modSourceInfos.emplace_back("Off", "", GranulatorModConfig::SourceIdentifier{0});
    for (uint32_t i = 0; i < GranulatorModMatrix::numLfos; ++i)
    {
        modSourceInfos.emplace_back(fmt::format("LFO {}", i + 1), "LFO",
                                    GranulatorModConfig::SourceIdentifier{i + 1});
    }
    for (uint32_t i = 0; i < 8; ++i)
    {
        modSourceInfos.emplace_back(fmt::format("StepSeq {}", i + 1), "Step Sequencer",
                                    GranulatorModConfig::SourceIdentifier{STEPS0 + i});
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
        modSourceInfos.emplace_back(fmt::format("Random {}", i + 1), "Random",
                                    GranulatorModConfig::SourceIdentifier{RANDOM0 + i});
    }
    modSourceInfos.emplace_back("Spectral Centroid", "Audio Input Analysis",
                                GranulatorModConfig::SourceIdentifier{AA_CENTROID});
    modSourceInfos.emplace_back("Spectral Spread", "Audio Input Analysis",
                                GranulatorModConfig::SourceIdentifier{AA_SPEAD});
    modSourceInfos.emplace_back("Volume Level", "Audio Input Analysis",
                                GranulatorModConfig::SourceIdentifier{AA_LEVEL});
    for (uint32_t i = 0; i < 16; ++i)
    {
        modSourceInfos.emplace_back(fmt::format("Host Parameter {}", i + 1), "Host Parameter",
                                    GranulatorModConfig::SourceIdentifier{HOSTPARAMSTART + i});
    }
    modSourceInfos.emplace_back("MIDI KEY", "MIDI NOTES",
                                GranulatorModConfig::SourceIdentifier{MIDINOTE});
    modSourceInfos.emplace_back("MIDI VELOCITY", "MIDI NOTES",
                                GranulatorModConfig::SourceIdentifier{MIDIVELO});
    modSourceInfos.emplace_back("MIDI AFTERTOUCH", "MIDI NOTES",
                                GranulatorModConfig::SourceIdentifier{MIDIAT});
    for (uint32_t i = 1; i < 128; ++i)
    {
        modSourceInfos.emplace_back(fmt::format("MIDI CC {}", i), "MIDI CC",
                                    GranulatorModConfig::SourceIdentifier{i + MIDICCSTART});
    }
    std::cout << "num mod sources " << modSourceInfos.size() << "\n";
    for (auto &v : modSourceValues)
        v = 0.0f;
    for (uint32_t i = 0; i < modSourceInfos.size(); ++i)
    {
        // std::print("{} binding {} {} to {}\n", i,
        // modSources[i].id.src, modSources[i].name,
        //            (void *)&modSourceValues[i]);
        modmatrix.m.bindSourceValue(modSourceInfos[i].id, modSourceValues[i]);
    }
    init_filter_infos();
}
void ToneGranulator::create_voices()
{
    std::fill(pitchBandAttensShared.begin(), pitchBandAttensShared.end(), 1.0f);
    // by default one to one mapping but for easier working with modulation
    // another mapping can be used
    for (size_t i = 0; i < osctypemapping.size(); ++i)
    {
        osctypemapping[i] = i;
    }
    tuning = Tunings::evenTemperament12NoteScale();
    for (int i = 0; i < numvoices; ++i)
    {
        auto v = std::make_unique<GranulatorVoice>();
        v->tuning = &tuning;
        v->aux_envelopes = &voiceaux_envelopes;
        v->pitchBandAttens = pitchBandAttensShared;
        v->osctypemapping = osctypemapping;
        v->eluts = &eluts;
        voices.push_back(std::move(v));
    }
}
void ToneGranulator::set_oscillator_type_mapping(std::span<int> mapping)
{
    std::lock_guard<choc::threading::SpinLock> locker(spinLock);
    for (size_t i = 0; i < osctypemapping.size(); ++i)
    {
        if (i < mapping.size())
            osctypemapping[i] = mapping[i];
    }
}
std::string ToneGranulator::load_scala_file(std::string path, bool called_from_audio_thread)
{
    try
    {
        auto scale = Tunings::readSCLFile(path);
        auto temp = Tunings::Tuning(scale);
        if (!is_monotonic_tuning(temp))
            throw std::runtime_error("tuning is not monotonic");
        if (!called_from_audio_thread)
            spinLock.lock();
        tuning = temp;
        currentScalaFile = path;
        if (!called_from_audio_thread)
            spinLock.unlock();
    }
    catch (std::exception &excep)
    {
        return excep.what();
    }
    return {};
}
void ToneGranulator::set_ambisonics_order(int order)
{
    assert(order > 0 && order < 8);
    if (current_ambisonic_order == order || fadeForLargeStateChange.is_active())
        return;
    pending_ambisonic_order = order;
    fadeForLargeStateChange.start(m_sr, 500.0f, [this]() {
        current_ambisonic_order = pending_ambisonic_order;
        num_out_chans = ambisonicOrderNumChannels(current_ambisonic_order);
        masterHighPassFilter.numactivechannels = num_out_chans;
        // std::print(std::cerr, "changed ambisonic order to {}\n", current_ambisonic_order);
        for (auto &vc : voices)
        {
            vc->active = false;
            vc->ambisonic_order = current_ambisonic_order;
            vc->num_outputchans = num_out_chans;
        }
    });
    return;
    current_ambisonic_order = order;
    for (auto &v : voices)
    {
        v->active = false;
        v->ambisonic_order = order;
        v->num_outputchans = ambisonicOrderNumChannels(order);
    }
    num_out_chans = ambisonicOrderNumChannels(order);
}
void ToneGranulator::advanceCloudPlayers()
{
    int i = 0;
    for (auto &p : cloudPlayers)
    {
        if (p.active)
        {
            CloudEvent *ev = nullptr;
            if (p.event_index >= 0 && p.event_index < p.cloud->events.size())
                ev = &p.cloud->events[p.event_index];
            while (ev && std::floor((ev->time_position + p.start_time) * m_sr) <
                             playposframes + granul_block_size)
            {
                // std::cout << playposframes << " cloud player " << i
                //           << " wants to start event with timepos " << ev->time_position <<
                //           "\n";
                bool wasfound = false;
                for (int j = 0; j < voices.size(); ++j)
                {
                    if (!voices[j]->active)
                    {
                        // std::print("starting voice {} for event {}\n", j, evindex);
                        voices[j]->grainid = graincount;
                        GrainEvent gev{0.0, 0.1, 0.0, 1.0};
                        gev.duration = modmatrix.m.getTargetValue(
                            GranulatorModConfig::TargetIdentifier{PAR_DURATION});
                        gev.azimuth = modmatrix.m.getTargetValue(
                            GranulatorModConfig::TargetIdentifier{PAR_AZIMUTH});
                        gev.generator_type = modmatrix.m.getTargetValue(
                            GranulatorModConfig::TargetIdentifier{PAR_OSCTYPE});
                        float syncoctaves = modmatrix.m.getTargetValue(
                            GranulatorModConfig::TargetIdentifier{PAR_OSC_SYNC});
                        gev.sync_octaves = syncoctaves;
                        gev.pulse_width = modmatrix.m.getTargetValue(
                            GranulatorModConfig::TargetIdentifier{PAR_OSC_PW});
                        for (auto &pc : ev->param_modulations)
                        {
                            if (pc.id == CLAP_INVALID_ID)
                                break;
                            if (pc.id == PAR_PITCH)
                                gev.pitch_semitones = pc.value;
                            else if (pc.id == PAR_GRAINVOLUME)
                                gev.volume = pc.value;
                            else if (pc.id == PAR_DURATION)
                                gev.duration = pc.value;
                            else if (pc.id == PAR_GRAINVOLUME)
                                gev.volume = pc.value;
                            else if (pc.id == PAR_OSCTYPE)
                                gev.generator_type = pc.value;
                            else if (pc.id == PAR_AZIMUTH)
                                gev.azimuth = pc.value;
                            else if (pc.id == PAR_OSC_SYNC)
                                gev.sync_octaves = pc.value;
                        }
                        if (p.cloud->after_touch_dest == PAR_GRAINVOLUME)
                        {
                            // for testing, absolute value but probably will be modulation later
                            gev.volume = p.after_touch_amount;
                        }
                        for (size_t j = 0; j < GranulatorVoice::numInsertSlots; ++j)
                        {
                            auto numpars = voices.front()->insert_fx[j].numParams;
                            for (size_t k = 0; k < numpars; ++k)
                            {
                                int insparid = PAR_INSERTAFIRST + 32 * j + k;
                                gev.insertparams[j][k] = modmatrix.m.getTargetValue(
                                    GranulatorModConfig::TargetIdentifier{insparid});
                            }
                        }
                        if (p.cloud->after_touch_dest >= PAR_INSERTAFIRST &&
                            p.cloud->after_touch_dest < PAR_INSERTAFIRST + 10)
                        {
                            gev.insertparams[0][p.cloud->after_touch_dest - PAR_INSERTAFIRST] =
                                p.after_touch_amount;
                        }
                        voices[j]->start(gev);
                        if (gatherGrainVisData)
                        {
                            GrainVisualizerMessage vmsg;
                            vmsg.timepos = playposframes / m_sr;
                            vmsg.pitch = voices[j]->pitch_base;
                            vmsg.duration = voices[j]->grain_end_phase / m_sr;
                            vmsg.gain = voices[j]->grain_base_volume;
                            vmsg.azimuth0degrees = voices[j]->used_azi0;
                            vmsg.azimuth1degrees = voices[j]->used_azi1;
                            vmsg.elevation0degrees = voices[j]->used_ele0;
                            vmsg.elevation1degrees = voices[j]->used_ele1;
                            visualizer_fifo.push(vmsg);
                        }
                        wasfound = true;
                        // std::cout << "grain " << graincount << " started on voice " << j
                        //           << "\n";
                        ++graincount;
                        break;
                    }
                }
                if (!wasfound)
                {
                    ++missedgrains;
                }
                ++p.event_index;
                if (p.event_index >= p.cloud->events.size())
                {
                    ev = nullptr;
                    p.active = false;
                    std::cout << playposframes << " cloudplayer " << i << " reached end\n";
                }
                else
                    ev = &p.cloud->events[p.event_index];
            }
            if (p.event_index >= p.cloud->events.size())
            {
                // p.event_index = 0;
                // p.active = true;
                // p.start_time = playposframes / m_sr;
            }
        }
        ++i;
    }
}
void ToneGranulator::advanceFullEventList()
{
    GrainEvent *ev = nullptr;
    if (evindex < events.size())
        ev = &events[evindex];
    while (ev && std::floor(ev->time_position * m_sr) < playposframes + granul_block_size)
    {
        bool wasfound = false;
        for (int j = 0; j < voices.size(); ++j)
        {
            if (!voices[j]->active)
            {
                // std::print("starting voice {} for event {}\n", j, evindex);
                voices[j]->grainid = graincount;
                voices[j]->start(*ev);
                wasfound = true;
                ++graincount;
                break;
            }
        }
        if (!wasfound)
        {
            ++missedgrains;
        }
        ++evindex;
        if (evindex >= events.size())
            ev = nullptr;
        else
            ev = &events[evindex];
    }
}

void ToneGranulator::prepare(float samplerate, int filter_routing, float tail_len,
                             float tail_fade_len)
{

    {
        std::lock_guard<choc::threading::SpinLock> locker(spinLock);
        for (int i = 0; i < numvoices; ++i)
        {
            auto &v = voices[i];
            v->set_samplerate(samplerate);
            v->filter_routing = (GranulatorVoice::FilterRouting)filter_routing;
            v->tail_len = tail_len;
            v->tail_fade_len = tail_fade_len;
        }

        m_sr = samplerate;
        missedgrains = 0;
        evindex = 0;
        playposframes = 0;
        gainlag.setRateInMilliseconds(1000.0, m_sr, 1.0);
        gainlag.snapTo(0.0);
        graingen_phase = 0.0;
        graingen_phase_prior = 2.0;
        modmatrix.set_sample_rate(samplerate);
        modmatrix.m.prepare(modmatrix.rt, samplerate, granul_block_size);
        masterHighPassFilter.prepare(m_sr);
        fxInstanceForMetadata.prepareInstance(m_sr, granul_block_size);
    }
}

void ToneGranulator::set_filter(int which, uint8_t mainmode, uint8_t awtype, sfpp::FilterModel mo,
                                sfpp::ModelConfig conf)
{
    if (which < 0 || which >= currentInsertConfs.size())
        return;
    if (currentInsertConfs[which].mainmode == mainmode &&
        currentInsertConfs[which].awtype == awtype && currentInsertConfs[which].sstmodel == mo &&
        currentInsertConfs[which].sstconfig == conf)
    {
        return;
    }
    int oldmainmode = currentInsertConfs[which].mainmode;
    currentInsertConfs[which].mainmode = mainmode;
    currentInsertConfs[which].awtype = awtype;
    currentInsertConfs[which].sstconfig = conf;
    currentInsertConfs[which].sstmodel = mo;
    GrainInsertFX::ModeInfo gmode;
    gmode.mainmode = mainmode;
    gmode.awtype = awtype;
    gmode.sstconfig = conf;
    gmode.sstmodel = mo;
    fxInstanceForMetadata.setMode(gmode);
    for (size_t j = 0; j < GranulatorVoice::maxParamsPerInsert; ++j)
    {
        int parid = PAR_INSERTAFIRST + 32 * which + j;
        // if old mode was already sst filter, don't set the parameter
        if (oldmainmode == GrainInsertFX::GFXNONE || oldmainmode == GrainInsertFX::GFXAIRWINDOWS ||
            oldmainmode == GrainInsertFX::GFXXENAKIOS)
        {
            *idtoparvalptr[parid] = fxInstanceForMetadata.paramvalues[j];
        }
        idtoparmetadata[parid]->name = fxInstanceForMetadata.getParameterName(j);
        idtoparmetadata[parid]->defaultVal = fxInstanceForMetadata.paramvalues[j];
    }
    for (int i = 0; i < numvoices; ++i)
    {
        auto &v = voices[i];
        v->set_insert_type(which, mainmode, awtype, mo, conf);
    }
}
