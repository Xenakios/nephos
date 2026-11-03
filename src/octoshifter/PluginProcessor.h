#pragma once

#include <algorithm>
#include <juce_audio_processors/juce_audio_processors.h>
#include <unordered_map>
#include "clap/id.h"
#include "containers/choc_Value.h"
#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_audio_processors_headless/juce_audio_processors_headless.h"
#include "juce_core/juce_core.h"
#include "signalsmith-stretch.h"
#include "sst/basic-blocks/modulators/SimpleLFO.h"
#include "sst/basic-blocks/dsp/EllipticBlepOscillators.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"
#include "sst/basic-blocks/params/ParamMetadata.h"
#include "sst/basic-blocks/dsp/PanLaws.h"
#include "sst/basic-blocks/dsp/OnePoles.h"
#include "sst/filters++.h"
#include <span>
#include "../../Common/xap_utils.h"
#include "sst/filters++/api.h"
#include "sst/filters++/enums.h"
#include "../../Common/xen_ambisonics.h"
#include <vector>
#include <cstddef>
#include <optional>
#include <utility>
#include "../juceplugin/juce_fmt_utils.h"
#include "sst/filters++/model_config.h"

static constexpr size_t BLOCKSIZE = 64;
static constexpr size_t BLOCK_SIZE_OS = BLOCKSIZE * 2;

template <typename T> inline T reflect_value_no_loop(const T minval, const T val, const T maxval)
{
    assert(maxval > minval);
    const T range = maxval - minval;
    const T doubled = range * T(2);

    // Normalize val into [0, 2*range), then reflect
    T temp = std::fmod(val - minval, doubled);
    if (temp < T(0))
        temp += doubled;

    return (temp <= range) ? minval + temp : maxval - (temp - range);
}

template <typename T>
[[nodiscard]] inline T wrap_value_no_loop(const T minval, const T val, const T maxval) noexcept
{
    assert(maxval > minval);
    const T range = maxval - minval;

    if constexpr (std::is_floating_point_v<T>)
    {
        T temp = std::fmod(val - minval, range);
        if (temp < T(0))
            temp += range;
        return temp + minval;
    }
    else
    {
        static_assert(std::is_signed_v<T>, "Use a signed integer type");
        T temp = (val - minval) % range;
        if (temp < T(0))
            temp += range;
        return temp + minval;
    }
}

struct ModulationEntry
{
    int source_index = -1;
    float depth = 0.0f;
    int curve = 0;
    float *target = nullptr;
};

struct FilterModeMessage
{
    sst::filtersplusplus::FilterModel model;
    sst::filtersplusplus::ModelConfig config;
};

struct CurveMessage
{
    int lfoindex = -1;
    int shifter_index = 0;
    int shifter_param_target = 0;
    int curveid = 0;
};

class PitchShifter
{
  public:
    enum MODSOURCES
    {
        MS_LFO0,
        MS_LFO1,
        MS_COUNT
    };
    // only add to the end of this because we are using
    // these directly as indexes...
    // delay time is not a modulatable target because
    // we can't modulate that well at the moment
    enum MODTARGETS
    {
        MT_PITCH,
        MT_FORMANT,
        MT_F0CUTOFF,
        MT_F0RESON,
        MT_AZI,
        MT_ELEVATION,
        MT_DELAYFEEDBACK,
        MT_INVOLUME,
        MT_OUTVOLUME,
        MT_COUNT
    };
    std::unique_ptr<signalsmith::stretch::SignalsmithStretch<>> stretch;
    bool enabled = true;
    float basepitch = 0.0f;
    float baseformant = 0.0f;
    float basefiltercutoff = 36.0f;
    float baseazimuth = 0.0f;
    float base_elevation = 0.0f;
    float base_filter_reson = 0.0f;

    float base_in_volume = 0.0f;
    float base_out_volume = 0.0f;
    float delaytime = 0.25f;
    float pending_delay_time = -1.0f;
    constexpr static int delay_change_fade_len = 128;
    int delay_change_fade_pos = 0;
    float basefeedbackamt = 0.0f;

    float mod_source_values[MS_COUNT] = {0.0f, 0.0f};
    double samplerate = 0.0;
    alignas(16) std::array<float, 64> spatcoeffs = {0.0f};
    alignas(16) sst::filtersplusplus::Filter sstfilter;
    alignas(16) sst::basic_blocks::dsp::DCBlocker<BLOCKSIZE> dcfilter;
    bool dcblocker_enabled = true;
    bool compensate_formants = false;
    int numoutchans = 2;
    sst::basic_blocks::dsp::OnePoleLag<float, true> ingainlag;
    sst::basic_blocks::dsp::OnePoleLag<float, true> outgainlag;
    sst::basic_blocks::dsp::OnePoleLag<float, true> azilag;
    sst::basic_blocks::dsp::OnePoleLag<float, true> fblag;
    alignas(16) float modsums[MT_COUNT] = {0.0f};
    std::vector<float> delaybuffer;
    alignas(16) int delaywritepos = 0;
    alignas(16) float feedbackaudio[BLOCKSIZE] = {0.0f};

    alignas(16) float modmatrix[MS_COUNT][MT_COUNT];
    alignas(16) std::function<float(float)> modmatrixcurves[MS_COUNT][MT_COUNT];
    std::atomic<int> pending_quality_change{-1};
    std::atomic<int> latency_in;
    std::atomic<int> latency_out;

    PitchShifter()
    {
        for (int i = 0; i < MS_COUNT; ++i)
        {
            for (int j = 0; j < MT_COUNT; ++j)
            {
                modmatrix[i][j] = 0.0f;
                modmatrixcurves[i][j] = [](float x) { return x; };
            }
        }
        modmatrixcurves[MS_LFO0][MT_DELAYFEEDBACK] = [](float x) {
            return std::clamp((x + 1.0f) * 0.5f, 0.0f, 1.0f);
        };
        // modmatrixcurves[MS_LFO1][MT_PITCH] = [](float x) { return xor_curve(x, 17844); };
        stretch = std::make_unique<signalsmith::stretch::SignalsmithStretch<>>();
        ingainlag.setRateInMilliseconds(5.0, 44100.0, 1.0f);
        ingainlag.snapTo(0.0);
        outgainlag.setRateInMilliseconds(5.0, 44100.0, 1.0f);
        outgainlag.snapTo(0.0);
        azilag.setRateInMilliseconds(5.0, 44100.0, 1.0f);
        azilag.snapTo(0.0);
        fblag.setRateInMilliseconds(5.0, 44100.0, 1.0f);
        fblag.snapTo(0.0);
        for (int i = 0; i < BLOCKSIZE; ++i)
            feedbackaudio[i] = 0.0f;
    }
    void set_enabled(bool b)
    {
        if (!enabled && b)
        {
            reset();
        }
        for (int i = 0; i < MT_COUNT; ++i)
            modsums[i] = 0.0f;
        enabled = b;
    }
    void reset()
    {
        for (int i = 0; i < BLOCKSIZE; ++i)
            feedbackaudio[i] = 0.0f;
        for (int i = 0; i < MT_COUNT; ++i)
            modsums[i] = 0.0f;
        std::fill(delaybuffer.begin(), delaybuffer.end(), 0.0f);
    }
    enum OUTCHANSMODE
    {
        OCM_STEREO,
        OCM_MONODISCRETE,
        OCM_AMBISONICS_1ST,
        OCM_AMBISONICS_2ND,
        OCM_AMBISONICS_3RD
    };
    OUTCHANSMODE ochanmode = OCM_STEREO;
    void set_output_mode(OUTCHANSMODE om)
    {

        numoutchans = 0;
        if (om == OCM_STEREO)
            numoutchans = 2;
        else if (om == OCM_MONODISCRETE)
            numoutchans = 1;
        else if (om == OCM_AMBISONICS_1ST)
            numoutchans = 4;
        else if (om == OCM_AMBISONICS_2ND)
            numoutchans = 9;
        else if (om == OCM_AMBISONICS_3RD)
        {
            numoutchans = 16;
            if (ochanmode != OCM_AMBISONICS_3RD)
            {
                for (int i = 0; i < 16; ++i)
                {
                    spatcoeffs[i] = 0.0f;
                }
            }
        }
        ochanmode = om;
        assert(numoutchans > 0);
    }
    void set_filter_mode(sst::filtersplusplus::FilterModel mo,
                         sst::filtersplusplus::ModelConfig conf)
    {
        sstfilter.setFilterModel(mo);
        sstfilter.setModelConfiguration(conf);
        sstfilter.setSampleRateAndBlockSize(samplerate, BLOCKSIZE);
        sstfilter.setMono();
        // sstfilter.provideAllDelayLines(delaylinememory.data());
        if (!sstfilter.prepareInstance())
        {
            Log::info("could not prepare sst filter");
        }
    }
    void set_sample_rate(double hz)
    {
        samplerate = hz;
        dcfilter.reset();
        ingainlag.setRateInMilliseconds(5.0, hz, 1.0f);
        outgainlag.setRateInMilliseconds(5.0, hz, 1.0f);
        azilag.setRateInMilliseconds(5.0, hz, 1.0f);
        fblag.setRateInMilliseconds(10.0, hz, 1.0f);

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
        delaybuffer.resize((size_t)(hz * 5.0));
        reset();
    }

    void push_to_delay(float x)
    {
        delaybuffer[delaywritepos] = x;
        ++delaywritepos;
        if (delaywritepos == delaybuffer.size())
            delaywritepos = 0;
    }
    float read_from_delay(float delaysecs)
    {
        int deltimesamples = delaysecs * samplerate;
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
        assert(numoutchans > 0);
        assert(audio_inout.size() >= 64 * BLOCKSIZE);
        if (pending_quality_change >= 0)
        {
            int q = pending_quality_change.load();
            if (q == 0)
                stretch->presetDefault(1, samplerate);
            else if (q == 1)
                stretch->presetCheaper(1, samplerate);
            else if (q == 2)
            {
                stretch->configure(1, static_cast<int>(samplerate * 0.08),
                                   static_cast<int>(samplerate * 0.04), false);
            }
            else if (q == 3)
            {
                stretch->configure(1, static_cast<int>(samplerate * 0.20),
                                   static_cast<int>(samplerate * 0.03), false);
            }
            pending_quality_change = -1;
        }
        latency_in = stretch->inputLatency();
        latency_out = stretch->outputLatency();
        alignas(16) float processinbuffer[1][BLOCKSIZE];
        alignas(16) float processoutbuffer[1][BLOCKSIZE];
        memset(modsums, 0, sizeof(float) * MT_COUNT);
        // this is getting complicated enough to start thinking about
        // a more proper modulation system, i guess...
        for (int i = 0; i < MS_COUNT; ++i)
        {
            for (int j = 0; j < MT_COUNT; ++j)
            {
                if (std::abs(modmatrix[i][j]) > 0.0f)
                {
                    float msval = mod_source_values[i];
                    msval = modmatrixcurves[i][j](msval);
                    modsums[j] += modmatrix[i][j] * msval;
                }
            }
        }

        float modulatedfeedback = basefeedbackamt + modsums[MT_DELAYFEEDBACK] * 0.5f;
        modulatedfeedback = std::clamp(modulatedfeedback, 0.0f, 0.999f);
        fblag.setTarget(modulatedfeedback);

        float modulated_in_volume = base_in_volume + modsums[MT_INVOLUME] * 12.0;
        modulated_in_volume = std::clamp(modulated_in_volume, -48.0f, 0.0f);
        modulated_in_volume = juce::Decibels::decibelsToGain(modulated_in_volume);
        ingainlag.setTarget(modulated_in_volume);

        float modulated_out_volume = base_out_volume + modsums[MT_OUTVOLUME] * 12.0;
        modulated_out_volume = std::clamp(modulated_out_volume, -48.0f, 0.0f);
        modulated_out_volume = juce::Decibels::decibelsToGain(modulated_out_volume);
        outgainlag.setTarget(modulated_out_volume);

        for (int i = 0; i < BLOCKSIZE; ++i)
        {
            float insample = audio_inout[i * 64] * ingainlag.getValue();
            insample += feedbackaudio[i] * fblag.getValue();
            processinbuffer[0][i] = insample;
            ingainlag.process();
            fblag.process();
        }
        if (dcblocker_enabled)
        {
            dcfilter.filter(processinbuffer[0], processinbuffer[0]);
        }
        float modulatedpitch = basepitch + modsums[MT_PITCH] * 12.0;
        stretch->setTransposeSemitones(modulatedpitch);
        float modulatedformant = baseformant + modsums[MT_FORMANT] * 12.0;
        stretch->setFormantSemitones(modulatedformant, compensate_formants);
        stretch->process(processinbuffer, BLOCKSIZE, processoutbuffer, BLOCKSIZE);

        float modulatedcutoff = basefiltercutoff + modsums[MT_F0CUTOFF] * 36.0;
        modulatedcutoff = std::clamp(modulatedcutoff, -48.0f, 65.0f);
        float modulatedreson = base_filter_reson + modsums[MT_F0RESON];
        modulatedreson = std::clamp(modulatedreson, 0.0f, 1.0f);
        sstfilter.makeCoefficients(0, modulatedcutoff, modulatedreson);

        float modulated_azi = baseazimuth + modsums[MT_AZI];
        if (ochanmode < OCM_AMBISONICS_1ST)
            modulated_azi = reflect_value_no_loop(-1.0f, modulated_azi, 1.0f);
        else
            modulated_azi = wrap_value_no_loop(-1.0f, modulated_azi, 1.0f);
        azilag.setTarget(modulated_azi);

        sstfilter.prepareBlock();
        alignas(16) sst::basic_blocks::dsp::pan_laws::panmatrix_t mat = {0.0f};
        alignas(16) float tempoutbuffer[BLOCKSIZE];
        for (int i = 0; i < BLOCKSIZE; ++i)
        {
            float filtered = sstfilter.processMonoSample(processoutbuffer[0][i]);
            push_to_delay(filtered);
            float delayed = 0.0f;
            if (pending_delay_time < 0.0f)
            {
                delayed = read_from_delay(delaytime);
            }
            else
            {
                const int half = BLOCKSIZE / 2;
                if (i < half)
                {
                    float olddelayed = read_from_delay(delaytime);
                    float fadegain = 1.0 - (1.0 / half);
                    delayed = olddelayed * fadegain;
                }
                else
                {
                    delaytime = pending_delay_time;
                    pending_delay_time = -1.0;
                    float fadegain = 1.0 / half * (i - half);
                    delayed = read_from_delay(delaytime) * fadegain;
                }
            }
            tempoutbuffer[i] = delayed * outgainlag.getValue();
            feedbackaudio[i] = std::tanh(delayed);
            // feedbackaudio[i] = delayed;
            outgainlag.process();
        }

        if (ochanmode == OCM_MONODISCRETE)
        {
            for (int i = 0; i < BLOCKSIZE; ++i)
            {
                audio_inout[i * 64 + 0] = tempoutbuffer[i];
            }
        }
        else if (ochanmode == OCM_STEREO)
        {
            for (int i = 0; i < BLOCKSIZE; ++i)
            {
                // obviously this is expensive and calculating Ambisonics
                // coefficients is going to be a lot lot worse...
                // we may need to look into some kind of look up table thing
                float panpos = (azilag.getValue() + 1.0) * 0.5;
                sst::basic_blocks::dsp::pan_laws::stereoEqualPower(panpos, mat);
                audio_inout[i * 64 + 0] = tempoutbuffer[i] * mat[0];
                audio_inout[i * 64 + 1] = tempoutbuffer[i] * mat[1];
                azilag.process();
            }
        }
        else if (ochanmode == OCM_AMBISONICS_3RD)
        {
            alignas(16) float newambicoeffs[64] = {0.0f};
            float azi_degrees = modulated_azi * 180.0f;
            float azi_radians = degreesToRadians(azi_degrees);
            float modulated_elev = base_elevation + modsums[MT_ELEVATION];
            modulated_elev = wrap_value_no_loop(-1.0f, modulated_elev, 1.0f) * 180.0f;
            float elev_radians = degreesToRadians(modulated_elev);
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            sphericalToCartesian(azi_radians, elev_radians, x, y, z);
            SHEval3(x, y, z, newambicoeffs);
            for (int i = 0; i < 16; ++i)
                newambicoeffs[i] *= n3d2sn3d[i];
            // obviously this could be optimized in various ways, but
            // will suffice for now...
            for (int i = 0; i < BLOCKSIZE; ++i)
            {
                float interpos = 1.0f / BLOCKSIZE * i;
                float ain = tempoutbuffer[i];
                for (int j = 0; j < 16; ++j)
                {
                    float coeffa = spatcoeffs[j];
                    float coeffb = newambicoeffs[j];
                    float interpcoeff = coeffa + (coeffb - coeffa) * interpos;
                    audio_inout[i * 64 + j] = ain * interpcoeff;
                }
            }
            for (int j = 0; j < 16; ++j)
            {
                spatcoeffs[j] = newambicoeffs[j];
            }
        }
        sstfilter.concludeBlock();
    }
};

using pmd = sst::basic_blocks::params::ParamMetaData;

struct ParamChangeMessage
{
    uint32_t id = CLAP_INVALID_ID;
    float val = 0.0f;
};

struct ModulationMessage
{
    int shifterindex = 0;
    float values[PitchShifter::MT_COUNT];
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
        PAR_OUTPUT_GAIN = 150,
        PAR_PITCH = 200,
        PAR_FORMANT = 300,
        PAR_DELAYTIME = 400,
        PAR_DELAYFEEDBACK = 450,
        PAR_FILTERCUTOFF = 500,
        PAR_FILTERRESON = 600,
        PAR_AZIMUTH = 700,
        PAR_ELEVATION = 750,
        PAR_PITCHQUALITY = 800,
        PAR_OUTPUTMODE = 900,
        PAR_LFOSHAPES = 10000,
        PAR_LFORATES = 10100,
        PAR_LFODEFORMS = 10300,
        PAR_LFOSHIFTS = 10400,
        PAR_LFOWARPS = 10500,
        PAR_PITCHLFOAMOUNTS = 50000,
        PAR_AZIMUTHLFOAMOUNTS = 51000,
        PAR_CUTOFF0LFOAMOUNTS = 52000,
        PAR_FORMANT_LFOAMOUNTS = 53000,
        PAR_DELAYFB_LFOAMOUNTS = 54000,
        PAR_INVOLUME_LFOAMOUNTS = 55000,
        PAR_RESON0_LFOAMOUNTS = 56000,
        PAR_OUTVOLUME_LFOAMOUNTS = 57000,
        PAR_ELEVATION_LFOAMOUNTS = 58000
    };
    void set_initialpatch();
    std::vector<pmd> param_metadatas;
    std::unordered_map<uint32_t, pmd *> par_id_to_metadata;
    std::array<float, 256> param_values;
    std::unordered_map<uint32_t, float *> par_id_to_value_pointer;
    choc::fifo::SingleReaderSingleWriterFIFO<ParamChangeMessage> params_from_gui_fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<ParamChangeMessage> params_to_gui_fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<ModulationMessage> modulations_to_gui_fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<FilterModeMessage> filter_mode_from_gui_fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<FilterModeMessage> filter_mode_to_gui_fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<CurveMessage> curve_from_gui_fifo;

    std::atomic<bool> gui_visible{false};
    choc::value::Value get_state();
    void set_state(choc::value::ValueView view);
    void set_parameter_value(uint32_t id, float value);
    void send_param_updates_to_gui();

    xenakios::Xoroshiro128Plus rng;
    static constexpr size_t max_input_channels = 8;
    static constexpr size_t max_output_channels = 64;
    std::atomic<int> outfifoavail = 0;
    std::atomic<int> infifoavail = 0;
    std::atomic<int> outsilencecount = 0;
    std::atomic<bool> dc_blocker_enabled = true;
    std::atomic<bool> formant_compensation = false;
    std::unordered_map<int, float> mod_ranges;
    // since we are not crazy enough to allow separate modulation amounts for each parameter
    // of each shifter, we have these patterns that can still produce different modulation
    // amounts per shifter
    static const size_t num_modulation_patterns = 4;
    const int8_t modulation_patterns[num_modulation_patterns][8] = {{1, 1, 1, 1, 1, 1, 1, 1},
                                                                    {1, -1, 1, -1, 1, -1, 1, -1},
                                                                    {-1, 1, -1, 1, -1, 1, -1, 1},
                                                                    {1, 0, 0, 0, 0, 0, 0, 0}};
    std::array<int, PitchShifter::MT_COUNT> modtarget_to_modpattern = {0};
    FilterModeMessage current_filter_mode;

  private:
    double osc0phase = 0.0;
    double osc1phase = 0.0;
    juce::AudioBuffer<float> workbuffer;
    alignas(16) SingleThreadedFifo<std::array<float, max_input_channels>> input_fifo{4096};
    alignas(16) SingleThreadedFifo<std::array<float, max_output_channels>> output_fifo{4096};
    alignas(16) int callbackcount = 0;

    std::unordered_map<int, int> choicetolfoshapemap;
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OctoShifterProcessor)
};
