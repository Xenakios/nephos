#include "granularsynth.h"
#include "Tunings.h"
#include "easing.h"
#include "xen_modulationsources.h"
#include <unordered_map>

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
    std::unordered_map<int, std::string> valuewarpmap;
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
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("")
                                   .withName(fmt::format("Aux Env {} Time Shift", i + 1))
                                   .withGroupName("Oscillator")
                                   .withID(PAR_AUXENVTIMESHIFT + i)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));

        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .deformable()
                                   .withDeformationCount(2)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("")
                                   .withName(fmt::format("Aux Env {} Value Curve", i + 1))
                                   .withGroupName("Oscillator")
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE)
                                   .withID(PAR_AUXENVVALUEWARP + i));
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
                               .withName("Noise P1")
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
                                         {5, "Sinc/Pulse"},
                                         {6, "Tilt filtered noise"}},
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
                               .withName("FX Spread")
                               .withGroupName("Spatialization")
                               .withID(PAR_AMBSPREAD)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-180.0f, 180.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("°")
                               .withName("FX Rotation")
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
                               .withOffsetPowerFormatting("s", 0.00f, 2.0, 2.0f, 1.0f)
                               .withName("Time Span")
                               .withDecimalPlaces(3)
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
                               .withRange(-1.0f, 1.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("%", 100)
                               .withName("Pitch Variation")
                               .withGroupName("Repeats")
                               .withID(PAR_STACKPITCHPAR1)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-1.0f, 1.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("", 1.0f)
                               .withName("Azimuth Variation")
                               .withGroupName("Repeats")
                               .withID(PAR_STACKSPATIALIZATIONPAR1)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-1.0f, 1.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("", 1.0f)
                               .withName("Elevation Variation")
                               .withGroupName("Repeats")
                               .withID(PAR_STACKSPATIALIZATIONPAR2)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(0.0f, 1.0f)
                               .withDefault(1.0)
                               .withLinearScaleFormatting("%", 100.0f)
                               .withName("Volume Variation")
                               .withGroupName("Repeats")
                               .withID(PAR_STACKVOLUMEPAR1)
                               .withFlags(CLAP_PARAM_IS_MODULATABLE));
    parmetadatas.push_back(pmd()
                               .withRange(-1.0f, 1.0f)
                               .withDefault(0.0)
                               .withLinearScaleFormatting("", 100.0f)
                               .withName("Duration Variation")
                               .withGroupName("Repeats")
                               .withID(PAR_STACKDURATIONPAR1)
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
    lagged_params.reserve(parmetadatas.size());
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
void ToneGranulator::set_parameter_value(uint32_t id, float value)
{
    assert(id != CLAP_INVALID_ID);
    float *destval = idtoparvalptr[id];
    assert(destval);
    if constexpr (!lag_parameters)
    {
        *destval = value;
        return;
    }
    else
    {
        assert(m_sr > 0.0);
        // if (value == *destval || id == PAR_OSCTYPE)
        if (id == PAR_OSCTYPE)
        {
            *destval = value;
            return;
        }
        bool found = false;
        for (auto &e : lagged_params)
        {
            if (e.id == id)
            {
                // e.lagger.setRateInMilliseconds(500.0, m_sr, 1.0 / granul_block_size);
                e.lagger.setTarget(value);
                found = true;
                // std::cout << "param " << id << " was alreagy lagged " << value << "\n";
                // break;
            }
        }
        if (!found)
        {
            LaggedParameter p;
            p.id = id;
            p.valptr = destval;
            p.lagger.setRateInMilliseconds(1000.0, m_sr, 1.0 / granul_block_size);
            p.lagger.v = *destval;
            p.lagger.target_v = *destval;
            p.lagger.setTarget(value);
            lagged_params_spinlock.lock();
            lagged_params.push_back(p);
            lagged_params_spinlock.unlock();
            /*
            if (p.lagger.isActive())
            {
                lagged_params_spinlock.lock();
                lagged_params.push_back(p);
                lagged_params_spinlock.unlock();
            }
            else
            {
                *destval = value;
            }
            */
        }
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

void ToneGranulator::set_state(choc::value::ValueView state)
{
    int64_t state_ignore_flags = state["state_ignore_flags"].getWithDefault(0);
    if (state_ignore_flags & SIF_DASHBOARDSETTING)
    {
        gvsettings.timespantoshow = state["gvs_timespan"].getWithDefault(8.0);
    }

    if (state.hasObjectMember("tuning"))
    {
        auto tunstate = state["tuning"];
        auto scalastring = tunstate["scala_string"].getWithDefault(std::string(""));
        auto scalafile = tunstate["scala_file"].getWithDefault(std::string(""));
        auto err = load_scala_string(scalastring, scalafile, true);
        if (!err.empty())
        {
            std::cout << err << std::endl;
        }
    }
    if (state.hasObjectMember("trigrandstates"))
    {
        auto randstates = state["trigrandstates"];
        for (int i = 0; i < randstates.size(); ++i)
        {
            randomModSources[i].set_state(randstates[i]);
        }
    }
    if (state.hasObjectMember("osctypemapping"))
    {
        auto osctypemap = state["osctypemapping"];
        if (osctypemap.isArray())
        {
            for (int i = 0; i < osctypemap.size(); ++i)
            {
                if (i < osctypemapping.size())
                {
                    osctypemapping[i] = osctypemap[i].getWithDefault(i);
                }
            }
        }
    }
    if (state.hasObjectMember("grainmodroutings"))
    {
        auto grainmodroutings = state["grainmodroutings"];
        for (int i = 0; i < grainmodroutings.size(); ++i)
        {
            auto routing = grainmodroutings[i];
            uint32_t src = routing["source"].getWithDefault(CLAP_INVALID_ID);
            uint32_t dest = routing["destination"].getWithDefault(CLAP_INVALID_ID);
            set_grain_modulation_routing(i, src, dest, true);
        }
    }
    if (state.hasObjectMember("auxenvstates"))
    {
        auto auxenvstates = state["auxenvstates"];
        for (int i = 0; i < auxenvstates.size(); ++i)
        {
            if (i >= voiceaux_envelopes.size())
                break;
            auto auxenvstate = auxenvstates[i];
            voiceaux_envelopes[i].set_state(auxenvstate);
        }
    }
    if (state.hasObjectMember("stepseqstates"))
    {
        auto stepseqstate = state["stepseqstates"];
        for (size_t i = 0; i < stepseqstate.size(); ++i)
        {
            if (i >= stepModSources.size())
                break;
            auto &ss = stepModSources[i];
            auto seqstate = stepseqstate[(int)i];
            auto steps = seqstate["steps"];
            for (size_t j = 0; j < steps.size(); ++j)
            {
                if (j < 128)
                {
                    StepModSource::Message msg;
                    msg.opcode = StepModSource::Message::OP_SETSTEP;
                    msg.dest = i;
                    msg.ival0 = j;
                    msg.fval0 = steps[(int)j].getWithDefault(0.0f);
                    fifo.push(msg);
                }
            }
            StepModSource::Message msg;
            msg.opcode = StepModSource::Message::OP_LOOPSTART;
            msg.dest = i;
            msg.ival0 = seqstate["startstep"].getWithDefault(0);
            fifo.push(msg);
            msg.opcode = StepModSource::Message::OP_LOOPLEN;
            msg.ival0 = seqstate["looplen"].getWithDefault(1);
            fifo.push(msg);
            msg.opcode = StepModSource::Message::OP_PLAYMODE;
            msg.ival0 = seqstate["playmode"].getWithDefault(0);
            fifo.push(msg);
        }
    }
    if (state.hasObjectMember("filterstates"))
    {
        auto filterstates = state["filterstates"];
        for (int i = 0; i < filterstates.size(); ++i)
        {
            auto filterstate = filterstates[i];
            if (i < 2)
            {
                sfpp::FilterModel m = (sfpp::FilterModel)filterstate["model"].getWithDefault(0);
                sfpp::ModelConfig conf;
                conf.dt = (decltype(conf.dt))filterstate["dt"].getWithDefault(0);
                conf.st = (decltype(conf.st))filterstate["st"].getWithDefault(0);
                conf.mt = (decltype(conf.mt))filterstate["mt"].getWithDefault(0);
                conf.pt = (decltype(conf.pt))filterstate["pt"].getWithDefault(0);
                int mainmode = filterstate["mainmode"].getWithDefault(0);
                int awtype = filterstate["awtype"].getWithDefault(0);
                set_filter(i, mainmode, awtype, m, conf);
            }
        }
    }
    if (state.hasObjectMember("params"))
    {
        auto params = state["params"];
        auto &pars = parmetadatas;
        bool ignoreMasterVolume = state_ignore_flags & SIF_MASTERVOLUME;
        bool ignoreAmbisonicOrder = state_ignore_flags & SIF_AMBISONICORDER;
        for (int i = 0; i < pars.size(); ++i)
        {
            if (ignoreMasterVolume && pars[i].id == ToneGranulator::PAR_MAINVOLUME)
                continue;
            if (ignoreAmbisonicOrder && pars[i].id == ToneGranulator::PAR_AMBORDER)
                continue;
            std::string id = std::to_string(pars[i].id);
            if (params.hasObjectMember(id))
            {
                float v = params[id].getWithDefault(pars[i].defaultVal);
                set_parameter_value(pars[i].id, v);
                //*idtoparvalptr[pars[i].id] = v;
            }
        }
    }
    if (state.hasObjectMember("modroutings"))
    {
        auto routings = state["modroutings"];
        auto &mm = modmatrix;
        for (int i = 0; i < GranulatorModConfig::FixedMatrixSize; ++i)
        {
            mm.rt.updateActiveAt(i, false);
        }
        for (int i = 0; i < routings.size(); ++i)
        {
            auto rstate = routings[i];
            int slot = rstate["slot"].get<int>();
            if (slot >= 0 && slot < GranulatorModConfig::FixedMatrixSize)
            {
                mm.rt.updateActiveAt(slot, true);
                uint32_t src = rstate["source"].getWithDefault(0);
                uint32_t srcvia = rstate["via"].getWithDefault(0);
                int curve = rstate["curve"].getWithDefault(1);
                float d = 0.0f;
                int dest = rstate["dest"].getWithDefault(1);
                mm.rt.updateRoutingAt(slot, GranulatorModConfig::SourceIdentifier{src},
                                      GranulatorModConfig::SourceIdentifier{srcvia},
                                      GranulatorModConfig::MyCurve{curve},
                                      GranulatorModConfig::TargetIdentifier{dest}, d);
                if (srcvia == 0)
                    mm.rt.routes[slot].sourceVia = std::nullopt;
            }
        }
        mm.m.prepare(mm.rt, m_sr, granul_block_size);
    }
}

std::string ToneGranulator::load_scala_string(std::string scalatext, std::string scalapath,
                                              bool called_from_audio_thread)
{
    try
    {
        auto scale = Tunings::parseSCLData(scalatext);
        auto temp = Tunings::Tuning(scale);
        if (!is_monotonic_tuning(temp))
            throw std::runtime_error("tuning is not monotonic");
        if (!called_from_audio_thread)
            spinLock.lock();
        tuning = temp;
        currentScalaFile = scalapath;
        if (!called_from_audio_thread)
            spinLock.unlock();
    }
    catch (std::exception &excep)
    {
        return excep.what();
    }
    return {};
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
void ToneGranulator::set_event_list(events_t evlist)
{
    if (evlist.size() > 0)
    {
        std::sort(evlist.begin(), evlist.end(), [](GrainEvent &lhs, GrainEvent &rhs) {
            return lhs.time_position < rhs.time_position;
        });
        std::erase_if(evlist, [](GrainEvent &e) {
            return e.time_position < 0.0 || (e.time_position + e.duration) > 600.0;
        });
        std::lock_guard<choc::threading::SpinLock> locker(spinLock);
        missedgrains = 0;
        evindex = 0;
        playposframes = 0;
        std::swap(evlist, events);
    }
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
                            vmsg.ambidegrees = voices[j]->current_coordinates;
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
void ToneGranulator::advanceAutoGeneratedGrains(float taillen)
{
    bool autogenpaused = *idtoparvalptr[PAR_PAUSEAUTOGEN];
    if (!autogenpaused)
    {
        generate_grain();
    }
    GrainEvent *ev = nullptr;
    if (scheduledIndex < scheduledGrains.size())
        ev = &scheduledGrains[scheduledIndex];
    while (ev && std::floor(ev->time_position * m_sr) < playposframes + granul_block_size)
    {
        bool voicewasfound = false;
        for (int j = 0; j < voices.size(); ++j)
        {
            if (!voices[j]->active)
            {
                // std::print("starting voice {} for scheduled event {}\n", j, evindex);
                // the grain oscillators and/or fx may produce lopsided waveforms
                // so have this to counteract
                if (graincount % 2 == 0)
                    voices[j]->polarity_gain = 1.0f;
                else
                    voices[j]->polarity_gain = -1.0f;
                // std::cout << graincount << " polarity gain " << voices[j]->polarity_gain
                //          << std::endl;
                voices[j]->grainid = graincount;
                voices[j]->doambnormalization = true;
                voices[j]->tail_len = taillen;
                voices[j]->tail_fade_len = std::clamp(taillen * 0.5, 0.002, 1.0);
                voices[j]->fmfollowsmainpitch = *idtoparvalptr[PAR_FMPITCHFOLLOWSMAINPITCH] >= 0.5f;
                voices[j]->start(*ev);
                voicewasfound = true;
                if (voicewasfound && gatherGrainVisData)
                {
                    GrainVisualizerMessage vmsg;
                    vmsg.timepos = ev->time_position;
                    vmsg.pitch = voices[j]->pitch_base;
                    vmsg.duration = voices[j]->grain_end_phase / m_sr;
                    vmsg.gain = voices[j]->grain_base_volume;
                    vmsg.ambidegrees = voices[j]->current_coordinates;
                    visualizer_fifo.push(vmsg);
                }
                modulatedOscType = ev->generator_type;
                ++graincount;
                break;
            }
        }
        // flag scheduled event for removal
        ev->time_position = -1.0;
        if (!voicewasfound)
        {
            ++missedgrains;
        }
        ++scheduledIndex;
        if (scheduledIndex >= scheduledGrains.size())
            ev = nullptr;
        else
            ev = &scheduledGrains[scheduledIndex];
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
void ToneGranulator::generate_grain()
{
    double actgrate =
        modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_DENSITY});
    actgrate = std::clamp(actgrate, -1.0, 8.0);

    double grate = 1.0 / std::pow(2.0, actgrate);
    for (int i = 0; i < granul_block_size; ++i)
    {
        if (graingen_phase_prior > graingen_phase)
        {
            std::erase_if(scheduledGrains, [](GrainEvent &e) { return e.time_position < 0.0; });
            float pitch =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_PITCH});
            float gdur =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_DURATION});
            float gvol =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_GRAINVOLUME});
            GrainEvent genev{0.0, gdur, pitch, gvol};
            genev.pitch_quantize_amount =
                std::clamp(modmatrix.m.getTargetValue(
                               GranulatorModConfig::TargetIdentifier{PAR_QUANTIZEPITCH}),
                           0.0f, 1.0f);
            genev.envelope_start_type = *idtoparvalptr[PAR_VOLENVEASINGSTART];
            genev.envelope_end_type = *idtoparvalptr[PAR_VOLENVEASINGEND];
            genev.envelope_shape =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_ENVMORPH});
            float azimuth =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_AZIMUTH});
            genev.azimuth = azimuth;
            float amb_spread =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_AMBSPREAD});
            float amb_rotate =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_AMBROTATE});
            float elevation =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_ELEVATION});
            genev.elevation = elevation;
            float omniboost =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_AMBOMNIBOOST});
            genev.generator_type =
                std::round(0.1f + modmatrix.m.getTargetValue(
                                      GranulatorModConfig::TargetIdentifier{PAR_OSCTYPE}));
            float fm_pitch =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_FMPITCH});
            genev.fm_pitch = fm_pitch;
            genev.fm_amount =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_FMDEPTH});
            genev.fm_feedback =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_FMFEEDBACK});
            genev.pulse_width =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_OSC_PW});
            genev.noisecorr = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{PAR_NOISECORRELATION});
            genev.noiseimode = *idtoparvalptr[PAR_NOISEMODE];
            genev.sync_octaves =
                modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_OSC_SYNC});
            for (size_t j = 0; j < GranulatorVoice::numInsertSlots; ++j)
            {
                auto numpars = voices.front()->insert_fx[j].numParams;
                for (size_t k = 0; k < numpars; ++k)
                {
                    int insparid = PAR_INSERTAFIRST + 32 * j + k;
                    genev.insertparams[j][k] =
                        modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{insparid});
                }
            }
            for (int k = 0; k < 4; ++k)
                genev.modamounts[k] = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_GRAINMODSLOTAMOUNT0 + k});
            auxenvdepthpmodulated = genev.modamounts[0];
            for (int k = 0; k < GranulatorVoice::num_aux_envelopes; ++k)
            {
                genev.auxenvparams[k] = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_AUXENVTIMEWARP + k});
                genev.auxenvparams[4 + k] = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_AUXENVTIMESHIFT + k});
                genev.auxenvparams[8 + k] = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_AUXENVVALUEWARP + k});
            }

            int numToSchedule = std::clamp(*idtoparvalptr[PAR_STACKCOUNT], 1.0f, 16.0f);
            float repreatpitchpar = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{PAR_STACKPITCHPAR1});
            repreatpitchpar = std::clamp(repreatpitchpar, -1.0f, 1.0f);
            float timeSpanToSchedule = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{PAR_STACKTIMESPAN});
            timeSpanToSchedule = std::clamp(timeSpanToSchedule, 0.0f, 1.0f);
            timeSpanToSchedule = 0.00f + 2.0f * std::pow(timeSpanToSchedule, 2.0f);
            float timeSpanCurve = *idtoparvalptr[PAR_STACKTIMECURVE];
            float azimuth_spread = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{PAR_STACKSPATIALIZATIONPAR1});
            azimuth_spread = std::clamp(azimuth_spread, -1.0f, 1.0f);
            float elevation_spread = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{PAR_STACKSPATIALIZATIONPAR2});
            elevation_spread = std::clamp(elevation_spread, -1.0f, 1.0f);
            uint32_t customrepeatwalkparam = CLAP_INVALID_ID;
            float *cust_target = nullptr;
            if (customrepeatwalkparam == PAR_OSC_SYNC)
                cust_target = &genev.sync_octaves;
            else if (customrepeatwalkparam == PAR_DURATION)
                cust_target = &genev.duration;
            auto azirepeats = process_spatial_repeats(
                rng, {numToSchedule, azimuth_repeats_mode, azimuth_spread}, genev.azimuth);
            auto elerepeats = process_spatial_repeats(
                rng, {numToSchedule, elevation_repeats_mode, elevation_spread}, genev.elevation);
            float volrepspread = *idtoparvalptr[PAR_STACKVOLUMEPAR1];
            auto volume_repeats = process_volume_repeats(
                rng, {numToSchedule, volume_repeats_mode, volrepspread}, gvol);
            float durrepsrap = *idtoparvalptr[PAR_STACKDURATIONPAR1];
            auto repeats_durations = process_duration_repeats(
                rng, {numToSchedule, RepeatsParameterProcessor::RPP_RAMP, durrepsrap},
                genev.duration);
            auto repeats_pitches = process_pitch_repeats(
                rng, {numToSchedule, pitch_repeats_mode, repreatpitchpar}, genev.pitch_semitones);
            float cachedpitch = genev.pitch_semitones;
            genev.ambi_spread = amb_spread;
            genev.ambi_rotate = amb_rotate;
            genev.ambi_omni_boost = omniboost;

            const float maxtimepow = 3.0f;
            for (int j = 0; j < numToSchedule; ++j)
            {
                double tpos = playposframes / this->m_sr;
                double normpos = 0.0;
                if (numToSchedule > 1)
                    normpos = 1.0 / (numToSchedule - 1) * j;
                if (timeSpanCurve < 0.0f)
                {
                    float ex = xenakios::mapvalue(timeSpanCurve, -1.0f, 0.0f, maxtimepow, 1.0f);
                    normpos = std::pow(normpos, ex);
                }
                else
                {
                    float ex = xenakios::mapvalue(timeSpanCurve, 0.0f, 1.0f, 1.0f, maxtimepow);
                    normpos = 1.0f - std::pow(1.0f - normpos, ex);
                }
                tpos += timeSpanToSchedule * normpos;
                genev.time_position = tpos;
                genev.volume = volume_repeats[j];
                genev.duration = repeats_durations[j];
                genev.pitch_semitones = repeats_pitches[j];
                genev.azimuth = azirepeats[j];
                genev.elevation = elerepeats[j];
                scheduledGrains.push_back(genev);
            }
            // need to sort because we may have previous unstarted events and our
            // playback scheduling logic needs the events sorted
            std::sort(scheduledGrains.begin(), scheduledGrains.end(),
                      [](auto &lhs, auto &rhs) { return lhs.time_position < rhs.time_position; });
            const bool printschedulesevents = false;
            if (printschedulesevents)
            {
                /*
                std::print("{:.2f} : ", playposframes / m_sr);
                for (auto &scgrain : scheduledGrains)
                    std::print("{:.2f} ", scgrain.time_position);
                std::print("\n");
                */
            }

            for (size_t sm = 0; sm < stepModSources.size(); ++sm)
            {
                stepModValues[sm] = stepModSources[sm].next();
            }
            for (size_t rm = 0; rm < randomModSources.size(); ++rm)
            {
                randomModValues[rm] = randomModSources[rm].next();
            }
            midiNoteModValue = midiNoteModSource.next();
            scheduledIndex = 0;
        }
        graingen_phase_prior = graingen_phase;
        graingen_phase += 1.0 / m_sr / grate;
        if (graingen_phase >= 1.0)
            graingen_phase -= 1.0;
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
void GranulatorVoice::start(GrainEvent &evpars)
{
    for (size_t i = 0; i < pendingInsertConfs.size(); ++i)
    {
        if (pendingInsertConfs[i].is_pending)
        {
            GrainInsertFX::ModeInfo gmode;
            gmode.mainmode = pendingInsertConfs[i].mainmode;
            gmode.awtype = pendingInsertConfs[i].awtype;
            gmode.sstconfig = pendingInsertConfs[i].sstconfig;
            gmode.sstmodel = pendingInsertConfs[i].sstmodel;
            insert_fx[i].setMode(gmode);
            pendingInsertConfs[i].is_pending = false;
        }
    }
    active = true;
    int newosctype = std::clamp(evpars.generator_type, 0, 6);
    assert(osctypemapping.size() == 7);
    newosctype = osctypemapping[newosctype];
    newosctype = std::clamp(newosctype, 0, 6);
    if (newosctype != prior_osc_type)
    {
        prior_osc_type = newosctype;
        if (newosctype == 0)
            theoscillator = sst::basic_blocks::dsp::EBApproxSin<>();
        else if (newosctype == 1)
            theoscillator = sst::basic_blocks::dsp::EBApproxSemiSin<>();
        else if (newosctype == 2)
            theoscillator = sst::basic_blocks::dsp::EBTri<>();
        else if (newosctype == 3)
            theoscillator = sst::basic_blocks::dsp::EBSaw<>();
        else if (newosctype == 4)
            theoscillator = sst::basic_blocks::dsp::EBPulse<>();
        else if (newosctype == 5)
            theoscillator = FMOsc();
        else if (newosctype == 6)
            theoscillator = NoiseGen();
        std::visit(
            [this](auto &q) {
                q.setSampleRate(sr);
                q.setFrequencySmoothingRateMS(5.0);
            },
            theoscillator);
    }
    if (samplerate_was_changed)
    {
        samplerate_was_changed = false;
        envgainlag.setRateInMilliseconds(5.0, sr, 1.0);
        std::visit(
            [this](auto &q) {
                q.setSampleRate(sr);
                q.setFrequencySmoothingRateMS(5.0);
            },
            theoscillator);
    }

    envgainlag.snapTo(0.0f);
    pitch_base = evpars.pitch_semitones;
    if (newosctype == 6)
        pitch_base += 12.0;
    pitch_base = std::clamp(pitch_base, -48.0f, 64.0f);
    if (evpars.pitch_quantize_amount > 0.0f && tuning)
    {
        // our middle C is 0.0, Tuning library has it at 60.0
        float quanamount = 1.0f - std::pow(1.0f - evpars.pitch_quantize_amount, 2.0f);
        auto quantpitch = quantize_pitch_binary(*tuning, pitch_base + 60.0, quanamount) - 60.0;
        pitch_base = quantpitch;
        // pitch_base = pitch_base * (1.0f - evpars.pitch_quantize_amount) +
        //             quantpitch * evpars.pitch_quantize_amount;
    }
    auto syncratio = std::clamp(evpars.sync_octaves, 0.0f, 4.0f);
    syncratio = std::pow(2.0f, syncratio);
    auto pw = evpars.pulse_width; // osc implementation clamps itself to 0..1
    fmpitch = evpars.fm_pitch;
    if (fmfollowsmainpitch)
        fmpitch += pitch_base;
    fmmodamount = std::clamp(evpars.fm_amount, 0.0f, 1.0f);
    fmfeedback = std::clamp(evpars.fm_feedback, -1.0f, 1.0f);

    auto logisticr = fmmodamount;

    auto noisecorr = std::clamp(evpars.noisecorr, -1.0f, 1.0f);
    auto noisemode = evpars.noiseimode;
    std::visit(
        [this, syncratio, pw, noisecorr, noisemode, logisticr](auto &q) {
            q.reset();
            q.setSyncRatio(syncratio);
            // handle extra parameters of osc types
            if constexpr (std::is_same_v<decltype(q), sst::basic_blocks::dsp::EBPulse<> &>)
            {
                q.setWidth(pw);
            }
            if constexpr (std::is_same_v<decltype(q), FMOsc &>)
            {
                float fmhz = 440.0 * std::pow(2.0, 1.0 / 12.0 * (fmpitch - 9.0));
                q.setModulatorFreq(fmhz);
                q.setModIndex(fmmodamount);
                q.setFeedbackAmount(fmfeedback);
            }
            if constexpr (std::is_same_v<decltype(q), NoiseGen &>)
            {
                q.setRandSeed(grainid);
                q.setCorrelation(noisecorr);
                q.tilt_filter = &tiltfilter;
                q.imode = noisemode;
                if (noisemode == 6)
                    q.init_tilt_filter();
                q.logisticx0 = 0.01 + 0.98 * (1.0 / 1024 * (grainid % 1024));
            }
        },
        theoscillator);

    float ambspread = std::clamp(evpars.ambi_spread, -180.0f, 180.0f);
    float ambrotate = std::clamp(evpars.ambi_rotate, -180.0f, 180.0f);
    float xa0 = -ambspread;
    float ya0 = 0.0f;
    float xb0 = ambspread;
    float yb0 = 0.0f;
    float rotrads = degreesToRadians(ambrotate);
    float rotsin = std::sin(rotrads);
    float rotcos = std::cos(rotrads);
    float xa1 = xa0 * rotcos - ya0 * rotsin;
    float ya1 = xa0 * rotsin + ya0 * rotcos;
    float xb1 = xb0 * rotcos - yb0 * rotsin;
    float yb1 = xb0 * rotsin + yb0 * rotcos;
    float azi0 = xa1 + -evpars.azimuth;
    float ele0 = ya1 + evpars.elevation;
    float azi1 = xb1 + -evpars.azimuth;
    float ele1 = yb1 + evpars.elevation;
    azi0 = wrap_value(-180.0f, azi0, 180.0f);
    azi1 = wrap_value(-180.0f, azi1, 180.0f);
    ele0 = wrap_value(-180.0f, ele0, 180.0f);
    ele1 = wrap_value(-180.0f, ele1, 180.0f);
    assert(azi0 >= -180.0f && azi0 <= 180.0f);
    assert(azi1 >= -180.0f && azi1 <= 180.0f);
    assert(ele0 >= -180.0f && ele0 <= 180.0f);
    assert(ele1 >= -180.0f && ele1 <= 180.0f);
    current_coordinates = {azi0, azi1, ele0, ele1};

    azi0 = degreesToRadians(azi0);
    azi1 = degreesToRadians(azi1);
    ele0 = degreesToRadians(ele0);
    ele1 = degreesToRadians(ele1);

    auto calc_ambicoeffs = [this](int inchan, float azimuth, float elevation) {
        assert(inchan >= 0 && inchan < 2);

        float x = 0.0;
        float y = 0.0;
        float z = 0.0;
        sphericalToCartesian(azimuth, elevation, x, y, z);
        float *coeffdata = ambcoeffs.data() + inchan * 64;
        switch (ambisonic_order)
        {
        case 1:
            SHEval1(x, y, z, coeffdata);
            break;
        case 2:
            SHEval2(x, y, z, coeffdata);
            break;
        case 3:
            SHEval3(x, y, z, coeffdata);
            break;
        case 4:
            SHEval4(x, y, z, coeffdata);
            break;
        case 5:
            SHEval5(x, y, z, coeffdata);
            break;
        case 6:
            SHEval6(x, y, z, coeffdata);
            break;
        case 7:
            SHEval7(x, y, z, coeffdata);
            break;
        }
        if (doambnormalization)
        {
            // if we use the actual output channel count, this won't autovectorize
            // but using the constant, it will and will always take 8 steps
            // so we lose a little with the lowest ambisonic orders, but otherwise
            // this works better than using the actual active output channel count
            for (int i = 0; i < 64; ++i)
                coeffdata[i] *= n3d2sn3d[i];
        }
    };
    calc_ambicoeffs(0, azi0, ele0);
    calc_ambicoeffs(1, azi1, ele1);
    omniboostinverse = -std::clamp(evpars.ambi_omni_boost, 0.0f, 18.0f);
    omniboostinverse = xenakios::decibelsToGain(omniboostinverse);
    phase = 0;
    float actdur = std::clamp(evpars.duration, 0.0f, 1.0f);
    actdur = actdur * actdur * actdur;
    actdur = 0.002f + 0.498f * actdur;
    grain_end_phase = sr * actdur;

    auxenvparams = evpars.auxenvparams;

    for (size_t i = 0; i < 2; ++i)
    {
        insert_fx[i].reset();
        if (insert_fx[i].mainmode == GrainInsertFX::GFXSSTFILTER)
        {
            /*
            for (size_t j = 0; j < 5; ++j)
            {
                assert(evpars.insertparams[i][j] >= 0.0f && evpars.insertparams[i][j] <= 1.0f);
            }
            */
            float filtpitch =
                xenakios::mapvalue(evpars.insertparams[i][0], 0.0f, 1.0f, -48.0f, 72.0f);
            insert_fx[i].paramvalues[0] = std::clamp(filtpitch - 9.0f, -48.0f, 64.0f);
            insert_fx[i].paramvalues[1] = std::clamp(evpars.insertparams[i][1], 0.0f, 1.0f);
            insert_fx[i].paramvalues[2] = std::clamp(evpars.insertparams[i][2], -1.0f, 1.0f);
            float filtpitchspread =
                xenakios::mapvalue(evpars.insertparams[i][3], 0.0f, 1.0f, -24.0f, 24.0f);
            insert_fx[i].paramvalues[3] = std::clamp(filtpitchspread, -24.0f, 24.0f);
            insert_fx[i].paramvalues[4] = std::clamp(evpars.insertparams[i][4], 0.0f, 1.0f);
        }
        else if (insert_fx[i].mainmode == GrainInsertFX::GFXAIRWINDOWS)
        {
            for (size_t j = 0; j < insert_fx[i].numParams; ++j)
            {
                // assert(evpars.insertparams[i][j] >= 0.0f && evpars.insertparams[i][j] <= 1.0f);
                insert_fx[i].paramvalues[j] = evpars.insertparams[i][j];
            }
        }
        else if (insert_fx[i].mainmode == GrainInsertFX::GFXXENAKIOS)
        {
            for (size_t j = 0; j < insert_fx[i].numParams; ++j)
            {
                // assert(evpars.insertparams[i][j] >= 0.0f && evpars.insertparams[i][j] <= 1.0f);
                insert_fx[i].paramvalues[j] = evpars.insertparams[i][j];
            }
        }
    }
    for (int i = 0; i < GrainEvent::max_grain_mod_slots; ++i)
        modulation_slots[i].depth = evpars.modamounts[i];

    grain_base_volume = std::clamp(evpars.volume, 0.0f, 1.0f);

    float bandpos =
        xenakios::mapvalue<float>(pitch_base, -48.0f, 64.0f, 0.0f, numPitchBandAttens - 1);
    bandpos = std::clamp(bandpos, 0.0f, (float)numPitchBandAttens - 1);
    int ind0 = bandpos;
    int ind1 = ind0 + 1;
    float frac = bandpos - ind0;
    float g0 = pitchBandAttens[ind0];
    float g1 = pitchBandAttens[ind1];
    float gatten = g0 + (g1 - g0) * frac;
    gatten = std::clamp(gatten, 0.0f, 1.0f);
    grain_base_volume *= gatten;

    auxsend1 = std::clamp(evpars.auxsend, 0.0f, 1.0f);

    envstarttype = std::clamp<uint8_t>(evpars.envelope_start_type, 0, EasingLUTS::numFunctions);
    envendtype = std::clamp<uint8_t>(evpars.envelope_end_type, 0, EasingLUTS::numFunctions);
    envshape = std::clamp(evpars.envelope_shape, 0.0f, 1.0f);
}
std::function<float(float)> GranulatorModConfig::getCurveOperator(CurveIdentifier id)
{
    if (id.id >= CURVE_STEPS1 && id.id < CURVE_STEPS1 + 16)
    {
        return [id](auto x) {
            const int numsteps = id.id - CURVE_STEPS1 + 1;
            x = (x + 1.0f) * 0.5;
            x = std::round(x * numsteps) / numsteps;
            return -1.0f + 2.0f * x;
        };
    }
    switch (id.id)
    {
    case CURVE_LINEAR:
        return [](auto x) { return x; };
    case CURVE_SQUARE:
        return [](auto x) { return std::abs(x) * x; };
    case CURVE_CUBE:
        return [](auto x) { return x * x * x; };
    case CURVE_ABS:
        return [](auto x) { return std::abs(x); };
    case CURVE_TOPOWER16:
        return [](auto x) {
            x = std::clamp(x, -1.0f, 1.0f);
            return std::pow(x, 16) * sgn(x);
        };
    case CURVE_EXPSIN1:
        return [](auto x) { return expsin(x, 1, 8.0f); };
    case CURVE_EXPSIN2:
        return [](auto x) { return expsin(x, 2, 12.0f); };
    case CURVE_XOR1:
        return [](auto x) { return xor_curve(x, 13107); };
    case CURVE_XOR2:
        return [](auto x) { return xor_curve(x, 43690); };
    case CURVE_XOR3:
        return [](auto x) { return xor_curve(x, 25027); };
    case CURVE_XOR4:
        return [](auto x) { return xor_curve(x, 10001); };
    case CURVE_XOR5:
        return [](auto x) { return xor_curve(x, 0x0F0F); };
    case CURVE_XOR6:
        return [](auto x) { return xor_curve(x, 0x5555); };
    case CURVE_XOR7:
        return [](auto x) { return xor_curve(x, 0x203F); };
    case CURVE_XOR8:
        return [](auto x) { return xor_curve(x, 0xef00); };
    case CURVE_BITMIRROR:
        return [](auto x) { return bit_reversal_curve(x); };
    case CURVE_UNIPOLARTOBIPOLAR:
        return [](auto x) { return std::clamp(-1.0f + 2.0f * x, -1.0f, 1.0f); };
    case CURVE_BIPOLARTOUNIPOLAR:
        return [](auto x) { return std::clamp((x + 1.0f) * 0.5f, 0.0f, 1.0f); };
    case CURVE_HARMONICSERIES3OCTAVES:
        return [](auto x) { return harmseries(x, 3); };
    case CURVE_HARMONICSERIES4OCTAVES:
        return [](auto x) { return harmseries(x, 4); };
    case CURVE_HARMONICSERIES5OCTAVES:
        return [](auto x) { return harmseries(x, 5); };
    case CURVE_PEAKING1:
        return [](auto x) { return peaking_curve(x, 0.2f); };
    case CURVE_PEAKING2:
        return [](auto x) { return peaking_curve(x, 0.5f); };
    case CURVE_PEAKING3:
        return [](auto x) { return peaking_curve(x, 1.0f); };
    case CURVE_PEAKING4:
        return [](auto x) { return peaking_curve(x, 2.0f); };
    case CURVE_PEAKING5:
        return [](auto x) { return peaking_curve(x, 3.0f); };
    case CURVE_PEAKING6:
        return [](auto x) { return peaking_curve(x, 4.0f); };
    case CURVE_POPCORN:
        return [](auto x) { return std::floor(std::tanh(x * 5.0) * 10.0) / 10.0; };
    case CURVE_SIGMOID1:
        return [](auto x) { return smoothstep(-1.0f, 1.0f, (x + 1.0f) * 0.5f); };
    case CURVE_BUTTERFLY:
        return [](auto x) {
            if (x != 0.0f)
                return std::sin(x * 10.0f) * std::cos(1.0f / x + 0.001f);
            return 0.0f;
        };
    };

    return [](auto x) { return x; };
}
