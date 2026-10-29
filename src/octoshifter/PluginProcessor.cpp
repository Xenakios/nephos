#include "PluginProcessor.h"
#include "PluginEditor.h"
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
    for (int i = 0; i < numshifters; ++i)
    {
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Input Volume {}", i + 1))
                                      .withRange(-48.0f, 0.0f)
                                      .withDefault(-6.0)
                                      .withLinearScaleFormatting("dB")
                                      .withID(PAR_INPUT_GAIN + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Pitch {}", i + 1))
                                      .withRange(-12.0f, 12.0f)
                                      .withLinearScaleFormatting("SEMITONES")
                                      .withID(PAR_PITCH + i));
        param_metadatas.push_back(pmd()
                                      .withName(fmt::format("Formant {}", i + 1))
                                      .withRange(-12.0f, 12.0f)
                                      .withLinearScaleFormatting("SEMITONES")
                                      .withID(PAR_FORMANT + i));
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
                                  .withID(PAR_LFOAMOUNT));
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
        par_id_to_metadata[param_metadatas[i].id] = &param_metadatas[i];
        param_values[i] = param_metadatas[i].defaultVal;
        par_id_to_value_pointer[param_metadatas[i].id] = &param_values[i];
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
    for (int i = 0; i < shifters.size(); ++i)
    {
        shifters[i].stretch->presetDefault(1, sampleRate);
        shifters[i].set_sample_rate(sampleRate);
    }

    workbuffer.setSize(getTotalNumInputChannels(), samplesPerBlock);
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
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        float s = 0.5 * std::sin(sinphase);
        writepointers[0][i] = s;
        writepointers[1][i] = s;
        sinphase += 2 * M_PI / getSampleRate() * 440.0;
    }
    ParamChangeMessage msg;
    while (params_from_gui_fifo.pop(msg))
    {
        // DBG(msg.id << " " << msg.val);
        if (msg.id == PAR_LFOSHAPE)
        {
            int shape = choicetolfoshapemap[(int)msg.val];
            for (int i = 0; i < numshifters; ++i)
            {
                shifters[i].lfoshape = shape;
            }
        }
        if (msg.id == PAR_LFOAMOUNT)
        {
            for (int i = 0; i < numshifters; ++i)
            {
                shifters[i].lfoamount = msg.val * 12.0f;
            }
        }
        if (msg.id == PAR_LFODEFORM)
        {
            for (int i = 0; i < numshifters; ++i)
            {
                shifters[i].lfodeform = msg.val;
            }
        }
        if (msg.id == PAR_LFOWARP)
        {
            for (int i = 0; i < numshifters; ++i)
            {
                shifters[i].lfowarp = msg.val;
            }
        }
        if (msg.id == PAR_LFOSHIFT)
        {
            for (int i = 0; i < numshifters; ++i)
            {
                float shift = 0.5 / (numshifters - 1) * i;
                shifters[i].lfoshift = msg.val * shift;
            }
        }

        if (msg.id == PAR_LFORATE)
        {
            for (int i = 0; i < numshifters; ++i)
            {
                shifters[i].lforate = msg.val;
            }
        }
        if (msg.id >= PAR_PITCH && msg.id < PAR_PITCH + numshifters)
        {
            shifters[msg.id - PAR_PITCH].basepitch = msg.val;
        }
        if (msg.id >= PAR_INPUT_GAIN && msg.id < PAR_INPUT_GAIN + numshifters)
        {
            shifters[msg.id - PAR_INPUT_GAIN].ingainlag.setTarget(
                juce::Decibels::decibelsToGain(msg.val));
        }
    }
    int shifterstouse = numshifters;
#if JUCE_DEBUG
    // we can barely manage 4 shifters in debug builds :-/
    shifterstouse = 4;
#endif
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        float insamples[2] = {writepointers[0][i], writepointers[1][i]};
        float outsamples[2] = {0.0f, 0.0f};
        for (int j = 0; j < shifterstouse; ++j)
        {
            outsamples[j % 2] += shifters[j].process_sample(insamples[j % 2]);
        }
        writepointers[0][i] = outsamples[0] * 0.5;
        writepointers[1][i] = outsamples[1] * 0.5;
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
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
    juce::ignoreUnused(destData);
}

void AudioPluginAudioProcessor::setStateInformation(const void *data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
    juce::ignoreUnused(data, sizeInBytes);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() { return new AudioPluginAudioProcessor(); }
