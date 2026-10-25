#pragma once
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <span>
// #include "sst/basic-blocks/dsp/CorrelatedNoise.h"
#include "clap/id.h"
#include "containers/choc_Value.h"
#include "fmt/format.h"
#include "sst/basic-blocks/dsp/EllipticBlepOscillators.h"
#include <variant>
#include "../Common/xen_ambisonics.h"
#include "../Common/xap_utils.h"
#include "sst/basic-blocks/dsp/Lag.h"
#include "sst/filters++/api.h"
#include "sst/filters++/enums.h"
#include "sst/filters++/model_config.h"
#include "sst/filters/FastTiltNoiseFilter.h"
#include "xen_modulationsources.h"
#include "grainoscillators.h"
#include "grainfx.h"
#include "sst/basic-blocks/mod-matrix/ModMatrix.h"
#include "sst/basic-blocks/modulators/SimpleLFO.h"
#include "sst/basic-blocks/params/ParamMetadata.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"
#include "threading/choc_SpinLock.h"
#include "easing.h"
#define SIMDE_ENABLE_NATIVE_ALIASES // lets you skip the simde_ prefix
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>
#include "Tunings.h"

using namespace sst::basic_blocks::mod_matrix;

inline constexpr int granul_block_size = 8;
inline constexpr uint8_t maxAmbiSonicOrder = 7;

inline constexpr int ambisonicOrderNumChannels(int order) { return (order + 1) * (order + 1); }

template <typename T> inline int sgn(T val) { return (T(0) < val) - (val < T(0)); }

struct ParameterMessage
{
    uint32_t id = 0;
    float value = 0.0f;
};

struct FilterBank
{
    static constexpr size_t maxfilters = 16;
    static constexpr size_t maxchannels = 64;
    // sst filter can do 4 channels, so 16 for up to 64 channels of processing
    alignas(16) std::array<sst::filtersplusplus::Filter, maxfilters> filters;
    size_t numactivechannels = 0;
    // which cutoff index is used for each channel
    alignas(16) std::array<int, maxchannels> cutoffmapping;
    FilterBank()
    {
        for (int i = 0; i < cutoffmapping.size(); ++i)
        {
            cutoffmapping[i] = 0;
        }
    }
    void prepare(double samplerate)
    {
        for (auto &f : filters)
        {
            f.setFilterModel(sst::filtersplusplus::FilterModel::CytomicSVF);
            f.setModelConfiguration(
                sst::filtersplusplus::ModelConfig{sst::filtersplusplus::Passband::HP});
            f.setQuad();
            f.setSampleRateAndBlockSize(samplerate, granul_block_size);
            if (!f.prepareInstance())
            {
                assert(false);
            }
        }
    }

    void set_cutoff_mapped(int index, float semitones)
    {
        assert(numactivechannels > 0);
        semitones = std::clamp(semitones, -48.0f, 12.0f);
        for (size_t i = 0; i < numactivechannels; ++i)
        {
            if (cutoffmapping[i] == index)
            {
                int filtindex = i / 4;
                int chanindex = i % 4;
                // note that this is inefficient if we happen to have duplicated parameter values,
                // which is likely at least for now...
                filters[filtindex].makeCoefficients(chanindex, semitones, 0.0f);
            }
        }
    }
    void set_cutoff(float semitones)
    {
        assert(numactivechannels > 0);
        semitones = std::clamp(semitones, -48.0f, 12.0f);
        size_t numFiltersToUpdate = (numactivechannels + 3) / 4; // ceiling division
        for (size_t i = 0; i < numFiltersToUpdate; ++i)
        {
            filters[i].makeCoefficients(0, semitones, 0.0f);
            filters[i].copyCoefficientsFromVoiceToVoice(0, 1);
            filters[i].copyCoefficientsFromVoiceToVoice(0, 2);
            filters[i].copyCoefficientsFromVoiceToVoice(0, 3);
        }
    }
    void process(std::span<float> buffer)
    {
        assert(numactivechannels > 0);
        size_t numFiltersToUpdate = (numactivechannels + 3) / 4; // ceiling division
        for (size_t i = 0; i < numFiltersToUpdate; ++i)
        {
            filters[i].prepareBlock();
        }
        alignas(16) float insamples[4];
        alignas(16) float outsamples[4];
        for (size_t sample = 0; sample < granul_block_size; ++sample)
        {
            for (size_t i = 0; i < numactivechannels; i += 4)
            {
                for (int j = 0; j < 4; ++j)
                {
                    size_t chan = i + j;
                    insamples[j] = (chan < numactivechannels)
                                       ? buffer[sample * numactivechannels + chan]
                                       : 0.0f;
                }
                filters[i / 4].processQuadSample(insamples, outsamples);
                for (int j = 0; j < 4; ++j)
                {
                    size_t chan = i + j;
                    if (chan < numactivechannels)
                        buffer[sample * numactivechannels + chan] = outsamples[j];
                }
            }
        }
        for (size_t i = 0; i < numFiltersToUpdate; ++i)
        {
            filters[i].concludeBlock();
        }
    }
};

struct GranulatorModConfig
{
    struct SourceIdentifier
    {
        uint32_t src{0};
        bool operator==(const SourceIdentifier &other) const { return src == other.src; }
    };

    struct TargetIdentifier
    {
        int target{0};
        // uint32_t nm{};
        int16_t depthPosition{-1};

        bool operator==(const TargetIdentifier &other) const
        {
            return target == other.target && depthPosition == other.depthPosition;
            // return baz == other.baz && nm == other.nm && depthPosition == other.depthPosition;
        }
    };
    static bool isTargetModMatrixDepth(const TargetIdentifier &t) { return t.depthPosition >= 0; }
    static bool supportsLag(const SourceIdentifier &s) { return true; }
    static size_t getTargetModMatrixElement(const TargetIdentifier &t)
    {
        assert(isTargetModMatrixDepth(t));
        return (size_t)t.depthPosition;
    }

    using RoutingExtraPayload = int;
    struct MyCurve
    {
        int id = 0;
        float par0 = 0.0f;
        bool operator==(const MyCurve &other) const { return id == other.id; }
    };
    using CurveIdentifier = MyCurve;
    enum CurveTypes
    {
        CURVE_LINEAR = 1,
        CURVE_SQUARE,
        CURVE_CUBE,
        CURVE_STEPS1,
        CURVE_EXPSIN1 = CURVE_STEPS1 + 16,
        CURVE_EXPSIN2,
        CURVE_XOR1,
        CURVE_XOR2,
        CURVE_XOR3,
        CURVE_XOR4,
        CURVE_XOR5,
        CURVE_XOR6,
        CURVE_XOR7,
        CURVE_XOR8,
        CURVE_BITMIRROR,
        CURVE_BIPOLARTOUNIPOLAR,
        CURVE_UNIPOLARTOBIPOLAR,
        CURVE_HARMONICSERIES3OCTAVES,
        CURVE_HARMONICSERIES4OCTAVES,
        CURVE_HARMONICSERIES5OCTAVES,
        CURVE_TOPOWER16,
        CURVE_PEAKING1,
        CURVE_PEAKING2,
        CURVE_PEAKING3,
        CURVE_PEAKING4,
        CURVE_PEAKING5,
        CURVE_PEAKING6,
        CURVE_ABS,
        CURVE_POPCORN,
        CURVE_BUTTERFLY,
        CURVE_SIGMOID1,
        NUM_CURVES
    };
    static float peaking_curve(float x, float y)
    {
        x = std::clamp(x, -1.0f, 1.0f);
        y = std::clamp(y, 0.1f, 4.0f);
        return -1.0f + 2.0f * (1.0f - std::pow(std::abs(x), y));
    }
    static float xor_curve(float x, uint16_t a)
    {
        x = std::clamp(x, -1.0f, 1.0f);
        x = (x + 1.0f) * 0.5f;
        uint16_t ival = static_cast<uint16_t>(x * 65535.0f);
        ival = ival ^ a;
        x = ival / 65535.0f;
        return -1.0f + 2.0f * x;
    }
    static uint16_t reverse_bits_16(uint16_t n)
    {
        n = ((n >> 1) & 0x5555) | ((n << 1) & 0xAAAA); // Swap adjacent bits
        n = ((n >> 2) & 0x3333) | ((n << 2) & 0xCCCC); // Swap 2-bit groups
        n = ((n >> 4) & 0x0F0F) | ((n << 4) & 0xF0F0); // Swap nibbles
        n = ((n >> 8) & 0x00FF) | ((n << 8) & 0xFF00); // Swap bytes
        return n;
    }

    static float bit_reversal_curve(float x)
    {
        x = std::clamp(x, -1.0f, 1.0f);
        x = (x + 1.0f) * 0.5f;

        uint16_t ival = static_cast<uint16_t>(x * 65535.0f);
        ival = reverse_bits_16(ival); // Mirror the bits

        x = ival / 65535.0f;
        return -1.0f + 2.0f * x;
    }
    static float expsin(float x, int ampmode, float frequency)
    {
        float norm = (x + 1.0f) * 0.5f;
        float amplitude = 1.0f;
        if (ampmode == 1)
            amplitude = norm * norm;
        else if (ampmode == 2)
            amplitude = norm * norm * norm;
        return amplitude * std::sin(2 * M_PI * norm * frequency);
    }
    static float harmseries(float x, int octaves)
    {
        int numpartials = std::pow(2, octaves);
        x = std::clamp(x, -1.0f, 1.0f);
        x = (x + 1.0f) * 0.5f;
        x = 1.0 + (numpartials - 1) * x;
        return std::log2(std::floor(x)) / octaves;
    }
    static std::function<float(float)> getCurveOperator(CurveIdentifier id);
    struct CurveMetadata
    {
        int id = 0;
        std::string groupname;
        std::string name;
    };
    static std::vector<CurveMetadata> get_curve_metadata()
    {
        std::vector<CurveMetadata> result;
        result.emplace_back(CURVE_LINEAR, "", "-Linear-");
        result.emplace_back(CURVE_ABS, "UTILITY", "Absolute value");
        result.emplace_back(CURVE_BIPOLARTOUNIPOLAR, "UTILITY", "Bipolar to Unipolar");
        result.emplace_back(CURVE_UNIPOLARTOBIPOLAR, "UTILITY", "Unipolar to Bipolar");
        result.emplace_back(CURVE_SQUARE, "POWER", "x^2");
        result.emplace_back(CURVE_CUBE, "POWER", "x^3");
        result.emplace_back(CURVE_TOPOWER16, "POWER", "x^16");
        result.emplace_back(CURVE_SIGMOID1, "POWER", "Sigmoid");
        for (int i = 0; i < 16; ++i)
        {
            int actnumsteps = i + 2;
            result.emplace_back(CURVE_STEPS1 + i, "STEPS",
                                fmt::format("{:02d} Steps", actnumsteps));
        }
        for (int i = 0; i < 8; ++i)
        {
            result.emplace_back(CURVE_XOR1 + i, "XOR", fmt::format("XOR {}", i + 1));
        }
        for (int i = 0; i < 4; ++i)
        {
            result.emplace_back(CURVE_PEAKING1 + i, "PEAKING", fmt::format("Peaking {}", i + 1));
        }
        result.emplace_back(CURVE_HARMONICSERIES3OCTAVES, "HARMONIC SERIES", "3 Octaves");
        result.emplace_back(CURVE_HARMONICSERIES4OCTAVES, "HARMONIC SERIES", "4 Octaves");
        result.emplace_back(CURVE_HARMONICSERIES5OCTAVES, "HARMONIC SERIES", "5 Octaves");
        result.emplace_back(CURVE_EXPSIN1, "EXP SINE", "Exp Sine 1");
        result.emplace_back(CURVE_EXPSIN2, "EXP SINE", "Exp Sine 2");
        result.emplace_back(CURVE_BITMIRROR, "STRANGE", "Bit Mirror");
        result.emplace_back(CURVE_POPCORN, "STRANGE", "Popcorn");
        result.emplace_back(CURVE_BUTTERFLY, "STRANGE", "Butterfly");
        std::sort(result.begin(), result.end(), [](auto &lhs, auto &rhs) {
            return lhs.groupname + "/" + lhs.name < rhs.groupname + "/" + rhs.name;
        });
        return result;
    }
    static constexpr bool IsFixedMatrix{true};
    static constexpr size_t FixedMatrixSize{16};
    static constexpr bool ProvidesNonZeroTargetBases{true};
};

template <> struct std::hash<GranulatorModConfig::MyCurve>
{
    std::size_t operator()(const GranulatorModConfig::MyCurve &c) const noexcept
    {
        auto h1 = std::hash<int>{}((int)c.id);
        return h1;
    }
};

template <> struct std::hash<GranulatorModConfig::SourceIdentifier>
{
    std::size_t operator()(const GranulatorModConfig::SourceIdentifier &s) const noexcept
    {
        auto h1 = std::hash<int>{}((int)s.src);
        return h1;
        // auto h2 = std::hash<int>{}((int)s.index0);
        // auto h3 = std::hash<int>{}((int)s.index1);
        // return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

template <> struct std::hash<GranulatorModConfig::TargetIdentifier>
{
    std::size_t operator()(const GranulatorModConfig::TargetIdentifier &s) const noexcept
    {
        auto h1 = std::hash<int>{}((int)s.target);
        return h1;
        // auto h2 = std::hash<uint32_t>{}((int)s.nm);

        // return h1 ^ (h2 << 1);
    }
};

class GranulatorModMatrix
{
  public:
    FixedMatrix<GranulatorModConfig> m;
    FixedMatrix<GranulatorModConfig>::RoutingTable rt;
    double samplerate = 0.0;

    static constexpr size_t BLOCKSIZE = granul_block_size;
    static constexpr size_t BLOCK_SIZE_OS = BLOCKSIZE * 2;
    static constexpr size_t numLfos = 8;
    using lfo_t = sst::basic_blocks::modulators::SimpleLFO<GranulatorModMatrix, BLOCKSIZE>;
    alignas(32) std::array<std::unique_ptr<lfo_t>, numLfos> m_lfos;
    alignas(32) float table_envrate_linear[512];
    alignas(32) std::array<sst::basic_blocks::dsp::RNG, numLfos> m_rngs;
    GranulatorModMatrix(double sr) : samplerate(sr)
    {
        initTables();
        for (size_t i = 0; i < numLfos; ++i)
        {
            m_lfos[i] = std::make_unique<lfo_t>(this);
            m_lfos[i]->attack(0);
        }
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

struct GrainEvent
{
    GrainEvent() {};
    GrainEvent(double tpos, float dur, float pitch, float vol)
        : time_position(tpos), duration(dur), pitch_semitones(pitch), volume(vol)
    {
        // clear_mod_amounts();
    }
    void clear_mod_amounts()
    {
        for (int i = 0; i < max_grain_mod_slots; ++i)
            modamounts[i] = 0.0f;
    }
    double time_position = 0.0;
    float duration = 0.0f;
    float pitch_semitones = 0.0f;
    float pitch_quantize_amount = 0.0f;
    int generator_type = 0;
    float volume = 0.0f;
    float auxsend = 0.0f;
    uint8_t envelope_start_type = 0;
    uint8_t envelope_end_type = 0;
    float envelope_shape = 0.5f;
    std::array<float, 12> auxenvparams = {0.0f};

    float azimuth = 0.0f;
    float ambi_spread = 0.0f;
    float ambi_rotate = 0.0f;
    float ambi_omni_boost = 0.0f;
    float elevation = 0.0f;
    float sync_octaves = 0.0f;
    float pulse_width = 0.5f;
    float fm_pitch = 0.0f;
    float fm_amount = 0.0f;
    float fm_feedback = 0.0f;
    float noisecorr = 0.0f;
    float noiseimode = 0.0f;
    static constexpr size_t max_grain_mod_slots = 4;
    float modamounts[max_grain_mod_slots] = {0.0f};
    float insertparams[4][10] = {0.0f};
};

struct tone_info
{
    int index = -1;
    const char *name = nullptr;
};

static tone_info osc_infos[7] = {{0, "EBSINE"},  {1, "EBSEMISINE"}, {2, "EBTRIANGLE"},
                                 {3, "EBSAW"},   {4, "EBSQUARE"},   {5, "DPWSAW"},
                                 {6, "DPWPULSE"}};

inline std::vector<std::string> osc_types()
{
    std::vector<std::string> result;
    for (int i = 0; i < 7; ++i)
        result.push_back(osc_infos[i].name);
    return result;
}

inline int osc_name_to_index(std::string name)
{
    for (int i = 0; i < 7; ++i)
        if (name == osc_infos[i].name)
            return osc_infos[i].index;
    return -1;
}

constexpr size_t numPitchBandAttens = 7;

inline bool is_monotonic_tuning(Tunings::Tuning &tuning)
{
    double prev = tuning.logScaledFrequencyForMidiNote(-256) * 12.0;
    for (int i = -255; i < 256; ++i)
    {
        double cur = tuning.logScaledFrequencyForMidiNote(i) * 12.0;
        if (cur < prev)
            return false;
        prev = cur;
    }
    return true;
}

inline double quantize_pitch_binary(Tunings::Tuning &tuning, double sourcepitch, double threshold)
{
    auto pitchAt = [&](int i) { return tuning.logScaledFrequencyForMidiNote(i) * 12.0; };

    constexpr int firstNote = -256;
    constexpr int lastNote = 255;

    int lo = firstNote;
    int hi = lastNote + 1; // exclusive

    while (lo < hi)
    {
        const int mid = lo + (hi - lo) / 2;

        if (pitchAt(mid) < sourcepitch)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo <= firstNote)
        return pitchAt(firstNote);

    if (lo > lastNote)
        return pitchAt(lastNote);

    const double higher = pitchAt(lo);
    const double lower = pitchAt(lo - 1);
    const double interval = higher - lower;

    // Normalized position within the interval: [0, 1].
    const double position = (sourcepitch - lower) / interval;

    // Quantize only near either grid point.
    const double halfThreshold = threshold * 0.5;

    if (position < halfThreshold)
        return lower;

    if (position > 1.0 - halfThreshold)
        return higher;

    return sourcepitch;
}

class GranulatorVoice
{
  public:
    GranulatorVoice() : tiltfilter(tfhost)
    {
        std::fill(ambcoeffs.begin(), ambcoeffs.end(), 0.0f);
        for (int i = 0; i < GrainEvent::max_grain_mod_slots; ++i)
            modulation_slots[i] = {CLAP_INVALID_ID, 0.0f, CLAP_INVALID_ID};
    }
    void start(GrainEvent &evpars);
    void process(float *outputs)
    {
        double normphase = (double)phase / grain_end_phase;
        alignas(16) float modulatedvalues[NUMMODTARGETS] = {0.0f};
        process_mod_matrix(normphase, auxenvparams, modulation_slots, *aux_envelopes,
                           modulatedvalues);
        float modulatedvolume =
            std::clamp(grain_base_volume + modulatedvalues[MT_VOLUME], 0.0f, 1.0f);
        modulatedvolume = modulatedvolume * modulatedvolume * modulatedvolume;
        envgainlag.setTarget(modulatedvolume);

        std::visit(
            [this, &modulatedvalues](auto &q) {
                double finalpitch = pitch_base + modulatedvalues[MT_PITCH] * 24.0f;
                double hz = 440.0 * std::pow(2.0, 1.0 / 12.0 * (finalpitch - 9.0));
                q.setFrequency(hz);
                if constexpr (std::is_same_v<decltype(q), FMOsc &>)
                {
                    float modulatedfmpitch =
                        std::clamp(fmpitch + modulatedvalues[MT_FMPITCH] * 24.0f, -48.0f, 48.0f);
                    float fmhz = 440.0 * std::pow(2.0, 1.0 / 12.0 * (modulatedfmpitch - 9.0));
                    q.setModulatorFreq(fmhz);
                    float modulatedfmamount =
                        std::clamp(fmmodamount + modulatedvalues[MT_FMDEPTH], 0.0f, 1.0f);
                    modulatedfmamount = std::pow(modulatedfmamount, 3.0f) * 128.0f;
                    q.setModIndex(modulatedfmamount);
                    q.setFeedbackAmount(fmfeedback);
                }
            },
            theoscillator);
        for (size_t i = 0; i < 2; ++i)
        {
            if (insert_fx[i].mainmode == GrainInsertFX::GFXSSTFILTER)
            {
                // cutoff
                insert_fx[i].parammodvalues[0] = modulatedvalues[MT_INSERTASTART + i * 10] * 36.0;
                // resonance
                insert_fx[i].parammodvalues[1] =
                    modulatedvalues[MT_INSERTASTART + i * 10 + 1] * 0.5f;
                // extra param
                insert_fx[i].parammodvalues[2] =
                    modulatedvalues[MT_INSERTASTART + i * 10 + 2] * 0.5f;
                // cut off stereo separation
                insert_fx[i].parammodvalues[3] =
                    modulatedvalues[MT_INSERTASTART + i * 10 + 3] * 12.0f;
                // drywet mix
                insert_fx[i].parammodvalues[4] =
                    modulatedvalues[MT_INSERTASTART + i * 10 + 4] * 0.5f;
            }
            insert_fx[i].prepareBlock();
        }
        int envpeakpos = envshape * grain_end_phase;
        envpeakpos = std::clamp(envpeakpos, 16, grain_end_phase - 16);

        int tail_len_samples = tail_len * sr;
        int tail_fade_samples = tail_fade_len * sr;
        int tail_fade_start = grain_end_phase + tail_len_samples - tail_fade_samples;
        int tail_fade_end = grain_end_phase + tail_len_samples;
        alignas(16) float tempbuffer[granul_block_size * 2];
        int cachedphase = phase;
        for (size_t i = 0; i < granul_block_size; ++i)
        {
            float outsample = 0.0f;
            if (phase >= 0 && phase < grain_end_phase)
            {
                outsample = std::visit([](auto &q) { return q.step(); }, theoscillator);
                float envgain = 0.0f;
                if (phase < envpeakpos)
                {
                    envgain = xenakios::mapvalue<float>(phase, 0.0, envpeakpos, 0.0f, 1.0f);
                    envgain = eluts->getValueLERP<false>(envstarttype, envgain);
                }
                else
                {
                    envgain =
                        xenakios::mapvalue<float>(phase, envpeakpos, grain_end_phase, 1.0f, 0.0f);
                    envgain = eluts->getValueLERP<false>(envendtype, envgain);
                }
                // envgain = std::clamp(envgain, 0.0f, 1.0f);
                outsample *= envgain * envgainlag.getValue() * polarity_gain;
                envgainlag.process();
            }
            tempbuffer[i] = outsample;
            tempbuffer[granul_block_size + i] = outsample;
            ++phase;
        }
        if (filter_routing == FR_ALLSERIAL)
        {
            for (size_t insertIndex = 0; insertIndex < 2; ++insertIndex)
            {
                insert_fx[insertIndex].processBlock<granul_block_size>(tempbuffer);
            }
        }
        else if (filter_routing == FR_ALLPARALLEL)
        {
            float signalstoinserts[4][2];
            float summedinserts[2] = {0.0f, 0.0f};
            for (size_t insertindex = 0; insertindex < numInsertSlots; ++insertindex)
            {
                if (insert_fx[insertindex].mainmode != GrainInsertFX::GFXNONE)
                {
                    // signalstoinserts[insertindex][0] = outsample0;
                    // signalstoinserts[insertindex][1] = outsample1;
                    // insert_fx[insertindex].processStereo(signalstoinserts[insertindex][0],
                    //                                     signalstoinserts[insertindex][1]);
                    summedinserts[0] += signalstoinserts[insertindex][0];
                    summedinserts[1] += signalstoinserts[insertindex][1];
                }
            }
        }

        for (size_t i = 0; i < granul_block_size; ++i)
        {
            float fadegain = 1.0f;
            if (cachedphase >= grain_end_phase)
            {
                if (cachedphase >= tail_fade_end)
                {
                    active = false;
                    fadegain = 0.0f;
                }
                else if (cachedphase >= tail_fade_start)
                {
                    fadegain = xenakios::mapvalue<float>(cachedphase, tail_fade_start,
                                                         tail_fade_end, 1.0f, 0.0f);
                    if (fadegain < 0.0f)
                        fadegain = 0.0f;
                }
            }
            ++cachedphase;
            tempbuffer[i] *= fadegain;
            tempbuffer[i + granul_block_size] *= fadegain;

#define USE_AVX2_AMBIS
#ifdef USE_AVX2_AMBIS
            // Process 8 channels at a time using AVX
            int chan = 0;
            for (; chan <= num_outputchans - 8; chan += 8)
            {
                // Load 8 ambisonics coefficients for each source
                __m256 coeffs0 = _mm256_load_ps(&ambcoeffs[chan]);      // coeffs for outsample0
                __m256 coeffs1 = _mm256_load_ps(&ambcoeffs[chan + 64]); // coeffs for outsample1

                // Broadcast the scalar audio samples across all 8 lanes
                __m256 sample0 = _mm256_set1_ps(tempbuffer[i]);
                __m256 sample1 = _mm256_set1_ps(tempbuffer[i + granul_block_size]);

                // Multiply-accumulate: sample * coefficients
                __m256 result =
                    _mm256_fmadd_ps(sample0, coeffs0,                 // outsample0 * coeffs0
                                    _mm256_mul_ps(sample1, coeffs1)); // + outsample1 * coeffs1

                // Store results into the output buffer
                _mm256_store_ps(&outputs[i * 64 + chan], result);
            }

            // Scalar fallback for any remaining channels (if num_outputchans isn't a multiple of 8)
            for (; chan < num_outputchans; ++chan)
            {
                outputs[i * 64 + chan] = tempbuffer[i] * ambcoeffs[chan] +
                                         tempbuffer[i + granul_block_size] * ambcoeffs[chan + 64];
            }
            for (chan = 1; chan < num_outputchans; ++chan)
            {
                outputs[i * 64 + chan] *= omniboostinverse;
            }

#else
            for (int chan = 0; chan < num_outputchans; ++chan)
            {
                outputs[i * 64 + chan] = 0.0f;
                outputs[i * 64 + chan] += outsample0 * ambcoeffs[chan];
                outputs[i * 64 + chan] += outsample1 * ambcoeffs[chan + 64];
            }
#endif
        }
        for (auto &f : insert_fx)
            f.concludeBlock();
    }
    std::variant<NoiseGen, FMOsc, sst::basic_blocks::dsp::EBApproxSin<>,
                 sst::basic_blocks::dsp::EBApproxSemiSin<>, sst::basic_blocks::dsp::EBTri<>,
                 sst::basic_blocks::dsp::EBSaw<>, sst::basic_blocks::dsp::EBPulse<>>
        theoscillator;

    alignas(16) TiltFilterHost tfhost;
    alignas(16) sst::filters::FastTiltNoiseFilter<TiltFilterHost> tiltfilter;
    static constexpr size_t numInsertSlots = 4;
    static constexpr size_t maxParamsPerInsert = 10;
    alignas(32) std::array<GrainInsertFX, numInsertSlots> insert_fx;

    int phase = 0;
    int grain_end_phase = 0;
    double sr = 0.0;
    bool samplerate_was_changed = false;
    bool active = false;
    float tail_len = 0.005;
    float tail_fade_len = 0.005;
    float polarity_gain = 1.0f;
    int prior_osc_type = -1;
    EasingLUTS *eluts = nullptr;
    Tunings::Tuning *tuning = nullptr;
    bool fmfollowsmainpitch = false;
    std::span<int> osctypemapping;
    // 2x up to 7th order Ambisonics
    alignas(32) std::array<float, 128> ambcoeffs;
    enum FilterRouting
    {
        FR_ALLOFF,
        FR_ALLSERIAL,
        FR_ALLPARALLEL
    };
    FilterRouting filter_routing = FR_ALLSERIAL;
    static constexpr size_t num_aux_envelopes = 4;
    std::array<SimpleEnvelope, num_aux_envelopes> *aux_envelopes = nullptr;
    struct ModSlot
    {
        uint32_t source_id = CLAP_INVALID_ID;
        float depth = 0.0f;
        uint32_t target_id = CLAP_INVALID_ID;
    };
    alignas(16) std::array<ModSlot, GrainEvent::max_grain_mod_slots> modulation_slots;
    alignas(16) sst::basic_blocks::dsp::OnePoleLag<float, true> envgainlag;
    float pitch_base = 0.0f;
    float grain_base_volume = 0.0;
    std::array<float, 4> current_coordinates = {0.0f};
    float auxsend1 = 0.0;
    std::span<float> pitchBandAttens;
    uint8_t envstarttype = 0;
    uint8_t envendtype = 0;
    double envshape = 0.5;
    std::array<float, num_aux_envelopes * 3> auxenvparams = {0.0f};
    int grainid = 0;
    bool doambnormalization = false;
    int ambisonic_order = 1;
    int num_outputchans = 0;

    void set_samplerate(double hz)
    {
        sr = hz;
        tfhost.sr = sr;
        for (auto &fx : insert_fx)
            fx.prepareInstance(sr, granul_block_size);
        samplerate_was_changed = true;
    }
    struct PendingInsertConf
    {
        bool is_pending = false;
        uint8_t mainmode = 0;
        uint8_t awtype = 0;
        sst::filtersplusplus::FilterModel sstmodel;
        sst::filtersplusplus::ModelConfig sstconfig;
    };
    std::array<PendingInsertConf, 4> pendingInsertConfs;
    void set_insert_type(size_t filtindex, uint8_t mainmode, uint8_t awtype,
                         sfpp::FilterModel model, sfpp::ModelConfig config)
    {
        assert(filtindex < pendingInsertConfs.size());
        pendingInsertConfs[filtindex] = {true, mainmode, awtype, model, config};
    }
    void calculate_ambisonic_coeffs(float *destarray, float azimuth, float elevation)
    {
        float x = 0.0;
        float y = 0.0;
        float z = 0.0;
        sphericalToCartesian(azimuth, elevation, x, y, z);
    }
    void update_ambisonic_coeffs()
    {
        // float azi0 = degreesToRadians(used_azi0);
        // float azi1 = degreesToRadians(used_azi1);
        // float ele = degreesToRadians(used_ele0);
        /*
        calculate_ambisonic_coeffs(ambcoeffs.data(), azi0, ele);
        calculate_ambisonic_coeffs(ambcoeffs.data() + 64, azi1, ele);
        if (ambisonic_order == 1)
            SHEval1(x, y, z, coeffdata);
        else if (ambisonic_order == 2)
            SHEval2(x, y, z, coeffdata);
        else if (ambisonic_order == 3)
            SHEval3(x, y, z, coeffdata);
        else if (ambisonic_order == 4)
            SHEval4(x, y, z, coeffdata);
        if (doambnormalization)
        {
            for (int i = 0; i < num_outputchans; ++i)
                coeffdata[i] *= n3d2sn3d[i];
        }
        */
    }
    float fmpitch = 0.0f;
    float fmmodamount = 0.0f;
    float fmfeedback = 0.0f;
    float omniboostinverse = 1.0f;

    enum MODTARGET
    {
        MT_PITCH = 0,
        MT_VOLUME,
        MT_FMDEPTH,
        MT_FMPITCH,
        MT_AZIMUTH,
        MT_ELEVATION,
        MT_INSERTASTART,
        MT_INSERTBSTART = MT_INSERTASTART + 10,
        NUMMODTARGETS = MT_INSERTBSTART + 10,
    };
    std::string get_mod_target_name(MODTARGET target)
    {
        if (target == MT_PITCH)
            return "PITCH";
        else if (target == MT_VOLUME)
            return "VOLUME";
        else if (target == MT_FMDEPTH)
            return "FM DEPTH";
        else if (target == MT_FMPITCH)
            return "FM PITCH";
        else if (target == MT_AZIMUTH)
            return "AZIMUTH (not implemented yet)";
        else if (target == MT_ELEVATION)
            return "ELEVATION (not implemented yet)";
        else if (target >= MT_INSERTASTART && target < NUMMODTARGETS)
        {
            int i = target - MT_INSERTASTART;
            int whichinsert = i / 10;
            int whichparam = i % 10;
            return fmt::format("INSERT {} PAR {}", char('A' + whichinsert),
                               insert_fx[whichinsert].getParameterName(whichparam));
        }
        return "error";
    }
    static void process_mod_matrix(
        double normphase, std::span<float> auxenvparams,
        std::array<ModSlot, GrainEvent::max_grain_mod_slots> &mod_slots,
        std::array<SimpleEnvelope, GranulatorVoice::num_aux_envelopes> &aux_envelopes,
        std::span<float> targetmodvalues)
    {
        assert(auxenvparams.size() == 12);
        alignas(16) float aux_env_values[4] = {0.0f};
        for (size_t i = 0; i < num_aux_envelopes; ++i)
        {
            float shiftedphase = wrap_value(0.0, normphase + auxenvparams[i + 4], 1.0);
            float val = aux_envelopes[i].get_value(shiftedphase, auxenvparams[i]);
            val = std::clamp(val, -1.0f, 1.0f);
            float valueshape = auxenvparams[i + 8];
            if (valueshape >= 0.0f)
            {
                valueshape = 1.0f + 3.0f * valueshape;
                if (val >= 0.0f)
                    val = std::pow(val, valueshape);
                else
                    val = -std::pow(-val, valueshape);
            }
            else
            {
                valueshape = 1.0f + 3.0f * -valueshape;
                if (val >= 0.0f)
                    val = 1.0f - std::pow(1.0f - val, valueshape);
                else
                    val = -1.0f + std::pow(val + 1.0f, valueshape);
            }
            aux_env_values[i] = val;
        }

        for (const auto &e : mod_slots)
        {
            if (e.source_id < CLAP_INVALID_ID && e.target_id < CLAP_INVALID_ID)
            {
                targetmodvalues[e.target_id] += aux_env_values[e.source_id] * e.depth;
            }
        }
        for (auto &e : targetmodvalues)
        {
            e = std::clamp(e, -1.0f, 1.0f);
        }
    }
};

using events_t = std::vector<GrainEvent>;

inline double calculate_inverse(double y, double a, double b, double d)
{
    // Ensure we don't divide by zero
    if (b == 0)
        return 0;

    double val = (y - a) / b;

    // Clamp to [0, 1] because we know x must be in that
    // range
    if (val < 0)
        val = 0;
    if (val > 1)
        val = 1;

    return pow(val, 1.0 / d);
}

struct CloudEvent
{
    double time_position = 0.0;
    struct ParamChange
    {
        uint32_t id = CLAP_INVALID_ID;
        float value = 0.0f;
    };
    static constexpr size_t max_param_changes = 16;
    std::array<ParamChange, max_param_changes> param_modulations;
    bool operator<(const CloudEvent &other) const { return time_position < other.time_position; }
};

struct Cloud
{
    Cloud() { events.reserve(128); }
    double time_position = 0.0;
    double duration = 0.0;
    bool looping = false;
    uint32_t after_touch_dest = CLAP_INVALID_ID;
    std::vector<CloudEvent> events;
};

struct CloudPlayer
{
    Cloud *cloud = nullptr;
    bool active = false;
    int event_index = -1;
    double start_time = 0.0;
    int id = -1;
    float after_touch_amount = 0.0f;
    void start(double time, int idarg, Cloud *c)
    {
        cloud = c;
        if (cloud->events.size() == 0)
            return;
        id = idarg;
        start_time = time;
        // std::cout << "starting cloud " << (void *)c << "\n";
        active = true;
        event_index = 0;
    }
};

enum StateIgnoreFlags
{
    SIF_MASTERVOLUME = 1 << 0,
    SIF_AMBISONICORDER = 1 << 1,
    SIF_MODULATIONROUTINGS = 1 << 2,
    SIF_DASHBOARDSETTING = 1 << 3,
    SIF_MIDIBINDINGS = 1 << 4
};

class ToneGranulator
{
  public:
    ToneGranulator();
    void create_voices();
    void set_state(choc::value::ValueView state);
    void set_filter(int which, uint8_t mainmode, uint8_t awtype, sfpp::FilterModel mo,
                    sfpp::ModelConfig conf);
    void prepare(float samplerate, int filter_routing, float tail_len, float tail_fade_len);
    void sum_voices(std::span<float> outputbuffer)
    {
        alignas(32) float ambiofadebuf[granul_block_size];
        for (int i = 0; i < granul_block_size; ++i)
            ambiofadebuf[i] = fadeForLargeStateChange.step();
        alignas(32) float mixsum[granul_block_size][64];
        for (int i = 0; i < num_out_chans; ++i)
        {
            for (int j = 0; j < granul_block_size; ++j)
            {
                mixsum[j][i] = 0.0f;
            }
        }

        int numactive = 0;
        alignas(32) float voiceout[64 * granul_block_size];
// #define USE_AVX_SUMMING
#ifdef USE_AVX_SUMMING
        for (size_t j = 0; j < voices.size(); ++j)
        {
            if (voices[j]->active)
            {
                ++numactive;
                voices[j]->process<true>(voiceout, granul_block_size);
                for (int k = 0; k < granul_block_size; ++k)
                {
                    float *src = &voiceout[64 * k];
                    float *dst = &mixsum[k][0];

                    int chan = 0;
                    for (; chan <= num_out_chans - 8; chan += 8)
                    {
                        _mm256_store_ps(&dst[chan], _mm256_add_ps(_mm256_load_ps(&dst[chan]),
                                                                  _mm256_load_ps(&src[chan])));
                    }
                    for (; chan < num_out_chans; ++chan)
                        dst[chan] += src[chan];
                }
            }
        }
#else

        for (size_t j = 0; j < voices.size(); ++j)
        {
            if (voices[j]->active)
            {
                ++numactive;
                voices[j]->process(voiceout);
                for (int k = 0; k < granul_block_size; ++k)
                {
                    for (int chan = 0; chan < num_out_chans; ++chan)
                    {
                        mixsum[k][chan] += voiceout[64 * k + chan];
                    }
                }
            }
        }
#endif
        double compengain = 1.0;
        if (numactive > 0)
            compengain = 1.0 / std::sqrt(numactive);
        float maingain =
            modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_MAINVOLUME});
        maingain = std::clamp(maingain, -96.0f, 0.0f);
        maingain = xenakios::decibelsToGain(maingain);
        gainlag.setTarget(compengain * maingain);

        for (int k = 0; k < granul_block_size; ++k)
        {
            gainlag.process();
            float gain = gainlag.getValue();
            float safefadegain = ambiofadebuf[k]; // fadeForLargeStateChange.step();
            gain *= safefadegain;
            for (int chan = 0; chan < num_out_chans; ++chan)
            {
                outputbuffer[k * num_out_chans + chan] = mixsum[k][chan] * gain;
            }
        }
        compensationgainforgui = gainlag.getValue();
    }
    void generate_grain();
    void process_modulations()
    {
        float master_rate =
            modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_MASTERLFORATE});
        master_rate = std::clamp(master_rate, -7.0f, 7.0f);
        for (uint32_t i = 0; i < modmatrix.numLfos; ++i)
        {
            float shift = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{(int)(PAR_LFOSHIFTS + i)});
            modmatrix.m_lfos[i]->applyPhaseOffset(shift);
            float rate = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{(int)(PAR_LFORATES + i)});
            bool ismastersynced = *idtoparvalptr[PAR_LFOMASTERSYNCS + i];
            if (ismastersynced)
                rate = std::clamp(rate + master_rate, -7.0f, 7.0f);
            else
                rate = std::clamp(rate, -7.0f, 7.0f);
            float deform = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{(int)(PAR_LFODEFORMS + i)});
            float warp = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{(int)(PAR_LFOWARPS + i)});
            int shape = *idtoparvalptr[PAR_LFOSHAPES + i];
            shape = shapeParToActualShape[shape];
            modmatrix.m_lfos[i]->process_block(rate, deform, shape, false, 1.0f, warp);
            bool unipolar = (*idtoparvalptr[PAR_LFOUNIPOLARS + i]) > 0.5f;
            if (!unipolar)
                modSourceValues[LFO0 + i] = modmatrix.m_lfos[i]->outputBlock[0];
            else
                modSourceValues[LFO0 + i] = (modmatrix.m_lfos[i]->outputBlock[0] + 1.0f) * 0.5f;
        }
        for (size_t i = 0; i < stepModSources.size(); ++i)
            modSourceValues[STEPS0 + i] = stepModValues[i];
        for (size_t i = 0; i < randomModSources.size(); ++i)
            modSourceValues[RANDOM0 + i] = randomModValues[i];
        modSourceValues[MIDINOTE] = midiNoteModValue;
        for (int i = 0; i < GranulatorModConfig::FixedMatrixSize; ++i)
        {
            if (modmatrix.rt.routes[i].active && modmatrix.rt.routes[i].target)
            {
                float modrange = modRanges[modmatrix.rt.routes[i].target->target];
                modmatrix.rt.updateDepthAt(i, *idtoparvalptr[PAR_MAINMODDEPTHSTART + i] * modrange);
            }
        }
        modmatrix.m.process();
        if (audiocallbackcount % 128 == 0)
        {
            GrainRepeatsVisMessage repmsg;
            repmsg.modulatedvalues[0] = modmatrix.m.getTargetValue({PAR_STACKCOUNT});
            repmsg.modulatedvalues[1] = modmatrix.m.getTargetValue({PAR_STACKTIMESPAN});
            repmsg.modulatedvalues[2] = modmatrix.m.getTargetValue({PAR_STACKTIMECURVE});
            repmsg.modulatedvalues[3] = modmatrix.m.getTargetValue({PAR_STACKPITCHPAR1});
            repmsg.modulatedvalues[4] = modmatrix.m.getTargetValue({PAR_STACKSPATIALIZATIONPAR1});
            repmsg.modulatedvalues[5] = modmatrix.m.getTargetValue({PAR_STACKVOLUMEPAR1});
            repeatsVisMessages.push(repmsg);
            GrainEnvelopeVisMessage msg;
            for (int i = 0; i < GrainEvent::max_grain_mod_slots; ++i)
            {
                msg.moddepths[i] =
                    modmatrix.m.getTargetValue({ToneGranulator::PAR_GRAINMODSLOTAMOUNT0 + i});
                msg.modsources[i] = voices[0]->modulation_slots[i].source_id;
                msg.modtargets[i] = voices[0]->modulation_slots[i].target_id;
            }
            for (int i = 0; i < 4; ++i)
            {
                msg.auxenvparams[i] =
                    modmatrix.m.getTargetValue({ToneGranulator::PAR_AUXENVTIMEWARP + i});
                msg.auxenvparams[4 + i] =
                    modmatrix.m.getTargetValue({ToneGranulator::PAR_AUXENVTIMESHIFT + i});
                msg.auxenvparams[8 + i] =
                    modmatrix.m.getTargetValue({ToneGranulator::PAR_AUXENVVALUEWARP + i});
            }
            msg.curvemorph = modmatrix.m.getTargetValue({ToneGranulator::PAR_ENVMORPH});
            msg.startcurve = *idtoparvalptr[ToneGranulator::PAR_VOLENVEASINGSTART];
            msg.endcurve = *idtoparvalptr[ToneGranulator::PAR_VOLENVEASINGEND];
            msg.grainvolume = modmatrix.m.getTargetValue({ToneGranulator::PAR_GRAINVOLUME});
            gevisfifo.push(msg);
        }
    }
    void process_parameter_lags()
    {
        for (auto &lagp : lagged_params)
        {
            assert(lagp.valptr);
            *lagp.valptr = lagp.lagger.getValue();
            if (audiocallbackcount % 128 == 0)
            {
                params_to_gui_fifo.push(ParameterMessage{lagp.id, *lagp.valptr});
            }
            lagp.lagger.process();
            if (!lagp.lagger.isActive())
            {
                params_to_gui_fifo.push(ParameterMessage{lagp.id, *lagp.valptr});
                lagp.id = CLAP_INVALID_ID;
                lagp.valptr = nullptr;
            }
        }
        lagged_params_spinlock.lock();
        std::erase_if(lagged_params,
                      [](LaggedParameter &lagp) { return lagp.id == CLAP_INVALID_ID; });
        lagged_params_spinlock.unlock();
    }
    void process_block(std::span<float> outputbuffer)
    {
        assert(outputbuffer.size() == granul_block_size * 64);
        // this will be contended only infrequently,
        // but we should not generally depend on being able to use this lock...
        std::lock_guard<choc::threading::SpinLock> locker(spinLock);
        set_ambisonics_order(1 + *idtoparvalptr[PAR_AMBORDER]);

        float taillen = *idtoparvalptr[PAR_GRAINTAIL];
        taillen = 0.002 + 0.998 * std::pow(taillen, 3.0);
        process_parameter_lags();

        handleStepSequencerMessages();
        handleRandomSourceMessages();
        bool self_generate = true;
        // if (events.size() == 0)
        //     self_generate = true;
        //  bool doambcoeffsnormalization = *idtoparvalptr[PAR_AMBUSENORMALIZATION];
        process_modulations();
        for (int i = 0; i < numPitchBandAttens; ++i)
        {
            pitchBandAttensShared[i] = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{PAR_PITCHBANDGAIN0 + i * 10});
        }
        pitchBandAttensShared[numPitchBandAttens] = pitchBandAttensShared[numPitchBandAttens - 1];
        if (modulatedParamToStore.load())
        {
            modulatedParValueForGUI.store(modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{(int)modulatedParamToStore.load()}));
        }

        if (!self_generate)
        {
            advanceCloudPlayers();
            // advanceFullEventList();
        }
        else
        {
            advanceAutoGeneratedGrains(taillen);
        }
        sum_voices(outputbuffer);
        if (masterHighPassFilter.numactivechannels > 0)
        {
            for (int foo = 1; foo < masterHighPassFilter.numactivechannels; ++foo)
            {
                // masterHighPassFilter.cutoffmapping[foo] = 1;
            }
            // masterHighPassFilter.set_cutoff_mapped(1, -48.0f);
            masterHighPassFilter.set_cutoff(modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{PAR_MASTERHIGHPASSCUTOFF}));
            masterHighPassFilter.process(outputbuffer);
        }

        playposframes += granul_block_size;

        int voicesused = 0;
        for (auto &v : voices)
        {
            if (v->active)
                ++voicesused;
        }
        numVoicesUsed = voicesused;
        ++audiocallbackcount;
    }
    enum PARAMS
    {
        PAR_MAINVOLUME = 100,
        PAR_PAUSEAUTOGEN = 105,
        PAR_AMBORDER = 200,
        PAR_OSCTYPE = 300,
        PAR_DENSITY = 400,
        PAR_PITCH = 500,
        PAR_QUANTIZEPITCH = 550,
        PAR_AZIMUTH = 600,
        PAR_ELEVATION = 700,
        PAR_AMBSPREAD = 710,
        PAR_AMBROTATE = 720,
        PAR_AMBOMNIBOOST = 730,
        PAR_DURATION = 800,
        PAR_GRAINTAIL = 850,
        PAR_INSERTAFIRST = 900,
        PAR_INSERTBFIRST = PAR_INSERTAFIRST + 32,
        PAR_INSERTCFIRST = PAR_INSERTBFIRST + 32,
        PAR_INSERTDFIRST = PAR_INSERTCFIRST + 32,
        PAR_FMPITCH = 1500,
        PAR_FMPITCHFOLLOWSMAINPITCH = 1550,
        PAR_FMDEPTH = 1600,
        PAR_FMFEEDBACK = 1700,
        PAR_OSC_SYNC = 1800,
        PAR_OSC_PW = 1850,
        PAR_ENVMORPH = 1900,
        PAR_GRAINVOLUME = 2000,
        PAR_PITCHBANDGAIN0 = 2010,
        PAR_PITCHBANDGAIN1 = 2020,
        PAR_PITCHBANDGAIN2 = 2030,
        PAR_PITCHBANDGAIN3 = 2040,
        PAR_PITCHBANDGAIN4 = 2050,
        PAR_PITCHBANDGAIN5 = 2060,
        PAR_PITCHBANDGAIN6 = 2070,
        PAR_NOISECORRELATION = 2100,
        PAR_NOISEMODE = 2150,
        PAR_STACKCOUNT = 2300,
        PAR_STACKTIMESPAN = 2400,
        PAR_STACKPITCHPAR1 = 2500,
        PAR_STACKSPATIALIZATIONPAR1 = 2600,
        PAR_STACKSPATIALIZATIONPAR2 = 2610,
        PAR_STACKTIMECURVE = 2700,
        PAR_STACKVOLUMEPAR1 = 2750,
        PAR_STACKDURATIONPAR1 = 2760,
        PAR_VOLENVEASINGSTART = 2800,
        PAR_VOLENVEASINGEND = 2900,
        PAR_GRAINMODSLOTAMOUNT0 = 3000,
        PAR_GRAINMODSLOTAMOUNT1 = 3001,
        PAR_GRAINMODSLOTAMOUNT2 = 3002,
        PAR_GRAINMODSLOTAMOUNT3 = 3003,
        PAR_AUXENVTIMEWARP = 3050,
        PAR_AUXENVTIMESHIFT = 3054,
        PAR_AUXENVVALUEWARP = 3058,
        PAR_MASTERHIGHPASSCUTOFF = 3100,
        PAR_MASTERLFORATE = 4000,
        PAR_MAINMODDEPTHSTART = 5000,
        PAR_LFORATES = 100000,
        PAR_LFODEFORMS = 100100,
        PAR_LFOSHIFTS = 100200,
        PAR_LFOWARPS = 100300,
        PAR_LFOSHAPES = 100400,
        PAR_LFOUNIPOLARS = 100500,
        PAR_LFOMASTERSYNCS = 100600
    };
    enum REPEATSPATTERN
    {
        RPP_RAMP,
        RPP_RANDOMWALK,
        RPP_RANDOM,
        RPP_ALTERNATE,
        RPP_EXPAMD
    };

    static constexpr size_t max_num_repeats = 16;
    struct RepeatsParameters
    {
        int count = 1;
        REPEATSPATTERN mode = REPEATSPATTERN::RPP_RAMP;
        float par0 = 0.0f;
        float par1 = 0.0f;
    };
    static std::array<float, max_num_repeats> process_repeats(xenakios::Xoroshiro128Plus &rng,
                                                              RepeatsParameters parameters)
    {
        std::array<float, max_num_repeats> result = {0.0f};
        if (parameters.count == 1)
        {
            result[0] = 0.0f;
            return result;
        }
        if (parameters.mode == RPP_RAMP)
        {
            for (int i = 0; i < parameters.count; ++i)
            {
                float start = parameters.par0;
                float end = parameters.par1;
                result[i] = start + (end - start) / (parameters.count - 1) * i;
            }
        }
        else if (parameters.mode == RPP_ALTERNATE)
        {
            for (int i = 0; i < parameters.count; ++i)
            {
                if (i % 2 == 0)
                    result[i] = parameters.par0;
                else
                    result[i] = parameters.par1;
            }
        }
        else if (parameters.mode == RPP_RANDOMWALK)
        {
            float y = 0.0f;
            for (int i = 0; i < parameters.count; ++i)
            {
                result[i] = y;
                y += rng.nextHypCos(0.0f, parameters.par0);
            }
        }
        else if (parameters.mode == RPP_RANDOM)
        {
            for (int i = 0; i < parameters.count; ++i)
            {
                result[i] = rng.nextFloatInRange(parameters.par0, parameters.par1);
            }
        }
        else if (parameters.mode == RPP_EXPAMD)
        {
            float x = 0.0f;
            for (int i = 0; i < parameters.count; ++i)
            {
                if (i % 2 == 0)
                    x = -1.0f;
                else
                    x = 1.0f;
                float a = 1.0 / (parameters.count - 1) * i;
                result[i] = parameters.par0 * x * a;
            }
        }
        return result;
    }
    static std::array<float, max_num_repeats>
    process_pitch_repeats(xenakios::Xoroshiro128Plus &rng, RepeatsParameters pars, float basevalue)
    {
        RepeatsParameters parameters = pars;
        parameters.par0 = -pars.par0;
        parameters.par1 = pars.par0;
        auto result = process_repeats(rng, parameters);
        float scale = 12.0f;
        if (pars.mode == RPP_RANDOMWALK)
            scale = 6.0f;
        for (auto &e : result)
        {
            e = std::clamp(e * scale, -24.0f, 24.0f);
            e = basevalue + e;
        }
        return result;
    }
    static std::array<float, max_num_repeats>
    process_spatial_repeats(xenakios::Xoroshiro128Plus &rng, RepeatsParameters pars,
                            float basevalue)
    {
        RepeatsParameters parameters = pars;
        parameters.par0 = -pars.par0;
        parameters.par1 = pars.par0;
        auto result = process_repeats(rng, parameters);
        for (auto &e : result)
        {
            e = basevalue + e * 180.0f;
        }
        return result;
    }
    static std::array<float, max_num_repeats>
    process_duration_repeats(xenakios::Xoroshiro128Plus &rng, RepeatsParameters pars,
                             float basevalue)
    {
        RepeatsParameters parameters = pars;
        parameters.par0 = -pars.par0;
        parameters.par1 = pars.par0;
        auto result = process_repeats(rng, parameters);
        for (auto &e : result)
        {
            e = basevalue + e * 0.5f;
        }
        return result;
    }
    static std::array<float, max_num_repeats>
    process_volume_repeats(xenakios::Xoroshiro128Plus &rng, RepeatsParameters pars, float gvol)
    {
        RepeatsParameters parameters = pars;
        if (pars.mode == RPP_RAMP)
        {
            if (pars.par0 < 0.0f)
            {
                parameters.par0 = gvol * (pars.par0 + 1.0f);
                parameters.par1 = gvol;
            }
            else
            {
                parameters.par0 = gvol;
                parameters.par1 = gvol - gvol * (pars.par0) * 1.0f;
            }
        }
        auto result = process_repeats(rng, parameters);
        return result;
    }
    std::atomic<REPEATSPATTERN> pitch_repeats_mode = RPP_RAMP;
    std::atomic<REPEATSPATTERN> azimuth_repeats_mode = RPP_RAMP;
    std::atomic<REPEATSPATTERN> elevation_repeats_mode = RPP_RAMP;
    std::atomic<REPEATSPATTERN> volume_repeats_mode = RPP_RAMP;
    const int numvoices = 64;
    double m_sr = 0.0;
    int graincount = 0;
    std::atomic<int> modulatedOscType{-1};
    std::vector<std::unique_ptr<GranulatorVoice>> voices;
    events_t events;
    events_t scheduledGrains;
    std::vector<Cloud> clouds;
    std::array<CloudPlayer, 8> cloudPlayers;
    alignas(16) int scheduledIndex = 0;
    int evindex = 0;
    int playposframes = 0;
    int num_out_chans = 0;
    int missedgrains = 0;
    // ideally we would not use a lock at all but it looks like it's cleaner
    // to revert to using that for some things
    alignas(16) choc::threading::SpinLock spinLock;
    alignas(16) double graingen_phase = 0.0;
    alignas(16) double graingen_phase_prior = 2.0;
    alignas(16) sst::basic_blocks::dsp::OnePoleLag<float, true> gainlag;
    alignas(16) xenakios::Xoroshiro128Plus rng;
    alignas(32) GranulatorModMatrix modmatrix;
    alignas(16) FilterBank masterHighPassFilter;
    std::atomic<float> compensationgainforgui{0.0f};
    using pmd = sst::basic_blocks::params::ParamMetaData;
    std::vector<pmd> parmetadatas;
    std::vector<float> paramvalues;
    struct LaggedParameter
    {
        uint32_t id = CLAP_INVALID_ID;
        float *valptr = nullptr;
        sst::basic_blocks::dsp::LinearLag<double, false> lagger;
    };
    std::vector<LaggedParameter> lagged_params;
    struct DummyLock
    {
        void lock() {};
        void unlock() {};
    };
    choc::threading::SpinLock lagged_params_spinlock;

  private:
    std::unordered_map<uint32_t, float *> idtoparvalptr;

  public:
    float get_parameter_value(uint32_t id) { return *idtoparvalptr[id]; }
    static constexpr bool lag_parameters = false;
    void set_parameter_value(uint32_t id, float value);
    std::unordered_map<uint32_t, pmd *> idtoparmetadata;
    std::unordered_map<uint32_t, float> modRanges;
    std::unordered_map<int, int> shapeParToActualShape;
    choc::fifo::SingleReaderSingleWriterFIFO<StepModSource::Message> fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<ParameterMessage> params_to_gui_fifo;
    alignas(16) Tunings::Tuning tuning;
    struct GrainRepeatsVisMessage
    {
        std::array<float, 6> modulatedvalues = {0.0f};
        bool operator==(const GrainRepeatsVisMessage &other) const
        {
            return modulatedvalues == other.modulatedvalues;
        }
    };
    choc::fifo::SingleReaderSingleWriterFIFO<GrainRepeatsVisMessage> repeatsVisMessages;
    struct GrainEnvelopeVisMessage
    {
        std::array<float, GrainEvent::max_grain_mod_slots> moddepths;
        std::array<uint32_t, GrainEvent::max_grain_mod_slots> modsources;
        std::array<uint32_t, GrainEvent::max_grain_mod_slots> modtargets;
        std::array<float, GranulatorVoice::num_aux_envelopes * 3> auxenvparams;
        float grainvolume = 0.0f;
        float curvemorph = 0.0f;
        float startcurve = 0.0f;
        float endcurve = 0.0f;
    };
    int audiocallbackcount = 0;
    choc::fifo::SingleReaderSingleWriterFIFO<GrainEnvelopeVisMessage> gevisfifo;
    struct GrainVisualizerMessage
    {
        double timepos = 0.0;
        float pitch = 0.0;
        float duration = 0.001;
        float gain = 0.0f;
        std::array<float, 4> ambidegrees = {0.0f};
        float visualfade = 1.0f;
    };
    struct GrainVisualizerSettings
    {
        double timespantoshow = 8.0;
    };
    GrainVisualizerSettings gvsettings;
    std::atomic<bool> gatherGrainVisData{false};
    choc::fifo::SingleReaderSingleWriterFIFO<GrainVisualizerMessage> visualizer_fifo;

    enum SI
    {
        NOSOURCE,
        LFO0,
        LFO1,
        LFO2,
        LFO3,
        LFO4,
        LFO5,
        LFO6,
        LFO7,
        STEPS0,
        STEPS1,
        STEPS2,
        STEPS3,
        STEPS4,
        STEPS5,
        STEPS6,
        STEPS7,
        RANDOM0,
        RANDOM1,
        RANDOM2,
        RANDOM3,
        AA_CENTROID,
        AA_SPEAD,
        AA_LEVEL,
        HOSTPARAMSTART,
        MIDINOTE = HOSTPARAMSTART + 16,
        MIDIVELO,
        MIDIAT,
        MIDICCSTART,
        MIDICCEND = MIDICCSTART + 128,

    };
    float dummyTargetValue = 0.0f;

    alignas(32) std::array<float, 8> stepModValues;
    alignas(32) std::array<StepModSource, 8> stepModSources;
    alignas(32) std::array<float, 8> randomModValues;
    alignas(32) std::array<TriggeredRandomSource, 4> randomModSources{1001, 1007, 5543, 90001};
    choc::fifo::SingleReaderSingleWriterFIFO<TriggeredRandomSource::Message> trngFifo;
    alignas(32) MidiNoteModSource midiNoteModSource;
    float midiNoteModValue = 0.0f;
    // we can share these between voices as we don't need them stateful, at least for now
    alignas(32) std::array<SimpleEnvelope, GranulatorVoice::num_aux_envelopes> voiceaux_envelopes;
    alignas(16) std::array<float, numPitchBandAttens + 5> pitchBandAttensShared;
    alignas(16) std::array<int, 7> osctypemapping;
    struct ModSourceInfo
    {
        std::string name;
        std::string groupname;
        GranulatorModConfig::SourceIdentifier id;
        float val = 0.0f;
    };
    std::vector<ModSourceInfo> modSourceInfos;
    alignas(16) std::array<float, 256> modSourceValues;
    std::unordered_map<int, int> midiCCMap;
    alignas(16) std::atomic<int> numVoicesUsed;
    void set_aux_envelope_interpolation_mode(int envindex, int m)
    {
        voiceaux_envelopes[envindex].interpmode = m;
    }
    int get_aux_envelope_interpolation_mode(int envindex) const
    {
        return voiceaux_envelopes[envindex].interpmode;
    }
    void handleRandomSourceMessages()
    {
        TriggeredRandomSource::Message rmsg;
        while (trngFifo.pop(rmsg))
        {
            auto &dest = randomModSources[rmsg.dest];
            if (rmsg.opcode == TriggeredRandomSource::Message::OP_DISTRIBUTION)
            {
                dest.set_distribution(static_cast<TriggeredRandomSource::Distribution>(rmsg.ival));
            }
            else if (rmsg.opcode == TriggeredRandomSource::Message::OP_LIMIT)
            {
                dest.limit_mode = static_cast<TriggeredRandomSource::Limiting>(rmsg.ival);
                dest.state_dirty = true;
            }
            else if (rmsg.opcode == TriggeredRandomSource::Message::OP_PARAM)
            {
                dest.parameter_values[rmsg.ival] = rmsg.fval;
            }
        }
    }
    void handleStepSequencerMessages()
    {
        StepModSource::Message msg;
        while (fifo.pop(msg))
        {
            if (msg.dest >= 1000 && msg.dest < 1000 + GranulatorVoice::num_aux_envelopes &&
                msg.opcode == StepModSource::Message::OP_SETSTEP)
            {
                const auto numsteps = SimpleEnvelope::maxnumsteps;
                int envindex = msg.dest - 1000;
                voiceaux_envelopes[envindex].set_step(msg.ival0, msg.fval0);
            }
            if (msg.dest < stepModSources.size())
            {
                auto &ms = stepModSources[msg.dest];
                if (msg.opcode == StepModSource::Message::OP_UNIPOLAR)
                {
                    ms.unipolar = msg.ival0;
                }
                else if (msg.opcode == StepModSource::Message::OP_OFFSET)
                {
                    ms.loopoffset = msg.ival0;
                }
                else if (msg.opcode == StepModSource::Message::OP_NUMSTEPS)
                    ms.numactivesteps = msg.ival0;
                else if (msg.opcode == StepModSource::Message::OP_LOOPSTART)
                    ms.loopstartstep = msg.ival0;
                else if (msg.opcode == StepModSource::Message::OP_LOOPLEN)
                    ms.looplen = msg.ival0;
                else if (msg.opcode == StepModSource::Message::OP_SETSTEP)
                {
                    if (msg.ival0 >= 0 && msg.ival0 < StepModSource::maxSteps)
                    {
                        ms.steps[msg.ival0] = msg.fval0;
                    }
                }
                else if (msg.opcode == StepModSource::Message::OP_PLAYMODE)
                {
                    ms.playmode = (StepModSource::PlayMode)msg.ival0;
                    ms.playdirection = 1;
                    if (ms.playmode == StepModSource::PM_REVERSELOOP)
                        ms.playdirection = -1;
                }
                if (ms.curstep < ms.loopstartstep || ms.curstep > ms.loopstartstep + ms.looplen)
                {
                    ms.curstep = ms.loopstartstep;
                    ms.curstepforgui = ms.curstep;
                }
            }
        }
    }

    std::atomic<int> currentSnapShot{-1};
    std::atomic<int64_t> current_preset_id = -1;
    struct RampDownUp
    {
        int pos = -1;
        int len = 0;
        int middlepos = 0;
        double curvalue = 0.0;
        double delta = 0.0;
        std::function<void(void)> callback;
        void start(float samplerate, float lenms, std::function<void(void)> action_at_zero)
        {
            pos = 0;
            len = samplerate * (lenms / 1000.0);
            middlepos = len / 2;
            curvalue = 1.0;
            delta = 1.0 / middlepos;
            callback = std::move(action_at_zero);
        }
        float step()
        {
            if (pos == -1)
                return 1.0f;
            if (pos == middlepos && callback)
                callback();
            float result = curvalue;
            if (pos >= 0 && pos < middlepos)
                curvalue -= delta;
            else
                curvalue += delta;
            ++pos;
            if (pos == len)
                pos = -1;
            result = std::clamp(result, 0.0f, 1.0f);
            return result * result;
        }
        bool is_active() const { return pos >= 0; }
    };
    RampDownUp fadeForLargeStateChange;

    void set_oscillator_type_mapping(std::span<int> mapping);
    std::string currentScalaFile;
    std::string load_scala_string(std::string scalatext, std::string scalapath,
                                  bool called_from_audio_thread);
    std::string load_scala_file(std::string path, bool called_from_audio_thread);

    void set_grain_modulation_routing(int slot, std::optional<uint32_t> source,
                                      std::optional<uint32_t> destination,
                                      bool calledfromaudiothread)
    {
        if (slot < 0 || slot >= GrainEvent::max_grain_mod_slots)
            return;
        if (!calledfromaudiothread)
            spinLock.lock();
        for (auto &v : voices)
        {
            if (source)
                v->modulation_slots[slot].source_id = *source;
            if (destination)
                v->modulation_slots[slot].target_id = *destination;
        }
        if (!calledfromaudiothread)
            spinLock.unlock();
    }
    const std::unordered_map<int, std::string> oscTypeToStringMap{
        {0, "SINE"},   {1, "SEMISINE"}, {2, "TRIANGLE"}, {3, "SAW"},
        {4, "SQUARE"}, {5, "FM"},       {6, "NOISE"}};

    void reset_lfos()
    {
        for (size_t i = 0; i < modmatrix.m_lfos.size(); ++i)
        {
            int shape = *idtoparvalptr[PAR_LFOSHAPES + i];
            modmatrix.m_lfos[i]->attack(shape);
        }
    }
    void reset_step_sequencers()
    {
        for (auto &ss : stepModSources)
        {
            ss.reset();
        }
    }
    std::array<GranulatorVoice::PendingInsertConf, 2> currentInsertConfs;
    alignas(32) EasingLUTS eluts;
    // this is not super ideal but maybe we can live with this...
    GrainInsertFX fxInstanceForMetadata;

    int current_ambisonic_order = 0;
    int pending_ambisonic_order = 0;
    void set_event_list(events_t evlist);

    void set_ambisonics_order(int order);

    std::atomic<float> auxenvdepthpmodulated = 0.0f;
    std::atomic<uint32_t> modulatedParamToStore{0};
    std::atomic<float> modulatedParValueForGUI{0.0f};

    // if triggered by MIDI, the MIDI key is the effective id
    void start_cloud(int cloudindex, int with_id)
    {
        if (cloudindex >= 0 && cloudindex < clouds.size())
        {
            for (auto &p : cloudPlayers)
            {
                if (!p.active)
                {
                    p.start(playposframes / m_sr, with_id, &clouds[cloudindex]);
                    break;
                }
            }
        }
    }
    void handle_cloud_aftertouch(int id, float value)
    {
        for (auto &p : cloudPlayers)
        {
            if (p.id == id)
            {
                p.after_touch_amount = value;
                // we should not be having the same id in the cloud players,
                // but may need to revisit this
                break;
            }
        }
    }
    void stop_cloud(int id)
    {
        for (auto &p : cloudPlayers)
        {
            if (p.id == id)
            {
                p.active = false;
                p.event_index = -1;
                p.id = -1;
            }
        }
    }
    void advanceCloudPlayers();
    void advanceFullEventList();
    void advanceAutoGeneratedGrains(float taillen);
};
