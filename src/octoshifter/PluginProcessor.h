#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <unordered_map>
#include "clap/id.h"
#include "containers/choc_Value.h"
#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_audio_processors_headless/juce_audio_processors_headless.h"
#include "signalsmith-stretch.h"
#include "sst/basic-blocks/modulators/SimpleLFO.h"
#include "sst/basic-blocks/dsp/EllipticBlepOscillators.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

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
    float basepitch = 0.0f;
    float lforate = 0.0f;
    float lfodeform = 0.0f;
    float lfowarp = 0.0f;
    float lfoshift = 0.0f;
    float lfoamount = 0.0f;
    float ingain = 1.0f;
    sst::basic_blocks::dsp::OnePoleLag<float, true> ingainlag;
    int lfoshape = 0;
    PitchShifter() : lfo(this)
    {
        stretch = std::make_unique<signalsmith::stretch::SignalsmithStretch<>>();
        infifo.reset(BLOCKSIZE);
        outfifo.reset(BLOCKSIZE);
    }

    void set_sample_rate(double hz)
    {
        samplerate = hz;
        ingainlag.setRateInMilliseconds(50.0, hz, 1.0f);
        ingainlag.snapTo(1.0);
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
    float process_sample(float input)
    {
        infifo.push(input * ingainlag.getValue());
        ingainlag.process();
        if (infifo.getUsedSlots() == BLOCKSIZE)
        {
            float x = 0.0f;
            float processinbuffer[1][BLOCKSIZE];
            float processoutbuffer[1][BLOCKSIZE];
            for (int i = 0; i < BLOCKSIZE; ++i)
            {
                infifo.pop(x);
                processinbuffer[0][i] = x;
            }
            lfo.applyPhaseOffset(lfoshift);
            lfo.process_block(lforate, lfodeform, lfoshape, false, 1.0, lfowarp);
            float finalpitch = basepitch + lfoamount * lfo.outputBlock[0];
            stretch->setTransposeSemitones(finalpitch);
            stretch->process(processinbuffer, BLOCKSIZE, processoutbuffer, BLOCKSIZE);
            for (int i = 0; i < BLOCKSIZE; ++i)
            {
                outfifo.push(processoutbuffer[0][i]);
            }
        }
        float x = 0.0f;
        outfifo.pop(x);
        return x;
    }
    choc::fifo::SingleReaderSingleWriterFIFO<float> infifo;
    choc::fifo::SingleReaderSingleWriterFIFO<float> outfifo;
};

using pmd = sst::basic_blocks::params::ParamMetaData;

struct ParamChangeMessage
{
    uint32_t id = CLAP_INVALID_ID;
    float val = 0.0f;
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
    static constexpr size_t numshifters = 8;
    std::array<PitchShifter, numshifters> shifters;
    enum PARAMS
    {
        PAR_INPUT_GAIN = 100,
        PAR_PITCH = 200,
        PAR_FORMANT = 300,
        PAR_LFOSHAPE = 10000,
        PAR_LFORATE = 10100,
        PAR_LFOAMOUNT = 10200,
        PAR_LFODEFORM = 10300,
        PAR_LFOSHIFT = 10400,
        PAR_LFOWARP = 10500
    };
    std::vector<pmd> param_metadatas;
    std::unordered_map<uint32_t, pmd *> par_id_to_metadata;
    std::array<float, 256> param_values;
    std::unordered_map<uint32_t, float *> par_id_to_value_pointer;
    choc::fifo::SingleReaderSingleWriterFIFO<ParamChangeMessage> params_from_gui_fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<ParamChangeMessage> params_to_gui_fifo;
    void set_state(choc::value::ValueView view);
    void set_parameter_value(uint32_t id, float value);
    void send_param_updates_to_gui();
  private:
    double sinphase = 0.0;
    juce::AudioBuffer<float> workbuffer;
    int samplecount = 0;
    std::unordered_map<int, int> choicetolfoshapemap;
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
};
