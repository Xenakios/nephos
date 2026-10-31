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
#include "sst/basic-blocks/dsp/PanLaws.h"
#include "sst/filters++.h"
#include <span>
#include "../../Common/xap_utils.h"
#include "sst/filters++/api.h"
#include "sst/filters++/enums.h"

#include <vector>
#include <cstddef>
#include <optional>
#include <utility>

static constexpr size_t BLOCKSIZE = 64;
static constexpr size_t BLOCK_SIZE_OS = BLOCKSIZE * 2;

class PitchShifter
{
  public:
    std::unique_ptr<signalsmith::stretch::SignalsmithStretch<>> stretch;
    bool enabled = true;
    float basepitch = 0.0f;
    float baseformant = 0.0f;
    float basefiltercutoff = 36.0f;
    float baseazimuth = 0.0f;
    float filter_reson = 0.0f;
    
    float ingain = 1.0f;
    float delaytime = 0.25f;
    float feedbackamt = 0.0f;
    
    float mod_source_values[2] = {0.0f, 0.0f};
    double samplerate = 0.0;
    alignas(16) std::array<float, 64> spatcoeffs = {0.0f};
    alignas(16) sst::filtersplusplus::Filter sstfilter;
    int numchs = 2;
    sst::basic_blocks::dsp::OnePoleLag<float, true> ingainlag;
    std::vector<float> delaybuffer;
    alignas(16) int delaywritepos = 0;
    alignas(16) float feedbackaudio[BLOCKSIZE] = {0.0f};
    enum MODSOURCES
    {
        MS_LFO0,
        MS_LFO1,
        MS_COUNT
    };
    enum MODTARGETS
    {
        MT_PITCH,
        MT_FORMANT,
        MT_F0CUTOFF,
        MT_F0RESON,
        MT_AZI,
        MT_COUNT
    };
    alignas(16) float modmatrix[MS_COUNT][MT_COUNT];
    PitchShifter()
    {
        for (int i = 0; i < MS_COUNT; ++i)
            for (int j = 0; j < MT_COUNT; ++j)
                modmatrix[i][j] = 0.0f;
        stretch = std::make_unique<signalsmith::stretch::SignalsmithStretch<>>();
        ingainlag.setRateInMilliseconds(50.0, 44100.0, 1.0f);
        ingainlag.snapTo(0.0);
        for (int i = 0; i < BLOCKSIZE; ++i)
            feedbackaudio[i] = 0.0f;
    }

    void set_sample_rate(double hz, int chans)
    {
        for (int i = 0; i < BLOCKSIZE; ++i)
            feedbackaudio[i] = 0.0f;
        numchs = chans;
        samplerate = hz;
        ingainlag.setRateInMilliseconds(50.0, hz, 1.0f);

        sstfilter.setFilterModel(sst::filtersplusplus::FilterModel::CytomicSVF);
        sstfilter.setModelConfiguration(
            sst::filtersplusplus::ModelConfig(sst::filtersplusplus::Passband::LP));
        sstfilter.setSampleRateAndBlockSize(hz, BLOCKSIZE);
        sstfilter.setMono();
        // without the delay lines Comb filter modes will explode,
        // so don't allow the user to choose those yet...
        // sstfilter.provideAllDelayLines(delaylinememory.data());
        if (!sstfilter.prepareInstance())
        {
            assert(false);
        }
        delaybuffer.resize(hz * 5.0);
    }
    void set_spatial_parameters(float azimuth, float elevation)
    {
        if (numchs == 2)
        {
            float panpos = xenakios::mapvalue(azimuth, -1.0f, 1.0f, 0.0f, 1.0f);
            sst::basic_blocks::dsp::pan_laws::panmatrix_t mat;
            sst::basic_blocks::dsp::pan_laws::monoEqualPower(panpos, mat);
            spatcoeffs[0] = mat[0];
            spatcoeffs[1] = mat[3];
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
    void process_block(std::span<float> audio_inout)
    {
        assert(numchs > 1);
        assert(audio_inout.size() >= 64 * BLOCKSIZE);
        const bool passthrough = false;
        if (passthrough)
        {
            for (int i = 0; i < BLOCKSIZE; ++i)
            {
                audio_inout[i * 64 + 0] = audio_inout[i * 64 + 0];
                audio_inout[i * 64 + 1] = audio_inout[i * 64 + 0];
            }
            return;
        }
        alignas(16) float processinbuffer[1][BLOCKSIZE];
        alignas(16) float processoutbuffer[1][BLOCKSIZE];
        for (int i = 0; i < BLOCKSIZE; ++i)
        {
            float insample = audio_inout[i * 64] * ingainlag.getValue();
            insample += feedbackaudio[i] * feedbackamt;
            processinbuffer[0][i] = insample;
            ingainlag.process();
        }
        float modulatedpitch =
            basepitch + modmatrix[MS_LFO0][MT_PITCH] * mod_source_values[0] * 12.0;
        stretch->setTransposeSemitones(modulatedpitch);
        float modulatedformant =
            baseformant + modmatrix[MS_LFO0][MT_FORMANT] * mod_source_values[0] * 12.0;
        stretch->setFormantSemitones(modulatedformant);
        stretch->process(processinbuffer, BLOCKSIZE, processoutbuffer, BLOCKSIZE);
        float modulatedcutoff =
            basefiltercutoff + mod_source_values[0] * modmatrix[MS_LFO0][MT_F0CUTOFF] * 24.0;
        modulatedcutoff = std::clamp(modulatedcutoff, -48.0f, 65.0f);

        float modulated_azi = baseazimuth + modmatrix[MS_LFO0][MT_AZI] * mod_source_values[0];
        modulated_azi = std::clamp(modulated_azi, -1.0f, 1.0f);
        set_spatial_parameters(modulated_azi, 0.0);

        sstfilter.makeCoefficients(0, modulatedcutoff, filter_reson);
        sstfilter.prepareBlock();
        for (int i = 0; i < BLOCKSIZE; ++i)
        {
            float filtered = sstfilter.processMonoSample(processoutbuffer[0][i]);
            push_to_delay(filtered);
            float delayed = pop_from_delay();
            audio_inout[i * 64 + 0] = delayed * spatcoeffs[0];
            audio_inout[i * 64 + 1] = delayed * spatcoeffs[1];
            feedbackaudio[i] = std::tanh(delayed);
        }
        sstfilter.concludeBlock();
    }
    SingleThreadedFifo<float> infifo{BLOCKSIZE};
    SingleThreadedFifo<float> outfifo{BLOCKSIZE};
};

using pmd = sst::basic_blocks::params::ParamMetaData;

struct ParamChangeMessage
{
    uint32_t id = CLAP_INVALID_ID;
    float val = 0.0f;
};

class OctoShifterProcessor final : public juce::AudioProcessor
{
  public:
    using lfo_t = sst::basic_blocks::modulators::SimpleLFO<OctoShifterProcessor, BLOCKSIZE>;
    double samplerate = 0.0;
    alignas(32) float table_envrate_linear[512];
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
    OctoShifterProcessor();
    ~OctoShifterProcessor() override;

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
    alignas(16) std::array<PitchShifter, numshifters> shifters;
    static constexpr size_t numLFOs = 2;
    alignas(!6) std::array<lfo_t, numLFOs> lfos{lfo_t(this), lfo_t(this)};
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
        PAR_PITCHLFO0AMOUNT = 10200,
        PAR_AZIMUTHLFO0AMOUNT = 10201,
        PAR_CUTOFF0LFO0AMOUNT = 10202,
        PAR_FORMANT_LFO0AMOUNT = 10203,
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
    // since we are not crazy enough to allow separate modulation amounts for each parameter
    // of each shifter, we have these patterns that can still produce different modulation
    // amounts per shifter
    const float modulation_patterns[2][8] = {{1, 1, 1, 1, 1, 1, 1, 1},
                                             {1, -1, 1, -1, 1, -1, 1, -1}};
    xenakios::Xoroshiro128Plus rng;
    static constexpr size_t max_input_channels = 8;
    static constexpr size_t max_output_channels = 64;
    std::atomic<int> outfifoavail = 0;
    std::atomic<int> infifoavail = 0;
    std::atomic<int> outsilencecount = 0;

  private:
    double osc0phase = 0.0;
    double osc1phase = 0.0;
    juce::AudioBuffer<float> workbuffer;
    alignas(16) SingleThreadedFifo<std::array<float, max_input_channels>> input_fifo{4096};
    alignas(16) SingleThreadedFifo<std::array<float, max_output_channels>> output_fifo{4096};
    alignas(16) int samplecount = 0;

    std::unordered_map<int, int> choicetolfoshapemap;
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OctoShifterProcessor)
};
