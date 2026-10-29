#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <unordered_map>
#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_audio_processors_headless/juce_audio_processors_headless.h"
#include "signalsmith-stretch.h"
#include "sst/basic-blocks/modulators/SimpleLFO.h"

class PitchShifter
{
  public:
    std::unique_ptr<signalsmith::stretch::SignalsmithStretch<>> stretch;
    static constexpr size_t BLOCKSIZE = 64;
    static constexpr size_t BLOCK_SIZE_OS = BLOCKSIZE * 2;
    double samplerate = 0.0;
    alignas(32) float table_envrate_linear[512];
    using lfo_t = sst::basic_blocks::modulators::SimpleLFO<PitchShifter, BLOCKSIZE>;
    lfo_t lfo;
    PitchShifter() : lfo(this)
    {
        stretch = std::make_unique<signalsmith::stretch::SignalsmithStretch<>>();
    }

    void set_sample_rate(double hz)
    {
        samplerate = hz;
        initTables();
    }
    void initTables()
    {
        double dsamplerate_os = samplerate * 2;
        for (int i = 0; i < 512; ++i)
        {
            double k =
                dsamplerate_os * pow(2.0, (((double)i - 256.0) / 16.0)) / (double)BLOCK_SIZE_OS;
            table_envrate_linear[i] = (float)(1.f / k);
        }
    }
    float envelope_rate_linear_nowrap(float x)
    {
        x *= 16.f;
        x += 256.f;
        int e = std::clamp<int>((int)x, 0, 0x1ff - 1);

        float a = x - (float)e;

        return (1 - a) * table_envrate_linear[e & 0x1ff] +
               a * table_envrate_linear[(e + 1) & 0x1ff];
    }
};

class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
  public:
    //==============================================================================
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;

    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String &newName) override;

    //==============================================================================
    void getStateInformation(juce::MemoryBlock &destData) override;
    void setStateInformation(const void *data, int sizeInBytes) override;
    PitchShifter shifter;

  private:
    juce::AudioBuffer<float> workbuffer;
    juce::AudioParameterFloat *par_pitch = nullptr;
    juce::AudioParameterFloat *par_lforate = nullptr;
    juce::AudioParameterFloat *par_lfodeform = nullptr;
    juce::AudioParameterFloat *par_lfowarp = nullptr;
    juce::AudioParameterChoice *par_lfoshape = nullptr;
    juce::AudioParameterFloat *par_lfoamount = nullptr;
    int samplecount = 0;
    std::unordered_map<int, int> choicetolfoshapemap;
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
};
