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
#include "sst/filters++.h"
#include <span>
#include "../../Common/xap_utils.h"
#include "sst/filters++/api.h"
#include "sst/filters++/enums.h"

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
    bool enabled = true;
    float basepitch = 0.0f;
    float baseformant = 0.0f;
    float basefiltercutoff = 36.0f;
    float filter_reson = 0.0f;
    float lforate = 0.0f;
    float lfodeform = 0.0f;
    float lfowarp = 0.0f;
    float lfoshift = 0.0f;
    float lfoamount = 0.0f;
    float ingain = 1.0f;
    float delaytime = 0.25f;
    float feedbackamt = 0.0f;
    float azimuth = 0.0f;
    alignas(16) std::array<float, 64> spatcoeffs = {0.0f};
    alignas(16) sst::filtersplusplus::Filter sstfilter;
    int numchs = 0;
    sst::basic_blocks::dsp::OnePoleLag<float, true> ingainlag;
    std::vector<float> delaybuffer;
    alignas(16) int delaywritepos = 0;
    alignas(16) float feedbackaudio = 0.0f;
    int lfoshape = 0;
    PitchShifter() : lfo(this)
    {
        stretch = std::make_unique<signalsmith::stretch::SignalsmithStretch<>>();
        infifo.reset(BLOCKSIZE);
        outfifo.reset(BLOCKSIZE);
        ingainlag.setRateInMilliseconds(50.0, 44100.0, 1.0f);
        ingainlag.snapTo(0.0);
    }

    void set_sample_rate(double hz, int chans)
    {
        numchs = chans;
        samplerate = hz;
        ingainlag.setRateInMilliseconds(50.0, hz, 1.0f);

        sstfilter.setFilterModel(sst::filtersplusplus::FilterModel::CytomicSVF);
        sstfilter.setModelConfiguration(
            sst::filtersplusplus::ModelConfig(sst::filtersplusplus::Passband::LP));
        sstfilter.setSampleRateAndBlockSize(hz, BLOCKSIZE);
        sstfilter.setMono();
        // sstfilter.provideAllDelayLines(delaylinememory.data());
        if (!sstfilter.prepareInstance())
        {
            assert(false);
        }
        delaybuffer.resize(hz * 5.0);
        initTables();
        set_spatial_parameters(0.0, 0.0);
    }
    void set_spatial_parameters(float azimuth, float elevation)
    {
        std::fill(spatcoeffs.begin(), spatcoeffs.end(), 0.0f);
        if (numchs == 2)
        {
            float panpos = xenakios::mapvalue(azimuth, -1.0f, 1.0f, 0.0f, 1.0f);
            spatcoeffs[0] = 1.0f - panpos;
            spatcoeffs[1] = panpos;
        }
    }
    void push_to_delay(float x)
    {
        delaybuffer[delaywritepos] = x;
        ++delaywritepos;
        if (delaywritepos == delaybuffer.size())
            delaywritepos = 0;
    }
    float pop_from_delay()
    {
        int deltimesamples = delaytime * samplerate;
        if (deltimesamples < 8)
            deltimesamples = 8;
        int readpos = delaywritepos - deltimesamples;
        if (readpos < 0)
            readpos += delaybuffer.size();
        assert(readpos >= 0 && readpos < delaybuffer.size());
        return delaybuffer[readpos];
    }
    // uses only first channel as input, outputs stereo or ambisonics
    void process_frame(std::span<float> audio_inout)
    {
        assert(numchs > 1);
        assert(audio_inout.size() >= numchs);
        float insample = audio_inout[0] * ingainlag.getValue();
        insample += feedbackaudio * feedbackamt;
        infifo.push(insample);
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
            stretch->setFormantSemitones(baseformant);
            stretch->process(processinbuffer, BLOCKSIZE, processoutbuffer, BLOCKSIZE);
            sstfilter.makeCoefficients(0, basefiltercutoff, filter_reson);
            sstfilter.prepareBlock();
            for (int i = 0; i < BLOCKSIZE; ++i)
            {
                float filtered = sstfilter.processMonoSample(processoutbuffer[0][i]);
                push_to_delay(filtered);
                float delayed = pop_from_delay();

                outfifo.push(delayed);
            }
            sstfilter.concludeBlock();
        }
        float x = 0.0f;
        outfifo.pop(x);
        feedbackaudio = std::tanh(x);
        // push_to_delay(x);
        // x = pop_from_delay();
        // feedbackaudio = std::tanh(x);
        audio_inout[0] = x * spatcoeffs[0];
        audio_inout[1] = x * spatcoeffs[1];
    }
    choc::fifo::SingleReaderSingleWriterFIFO<float> infifo;
    choc::fifo::SingleReaderSingleWriterFIFO<float> outfifo;
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
        PAR_ENABLED = 50,
        PAR_INPUT_GAIN = 100,
        PAR_PITCH = 200,
        PAR_FORMANT = 300,
        PAR_DELAYTIME = 400,
        PAR_DELAYFEEDBACK = 450,
        PAR_FILTERCUTOFF = 500,
        PAR_FILTERRESON = 600,
        PAR_AZIMUTH = 700,
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
