#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "containers/choc_Value.h"
#include "fmt/format.h"
#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_audio_processors/juce_audio_processors.h"
#include "juce_audio_processors_headless/juce_audio_processors_headless.h"
#include "juce_core/juce_core.h"
#include <unordered_map>

//==============================================================================
AudioPluginAudioProcessor::AudioPluginAudioProcessor()
    : AudioProcessor(BusesProperties()
#if !JucePlugin_IsMidiEffect
#if !JucePlugin_IsSynth
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
      )
{
    params_from_gui_fifo.reset(2048);
    params_to_gui_fifo.reset(2048);
    for (int i = 0; i < numshifters; ++i)
    {

        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("{}", i + 1))
                                      .asOnOffBool()
                                      .withDefault(true)
                                      .withID(PAR_ENABLED + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Input Volume {}", i + 1))
                                      .withRange(-48.0f, 0.0f)
                                      .withDefault(-6.0)
                                      .withLinearScaleFormatting("dB")
                                      .withID(PAR_INPUT_GAIN + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Pitch {}", i + 1))
                                      .withRange(-12.0f, 12.0f)
                                      .withQuantizedInterval(0.5)
                                      .withLinearScaleFormatting("st")
                                      .withID(PAR_PITCH + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Formant {}", i + 1))
                                      .withRange(-12.0f, 12.0f)
                                      .withLinearScaleFormatting("st")
                                      .withID(PAR_FORMANT + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Delay time {}", i + 1))
                                      .withRange(0.0f, 1.0f)
                                      .withDefault(0.1)
                                      .withOffsetPowerFormatting("ms", 0.0, 5.0, 3.0, 1000.0)
                                      .withID(PAR_DELAYTIME + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Delay Feedback {}", i + 1))
                                      .withRange(0.0, 1.0)
                                      .withDefault(0.0)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_DELAYFEEDBACK + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Filter Cutoff {}", i + 1))
                                      .withRange(-48, 65)
                                      .withDefault(0)
                                      .withSemitoneZeroAt440Formatting()
                                      .withID(PAR_FILTERCUTOFF + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Filter Resonance {}", i + 1))
                                      .withRange(0.0, 1.0)
                                      .withDefault(0.0)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_FILTERRESON + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Azimuth {}", i + 1))
                                      .withRange(-1.0f, 1.0f)
                                      .withDefault(0.0)
                                      .withLinearScaleFormatting("%", 100.0)
                                      .withID(PAR_AZIMUTH + i));
    }
    param_metadatas.push_back(pmd()
                                  .withName("LFO RATE")
                                  .withRange(-6.0f, 3.0f)
                                  .withATwoToTheBFormatting(1.0f, 1.0f, "Hz")
                                  .withID(PAR_LFORATE));
    param_metadatas.push_back(pmd()
                                  .withName("LFO AMOUNT")
                                  .withRange(-1.0f, 1.0f)
                                  .withLinearScaleFormatting("%", 100.0)
                                  .withID(PAR_PITCHLFO0AMOUNT));
    param_metadatas.push_back(pmd()
                                  .withName("LFO AMOUNT")
                                  .withRange(-1.0f, 1.0f)
                                  .withLinearScaleFormatting("%", 100.0)
                                  .withID(PAR_AZIMUTHLFO0AMOUNT));
    param_metadatas.push_back(pmd()
                                  .withName("LFO DEFORM")
                                  .withRange(-1.0f, 1.0f)
                                  .withLinearScaleFormatting("%", 100.0)
                                  .withID(PAR_LFODEFORM));
    param_metadatas.push_back(pmd()
                                  .withName("LFO SHIFT")
                                  .withRange(-1.0f, 1.0f)
                                  .withLinearScaleFormatting("%", 100.0)
                                  .withID(PAR_LFOSHIFT));
    param_metadatas.push_back(pmd()
                                  .withName("LFO WARP")
                                  .withRange(-1.0f, 1.0f)
                                  .withLinearScaleFormatting("%", 100.0)
                                  .withID(PAR_LFOWARP));
    param_metadatas.push_back(pmd()
                                  .withName("LFO SHAPE")
                                  .withUnorderedMapFormatting({{0, "SINE"},
                                                               {1, "SIN<>SQR<>TRI"},
                                                               {2, "DOWN<>TRI<>UP"},
                                                               {3, "SMOOTH RANDOM"},
                                                               {4, "RANDOM"}},
                                                              true)
                                  .withID(PAR_LFOSHAPE));

    for (int i = 0; i < param_metadatas.size(); ++i)
    {
        auto id = param_metadatas[i].id;
        par_id_to_metadata[id] = &param_metadatas[i];
        param_values[i] = param_metadatas[i].defaultVal;
        par_id_to_value_pointer[id] = &param_values[i];
        set_parameter_value(id, param_metadatas[i].defaultVal);
    }
    choicetolfoshapemap[0] = PitchShifter::lfo_t::SINE;
    choicetolfoshapemap[1] = PitchShifter::lfo_t::PULSE;
    choicetolfoshapemap[2] = PitchShifter::lfo_t::SAW_TRI_RAMP;
    choicetolfoshapemap[3] = PitchShifter::lfo_t::SMOOTH_NOISE;
    choicetolfoshapemap[4] = PitchShifter::lfo_t::SH_NOISE;
}

AudioPluginAudioProcessor::~AudioPluginAudioProcessor() {}

//==============================================================================
const juce::String AudioPluginAudioProcessor::getName() const { return JucePlugin_Name; }

bool AudioPluginAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool AudioPluginAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool AudioPluginAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double AudioPluginAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int AudioPluginAudioProcessor::getNumPrograms()
{
    return 1; // NB: some hosts don't cope very well if you tell them there are 0 programs,
              // so this should be at least 1, even if you're not really implementing programs.
}

int AudioPluginAudioProcessor::getCurrentProgram() { return 0; }

void AudioPluginAudioProcessor::setCurrentProgram(int index) { juce::ignoreUnused(index); }

const juce::String AudioPluginAudioProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void AudioPluginAudioProcessor::changeProgramName(int index, const juce::String &newName)
{
    juce::ignoreUnused(index, newName);
}

//==============================================================================
void AudioPluginAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    DBG("prepareToPlay");
    for (int i = 0; i < shifters.size(); ++i)
    {
        shifters[i].stretch->presetDefault(1, sampleRate);
        shifters[i].set_sample_rate(sampleRate, 2);
        shifters[i].set_spatial_parameters(*par_id_to_value_pointer[i + PAR_AZIMUTH], 0.0);
    }

    workbuffer.setSize(getTotalNumOutputChannels(), samplesPerBlock);
    workbuffer.clear();
}

void AudioPluginAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

bool AudioPluginAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
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
#endif
}

void AudioPluginAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                             juce::MidiBuffer &midiMessages)
{
    juce::ignoreUnused(midiMessages);

    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());
    auto writepointers = buffer.getArrayOfWritePointers();
    // test with a synth tone in standalone mode as it's painful to get any proper
    // sound input otherwise...
    if (wrapperType == wrapperType_Standalone)
    {
        const double sininc = 2 * M_PI / getSampleRate() * 110.0;
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            float s = 0.5 * std::tanh(std::sin(sinphase) * 8.0);
            writepointers[0][i] = s;
            writepointers[1][i] = s;
            sinphase += sininc;
        }
    }
    ParamChangeMessage msg;
    while (params_from_gui_fifo.pop(msg))
    {
        // DBG(msg.id << " " << msg.val);
        set_parameter_value(msg.id, msg.val);
    }
    int shifterstouse = numshifters;
    auto sumpointers = workbuffer.getArrayOfWritePointers();
    workbuffer.clear();
    for (int j = 0; j < shifterstouse; ++j)
    {
        if (!shifters[j].enabled)
            continue;
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            alignas(16) float procbuf[64];
            procbuf[0] = writepointers[j % 2][i];
            shifters[j].process_frame(procbuf);
            sumpointers[0][i] += procbuf[0];
            sumpointers[1][i] += procbuf[1];
        }
    }
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        writepointers[0][i] = std::clamp(sumpointers[0][i] * 0.5f, -1.0f, 1.0f);
        writepointers[1][i] = std::clamp(sumpointers[1][i] * 0.5f, -1.0f, 1.0f);
    }
}

//==============================================================================
bool AudioPluginAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor *AudioPluginAudioProcessor::createEditor()
{
    // return new juce::GenericAudioProcessorEditor(*this);
    return new AudioPluginAudioProcessorEditor(*this);
}

//==============================================================================
void AudioPluginAudioProcessor::getStateInformation(juce::MemoryBlock &destData)
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
    auto sdata = state.serialise();
    destData.append(sdata.data.data(), sdata.data.size());
}

void AudioPluginAudioProcessor::send_param_updates_to_gui()
{
    for (auto &e : par_id_to_value_pointer)
    {
        ParamChangeMessage msg;
        msg.id = e.first;
        msg.val = *e.second;
        params_to_gui_fifo.push(msg);
    }
}

void AudioPluginAudioProcessor::set_parameter_value(uint32_t id, float value)
{
    *par_id_to_value_pointer[id] = value;
    for (int i = 0; i < numshifters; ++i)
    {
        if (id == PAR_LFOSHAPE)
        {
            int shape = choicetolfoshapemap[(int)value];
            shifters[i].lfoshape = shape;
        }
        if (id == PAR_PITCHLFO0AMOUNT)
        {
            shifters[i].lfoamount = value * 12.0f;
        }
        if (id == PAR_AZIMUTHLFO0AMOUNT)
        {
            shifters[i].azimuth_lfo_amount = value * modulation_patterns[1][i];
        }
        if (id == PAR_LFODEFORM)
        {
            shifters[i].lfodeform = value;
        }
        if (id == PAR_LFOWARP)
        {
            shifters[i].lfowarp = value;
        }
        if (id == PAR_LFOSHIFT)
        {
            float shift = 0.5 / (numshifters - 1) * i;
            shifters[i].lfoshift = value * shift;
        }
        if (id == PAR_LFORATE)
        {
            shifters[i].lforate = value;
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
        shifters[id - PAR_FILTERRESON].filter_reson = value;
    }
    if (id >= PAR_INPUT_GAIN && id < PAR_INPUT_GAIN + numshifters)
    {
        shifters[id - PAR_INPUT_GAIN].ingainlag.setTarget(juce::Decibels::decibelsToGain(value));
    }
    if (id >= PAR_AZIMUTH && id < PAR_AZIMUTH + numshifters)
    {
        shifters[id - PAR_AZIMUTH].baseazimuth = value;
    }
    if (id >= PAR_DELAYFEEDBACK && id < PAR_DELAYFEEDBACK + numshifters)
    {
        shifters[id - PAR_DELAYFEEDBACK].feedbackamt = value;
    }
    if (id >= PAR_DELAYTIME && id < PAR_DELAYTIME + numshifters)
    {
        value = (value * value * value) * 5.0;
        shifters[id - PAR_DELAYTIME].delaytime = std::clamp(value, 0.0f, 5.0f);
    }
    if (id >= PAR_ENABLED && id < PAR_ENABLED + numshifters)
    {
        shifters[id - PAR_ENABLED].enabled = value >= 0.5;
    }
}

void AudioPluginAudioProcessor::set_state(choc::value::ValueView view)
{
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

void AudioPluginAudioProcessor::setStateInformation(const void *data, int sizeInBytes)
{
    DBG("setStateInformation");
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
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() { return new AudioPluginAudioProcessor(); }
