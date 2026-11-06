#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "containers/choc_Value.h"
#include "fmt/format.h"
#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_audio_processors/juce_audio_processors.h"
#include "juce_audio_processors_headless/juce_audio_processors_headless.h"
#include "juce_core/juce_core.h"
#include "modcurves.h"
#include "sst/filters++/api.h"
#include "text/choc_JSON.h"
#include <unordered_map>

void OctoShifterProcessor::toggle_input_routing(int input, int destination)
{
    juce::ScopedLock locker(cs);
    for (int i = 0; i < input_routings.size(); ++i)
    {
        if (input_routings[i].input == input && input_routings[i].output == destination)
        {
            input_routings.erase(input_routings.begin() + i);
            return;
        }
    }
    AudioRouting rt;
    rt.input = (AUDIOINPUT)input;
    rt.output = destination;
    input_routings.push_back(rt);
}

//==============================================================================
OctoShifterProcessor::OctoShifterProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::ambisonic(3), true)
                         .withOutput("Output", juce::AudioChannelSet::ambisonic(3), true))
{
    juce::Logger::writeToLog("OctoShifterProcessor CTOR");
    params_from_gui_fifo.reset(2048);
    params_to_gui_fifo.reset(2048);
    modulations_to_gui_fifo.reset(2048);
    // should be infrequent events so small fifos should be fine...
    filter_mode_from_gui_fifo.reset(32);
    filter_mode_to_gui_fifo.reset(32);
    curve_from_gui_fifo.reset(32);

    input_routings.reserve(32);
    input_routings.emplace_back(IN0, 0);
    input_routings.emplace_back(IN0, 1);
    input_routings.emplace_back(SAW0, 7);
    input_routings.emplace_back(SINE0, 6);
    input_routings.emplace_back(NOISE0, 3);
    for (int i = 0; i < numshifters; ++i)
    {
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Enabled", i + 1))
                                      .asOnOffBool()
                                      .withDefault(true)
                                      .withID(PAR_ENABLED + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Input Volume", i + 1))
                                      .withRange(-48.0f, 0.0f)
                                      .withDefault(-6.0)
                                      .withLinearScaleFormatting("dB")
                                      .withID(PAR_INPUT_GAIN + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Output Volume", i + 1))
                                      .withRange(-48.0f, 0.0f)
                                      .withDefault(-6.0)
                                      .withLinearScaleFormatting("dB")
                                      .withID(PAR_OUTPUT_GAIN + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Pitch", i + 1))
                                      .withRange(-12.0f, 12.0f)
                                      .withQuantizedInterval(0.5)
                                      .withLinearScaleFormatting("st")
                                      .withID(PAR_PITCH + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Formant", i + 1))
                                      .withRange(-12.0f, 12.0f)
                                      .withLinearScaleFormatting("st")
                                      .withID(PAR_FORMANT + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Delay time", i + 1))
                                      .withRange(0.0f, 1.0f)
                                      .withDefault(0.5)
                                      .withDecimalPlaces(0)
                                      .withOffsetPowerFormatting("ms", 0.0, 5.0, 3.0, 1000.0)
                                      .withID(PAR_DELAYTIME + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Delay Feedback", i + 1))
                                      .withRange(0.0, 1.0)
                                      .withDefault(0.0)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_DELAYFEEDBACK + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Filter Cutoff", i + 1))
                                      .withRange(-48, 65)
                                      .withDefault(60)
                                      .withDecimalPlaces(0)
                                      .withSemitoneZeroAt440Formatting()
                                      .withID(PAR_FILTERCUTOFF + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Filter Resonance", i + 1))
                                      .withRange(0.0, 1.0)
                                      .withDefault(0.0)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_FILTERRESON + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Azimuth", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withDefault(0.0)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_AZIMUTH + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{} : Elevation", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withDefault(0.0)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_ELEVATION + i));
    }
    param_metadatas.push_back(
        pmd()
            .withName("PITCH SHIFT MODE")
            .withUnorderedMapFormatting(
                {{0, "Default"}, {1, "Cheap"}, {2, "Borken"}, {3, "Slow and high CPU"}}, true)
            .withDefault(0)
            .withID(PAR_PITCHQUALITY));
    param_metadatas.push_back(pmd()
                                  .withName("OUTPUT MODE")
                                  .withUnorderedMapFormatting({{0, "Stereo"},
                                                               {1, "Discrete 8 ch"},
                                                               {2, "1OA(mbisonics)"},
                                                               {3, "2OA(mbisonics)"},
                                                               {4, "3OA(mbisonics)"}},
                                                              true)
                                  .withDefault(0)
                                  .withID(PAR_OUTPUTMODE));
    for (int i = 0; i < numLFOs; ++i)
    {
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} DEFORM", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_LFODEFORMS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} SHIFT", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_LFOSHIFTS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} WARP", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_LFOWARPS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} SHAPE", i + 1))
                                      .withUnorderedMapFormatting({{0, "SINE"},
                                                                   {1, "SIN<>SQR<>TRI"},
                                                                   {2, "DOWN<>TRI<>UP"},
                                                                   {3, "SMOOTH RANDOM"},
                                                                   {4, "RANDOM"}},
                                                                  true)
                                      .withID(PAR_LFOSHAPES + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} RATE", i + 1))
                                      .withRange(-6.0f, 3.0f)
                                      .withATwoToTheBFormatting(1.0f, 1.0f, "Hz")
                                      .withID(PAR_LFORATES + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} PITCH AMOUNT", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_PITCHLFOAMOUNTS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} FORMANT AMOUNT", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_FORMANT_LFOAMOUNTS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} AZIMUTH AMOUNT", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_AZIMUTHLFOAMOUNTS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} ELEVATION AMOUNT", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_ELEVATION_LFOAMOUNTS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} FILTER 1 CUTOFF AMOUNT", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_CUTOFF0LFOAMOUNTS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} DELAY FEEDBACK AMOUNT", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_DELAYFB_LFOAMOUNTS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} INPUT VOLUME AMOUNT", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_INVOLUME_LFOAMOUNTS + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("LFO {} OUTPUT VOLUME AMOUNT", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_OUTVOLUME_LFOAMOUNTS + i));
        param_metadatas.push_back(
            pmd()
                .withName(fmt::format("LFO {} FILTER 1 RESONANCE AMOUNT", i + 1))
                .withRange(-1.0f, 1.0f)
                .withLinearScaleFormatting("%", 100.0)
                .withID(PAR_RESON0_LFOAMOUNTS + i));
    }

    for (int i = 0; i < param_metadatas.size(); ++i)
    {
        auto id = param_metadatas[i].id;
        par_id_to_metadata[id] = &param_metadatas[i];
        param_values[i] = param_metadatas[i].defaultVal;
        par_id_to_value_pointer[id] = &param_values[i];
        float minv = param_metadatas[i].minVal;
        float maxv = param_metadatas[i].maxVal;
        float def = param_metadatas[i].defaultVal;
        auto parname = param_metadatas[i].name;
        auto par = new juce::AudioParameterFloat(juce::ParameterID(juce::String(id), 1), parname,
                                                 {minv, maxv}, def);
        addParameter(par);
        clap_id_to_juce_param[id] = par;
        juce_param_to_clap_id[par] = id;
        par->addListener(this);
        set_parameter_value(id, param_metadatas[i].defaultVal);
    }

    choicetolfoshapemap[0] = lfo_t::SINE;
    choicetolfoshapemap[1] = lfo_t::PULSE;
    choicetolfoshapemap[2] = lfo_t::SAW_TRI_RAMP;
    choicetolfoshapemap[3] = lfo_t::SMOOTH_NOISE;
    choicetolfoshapemap[4] = lfo_t::SH_NOISE;

    mod_ranges[PitchShifter::MT_PITCH] = 12.0f;
    mod_ranges[PitchShifter::MT_INVOLUME] = 18.0f;
    mod_ranges[PitchShifter::MT_OUTVOLUME] = 18.0f;
    mod_ranges[PitchShifter::MT_F0CUTOFF] = 36.0f;
    mod_ranges[PitchShifter::MT_FORMANT] = 12.0f;
    mod_ranges[PitchShifter::MT_F0RESON] = 1.0f;
    mod_ranges[PitchShifter::MT_AZI] = 1.0f;
    mod_ranges[PitchShifter::MT_ELEVATION] = 1.0f;
    mod_ranges[PitchShifter::MT_DELAYFEEDBACK] = 0.5f;

    set_initialpatch();
}

void OctoShifterProcessor::parameterValueChanged(int parameterIndex, float newValue)
{
    ParamChangeMessage msg;
    auto &pars = getParameters();
    auto basepar = pars[parameterIndex];
    auto rangedpar = static_cast<juce::RangedAudioParameter *>(basepar);
    msg.id = juce_param_to_clap_id[rangedpar];
    msg.val = rangedpar->convertFrom0to1(newValue);
    msg.ishostautomation = true;
    params_from_gui_fifo.push(msg);
}

void OctoShifterProcessor::parameterGestureChanged(int parameterIndex, bool gestureIsStarting) {}

void OctoShifterProcessor::set_initialpatch()
{
    // initial patch that has some values at non default
    for (int i = 0; i < 6; ++i)
    {
        set_parameter_value(PAR_ENABLED + i + 2, false);
    }
    for (int i = 0; i < 8; ++i)
    {
        float delaytime = 0.2 + 0.4 / 8 * i;
        set_parameter_value(PAR_DELAYTIME + i, delaytime);
    }
    set_parameter_value(PAR_AZIMUTH, -1.0);
    set_parameter_value(PAR_AZIMUTH + 1, 1.0);
    set_parameter_value(PAR_PITCH, 0.05);
    set_parameter_value(PAR_PITCH + 1, -0.05);
    set_parameter_value(PAR_DELAYFEEDBACK, 0.2);
    set_parameter_value(PAR_DELAYFEEDBACK + 1, 0.2);
}

OctoShifterProcessor::~OctoShifterProcessor()
{
    juce::Logger::writeToLog("OctoShifterProcessor DTOR");
}

//==============================================================================
const juce::String OctoShifterProcessor::getName() const { return JucePlugin_Name; }

bool OctoShifterProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool OctoShifterProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool OctoShifterProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double OctoShifterProcessor::getTailLengthSeconds() const { return 0.0; }

int OctoShifterProcessor::getNumPrograms()
{
    return 1; // NB: some hosts don't cope very well if you tell them there are 0 programs,
              // so this should be at least 1, even if you're not really implementing programs.
}

int OctoShifterProcessor::getCurrentProgram() { return 0; }

void OctoShifterProcessor::setCurrentProgram(int index) { juce::ignoreUnused(index); }

const juce::String OctoShifterProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void OctoShifterProcessor::changeProgramName(int index, const juce::String &newName)
{
    juce::ignoreUnused(index, newName);
}

//==============================================================================
void OctoShifterProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    Log::info("prepareToPlay {} Hz {} samples, {} total num in channels {} total out channels",
              sampleRate, samplesPerBlock, getTotalNumInputChannels(), getTotalNumOutputChannels());
    callbackcount = 0;
    for (int i = 0; i < shifters.size(); ++i)
    {
        shifters[i].stretch->presetDefault(1, sampleRate);
        shifters[i].set_sample_rate(sampleRate);
    }
    samplerate = sampleRate;
    initTables();
    workbuffer.setSize(max_output_channels, samplesPerBlock);
    workbuffer.clear();
    synthbuffer.setSize(8, samplesPerBlock);
    synthbuffer.clear();
    current_filter_mode.model = shifters[0].sstfilter.getFilterModel();
    current_filter_mode.config = shifters[0].sstfilter.getModelConfiguration();
}

void OctoShifterProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

juce::String getBusesLayoutDescription(const juce::AudioProcessor::BusesLayout &layout)
{
    juce::String description;

    auto appendBuses = [&description](const juce::Array<juce::AudioChannelSet> &buses,
                                      juce::String type) {
        description << type << " Buses (" << buses.size() << "):\n";

        if (buses.isEmpty())
        {
            description << "  None\n";
            return;
        }

        for (int i = 0; i < buses.size(); ++i)
        {
            const auto &bus = buses.getReference(i);
            description << "  Bus " << i << ": "
                        << bus.getDescription() // e.g., "Stereo" or "5.1 Surround"
                        << " [" << bus.size() << " channels]\n";
        }
    };

    appendBuses(layout.inputBuses, "Input");
    description << "\n";
    appendBuses(layout.outputBuses, "Output");

    return description;
}

bool OctoShifterProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
    // juce::Logger::writeToLog("host requested I/O : " + getBusesLayoutDescription(layouts));
    return true;
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
#if !JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    return true;
}

void OctoShifterProcessor::process_synth_signals(int numframes)
{
    const double oscinc0 = 2 * M_PI / getSampleRate() * 80.0;
    const double oscinc1 = 2 * M_PI / getSampleRate() * 80.0 * (21.0 / 9);
    const double oscinc2 = 2 * M_PI / getSampleRate() * 440.0;
    const float TWO_PI = 6.28318530718f;
    auto synthbuf = synthbuffer.getArrayOfWritePointers();
    float oscgain = 0.5;
    for (int i = 0; i < numframes; ++i)
    {
        float sine0 = std::sin(oscphases[1]);
        float sine1 = std::sin(oscphases[2]);
        float phase = oscphases[0] / TWO_PI;
        float saw0 = 2.0f * (phase - std::floor(phase + 0.5f));
        phase = oscphases[1] / TWO_PI;
        float saw1 = 2.0f * (phase - std::floor(phase + 0.5f));
        synthbuf[0][i] = sine0 * oscgain;
        synthbuf[1][i] = sine1 * oscgain;
        synthbuf[2][i] = saw0 * oscgain;
        synthbuf[3][i] = saw1 * oscgain;
        synthbuf[4][i] = reflect_value_no_loop(-1.0f, sine0 * 8, 1.0f) * oscgain;
        synthbuf[5][i] = reflect_value_no_loop(-1.0f, sine1 * 8, 1.0f) * oscgain;
        float noise0 = rng.nextFloatInRange(-0.5, 0.5);
        float noise1 = rng.nextFloatInRange(-0.5, 0.5);
        synthbuf[6][i] = noise0;
        synthbuf[7][i] = noise1;
        oscphases[0] += oscinc0;
        oscphases[1] += oscinc1;
        oscphases[2] += oscinc2;
    }
}

void OctoShifterProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                        juce::MidiBuffer &midiMessages)
{
    juce::ScopedLock locker(cs);
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());
    auto pluginbufferpointers = buffer.getArrayOfWritePointers();
    workbuffer.clear();
    auto workpointers = workbuffer.getArrayOfWritePointers();
    bool any_synth_active = false;
    for (auto &r : input_routings)
    {
        if (r.input >= SINE0 && r.input <= NOISE1)
        {
            any_synth_active = true;
            break;
        }
    }
    if (any_synth_active)
        process_synth_signals(buffer.getNumSamples());
    for (auto &r : input_routings)
    {
        int srchan = r.input;
        int destchan = std::clamp(r.output, 0, (int)numshifters - 1);
        if (srchan >= IN0 && srchan <= IN7 && srchan < totalNumInputChannels)
        {
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                workpointers[destchan][i] += pluginbufferpointers[srchan][i];
            }
        }
        else if (srchan >= SINE0 && srchan <= NOISE1)
        {
            auto synthpointers = synthbuffer.getArrayOfWritePointers();
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                workpointers[destchan][i] += synthpointers[srchan - SINE0][i];
            }
        }
    }
    std::array<float, max_input_channels> fifoframe;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        for (int j = 0; j < 8; ++j)
        {
            fifoframe[j] = workpointers[j][i];
        }
        input_fifo.push(fifoframe);
    }

    int omode = *par_id_to_value_pointer[PAR_OUTPUTMODE];
    int engineInChannels = 2;
    int engineOutChannels = 2;

    PitchShifter::OUTCHANSMODE shifteroutmode = PitchShifter::OCM_STEREO;
    if (omode == 1)
    {
        engineOutChannels = 1;
        shifteroutmode = PitchShifter::OCM_MONODISCRETE;
    }
    else if (omode == 2)
    {
        engineOutChannels = 4;
        shifteroutmode = PitchShifter::OCM_AMBISONICS_1ST;
    }
    else if (omode == 3)
    {
        engineOutChannels = 9;
        shifteroutmode = PitchShifter::OCM_AMBISONICS_2ND;
    }
    else if (omode == 4)
    {
        engineOutChannels = 16;
        shifteroutmode = PitchShifter::OCM_AMBISONICS_3RD;
    }
    for (auto &s : shifters)
    {
        s.set_output_mode(shifteroutmode);
    }
    FilterModeMessage filmsg;
    while (filter_mode_from_gui_fifo.pop(filmsg))
    {
        current_filter_mode = filmsg;
        for (auto &s : shifters)
        {
            s.set_filter_mode(filmsg.model, filmsg.config);
        }
    }
    CurveMessage curmsg;
    while (curve_from_gui_fifo.pop(curmsg))
    {
        auto func = getCurveOperator(curmsg.curveid);
        assert(func);
        assert(curmsg.shifter_index >= 0);
        shifters[curmsg.shifter_index]
            .modmatrixcurves[curmsg.lfoindex][curmsg.shifter_param_target] = {curmsg.curveid, func};
    }
    ParamChangeMessage msg;
    while (params_from_gui_fifo.pop(msg))
    {
        // DBG(msg.id << " " << msg.val);
        if (!msg.ishostautomation)
        {
            // our gui sent the message, so notify host
            // so it can record automation, keep track of last moved parameter etc
            auto it = clap_id_to_juce_param.find(msg.id);
            assert(it != clap_id_to_juce_param.end());
            if (it != clap_id_to_juce_param.end())
            {
                float norm = it->second->convertTo0to1(msg.val);
                it->second->setValueNotifyingHost(norm);
            }
        }
        else
        {
            // host sent the message, so notify our GUI
            // to visually see host automation/modulation
            ParamChangeMessage togui;
            togui.id = msg.id;
            togui.val = msg.val;
            params_to_gui_fifo.push(togui);
        }
        // in either case, obviously actually set the param value
        set_parameter_value(msg.id, msg.val);
    }

    while (output_fifo.usedSlots() < buffer.getNumSamples())
    {
        int shifterstouse = numshifters;
        auto sumpointers = workbuffer.getArrayOfWritePointers();
        workbuffer.clear();
        alignas(16) float inputbuffer[max_input_channels][BLOCKSIZE];
        for (int i = 0; i < BLOCKSIZE; ++i)
        {
            std::array<float, max_input_channels> frame;
            input_fifo.pop(frame);
            for (int j = 0; j < max_input_channels; ++j)
            {
                inputbuffer[j][i] = frame[j];
            }
        }
        alignas(16) float procbuf[BLOCKSIZE * max_output_channels];
        memset(procbuf, 0, BLOCKSIZE * max_output_channels * sizeof(float));
        for (int i = 0; i < numLFOs; ++i)
        {
            int lfo0shape = *par_id_to_value_pointer[PAR_LFOSHAPES + i];
            lfo0shape = choicetolfoshapemap[lfo0shape];
            float lfo0rate = *par_id_to_value_pointer[PAR_LFORATES + i];
            float lfo0deform = *par_id_to_value_pointer[PAR_LFODEFORMS + i];
            float lfo0warp = *par_id_to_value_pointer[PAR_LFOWARPS + i];
            lfos[i].process_block(lfo0rate, lfo0deform, lfo0shape, false, 1.0, lfo0warp);
        }

        for (int shindex = 0; shindex < shifterstouse; ++shindex)
        {
            if (!shifters[shindex].enabled)
            {
                continue;
            }

            shifters[shindex].mod_source_values[PitchShifter::MS_LFO0] = lfos[0].outputBlock[0];
            shifters[shindex].mod_source_values[PitchShifter::MS_LFO1] = lfos[1].outputBlock[0];
            for (int i = 0; i < numLFOs; ++i)
            {
                shifters[shindex].modmatrix[PitchShifter::MS_LFO0 + i][PitchShifter::MT_PITCH] =
                    *par_id_to_value_pointer[PAR_PITCHLFOAMOUNTS + i];
                float azimod = *par_id_to_value_pointer[PAR_AZIMUTHLFOAMOUNTS + i];
                shifters[shindex].modmatrix[PitchShifter::MS_LFO0 + i][PitchShifter::MT_AZI] =
                    azimod;
                float elevmod = *par_id_to_value_pointer[PAR_ELEVATION_LFOAMOUNTS + i];
                shifters[shindex].modmatrix[PitchShifter::MS_LFO0 + i][PitchShifter::MT_ELEVATION] =
                    elevmod;
                shifters[shindex].modmatrix[PitchShifter::MS_LFO0 + i][PitchShifter::MT_FORMANT] =
                    *par_id_to_value_pointer[PAR_FORMANT_LFOAMOUNTS + i];
                shifters[shindex].modmatrix[PitchShifter::MS_LFO0 + i][PitchShifter::MT_F0CUTOFF] =
                    *par_id_to_value_pointer[PAR_CUTOFF0LFOAMOUNTS + i];
                shifters[shindex]
                    .modmatrix[PitchShifter::MS_LFO0 + i][PitchShifter::MT_DELAYFEEDBACK] =
                    *par_id_to_value_pointer[PAR_DELAYFB_LFOAMOUNTS + i];
                shifters[shindex].modmatrix[PitchShifter::MS_LFO0 + i][PitchShifter::MT_INVOLUME] =
                    *par_id_to_value_pointer[PAR_INVOLUME_LFOAMOUNTS + i];
                shifters[shindex].modmatrix[PitchShifter::MS_LFO0 + i][PitchShifter::MT_OUTVOLUME] =
                    *par_id_to_value_pointer[PAR_OUTVOLUME_LFOAMOUNTS + i];
                shifters[shindex].modmatrix[PitchShifter::MS_LFO0 + i][PitchShifter::MT_F0RESON] =
                    *par_id_to_value_pointer[PAR_RESON0_LFOAMOUNTS + i];
            }

            for (int i = 0; i < BLOCKSIZE; ++i)
            {
                procbuf[i * max_output_channels] = inputbuffer[shindex][i];
            }
            shifters[shindex].dcblocker_enabled = dc_blocker_enabled;
            shifters[shindex].compensate_formants = formant_compensation;
            shifters[shindex].process_block(procbuf);
            if (gui_visible && callbackcount % 8 == 0)
            {
                ModulationMessage msg;
                msg.shifterindex = shindex;
                for (int i = 0; i < PitchShifter::MT_COUNT; ++i)
                {
                    msg.values[i] = shifters[shindex].modsums[i];
                }
                modulations_to_gui_fifo.push(msg);
            }
            if (shifteroutmode != PitchShifter::OCM_MONODISCRETE)
            {
                for (int k = 0; k < engineOutChannels; ++k)
                {
                    for (int i = 0; i < BLOCKSIZE; ++i)
                    {
                        sumpointers[k][i] += procbuf[i * max_output_channels + k];
                    }
                }
            }
            else
            {
                for (int i = 0; i < BLOCKSIZE; ++i)
                {
                    sumpointers[shindex][i] = procbuf[i * max_output_channels];
                }
            }
        }
        std::array<float, max_output_channels> outframe;
        for (int i = 0; i < BLOCKSIZE; ++i)
        {
            for (int j = 0; j < 16; ++j)
            {
                outframe[j] = sumpointers[j][i];
            }
            output_fifo.push(outframe);
        }
        ++callbackcount;
    }
    assert(output_fifo.usedSlots() >= buffer.getNumSamples());
    int final_out_chans = std::min<int>(max_output_channels, totalNumOutputChannels);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        std::array<float, max_output_channels> outframe;
        output_fifo.pop(outframe);
        for (int j = 0; j < final_out_chans; ++j)
        {
            pluginbufferpointers[j][i] = std::clamp(outframe[j] * 0.5f, -1.0f, 1.0f);
        }
    }

    infifoavail = input_fifo.usedSlots();
    outfifoavail = output_fifo.usedSlots();
}

//==============================================================================
bool OctoShifterProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor *OctoShifterProcessor::createEditor()
{
    auto ed = new OctoShifterProcessorEditor(*this);
    send_param_updates_to_gui();
    return ed;
}

//==============================================================================
void OctoShifterProcessor::getStateInformation(juce::MemoryBlock &destData)
{
    juce::Logger::writeToLog("getStateInformation");
    auto state = get_state();
    auto sdata = state.serialise();
    destData.append(sdata.data.data(), sdata.data.size());
}

void OctoShifterProcessor::send_param_updates_to_gui()
{
    for (auto &e : par_id_to_value_pointer)
    {
        ParamChangeMessage msg;
        msg.id = e.first;
        msg.val = *e.second;
        params_to_gui_fifo.push(msg);
    }
    filter_mode_to_gui_fifo.push(current_filter_mode);
}

void OctoShifterProcessor::set_parameter_value(uint32_t id, float value)
{
    *par_id_to_value_pointer[id] = value;
    if (id == PAR_PITCHQUALITY)
    {
        int qual = *par_id_to_value_pointer[PAR_PITCHQUALITY];
        for (auto &e : shifters)
        {
            e.pending_quality_change = qual;
        }
    }
    if (id >= PAR_PITCH && id < PAR_PITCH + numshifters)
    {
        shifters[id - PAR_PITCH].basepitch = value;
    }
    if (id >= PAR_FORMANT && id < PAR_FORMANT + numshifters)
    {
        shifters[id - PAR_FORMANT].baseformant = value;
    }
    if (id >= PAR_FILTERCUTOFF && id < PAR_FILTERCUTOFF + numshifters)
    {
        shifters[id - PAR_FILTERCUTOFF].basefiltercutoff = value;
    }
    if (id >= PAR_FILTERRESON && id < PAR_FILTERRESON + numshifters)
    {
        shifters[id - PAR_FILTERRESON].base_filter_reson = value;
    }
    if (id >= PAR_INPUT_GAIN && id < PAR_INPUT_GAIN + numshifters)
    {
        shifters[id - PAR_INPUT_GAIN].base_in_volume = value;
    }
    if (id >= PAR_OUTPUT_GAIN && id < PAR_OUTPUT_GAIN + numshifters)
    {
        shifters[id - PAR_OUTPUT_GAIN].base_out_volume = value;
    }
    if (id >= PAR_AZIMUTH && id < PAR_AZIMUTH + numshifters)
    {
        shifters[id - PAR_AZIMUTH].baseazimuth = value;
    }
    if (id >= PAR_ELEVATION && id < PAR_ELEVATION + numshifters)
    {
        shifters[id - PAR_ELEVATION].base_elevation = value;
    }
    if (id >= PAR_DELAYFEEDBACK && id < PAR_DELAYFEEDBACK + numshifters)
    {
        shifters[id - PAR_DELAYFEEDBACK].basefeedbackamt = value;
    }
    if (id >= PAR_DELAYTIME && id < PAR_DELAYTIME + numshifters)
    {
        value = (value * value * value) * 5.0;
        shifters[id - PAR_DELAYTIME].pending_delay_time = std::clamp(value, 0.0f, 5.0f);
        shifters[id - PAR_DELAYTIME].delay_change_fade_pos = 0;
    }
    if (id >= PAR_ENABLED && id < PAR_ENABLED + numshifters)
    {
        shifters[id - PAR_ENABLED].set_enabled(value >= 0.5);
    }
}

choc::value::Value OctoShifterProcessor::get_state()
{
    auto state = choc::value::createObject("state");
    state.setMember("version", 1);
    auto params_state = choc::value::createObject("params");
    for (auto &e : param_metadatas)
    {
        std::string idstr = std::to_string(e.id);
        params_state.setMember(idstr, *par_id_to_value_pointer[e.id]);
    }
    state.setMember("parameters", params_state);
    auto filterstate = choc::value::createObject("filter");
    filterstate.setMember("model", (int64_t)current_filter_mode.model);
    filterstate.setMember("pt", (int64_t)current_filter_mode.config.pt);
    filterstate.setMember("st", (int64_t)current_filter_mode.config.st);
    filterstate.setMember("dt", (int64_t)current_filter_mode.config.dt);
    filterstate.setMember("mt", (int64_t)current_filter_mode.config.mt);
    state.setMember("filter0state", filterstate);
    auto modcurves_state = choc::value::createEmptyArray();
    for (int shindex = 0; shindex < shifters.size(); ++shindex)
    {
        for (int i = 0; i < PitchShifter::MS_COUNT; ++i)
        {
            for (int j = 0; j < PitchShifter::MT_COUNT; ++j)
            {
                auto info = shifters[shindex].modmatrixcurves[i][j];
                if (info.id != CURVE_LINEAR)
                {
                    // Log::info("storing non-default curve {} {} {} {}", shindex, i, j, info.id);
                    auto ob = choc::value::createObject("curvefunc");
                    ob.setMember("shifter", shindex);
                    ob.setMember("src", i);
                    ob.setMember("dest", j);
                    ob.setMember("curveid", info.id);
                    modcurves_state.addArrayElement(ob);
                }
            }
        }
    }
    state.setMember("modcurves", modcurves_state);
    DBG(choc::json::toString(modcurves_state));
    return state;
}

void OctoShifterProcessor::set_state(choc::value::ValueView view)
{
    if (view.hasObjectMember("filter0state"))
    {
        namespace sfpp = sst::filtersplusplus;
        auto filterstate = view["filter0state"];
        FilterModeMessage fm;
        fm.model = (sfpp::FilterModel)filterstate["model"].getWithDefault(0);
        fm.config.dt = (decltype(fm.config.dt))filterstate["dt"].getWithDefault(0);
        fm.config.st = (decltype(fm.config.st))filterstate["st"].getWithDefault(0);
        fm.config.mt = (decltype(fm.config.mt))filterstate["mt"].getWithDefault(0);
        fm.config.pt = (decltype(fm.config.pt))filterstate["pt"].getWithDefault(0);
        filter_mode_from_gui_fifo.push(fm);
    }
    if (view.hasObjectMember("modcurves"))
    {
        auto modar = view["modcurves"];
        for (auto &sh : shifters)
            sh.reset_mod_curves();
        for (int i = 0; i < modar.size(); ++i)
        {
            auto ob = modar[i];
            int shifter = ob["shifter"].getWithDefault(-1);
            if (shifter >= 0 && shifter < numshifters)
            {
                int src = ob["src"].getWithDefault(-1);
                int dest = ob["dest"].getWithDefault(-1);
                if (src >= 0 && src < PitchShifter::MS_COUNT && dest >= 0 &&
                    dest < PitchShifter::MT_COUNT)
                {
                    int id = ob["curveid"].getWithDefault(-1);
                    if (id >= CURVE_LINEAR && id < NUM_CURVES)
                    {
                        auto func = getCurveOperator(id);
                        if (func)
                            shifters[shifter].modmatrixcurves[src][dest] = {id,
                                                                            getCurveOperator(id)};
                    }
                }
            }
        }
    }

    if (view.hasObjectMember("parameters"))
    {
        auto params = view["parameters"];
        auto &pars = param_metadatas;
        for (int i = 0; i < pars.size(); ++i)
        {
            std::string id = std::to_string(pars[i].id);
            if (params.hasObjectMember(id))
            {
                float v = params[id].getWithDefault(pars[i].defaultVal);
                set_parameter_value(pars[i].id, v);
            }
        }
        send_param_updates_to_gui();
    }
}

void OctoShifterProcessor::setStateInformation(const void *data, int sizeInBytes)
{
    juce::Logger::writeToLog("setStateInformation");
    try
    {
        if (sizeInBytes < 1)
            return;
        choc::value::InputData idata{(const uint8_t *)data, (const uint8_t *)data + sizeInBytes};
        auto state = choc::value::Value::deserialise(idata);
        set_state(state.getView());
    }
    catch (std::exception &ex)
    {
        DBG("OctoShifter error restoring state : " << ex.what());
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() { return new OctoShifterProcessor(); }
