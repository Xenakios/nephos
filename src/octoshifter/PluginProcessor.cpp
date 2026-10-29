#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "juce_audio_processors/juce_audio_processors.h"
#include "juce_audio_processors_headless/juce_audio_processors_headless.h"
#include "juce_core/juce_core.h"

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
    addParameter(par_pitch = new juce::AudioParameterFloat(
                     juce::ParameterID{"pitch0", 1}, "Pitch",
                     juce::NormalisableRange<float>{-12.0f, 12.0f}, 0.0f));
    choicetolfoshapemap[0] = PitchShifter::lfo_t::SINE;
    choicetolfoshapemap[1] = PitchShifter::lfo_t::TRI;
    choicetolfoshapemap[2] = PitchShifter::lfo_t::RAMP;
    choicetolfoshapemap[3] = PitchShifter::lfo_t::PULSE;
    choicetolfoshapemap[4] = PitchShifter::lfo_t::SH_NOISE;
    choicetolfoshapemap[5] = PitchShifter::lfo_t::SMOOTH_NOISE;
    addParameter(
        par_lfoshape = new juce::AudioParameterChoice(
            juce::ParameterID{"lfoshape0", 1}, "LFO Shape",
            juce::StringArray{"SINE", "TRIANGLE", "RAMP", "SQUARE", "RANDOM", "SMOOTH RANDOM"}, 2));
    addParameter(par_lforate = new juce::AudioParameterFloat(
                     juce::ParameterID{"lforate0", 1}, "LFO Rate",
                     juce::NormalisableRange<float>{-2.0f, 2.0f}, 0.0f));
    addParameter(par_lfodeform = new juce::AudioParameterFloat(
                     juce::ParameterID{"lfodeform0", 1}, "LFO Deform",
                     juce::NormalisableRange<float>{-1.0f, 1.0f}, 0.0f));
    addParameter(par_lfowarp = new juce::AudioParameterFloat(
                     juce::ParameterID{"lfowarp0", 1}, "LFO Warp",
                     juce::NormalisableRange<float>{-1.0f, 1.0f}, 0.0f));
    addParameter(par_lfoamount = new juce::AudioParameterFloat(
                     juce::ParameterID{"lfoamount0", 1}, "LFO To Pitch Amount",
                     juce::NormalisableRange<float>{-12.0f, 12.0f}, 0.0f));
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
    shifter.stretch->presetDefault(getTotalNumInputChannels(), sampleRate);
    shifter.set_sample_rate(sampleRate);
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
    workbuffer.setSize(getTotalNumOutputChannels(), buffer.getNumSamples(), true, true);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        if (samplecount == 0)
        {
            float rate = *par_lforate;
            float deform = *par_lfodeform;
            float warp = *par_lfowarp;
            int shape = *par_lfoshape;
            shape = choicetolfoshapemap[shape];
            shifter.lfo.process_block(rate, deform, shape, false, 1.0, warp);
        }
        ++samplecount;
        if (samplecount == PitchShifter::BLOCKSIZE)
        {
            samplecount = 0;
        }
    }
    float pitchmod = *par_lfoamount * shifter.lfo.outputBlock[0];
    float finalpitch = *par_pitch + pitchmod;
    shifter.stretch->setTransposeSemitones(finalpitch);
    shifter.stretch->process(buffer.getArrayOfReadPointers(), buffer.getNumSamples(),
                             workbuffer.getArrayOfWritePointers(), buffer.getNumSamples());
    buffer.copyFrom(0, 0, workbuffer, 0, 0, buffer.getNumSamples());
    buffer.copyFrom(1, 0, workbuffer, 1, 0, buffer.getNumSamples());
}

//==============================================================================
bool AudioPluginAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor *AudioPluginAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor(*this);
    // return new AudioPluginAudioProcessorEditor(*this);
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
