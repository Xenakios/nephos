#pragma once
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <span>
// #include "sst/basic-blocks/dsp/CorrelatedNoise.h"
#include "clap/id.h"
#include "sst/basic-blocks/dsp/EllipticBlepOscillators.h"
#include <variant>
#include "../Common/xen_ambisonics.h"
#include "../Common/xap_utils.h"
#include "sst/filters++/api.h"
#include "sst/filters++/enums.h"
#include "sst/filters++/model_config.h"
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

using namespace sst::basic_blocks::mod_matrix;

inline constexpr int granul_block_size = 8;
inline constexpr uint8_t maxAmbiSonicOrder = 7;

inline constexpr int ambisonicOrderNumChannels(int order) { return (order + 1) * (order + 1); }

template <typename T> inline int sgn(T val) { return (T(0) < val) - (val < T(0)); }

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
        for (size_t i = 0; i < numactivechannels / 4; ++i)
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
        for (size_t i = 0; i < numactivechannels / 4; ++i)
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
                    int chan = i + j;
                    insamples[j] = buffer[sample * numactivechannels + chan];
                }
                filters[i].processQuadSample(insamples, outsamples);
                for (int j = 0; j < 4; ++j)
                {
                    int chan = i + j;
                    buffer[sample * numactivechannels + chan] = outsamples[j];
                }
            }
        }
        for (size_t i = 0; i < numactivechannels / 4; ++i)
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
        int baz{0};
        // uint32_t nm{};
        int16_t depthPosition{-1};

        bool operator==(const TargetIdentifier &other) const
        {
            return baz == other.baz && depthPosition == other.depthPosition;
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
        CURVE_BUTTERFLY
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
    static std::function<float(float)> getCurveOperator(CurveIdentifier id)
    {
        if (id.id >= CURVE_STEPS1 && id.id < CURVE_STEPS1 + 16)
        {
            return [id](auto x) {
                const int numsteps = id.id - CURVE_STEPS1 + 1;
                x = (x + 1.0f) * 0.5;
                x = std::round(x * numsteps) / numsteps;
                return -1.0f + 2.0f * x;
            };
        }
        switch (id.id)
        {
        case CURVE_LINEAR:
            return [](auto x) { return x; };
        case CURVE_SQUARE:
            return [](auto x) { return std::abs(x) * x; };
        case CURVE_CUBE:
            return [](auto x) { return x * x * x; };
        case CURVE_ABS:
            return [](auto x) { return std::abs(x); };
        case CURVE_TOPOWER16:
            return [](auto x) {
                x = std::clamp(x, -1.0f, 1.0f);
                return std::pow(x, 16) * sgn(x);
            };
        case CURVE_EXPSIN1:
            return [](auto x) { return expsin(x, 1, 8.0f); };
        case CURVE_EXPSIN2:
            return [](auto x) { return expsin(x, 2, 12.0f); };
        case CURVE_XOR1:
            return [](auto x) { return xor_curve(x, 13107); };
        case CURVE_XOR2:
            return [](auto x) { return xor_curve(x, 43690); };
        case CURVE_XOR3:
            return [](auto x) { return xor_curve(x, 25027); };
        case CURVE_XOR4:
            return [](auto x) { return xor_curve(x, 10001); };
        case CURVE_BITMIRROR:
            return [](auto x) { return bit_reversal_curve(x); };
        case CURVE_UNIPOLARTOBIPOLAR:
            return [](auto x) { return std::clamp(-1.0f + 2.0f * x, -1.0f, 1.0f); };
        case CURVE_BIPOLARTOUNIPOLAR:
            return [](auto x) { return std::clamp((x + 1.0f) * 0.5f, 0.0f, 1.0f); };
        case CURVE_HARMONICSERIES3OCTAVES:
            return [](auto x) { return harmseries(x, 3); };
        case CURVE_HARMONICSERIES4OCTAVES:
            return [](auto x) { return harmseries(x, 4); };
        case CURVE_HARMONICSERIES5OCTAVES:
            return [](auto x) { return harmseries(x, 5); };
        case CURVE_PEAKING1:
            return [](auto x) { return peaking_curve(x, 0.2f); };
        case CURVE_PEAKING2:
            return [](auto x) { return peaking_curve(x, 0.5f); };
        case CURVE_PEAKING3:
            return [](auto x) { return peaking_curve(x, 1.0f); };
        case CURVE_PEAKING4:
            return [](auto x) { return peaking_curve(x, 2.0f); };
        case CURVE_PEAKING5:
            return [](auto x) { return peaking_curve(x, 3.0f); };
        case CURVE_PEAKING6:
            return [](auto x) { return peaking_curve(x, 4.0f); };
        case CURVE_POPCORN:
            return [](auto x) { return std::floor(std::tanh(x * 5.0) * 10.0) / 10.0; };
        case CURVE_BUTTERFLY:
            return [](auto x) {
                if (x != 0.0f)
                    return std::sin(x * 10.0f) * std::cos(1.0f / x + 0.001f);
                return 0.0f;
            };
        };

        return [](auto x) { return x; };
    }
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
        for (int i = 0; i < 16; ++i)
        {
            int actnumsteps = i + 2;
            result.emplace_back(CURVE_STEPS1 + i, "STEPS",
                                fmt::format("{:02d} Steps", actnumsteps));
        }
        for (int i = 0; i < 4; ++i)
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
        auto h1 = std::hash<int>{}((int)s.baz);
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
    // std::array<GranulatorModConfig::SourceIdentifier, 32> sourceIds;

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
        // sourceIds[0] =
        //     GranulatorModConfig::SourceIdentifier{GranulatorModConfig::SourceIdentifier::NOSOURCE};
        for (size_t i = 0; i < numLfos; ++i)
        {
            auto lfo = std::make_unique<lfo_t>(this);
            m_lfos[i] = std::move(lfo);
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
    int generator_type = 0;
    float volume = 0.0f;
    float auxsend = 0.0f;
    uint8_t envelope_start_type = 0;
    uint8_t envelope_end_type = 0;
    float envelope_shape = 0.5f;
    float auxenvtimewarp = 0.0f;
    float azimuth = 0.0f;
    float ambi_spread = 0.0f;
    float ambi_rotate = 0.0f;
    float elevation = 0.0f;
    float sync_ratio = 1.0f;
    float pulse_width = 0.5f;
    float fm_frequency_hz = 0.0f;
    float fm_amount = 0.0f;
    float fm_feedback = 0.0f;
    float noisecorr = 0.0f;
    float noiseimode = 0.0f;
    static constexpr size_t max_grain_mod_slots = 4;
    float modamounts[max_grain_mod_slots] = {0.0f};
    float insertparams[4][10];
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

template <bool TaperEnabled> struct SimpleEnvelope
{
    static constexpr int maxnumsteps = 16;
    alignas(32) std::array<float, maxnumsteps + 5> steps;
    alignas(16) int curstep = 0;
    alignas(16) double steplen = 0.0;
    alignas(16) double phase = 0.0;
    alignas(16) int taper_phase = 0;
    alignas(16) int taper_len = 0;
    enum InterpolationMode
    {
        IM_NONE,
        IM_LINEAR,
        IM_SPLINE
    };
    int interpmode = IM_SPLINE;
    choc::value::Value getState()
    {
        auto result = choc::value::createObject("stepenvstate");
        result.setMember("interpmode", interpmode);
        auto auxenvsteps = choc::value::createEmptyArray();
        for (auto &v : steps)
        {
            auxenvsteps.addArrayElement(v);
        }
        result.setMember("steps", auxenvsteps);
        return result;
    }
    void setState(choc::value::ValueView state) {}
    SimpleEnvelope()
    {
        std::fill(steps.begin(), steps.end(), 0.0f);
        for (size_t i = 0; i < maxnumsteps; ++i)
        {
            steps[i] = xenakios::mapvalue<float>(i, 0, maxnumsteps - 1, -1.0, 1.0);
        }
    }
    void start(int dursamples)
    {
        taper_phase = 0;
        taper_len = dursamples;
        curstep = 0;
        phase = 0.0;
        steplen = (double)dursamples / (maxnumsteps - 1);
    }
    float get_value(float xpos, float xwarp) const
    {
        xpos = std::clamp(xpos, 0.0f, 1.0f);
        if (xwarp < 0.0f)
        {
            float ex = xenakios::mapvalue(xwarp, -1.0f, 0.0f, 4.0f, 1.0f);
            xpos = std::pow(xpos, ex);
        }
        else
        {
            float ex = xenakios::mapvalue(xwarp, 0.0f, 1.0f, 1.0f, 4.0f);
            xpos = 1.0f - std::pow(1.0f - xpos, ex);
        }
        xpos *= maxnumsteps;
        int index = xpos;
        float y0 = steps[index];
        if (interpmode == IM_NONE)
            return y0;
        float y1 = steps[index + 1];
        float mu = xpos - index;
        if (interpmode == IM_LINEAR)
            return y0 + (y1 - y0) * mu;
        float y2 = steps[index + 2];
        return sst::basic_blocks::dsp::quad_bspline(y0, y1, y2, mu);
    }
    double step()
    {
        double y0 = steps[curstep];
        double y1 = 0.0;
        if (curstep + 1 < maxnumsteps - 1)
        {
            y1 = steps[curstep + 1];
        }
        else
        {
            y1 = steps[maxnumsteps - 1];
        }
        double y2 = y0 + (y1 - y0) / steplen * phase;
        phase += 1.0;
        if (phase >= steplen)
        {
            phase = 0.0;
            ++curstep;
            if (curstep >= maxnumsteps)
                curstep = maxnumsteps - 1;
        }
        if constexpr (TaperEnabled)
        {
            double tapergain = 1.0;
            const int taperfadelen = 32;
            if (taper_phase < taperfadelen)
                tapergain = xenakios::mapvalue<double>(taper_phase, 0, taperfadelen, 0.0, 1.0);
            else if (taper_phase >= taper_len - taperfadelen)
                tapergain = xenakios::mapvalue<double>(taper_phase, taper_len - taperfadelen,
                                                       taper_len, 1.0, 0.0);
            ++taper_phase;
            return y2 * tapergain;
        }
        return y2;
    }
};

constexpr size_t numPitchBandAttens = 7;

class GranulatorVoice
{
  public:
    std::variant<NoiseGen, FMOsc, sst::basic_blocks::dsp::EBApproxSin<>,
                 sst::basic_blocks::dsp::EBApproxSemiSin<>, sst::basic_blocks::dsp::EBTri<>,
                 sst::basic_blocks::dsp::EBSaw<>, sst::basic_blocks::dsp::EBPulse<>>
        theoscillator;

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
    std::array<SimpleEnvelope<false>, num_aux_envelopes> *aux_envelopes = nullptr;
    struct ModSlot
    {
        uint32_t source_id = CLAP_INVALID_ID;
        float depth = 0.0f;
        uint32_t target_id = CLAP_INVALID_ID;
    };
    alignas(16) ModSlot modulation_slots[GrainEvent::max_grain_mod_slots];

    float pitch_base = 0.0f;
    float graingain = 0.0;
    float used_azi0 = 0.0f;
    float used_azi1 = 0.0f;
    float used_ele0 = 0.0f;
    float used_ele1 = 0.0f;
    float auxsend1 = 0.0;
    std::span<float> pitchBandAttens;
    uint8_t envstarttype = 0;
    uint8_t envendtype = 0;
    double envshape = 0.5;
    float auxenvtimewarp = 0.0;
    int grainid = 0;
    bool doambnormalization = false;
    int ambisonic_order = 1;
    int num_outputchans = 0;

    GranulatorVoice()
    {
        std::fill(ambcoeffs.begin(), ambcoeffs.end(), 0.0f);
        for (int i = 0; i < GrainEvent::max_grain_mod_slots; ++i)
            modulation_slots[i] = {0, 0.0f, CLAP_INVALID_ID};
    }
    void set_samplerate(double hz)
    {
        sr = hz;
        for (auto &fx : insert_fx)
            fx.prepareInstance(sr, granul_block_size);
        samplerate_was_changed = true;
    }
    void set_insert_type(size_t filtindex, uint8_t mainmode, uint8_t awtype,
                         sfpp::FilterModel model, sfpp::ModelConfig config)
    {
        GrainInsertFX::ModeInfo gmode;
        gmode.mainmode = mainmode;
        gmode.awtype = awtype;
        gmode.sstmodel = model;
        gmode.sstconfig = config;
        insert_fx[filtindex].setMode(gmode);
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
        float azi0 = degreesToRadians(used_azi0);
        float azi1 = degreesToRadians(used_azi1);
        float ele = degreesToRadians(used_ele0);
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
    void start(GrainEvent &evpars)
    {
        active = true;
        int newosctype = std::clamp(evpars.generator_type, 0, 6);
        assert(osctypemapping.size() == 7);
        newosctype = osctypemapping[newosctype];
        newosctype = std::clamp(newosctype, 0, 6);
        if (newosctype != prior_osc_type)
        {
            prior_osc_type = newosctype;
            if (newosctype == 0)
                theoscillator = sst::basic_blocks::dsp::EBApproxSin<>();
            else if (newosctype == 1)
                theoscillator = sst::basic_blocks::dsp::EBApproxSemiSin<>();
            else if (newosctype == 2)
                theoscillator = sst::basic_blocks::dsp::EBTri<>();
            else if (newosctype == 3)
                theoscillator = sst::basic_blocks::dsp::EBSaw<>();
            else if (newosctype == 4)
                theoscillator = sst::basic_blocks::dsp::EBPulse<>();
            else if (newosctype == 5)
                theoscillator = FMOsc();
            else if (newosctype == 6)
                theoscillator = NoiseGen();
            std::visit(
                [this](auto &q) {
                    q.setSampleRate(sr);
                    q.setFrequencySmoothingRateMS(5.0);
                },
                theoscillator);
        }
        if (samplerate_was_changed)
        {
            samplerate_was_changed = false;
            std::visit(
                [this](auto &q) {
                    q.setSampleRate(sr);
                    q.setFrequencySmoothingRateMS(5.0);
                },
                theoscillator);
        }
        pitch_base = evpars.pitch_semitones;
        if (newosctype == 6)
            pitch_base += 12.0;
        pitch_base = std::clamp(pitch_base, -48.0f, 64.0f);
        auto syncratio = std::clamp(evpars.sync_ratio, 1.0f, 16.0f);
        auto pw = evpars.pulse_width; // osc implementation clamps itself to 0..1
        auto fmhz = evpars.fm_frequency_hz;
        auto fmmodamount = std::clamp(evpars.fm_amount, 0.0f, 1.0f);
        auto logisticr = fmmodamount;
        fmmodamount = std::pow(fmmodamount, 3.0f) * 128.0f;
        auto fmfeedback = std::clamp(evpars.fm_feedback, -1.0f, 1.0f);
        auto noisecorr = std::clamp(evpars.noisecorr, -1.0f, 1.0f);
        auto noisemode = evpars.noiseimode;
        std::visit(
            [syncratio, pw, fmhz, fmfeedback, fmmodamount, noisecorr, noisemode, logisticr,
             this](auto &q) {
                q.reset();
                q.setSyncRatio(syncratio);
                // handle extra parameters of osc types
                if constexpr (std::is_same_v<decltype(q), sst::basic_blocks::dsp::EBPulse<> &>)
                {
                    q.setWidth(pw);
                }
                if constexpr (std::is_same_v<decltype(q), FMOsc &>)
                {
                    q.setModulatorFreq(fmhz);
                    q.setModIndex(fmmodamount);
                    q.setFeedbackAmount(fmfeedback);
                }
                if constexpr (std::is_same_v<decltype(q), NoiseGen &>)
                {
                    q.setRandSeed(grainid);
                    q.setCorrelation(noisecorr);
                    q.imode = noisemode;
                    q.logisticr = 3.4 + logisticr * 0.6;
                    q.logisticx0 = 0.01 + 0.98 * (1.0 / 1024 * (grainid % 1024));
                }
            },
            theoscillator);

        float ambspread = std::clamp(evpars.ambi_spread, -180.0f, 180.0f);
        float ambrotate = std::clamp(evpars.ambi_rotate, -180.0f, 180.0f);
        float xa0 = -ambspread;
        float ya0 = 0.0f;
        float xb0 = ambspread;
        float yb0 = 0.0f;
        float rotrads = degreesToRadians(ambrotate);
        float rotsin = std::sin(rotrads);
        float rotcos = std::cos(rotrads);
        float xa1 = xa0 * rotcos - ya0 * rotsin;
        float ya1 = xa0 * rotsin + ya0 * rotcos;
        float xb1 = xb0 * rotcos - yb0 * rotsin;
        float yb1 = xb0 * rotsin + yb0 * rotcos;
        float azi0 = xa1 + -evpars.azimuth;
        float ele0 = ya1 + evpars.elevation;
        float azi1 = xb1 + -evpars.azimuth;
        float ele1 = yb1 + evpars.elevation;
        azi0 = wrap_value(-180.0f, azi0, 180.0f);
        azi1 = wrap_value(-180.0f, azi1, 180.0f);
        ele0 = wrap_value(-180.0f, ele0, 180.0f);
        ele1 = wrap_value(-180.0f, ele1, 180.0f);
        assert(azi0 >= -180.0f && azi0 <= 180.0f);
        assert(azi1 >= -180.0f && azi1 <= 180.0f);
        assert(ele0 >= -180.0f && ele0 <= 180.0f);
        assert(ele1 >= -180.0f && ele1 <= 180.0f);
        used_azi0 = azi0;
        used_azi1 = azi1;
        used_ele0 = ele0;
        used_ele1 = ele1;
        azi0 = degreesToRadians(azi0);
        azi1 = degreesToRadians(azi1);
        ele0 = degreesToRadians(ele0);
        ele1 = degreesToRadians(ele1);

        auto calc_ambicoeffs = [this](int inchan, float azimuth, float elevation) {
            assert(inchan >= 0 && inchan < 2);

            float x = 0.0;
            float y = 0.0;
            float z = 0.0;
            sphericalToCartesian(azimuth, elevation, x, y, z);
            float *coeffdata = ambcoeffs.data() + inchan * 64;
            if (ambisonic_order == 1)
                SHEval1(x, y, z, coeffdata);
            else if (ambisonic_order == 2)
                SHEval2(x, y, z, coeffdata);
            else if (ambisonic_order == 3)
                SHEval3(x, y, z, coeffdata);
            else if (ambisonic_order == 4)
                SHEval4(x, y, z, coeffdata);
            else if (ambisonic_order == 5)
                SHEval5(x, y, z, coeffdata);
            else if (ambisonic_order == 6)
                SHEval6(x, y, z, coeffdata);
            else if (ambisonic_order == 7)
                SHEval7(x, y, z, coeffdata);
            if (doambnormalization)
            {
                // if we use the actual output channel count, this won't autovectorize
                // but using the constant, it will and will always take 8 steps
                // so we lose a little with the lowest ambisonic orders, but otherwise
                // this works better than using the actual active output channel count
                for (int i = 0; i < 64; ++i)
                    coeffdata[i] *= n3d2sn3d[i];
            }
        };
        calc_ambicoeffs(0, azi0, ele0);
        calc_ambicoeffs(1, azi1, ele1);
        phase = 0;
        float actdur = std::clamp(evpars.duration, 0.0f, 1.0f);
        actdur = actdur * actdur * actdur;
        actdur = 0.002f + 0.498f * actdur;
        grain_end_phase = sr * actdur;

        // aux_envelope.start(grain_end_phase);
        auxenvtimewarp = evpars.auxenvtimewarp;

        for (size_t i = 0; i < 2; ++i)
        {
            insert_fx[i].reset();
            if (insert_fx[i].mainmode == GrainInsertFX::GFXSSTFILTER)
            {
                float filtpitch =
                    xenakios::mapvalue(evpars.insertparams[i][0], 0.0f, 1.0f, -48.0f, 72.0f);
                insert_fx[i].paramvalues[0] = std::clamp(filtpitch - 9.0f, -48.0f, 64.0f);
                insert_fx[i].paramvalues[1] = std::clamp(evpars.insertparams[i][1], 0.0f, 1.0f);
                insert_fx[i].paramvalues[2] = std::clamp(evpars.insertparams[i][2], -1.0f, 1.0f);
                float filtpitchspread =
                    xenakios::mapvalue(evpars.insertparams[i][3], 0.0f, 1.0f, -24.0f, 24.0f);
                insert_fx[i].paramvalues[3] = std::clamp(filtpitchspread, -24.0f, 24.0f);
                insert_fx[i].paramvalues[4] = std::clamp(evpars.insertparams[i][4], 0.0f, 1.0f);
            }
            else if (insert_fx[i].mainmode == GrainInsertFX::GFXAIRWINDOWS)
            {
                for (size_t j = 0; j < insert_fx[i].numParams; ++j)
                {
                    insert_fx[i].paramvalues[j] = evpars.insertparams[i][j];
                }
            }
            else if (insert_fx[i].mainmode == GrainInsertFX::GFXXENAKIOS)
            {
                for (size_t j = 0; j < insert_fx[i].numParams; ++j)
                {
                    insert_fx[i].paramvalues[j] = evpars.insertparams[i][j];
                }
            }
        }
        for (int i = 0; i < GrainEvent::max_grain_mod_slots; ++i)
            modulation_slots[i].depth = evpars.modamounts[i];

        graingain = std::clamp(evpars.volume, 0.0f, 1.0f);

        float bandpos =
            xenakios::mapvalue<float>(pitch_base, -48.0f, 64.0f, 0.0f, numPitchBandAttens - 1);
        bandpos = std::clamp(bandpos, 0.0f, (float)numPitchBandAttens - 1);
        int ind0 = bandpos;
        int ind1 = ind0 + 1;
        float frac = bandpos - ind0;
        float g0 = pitchBandAttens[ind0];
        float g1 = pitchBandAttens[ind1];
        float gatten = g0 + (g1 - g0) * frac;
        gatten = std::clamp(gatten, 0.0f, 1.0f);
        graingain *= gatten;

        graingain = graingain * graingain * graingain;
        auxsend1 = std::clamp(evpars.auxsend, 0.0f, 1.0f);

        envstarttype = std::clamp<uint8_t>(evpars.envelope_start_type, 0, 30);
        envendtype = std::clamp<uint8_t>(evpars.envelope_end_type, 0, 30);
        envshape = std::clamp(evpars.envelope_shape, 0.0f, 1.0f);
    }
    enum MODTARGET
    {
        MT_PITCH = 0,
        MT_VOLUME = 1,
        MT_AZIMUTH = 2,
        MT_ELEVATION = 3,
        MT_INSERTASTART = 4,
        MT_INSERTBSTART = MT_INSERTASTART + 10,
        NUMMODTARGETS = MT_INSERTBSTART + 10,
    };
    void process(float *outputs, int nframes)
    {
        float aux_env_values[4] = {0.0f};
        double normphase = (double)phase / grain_end_phase;
        for (size_t i = 0; i < num_aux_envelopes; ++i)
        {
            aux_env_values[i] = (*aux_envelopes)[i].get_value(normphase, auxenvtimewarp);
        }
        alignas(16) float modulatedvalues[NUMMODTARGETS] = {0.0f};
        for (auto &e : modulation_slots)
        {
            if (e.source_id < CLAP_INVALID_ID && e.target_id < CLAP_INVALID_ID)
            {
                modulatedvalues[e.target_id] += aux_env_values[e.source_id] * e.depth;
            }
        }
        for (auto &e : modulatedvalues)
        {
            e = std::clamp(e, -1.0f, 1.0f);
        }
        std::visit(
            [this, &modulatedvalues](auto &q) {
                double finalpitch = pitch_base + modulatedvalues[MT_PITCH] * 12.0f;
                double hz = 440.0 * std::pow(2.0, 1.0 / 12.0 * (finalpitch - 9.0));
                q.setFrequency(hz);
            },
            theoscillator);
        for (size_t i = 0; i < 2; ++i)
        {
            if (insert_fx[i].mainmode == GrainInsertFX::GFXSSTFILTER)
            {
                // cutoff
                insert_fx[i].parammodvalues[0] = modulatedvalues[MT_INSERTASTART + i * 10] * 24.0;
                // resonance
                insert_fx[i].parammodvalues[1] = modulatedvalues[MT_INSERTASTART + i * 10 + 1];
                // extra param
                insert_fx[i].parammodvalues[2] = modulatedvalues[MT_INSERTASTART + i * 10 + 2];
                // cut off stereo separation
                // insert_fx[i].parammodvalues[2] = modulatedvalues[MT_INSERTASTART + i * 10 + 2];
                // drywet mix
                insert_fx[i].parammodvalues[4] = modulatedvalues[MT_INSERTASTART + i * 10 + 4];
            }
            /*
            else if (i == 1 && insert_fx[i].mainmode == GrainInsertFX::GFXAIRWINDOWS &&
                     insert_fx[i].submode == 3)
            {
                // for testing purposes using airwindows ringmodulator freq a
                insert_fx[i].parammodvalues[0] = modulation_slots[2].depth * modulatedvalues[2];
            }
                */
            insert_fx[i].prepareBlock();
        }
        int envpeakpos = envshape * grain_end_phase;
        envpeakpos = std::clamp(envpeakpos, 16, grain_end_phase - 16);

        int tail_len_samples = tail_len * sr;
        int tail_fade_samples = tail_fade_len * sr;
        int tail_fade_start = grain_end_phase + tail_len_samples - tail_fade_samples;
        int tail_fade_end = grain_end_phase + tail_len_samples;
        for (int i = 0; i < nframes; ++i)
        {
            float outsample = 0.0f;
            if (phase >= 0 && phase < grain_end_phase)
            {
                outsample = std::visit([](auto &q) { return q.step(); }, theoscillator);
                float envgain = 0.0f;
                /*
                if (envstarttype == 100)
                {
                    envgain = gain_envelope.step();
                }
                else if (envtype == 0 || envtype == 1)
                {
                    if (phase < envpeakpos)
                    {
                        envgain = xenakios::mapvalue<float>(phase, 0.0, envpeakpos, 0.0f, 1.0f);
                        if (envtype == 1)
                        {
                            envgain = 1.0f - envgain;
                            envgain = 1.0f - (envgain * envgain * envgain);
                        }
                    }
                    else if (phase >= envpeakpos)
                    {
                        envgain = xenakios::mapvalue<float>(phase, envpeakpos, grain_end_phase,
                                                            1.0f, 0.0f);
                        if (envtype == 1)
                            envgain = envgain * envgain * envgain;
                    }
                }
                else if (envtype == 2)
                {
                    float envfreq = 1.0f + std::floor(15.0f * envshape);
                    envgain = 0.5f + 0.5f * std::sin(M_PI * 2 / grain_end_phase * phase * envfreq +
                                                     (1.5f * M_PI));
                }
                */
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
                outsample *= envgain * graingain * polarity_gain;
            }
            float outsample0 = outsample;
            float outsample1 = outsample;
            if (filter_routing == FR_ALLSERIAL)
            {
                for (size_t insertIndex = 0; insertIndex < 2; ++insertIndex)
                {
                    insert_fx[insertIndex].processStereo(outsample0, outsample1);
                }
                // feedbacksignals[0] = outsample * feedbackamt;
            }
            else if (filter_routing == FR_ALLPARALLEL)
            {
                float signalstoinserts[4][2];
                float summedinserts[2] = {0.0f, 0.0f};
                for (size_t insertindex = 0; insertindex < numInsertSlots; ++insertindex)
                {
                    if (insert_fx[insertindex].mainmode != GrainInsertFX::GFXNONE)
                    {
                        signalstoinserts[insertindex][0] = outsample0;
                        signalstoinserts[insertindex][1] = outsample1;
                        insert_fx[insertindex].processStereo(signalstoinserts[insertindex][0],
                                                             signalstoinserts[insertindex][1]);
                        summedinserts[0] += signalstoinserts[insertindex][0];
                        summedinserts[1] += signalstoinserts[insertindex][1];
                    }
                }

                /*
                float split = outsample;
                split = filters[0].processMonoSample(split + feedbacksignals[0]);
                feedbacksignals[0] = split * feedbackamt;
                outsample = filters[1].processMonoSample(outsample);
                outsample = split + outsample;
                */
            }

            ++phase;
            float fadegain = 1.0f;
            if (phase >= grain_end_phase)
            {
                if (phase >= tail_fade_end)
                {
                    active = false;
                    fadegain = 0.0f;
                }
                else if (phase >= tail_fade_start)
                {
                    fadegain = xenakios::mapvalue<float>(phase, tail_fade_start, tail_fade_end,
                                                         1.0f, 0.0f);
                    if (fadegain < 0.0f)
                        fadegain = 0.0f;
                }
            }
            outsample0 *= fadegain;
            outsample1 *= fadegain;
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
                __m256 sample0 = _mm256_set1_ps(outsample0);
                __m256 sample1 = _mm256_set1_ps(outsample1);

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
                outputs[i * 64 + chan] =
                    outsample0 * ambcoeffs[chan] + outsample1 * ambcoeffs[chan + 64];
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

class ToneGranulator
{
  public:
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
    // to just use revert to using that for a some things
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
    std::unordered_map<uint32_t, float *> idtoparvalptr;
    std::unordered_map<uint32_t, pmd *> idtoparmetadata;
    std::unordered_map<uint32_t, float> modRanges;
    std::unordered_map<int, int> shapeParToActualShape;
    choc::fifo::SingleReaderSingleWriterFIFO<StepModSource::Message> fifo;
    struct GrainVisualizerMessage
    {
        double timepos = 0.0;
        float pitch = 0.0;
        float duration = 0.001;
        float gain = 0.0f;
        float azimuth0degrees = 0.0f;
        float azimuth1degrees = 0.0f;
        float elevation0degrees = 0.0f;
        float elevation1degrees = 0.0f;
        float visualfade = 1.0f;
    };
    struct GrainVisualizerSettings
    {
        double timespantoshow = 8.0;
    };
    GrainVisualizerSettings gvsettings;
    std::atomic<bool> gatherGrainVisData{false};
    choc::fifo::SingleReaderSingleWriterFIFO<GrainVisualizerMessage> visualizer_fifo;

    enum PARAMS
    {
        PAR_MAINVOLUME = 100,
        PAR_AMBORDER = 200,
        PAR_OSCTYPE = 300,
        PAR_DENSITY = 400,
        PAR_PITCH = 500,
        PAR_AZIMUTH = 600,
        PAR_ELEVATION = 700,
        PAR_AMBSPREAD = 710,
        PAR_AMBROTATE = 720,
        PAR_DURATION = 800,
        PAR_GRAINTAIL = 850,
        PAR_INSERTAFIRST = 900,
        PAR_INSERTBFIRST = PAR_INSERTAFIRST + 32,
        PAR_INSERTCFIRST = PAR_INSERTBFIRST + 32,
        PAR_INSERTDFIRST = PAR_INSERTCFIRST + 32,
        PAR_FMPITCH = 1500,
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
        PAR_STACKRANDOMPITCH = 2500,
        PAR_STACKRANDOMSPATIALIZATION = 2600,
        PAR_STACKTIMECURVE = 2700,
        PAR_VOLENVEASINGSTART = 2800,
        PAR_VOLENVEASINGEND = 2900,
        PAR_GRAINMODSLOTAMOUNT0 = 3000,
        PAR_GRAINMODSLOTAMOUNT1 = 3001,
        PAR_GRAINMODSLOTAMOUNT2 = 3002,
        PAR_GRAINMODSLOTAMOUNT3 = 3003,
        PAR_AUXENVTIMEWARP = 3050,
        PAR_MASTERHIGHPASSCUTOFF = 3100,
        PAR_LFORATES = 100000,
        PAR_LFODEFORMS = 100100,
        PAR_LFOSHIFTS = 100200,
        PAR_LFOWARPS = 100300,
        PAR_LFOSHAPES = 100400,
        PAR_LFOUNIPOLARS = 100500
    };
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
    alignas(32) MidiNoteModSource midiNoteModSource;
    float midiNoteModValue = 0.0f;
    // we can share these between voices as we don't need it stateful, at least for now
    alignas(32)
        std::array<SimpleEnvelope<false>, GranulatorVoice::num_aux_envelopes> voiceaux_envelopes;
    alignas(16) std::array<float, numPitchBandAttens + 5> pitchBandAttensShared;
    alignas(16) std::array<int, 7> osctypemapping;
    struct ModSourceInfo
    {
        std::string name;
        std::string groupname;
        GranulatorModConfig::SourceIdentifier id;
        float val = 0.0f;
    };
    std::vector<ModSourceInfo> modSources;
    alignas(16) std::array<float, 256> modSourceValues;
    std::unordered_map<int, int> midiCCMap;
    alignas(16) std::atomic<int> numVoicesUsed;
    void set_aux_envelope_interpolation_mode(int m) { voiceaux_envelopes[0].interpmode = m; }
    int get_aux_envelope_interpolation_mode() const { return voiceaux_envelopes[0].interpmode; }
    void handleStepSequencerMessages()
    {
        StepModSource::Message msg;
        while (fifo.pop(msg))
        {
            if (msg.dest >= 1000 && msg.dest < 1000 + GranulatorVoice::num_aux_envelopes &&
                msg.opcode == StepModSource::Message::OP_SETSTEP)
            {
                const auto numsteps = SimpleEnvelope<false>::maxnumsteps;
                int envindex = msg.dest - 1000;
                voiceaux_envelopes[envindex].steps[msg.ival0] = msg.fval0;
                if (msg.ival0 == numsteps - 1)
                {
                    // steps array has extra space for interpolation
                    voiceaux_envelopes[envindex].steps[numsteps] = msg.fval0;
                    voiceaux_envelopes[envindex].steps[numsteps + 1] = msg.fval0;
                    voiceaux_envelopes[envindex].steps[numsteps + 2] = msg.fval0;
                }

                // voiceaux_envelope.steps[msg.ival0] = msg.fval0;
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

    /*
        stepModSources[0].setSteps(generate_from_js(R"(
        function generate_steps()
        {
            arr = [];
            for (var i=0;i<100;++i)
            {
              if (i % 5 == 0 || i % 11 == 0)
                arr.push(1.0);
              else arr.push(-1.0);
            }
            return arr;
        }

    )"));
        */
    std::atomic<int> currentSnapShot{-1};
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

    void set_oscillator_type_mapping(std::span<int> mapping)
    {
        std::lock_guard<choc::threading::SpinLock> locker(spinLock);
        for (size_t i = 0; i < osctypemapping.size(); ++i)
        {
            if (i < mapping.size())
                osctypemapping[i] = mapping[i];
        }
    }
    void create_voices()
    {
        std::fill(pitchBandAttensShared.begin(), pitchBandAttensShared.end(), 1.0f);
        // by default one to one mapping but for easier working with modulation
        // another mapping can be used
        for (size_t i = 0; i < osctypemapping.size(); ++i)
        {
            osctypemapping[i] = i;
        }
        for (int i = 0; i < numvoices; ++i)
        {
            auto v = std::make_unique<GranulatorVoice>();
            v->modulation_slots[0] = {0, 0.0f, GranulatorVoice::MT_PITCH};
            v->modulation_slots[1] = {1, 0.0f, GranulatorVoice::MT_PITCH};
            v->modulation_slots[2] = {2, 0.0f, GranulatorVoice::MT_INSERTBSTART + 4};
            v->aux_envelopes = &voiceaux_envelopes;
            v->pitchBandAttens = pitchBandAttensShared;
            v->osctypemapping = osctypemapping;
            v->eluts = &eluts;
            voices.push_back(std::move(v));
        }
    }
    const std::unordered_map<int, std::string> oscTypeToStringMap{
        {0, "SINE"},   {1, "SEMISINE"}, {2, "TRIANGLE"}, {3, "SAW"},
        {4, "SQUARE"}, {5, "FM"},       {6, "NOISE"}};
    ToneGranulator() : m_sr(44100.0), modmatrix(44100.0)
    {
        visualizer_fifo.reset(2048);

        shapeParToActualShape[0] = GranulatorModMatrix::lfo_t::SINE;
        shapeParToActualShape[1] = GranulatorModMatrix::lfo_t::PULSE;
        shapeParToActualShape[2] = GranulatorModMatrix::lfo_t::SAW_TRI_RAMP;
        shapeParToActualShape[3] = GranulatorModMatrix::lfo_t::SMOOTH_NOISE;
        shapeParToActualShape[4] = GranulatorModMatrix::lfo_t::SH_NOISE;
        for (int i = 0; i < 127; ++i)
        {
            midiCCMap[i + 1] = MIDICCSTART + i;
        }
        fifo.reset(2048);
        scheduledGrains.reserve(2048);
        for (auto &v : stepModValues)
            v = 0.0f;
        for (auto &v : randomModValues)
            v = 0.0f;
        randomModSources[0] = TriggeredRandomSource{1001};
        randomModSources[1].set_distribution(TriggeredRandomSource::D_CAUCHY);
        randomModSources[1].parameter_values[1] = 0.02;
        randomModSources[2].set_distribution(TriggeredRandomSource::D_UNIFORM);
        randomModSources[3].set_distribution(TriggeredRandomSource::D_HYPCOS);
        auto initssfunc = [](StepModSource &sms, std::initializer_list<float> values) {
            for (int i = 0; i < values.size(); ++i)
            {
                sms.steps[i] = *(values.begin() + i);
            }
            sms.numactivesteps = StepModSource::maxSteps;
            sms.loopstartstep = 0;
            sms.looplen = values.size();
        };
        initssfunc(stepModSources[0], {-1.0f, 1.0f});
        initssfunc(stepModSources[1], {-1.0f, 0.0f, 1.0f});
        initssfunc(stepModSources[2], {-1.0f, -0.333f, 0.333f, 1.0f});
        initssfunc(stepModSources[3], {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f});
        for (size_t i = 0; i < 4; ++i)
        {
            stepModSources[4 + i].steps.resize(128);
            stepModSources[4 + i].numactivesteps = StepModSource::maxSteps;
            stepModSources[4 + i].loopstartstep = 0;
            stepModSources[4 + i].looplen = 128;
        }
        for (size_t i = 0; i < 128; ++i)
        {
            stepModSources[4].steps[i] = rng.nextFloatInRange(-1.0f, 1.0f);
            stepModSources[5].steps[i] = rng.nextFloatInRange(-1.0f, 1.0f);
            float z = rng.nextFloat();
            if (z < 0.5f)
                stepModSources[6].steps[i] = -1.0f;
            else
                stepModSources[6].steps[i] = 1.0f;
            z = rng.nextFloat();
            if (z < 0.5f)
                stepModSources[7].steps[i] = -1.0f;
            else
                stepModSources[7].steps[i] = 1.0f;
        }
        parmetadatas.reserve(128);
        parmetadatas.push_back(pmd()
                                   .withRange(-24.0, 0.0)
                                   .withDefault(-6.0)
                                   .withLinearScaleFormatting("dB")
                                   .withName("Main volume")
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE)
                                   .withGroupName("Main output")
                                   .withID(PAR_MAINVOLUME));
        parmetadatas.push_back(pmd()
                                   .withType(pmd::FLOAT)
                                   .withRange(-48, 0)
                                   .withDefault(-36)
                                   .withSemitoneZeroAt440Formatting()
                                   .withName("Main Highpass Cutoff")
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE)
                                   .withGroupName("Main output")
                                   .withID(PAR_MASTERHIGHPASSCUTOFF));
        parmetadatas.push_back(pmd()
                                   .withUnorderedMapFormatting({{0, "Ambisonic 1st Order"},
                                                                {1, "Ambisonic 2nd Order"},
                                                                {2, "Ambisonic 3rd Order"},
                                                                {3, "Ambisonic 4th Order"},
                                                                {4, "Ambisonic 5th Order"},
                                                                {5, "Ambisonic 6th Order"},
                                                                {6, "Ambisonic 7th Order"}},
                                                               true)
                                   .withDefault(2)
                                   .withName("Spatialization mode")
                                   .withGroupName("Spatialization")
                                   .withID(PAR_AMBORDER));
        parmetadatas.push_back(pmd()
                                   .withUnorderedMapFormatting(oscTypeToStringMap, true)
                                   .withDefault(0)
                                   .withName("Oscillator type")
                                   .withGroupName("Oscillator")
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE)
                                   .withID(PAR_OSCTYPE));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0f, 7.0f)
                                   .withDefault(4.0)
                                   .withATwoToTheBFormatting(1.0f, 1.0, "Hz")
                                   .withName("Density")
                                   .withGroupName("Time")
                                   .withID(PAR_DENSITY)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0f, 1.0f)
                                   .withDefault(0.75f)
                                   .asCubicDecibelAttenuation()
                                   .withName("Grain volume")
                                   .withGroupName("Volume")
                                   .withID(PAR_GRAINVOLUME)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0f, 1.0f)
                                   .withDefault(0.5f)
                                   .withOffsetPowerFormatting("ms", 0.002f, 0.498f, 3.0f, 1000.0f)
                                   .withDecimalPlaces(0)
                                   .withName("Duration")
                                   .withGroupName("Time")
                                   .withID(PAR_DURATION)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0f, 1.0f)
                                   .withDefault(0.0f)
                                   .withOffsetPowerFormatting("ms", 0.002f, 0.998f, 3.0f, 1000.0f)
                                   .withDecimalPlaces(0)
                                   .withName("Tail")
                                   .withGroupName("Time")
                                   .withID(PAR_GRAINTAIL));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0f, 1.0f)
                                   .withDefault(0.5f)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName("Volume Envelope Morph")
                                   .withGroupName("Volume")
                                   .withID(PAR_ENVMORPH)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        std::unordered_map<int, std::string> easingCurveMap;
        for (int i = 0; i < 40; ++i)
        {
            if (easing_table[i].name && easing_table[i].function)
            {
                easingCurveMap[i] = easing_table[i].name;
            }
            else
                break;
        }
        parmetadatas.push_back(pmd()
                                   .withUnorderedMapFormatting(easingCurveMap, true)
                                   .withDefault(0.0)
                                   .withName("Vol Env Start Curve")
                                   .withGroupName("Volume")
                                   .withID(PAR_VOLENVEASINGSTART));
        parmetadatas.push_back(pmd()
                                   .withUnorderedMapFormatting(easingCurveMap, true)
                                   .withDefault(0.0)
                                   .withName("Vol Env End Curve")
                                   .withGroupName("Volume")
                                   .withID(PAR_VOLENVEASINGEND));
        for (int i = 0; i < numPitchBandAttens; ++i)
        {
            parmetadatas.push_back(pmd()
                                       .withRange(0.0, 1.0)
                                       .withDefault(1.0)
                                       .withLinearScaleFormatting("%", 100.0f)
                                       .withName(fmt::format("Pitch Gain {}", i + 1))
                                       .withGroupName("Volume")
                                       .withID(PAR_PITCHBANDGAIN0 + i * 10)
                                       .withFlags(CLAP_PARAM_IS_MODULATABLE));
        }

        parmetadatas.push_back(pmd()
                                   .withRange(-48.0, 48.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("ST")
                                   .withName("Pitch")
                                   .withGroupName("Oscillator")
                                   .withID(PAR_PITCH)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        for (int i = 0; i < 4; ++i)
        {
            parmetadatas.push_back(pmd()
                                       .withRange(-1.0, 1.0)
                                       .withDefault(0.0)
                                       .withLinearScaleFormatting("%", 100.0f)
                                       .withName(fmt::format("Mod Slot {} Depth", i + 1))
                                       .withGroupName("Oscillator")
                                       .withID(PAR_GRAINMODSLOTAMOUNT0 + i)
                                       .withFlags(CLAP_PARAM_IS_MODULATABLE));
        }

        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("")
                                   .withName("Aux Env Time Warp")
                                   .withGroupName("Oscillator")
                                   .withID(PAR_AUXENVTIMEWARP)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0, 4.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("ST", 12.0f)
                                   .withName("OSC Sync")
                                   .withID(PAR_OSC_SYNC)
                                   .withGroupName("Oscillator")
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0, 1.0)
                                   .withDefault(0.5)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName("OSC PW")
                                   .withID(PAR_OSC_PW)
                                   .withGroupName("Oscillator")
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(-48.0, 48.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("ST")
                                   .withName("FM Pitch")
                                   .withID(PAR_FMPITCH)
                                   .withGroupName("Oscillator")
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName("FM Depth")
                                   .withGroupName("Oscillator")
                                   .withID(PAR_FMDEPTH)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName("FM Feedback")
                                   .withGroupName("Oscillator")
                                   .withID(PAR_FMFEEDBACK)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0, 1.0)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("%", 100.0f)
                                   .withName("Noise Correlation")
                                   .withGroupName("Oscillator")
                                   .withID(PAR_NOISECORRELATION)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(
            pmd()
                .withUnorderedMapFormatting({{0, "Corr noise No interpolation"},
                                             {1, "Corr noise Linear interpolation"},
                                             {2, "Corr noise Corrupted output"},
                                             {3, "Corr noise BounceIn interpolation"},
                                             {4, "Logistic Chaos Linear interpolation"},
                                             {5, "Sinc/Pulse"}},
                                            true)
                .withDefault(1)
                .withName("Noise Mode")
                .withGroupName("Oscillator")
                .withID(PAR_NOISEMODE));
        for (size_t i = 0; i < GranulatorVoice::numInsertSlots; ++i)
        {
            auto groupname = fmt::format("Insert {}", char('A' + i));
            for (size_t j = 0; j < GranulatorVoice::maxParamsPerInsert; ++j)
            {
                size_t insparid = PAR_INSERTAFIRST + 32 * i + j;
                if (i == 3 && j == 0)
                    assert(insparid == PAR_INSERTDFIRST);
                auto insparname = fmt::format("Ins {} Par {}", char('A' + i), j);
                parmetadatas.push_back(pmd()
                                           .withRange(0.0, 1.0)
                                           .withDefault(0.0)
                                           .withLinearScaleFormatting("")
                                           .withName(insparname)
                                           .withID(insparid)
                                           .withGroupName(groupname)
                                           .withFlags(CLAP_PARAM_IS_MODULATABLE));
            }
        }

        parmetadatas.push_back(pmd()
                                   .withRange(-180.0f, 180.0f)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("°")
                                   .withName("Azimuth")
                                   .withGroupName("Spatialization")
                                   .withID(PAR_AZIMUTH)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(-180.0f, 180.0f)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("°")
                                   .withName("Elevation")
                                   .withGroupName("Spatialization")
                                   .withID(PAR_ELEVATION)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(-180.0f, 180.0f)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("°")
                                   .withName("Ambisonic Spread")
                                   .withGroupName("Spatialization")
                                   .withID(PAR_AMBSPREAD)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(-180.0f, 180.0f)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("°")
                                   .withName("Ambisonic Rotation")
                                   .withGroupName("Spatialization")
                                   .withID(PAR_AMBROTATE)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .asInt()
                                   .withRange(1.0f, 16.0f)
                                   .withDefault(1.0)
                                   .withIntegerQuantization()
                                   .withName("Count")
                                   .withGroupName("Stacking")
                                   .withID(PAR_STACKCOUNT));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0f, 1.0f)
                                   .withDefault(0.5)
                                   .withOffsetPowerFormatting("s", 0.05f, 1.95, 2.0f, 1.0f)
                                   .withName("Time Span")
                                   .withGroupName("Stacking")
                                   .withID(PAR_STACKTIMESPAN));
        parmetadatas.push_back(pmd()
                                   .withRange(-1.0f, 1.0f)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("", 1.0f)
                                   .withName("Time Curve")
                                   .withGroupName("Stacking")
                                   .withID(PAR_STACKTIMECURVE));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0f, 1.0f)
                                   .withDefault(0.0)
                                   .withOffsetPowerFormatting("st", 0.0f, 12.0f, 2.0f, 1.0f)
                                   .withName("Pitch RND")
                                   .withGroupName("Stacking")
                                   .withID(PAR_STACKRANDOMPITCH)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        parmetadatas.push_back(pmd()
                                   .withRange(0.0f, 90.0f)
                                   .withDefault(0.0)
                                   .withLinearScaleFormatting("", 1.0f)
                                   .withName("Spat RND")
                                   .withGroupName("Stacking")
                                   .withID(PAR_STACKRANDOMSPATIALIZATION)
                                   .withFlags(CLAP_PARAM_IS_MODULATABLE));
        for (int i = 0; i < GranulatorModMatrix::numLfos; ++i)
        {
            parmetadatas.push_back(pmd()
                                       .asOnOffBool()
                                       .withID(PAR_LFOUNIPOLARS + i)
                                       .withName(fmt::format("LFO {} UNIPOLAR", i + 1))
                                       .withGroupName(fmt::format("LFO {}", i + 1)));
            parmetadatas.push_back(pmd()
                                       .withUnorderedMapFormatting({{0, "SIN"},
                                                                    {1, "SIN🡄🡆SQR🡄🡆TRI"},
                                                                    {2, "DOWN🡄🡆TRI🡄🡆UP"},
                                                                    {3, "SMOOTH NOISE"},
                                                                    {4, "S&H NOISE"}},
                                                                   true)
                                       .withName(fmt::format("LFO {} SHAPE", i + 1))
                                       .withGroupName(fmt::format("LFO {}", i + 1))
                                       .withID(PAR_LFOSHAPES + i));
            parmetadatas.push_back(pmd()
                                       .withRange(-6.0, 6.0)
                                       .withDefault(0.0)
                                       .withDecimalPlaces(3)
                                       .withATwoToTheBFormatting(1.0f, 1.0f, "Hz")
                                       .withName(fmt::format("LFO {} RATE", i + 1))
                                       .withShortName("RATE")
                                       .withGroupName(fmt::format("LFO {}", i + 1))
                                       .withID(PAR_LFORATES + i)
                                       .withFlags(CLAP_PARAM_IS_MODULATABLE));
            parmetadatas.push_back(pmd()
                                       .withRange(-1.0, 1.0)
                                       .withDefault(0.0)
                                       .withLinearScaleFormatting("%", 100.0f)
                                       .withName(fmt::format("LFO {} DEFORM", i + 1))
                                       .withShortName("DEFORM")
                                       .withGroupName(fmt::format("LFO {}", i + 1))
                                       .withID(PAR_LFODEFORMS + i)
                                       .withFlags(CLAP_PARAM_IS_MODULATABLE));
            parmetadatas.push_back(pmd()
                                       .withRange(-1.0, 1.0)
                                       .withDefault(0.0)
                                       .withLinearScaleFormatting("%", 100.0f)
                                       .withName(fmt::format("LFO {} SHIFT", i + 1))
                                       .withGroupName(fmt::format("LFO {}", i + 1))
                                       .withID(PAR_LFOSHIFTS + i)
                                       .withFlags(CLAP_PARAM_IS_MODULATABLE));
            parmetadatas.push_back(pmd()
                                       .withRange(-1.0, 1.0)
                                       .withDefault(0.0)
                                       .withLinearScaleFormatting("%", 100.0f)
                                       .withName(fmt::format("LFO {} WARP", i + 1))
                                       .withGroupName(fmt::format("LFO {}", i + 1))
                                       .withID(PAR_LFOWARPS + i)
                                       .withFlags(CLAP_PARAM_IS_MODULATABLE));
        }

        paramvalues.resize(parmetadatas.size());
        for (int i = 0; i < parmetadatas.size(); ++i)
        {
            idtoparmetadata[parmetadatas[i].id] = &parmetadatas[i];
            paramvalues[i] = parmetadatas[i].defaultVal;
            idtoparvalptr[parmetadatas[i].id] = &paramvalues[i];
            if (parmetadatas[i].flags & CLAP_PARAM_IS_MODULATABLE)
            {
                // we might want to have custom ranges too, but these
                // autogenerated ones shuffice for now
                float range = (parmetadatas[i].maxVal - parmetadatas[i].minVal);
                modRanges[parmetadatas[i].id] = range;
            }
        }

        create_voices();

        for (size_t i = 0; i < parmetadatas.size(); ++i)
        {
            const auto &md = parmetadatas[i];
            if (md.flags & CLAP_PARAM_IS_MODULATABLE)
            {
                modmatrix.m.bindTargetBaseValue(GranulatorModConfig::TargetIdentifier{(int)md.id},
                                                *idtoparvalptr[md.id]);
            }
        }
        modmatrix.m.bindTargetBaseValue(GranulatorModConfig::TargetIdentifier{(int)1},
                                        dummyTargetValue);

        modSources.reserve(64);
        modSources.emplace_back("Off", "", GranulatorModConfig::SourceIdentifier{0});
        for (uint32_t i = 0; i < GranulatorModMatrix::numLfos; ++i)
        {
            modSources.emplace_back(fmt::format("LFO {}", i + 1), "LFO",
                                    GranulatorModConfig::SourceIdentifier{i + 1});
        }
        for (uint32_t i = 0; i < 8; ++i)
        {
            modSources.emplace_back(fmt::format("StepSeq {}", i + 1), "Step Sequencer",
                                    GranulatorModConfig::SourceIdentifier{STEPS0 + i});
        }
        for (uint32_t i = 0; i < 4; ++i)
        {
            modSources.emplace_back(fmt::format("Random {}", i + 1), "Random",
                                    GranulatorModConfig::SourceIdentifier{RANDOM0 + i});
        }
        for (uint32_t i = 0; i < 16; ++i)
        {
            modSources.emplace_back(fmt::format("Host Parameter {}", i + 1), "Host Parameter",
                                    GranulatorModConfig::SourceIdentifier{HOSTPARAMSTART + i});
        }
        modSources.emplace_back("MIDI KEY", "MIDI NOTES",
                                GranulatorModConfig::SourceIdentifier{MIDINOTE});
        modSources.emplace_back("MIDI VELOCITY", "MIDI NOTES",
                                GranulatorModConfig::SourceIdentifier{MIDIVELO});
        modSources.emplace_back("MIDI AFTERTOUCH", "MIDI NOTES",
                                GranulatorModConfig::SourceIdentifier{MIDIAT});
        for (uint32_t i = 1; i < 128; ++i)
        {
            modSources.emplace_back(fmt::format("MIDI CC {}", i), "MIDI CC",
                                    GranulatorModConfig::SourceIdentifier{i + MIDICCSTART});
        }

        for (auto &v : modSourceValues)
            v = 0.0f;
        for (uint32_t i = 0; i < modSources.size(); ++i)
        {
            // std::print("{} binding {} {} to {}\n", i,
            // modSources[i].id.src, modSources[i].name,
            //            (void *)&modSourceValues[i]);
            modmatrix.m.bindSourceValue(modSources[i].id, modSourceValues[i]);
        }
        init_filter_infos();
    }
    std::array<size_t, 2> insertsMainModes = {0, 0};
    std::array<size_t, 2> insertsAWTypes = {0, 0};
    std::array<sfpp::FilterModel, 2> filtersModels{sfpp::FilterModel(), sfpp::FilterModel()};
    std::array<sfpp::ModelConfig, 2> filtersConfigs{sfpp::ModelConfig(), sfpp::ModelConfig()};
    alignas(32) EasingLUTS eluts;
    void set_filter(int which, uint8_t mainmode, uint8_t awtype, sfpp::FilterModel mo,
                    sfpp::ModelConfig conf)
    {
        filtersModels[which] = mo;
        filtersConfigs[which] = conf;
        int oldmainmode = insertsMainModes[which];
        insertsMainModes[which] = mainmode;
        insertsAWTypes[which] = awtype;
        for (int i = 0; i < numvoices; ++i)
        {
            auto &v = voices[i];
            // v->set_samplerate(sr);

            v->set_insert_type(which, mainmode, awtype, mo, conf);
            if (i == 0)
            {
                for (size_t j = 0; j < GranulatorVoice::maxParamsPerInsert; ++j)
                {
                    int parid = PAR_INSERTAFIRST + 32 * which + j;
                    // if old mode was already sst filter, don't set the parameter
                    if (oldmainmode == GrainInsertFX::GFXNONE ||
                        oldmainmode == GrainInsertFX::GFXAIRWINDOWS ||
                        oldmainmode == GrainInsertFX::GFXXENAKIOS)
                        *idtoparvalptr[parid] = v->insert_fx[which].paramvalues[j];
                    idtoparmetadata[parid]->name = v->insert_fx[which].getParameterName(j);
                    idtoparmetadata[parid]->defaultVal = v->insert_fx[which].paramvalues[j];
                }
            }
        }
    }

    void set_voice_aux_envelope(std::array<float, SimpleEnvelope<false>::maxnumsteps> env)
    {
        for (auto &v : voices)
        {
            // v->aux_envelope.steps = env;
        }
    }
    void set_voice_gain_envelope(std::array<float, SimpleEnvelope<false>::maxnumsteps> env)
    {
        for (auto &v : voices)
        {
            // v->gain_envelope.steps = env;
        }
    }

    int current_ambisonic_order = 0;
    int pending_ambisonic_order = 0;
    void set_event_list(events_t evlist)
    {
        if (evlist.size() > 0)
        {
            std::sort(evlist.begin(), evlist.end(), [](GrainEvent &lhs, GrainEvent &rhs) {
                return lhs.time_position < rhs.time_position;
            });
            std::erase_if(evlist, [](GrainEvent &e) {
                return e.time_position < 0.0 || (e.time_position + e.duration) > 600.0;
            });
            std::lock_guard<choc::threading::SpinLock> locker(spinLock);
            missedgrains = 0;
            evindex = 0;
            playposframes = 0;
            std::swap(evlist, events);
        }
    }
    void prepare(float samplerate, int filter_routing, float tail_len, float tail_fade_len)
    {

        {
            std::lock_guard<choc::threading::SpinLock> locker(spinLock);
            for (int i = 0; i < numvoices; ++i)
            {
                auto &v = voices[i];
                v->set_samplerate(samplerate);
                v->filter_routing = (GranulatorVoice::FilterRouting)filter_routing;
                v->tail_len = tail_len;
                v->tail_fade_len = tail_fade_len;
            }

            m_sr = samplerate;
            missedgrains = 0;
            evindex = 0;
            playposframes = 0;
            gainlag.setRateInMilliseconds(1000.0, m_sr, 1.0);
            gainlag.snapTo(0.0);
            graingen_phase = 0.0;
            graingen_phase_prior = 2.0;
            modmatrix.set_sample_rate(samplerate);
            modmatrix.m.prepare(modmatrix.rt, samplerate, granul_block_size);
            masterHighPassFilter.prepare(m_sr);
        }
    }
    void set_ambisonics_order(int order)
    {
        assert(order > 0 && order < 8);
        if (current_ambisonic_order == order || fadeForLargeStateChange.is_active())
            return;
        pending_ambisonic_order = order;
        fadeForLargeStateChange.start(m_sr, 500.0f, [this]() {
            current_ambisonic_order = pending_ambisonic_order;
            num_out_chans = ambisonicOrderNumChannels(current_ambisonic_order);
            masterHighPassFilter.numactivechannels = num_out_chans;
            // std::print(std::cerr, "changed ambisonic order to {}\n", current_ambisonic_order);
            for (auto &vc : voices)
            {
                vc->active = false;
                vc->ambisonic_order = current_ambisonic_order;
                vc->num_outputchans = num_out_chans;
            }
        });
        return;
        current_ambisonic_order = order;
        for (auto &v : voices)
        {
            v->active = false;
            v->ambisonic_order = order;
            v->num_outputchans = ambisonicOrderNumChannels(order);
        }
        num_out_chans = ambisonicOrderNumChannels(order);
    }
    std::atomic<float> auxenvwarpmodulated = 0.0f;
    std::atomic<float> auxenvdepthpmodulated = 0.0f;
    std::atomic<uint32_t> modulatedParamToStore{0};
    std::atomic<float> modulatedParValueForGUI{0.0f};
    void process_modulations()
    {
        for (uint32_t i = 0; i < modmatrix.numLfos; ++i)
        {
            float shift = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{(int)(PAR_LFOSHIFTS + i)});
            modmatrix.m_lfos[i]->applyPhaseOffset(shift);
            float rate = modmatrix.m.getTargetValue(
                GranulatorModConfig::TargetIdentifier{(int)(PAR_LFORATES + i)});
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
        modmatrix.m.process();
    }
    void generate_grain()
    {
        double actgrate =
            modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_DENSITY});
        actgrate = std::clamp(actgrate, -1.0, 8.0);

        double grate = 1.0 / std::pow(2.0, actgrate);
        for (int i = 0; i < granul_block_size; ++i)
        {
            if (graingen_phase_prior > graingen_phase)
            {
                std::erase_if(scheduledGrains, [](GrainEvent &e) { return e.time_position < 0.0; });
                float pitch =
                    modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_PITCH});
                float gdur =
                    modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_DURATION});
                float gvol = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_GRAINVOLUME});
                GrainEvent genev{0.0, gdur, pitch, gvol};
                genev.envelope_start_type = *idtoparvalptr[PAR_VOLENVEASINGSTART];
                genev.envelope_end_type = *idtoparvalptr[PAR_VOLENVEASINGEND];
                genev.envelope_shape =
                    modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_ENVMORPH});
                float azimuth =
                    modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_AZIMUTH});
                float amb_spread = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_AMBSPREAD});
                float amb_rotate = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_AMBROTATE});
                float elevation = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_ELEVATION});
                genev.generator_type =
                    std::round(0.1f + modmatrix.m.getTargetValue(
                                          GranulatorModConfig::TargetIdentifier{PAR_OSCTYPE}));
                float fm_pitch =
                    modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_FMPITCH});
                genev.fm_frequency_hz = 440.0 * std::pow(2.0, 1.0 / 12 * (fm_pitch - 9.0));
                genev.fm_amount =
                    modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_FMDEPTH});
                genev.fm_feedback = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_FMFEEDBACK});
                genev.pulse_width =
                    modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_OSC_PW});
                genev.noisecorr = modmatrix.m.getTargetValue(
                    GranulatorModConfig::TargetIdentifier{PAR_NOISECORRELATION});
                genev.noiseimode = *idtoparvalptr[PAR_NOISEMODE];
                genev.sync_ratio =
                    std::pow(2.0, modmatrix.m.getTargetValue(
                                      GranulatorModConfig::TargetIdentifier{PAR_OSC_SYNC}));
                for (size_t j = 0; j < GranulatorVoice::numInsertSlots; ++j)
                {
                    auto numpars = voices.front()->insert_fx[j].numParams;
                    for (size_t k = 0; k < numpars; ++k)
                    {
                        int insparid = PAR_INSERTAFIRST + 32 * j + k;
                        genev.insertparams[j][k] = modmatrix.m.getTargetValue(
                            GranulatorModConfig::TargetIdentifier{insparid});
                    }
                }
                for (int k = 0; k < 4; ++k)
                    genev.modamounts[k] = modmatrix.m.getTargetValue(
                        GranulatorModConfig::TargetIdentifier{PAR_GRAINMODSLOTAMOUNT0 + k});
                auxenvdepthpmodulated = genev.modamounts[0];
                genev.auxenvtimewarp = auxenvwarpmodulated;

                int numToSchedule = std::clamp(*idtoparvalptr[PAR_STACKCOUNT], 1.0f, 16.0f);
                float pitchrand = std::clamp(*idtoparvalptr[PAR_STACKRANDOMPITCH], 0.0f, 1.0f);
                pitchrand = 12.0f * std::pow(pitchrand, 2.0f);
                float timeSpanToSchedule =
                    std::clamp(*idtoparvalptr[PAR_STACKTIMESPAN], 0.0f, 1.0f);
                timeSpanToSchedule = 0.05f + 1.95f * std::pow(timeSpanToSchedule, 2.0f);
                float timeSpanCurve = *idtoparvalptr[PAR_STACKTIMECURVE];
                float spatrand = *idtoparvalptr[PAR_STACKRANDOMSPATIALIZATION];
                for (int j = 0; j < numToSchedule; ++j)
                {
                    double tpos = playposframes / this->m_sr;
                    double normpos = 1.0 / numToSchedule * j;
                    if (timeSpanCurve < 0.0f)
                    {
                        float ex = xenakios::mapvalue(timeSpanCurve, -1.0f, 0.0f, 4.0f, 1.0f);
                        normpos = std::pow(normpos, ex);
                    }
                    else
                    {
                        float ex = xenakios::mapvalue(timeSpanCurve, 0.0f, 1.0f, 1.0f, 4.0f);
                        normpos = 1.0f - std::pow(1.0f - normpos, ex);
                    }
                    tpos += timeSpanToSchedule * normpos;
                    genev.time_position = tpos;
                    // main grain parameters without randomization
                    if (j == 0)
                    {
                        genev.pitch_semitones = pitch;
                        genev.azimuth = azimuth;
                        genev.ambi_spread = amb_spread;
                        genev.ambi_rotate = amb_rotate;
                        genev.elevation = elevation;
                    }
                    else
                    {
                        genev.pitch_semitones = pitch + rng.nextFloatInRange(-pitchrand, pitchrand);
                        genev.azimuth = azimuth + rng.nextFloatInRange(-spatrand, spatrand);
                        genev.elevation = elevation + rng.nextFloatInRange(-spatrand, spatrand);
                    }
                    // fading volume for now but should be more adjustable...
                    genev.volume = gvol * (1.0f - (0.5f * normpos));
                    scheduledGrains.push_back(genev);
                }
                // need to sort because we may have previous unstarted events and our
                // playback scheduling logic needs the events sorted
                std::sort(scheduledGrains.begin(), scheduledGrains.end(), [](auto &lhs, auto &rhs) {
                    return lhs.time_position < rhs.time_position;
                });
                const bool printschedulesevents = false;
                if (printschedulesevents)
                {
                    /*
                    std::print("{:.2f} : ", playposframes / m_sr);
                    for (auto &scgrain : scheduledGrains)
                        std::print("{:.2f} ", scgrain.time_position);
                    std::print("\n");
                    */
                }

                for (size_t sm = 0; sm < stepModSources.size(); ++sm)
                {
                    stepModValues[sm] = stepModSources[sm].next();
                }
                for (size_t rm = 0; rm < randomModSources.size(); ++rm)
                {
                    randomModValues[rm] = randomModSources[rm].next();
                }
                midiNoteModValue = midiNoteModSource.next();
                scheduledIndex = 0;
                // why is this still around...?
                if (false)
                {
                    bool wasfound = false;
                    for (int j = 0; j < voices.size(); ++j)
                    {
                        if (!voices[j]->active)
                        {
                            // std::print("starting voice {} alternating value {}\n", j,
                            // alternatingValue);
                            for (size_t sm = 0; sm < stepModSources.size(); ++sm)
                                stepModValues[sm] = stepModSources[sm].next();
                            voices[j]->grainid = graincount;
                            voices[j]->start(genev);
                            float ambdif = 0.0f; // *idtoparvalptr[PAR_AMBIDIFFUSION];
                            if (ambdif > 0.0f)
                            {
                                ambdif *= 0.1f;
                                for (size_t coeff = 4; coeff < 16; ++coeff)
                                {
                                    float diffamount = rng.nextHypCos(0.0f, ambdif);
                                    voices[j]->ambcoeffs[coeff] += diffamount;
                                }
                            }
                            wasfound = true;
                            ++graincount;
                            break;
                        }
                    }
                    if (!wasfound)
                    {
                        ++missedgrains;
                    }
                }
            }
            graingen_phase_prior = graingen_phase;
            graingen_phase += 1.0 / m_sr / grate;
            if (graingen_phase >= 1.0)
                graingen_phase -= 1.0;
        }
    }
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
    void advanceCloudPlayers()
    {
        int i = 0;
        for (auto &p : cloudPlayers)
        {
            if (p.active)
            {
                CloudEvent *ev = nullptr;
                if (p.event_index >= 0 && p.event_index < p.cloud->events.size())
                    ev = &p.cloud->events[p.event_index];
                while (ev && std::floor((ev->time_position + p.start_time) * m_sr) <
                                 playposframes + granul_block_size)
                {
                    // std::cout << playposframes << " cloud player " << i
                    //           << " wants to start event with timepos " << ev->time_position <<
                    //           "\n";
                    bool wasfound = false;
                    for (int j = 0; j < voices.size(); ++j)
                    {
                        if (!voices[j]->active)
                        {
                            // std::print("starting voice {} for event {}\n", j, evindex);
                            voices[j]->grainid = graincount;
                            GrainEvent gev{0.0, 0.1, 0.0, 1.0};
                            gev.duration = modmatrix.m.getTargetValue(
                                GranulatorModConfig::TargetIdentifier{PAR_DURATION});
                            gev.azimuth = modmatrix.m.getTargetValue(
                                GranulatorModConfig::TargetIdentifier{PAR_AZIMUTH});
                            gev.generator_type = modmatrix.m.getTargetValue(
                                GranulatorModConfig::TargetIdentifier{PAR_OSCTYPE});
                            float syncoctaves = modmatrix.m.getTargetValue(
                                GranulatorModConfig::TargetIdentifier{PAR_OSC_SYNC});
                            gev.sync_ratio = std::pow(2.0, syncoctaves);
                            gev.pulse_width = modmatrix.m.getTargetValue(
                                GranulatorModConfig::TargetIdentifier{PAR_OSC_PW});
                            for (auto &pc : ev->param_modulations)
                            {
                                if (pc.id == CLAP_INVALID_ID)
                                    break;
                                if (pc.id == PAR_PITCH)
                                    gev.pitch_semitones = pc.value;
                                else if (pc.id == PAR_GRAINVOLUME)
                                    gev.volume = pc.value;
                                else if (pc.id == PAR_DURATION)
                                    gev.duration = pc.value;
                                else if (pc.id == PAR_GRAINVOLUME)
                                    gev.volume = pc.value;
                                else if (pc.id == PAR_OSCTYPE)
                                    gev.generator_type = pc.value;
                                else if (pc.id == PAR_AZIMUTH)
                                    gev.azimuth = pc.value;
                                else if (pc.id == PAR_OSC_SYNC)
                                    gev.sync_ratio = pc.value;
                            }
                            if (p.cloud->after_touch_dest == PAR_GRAINVOLUME)
                            {
                                // for testing, absolute value but probably will be modulation later
                                gev.volume = p.after_touch_amount;
                            }
                            for (size_t j = 0; j < GranulatorVoice::numInsertSlots; ++j)
                            {
                                auto numpars = voices.front()->insert_fx[j].numParams;
                                for (size_t k = 0; k < numpars; ++k)
                                {
                                    int insparid = PAR_INSERTAFIRST + 32 * j + k;
                                    gev.insertparams[j][k] = modmatrix.m.getTargetValue(
                                        GranulatorModConfig::TargetIdentifier{insparid});
                                }
                            }
                            if (p.cloud->after_touch_dest >= PAR_INSERTAFIRST &&
                                p.cloud->after_touch_dest < PAR_INSERTAFIRST + 10)
                            {
                                gev.insertparams[0][p.cloud->after_touch_dest - PAR_INSERTAFIRST] =
                                    p.after_touch_amount;
                            }
                            voices[j]->start(gev);
                            if (gatherGrainVisData)
                            {
                                GrainVisualizerMessage vmsg;
                                vmsg.timepos = playposframes / m_sr;
                                vmsg.pitch = voices[j]->pitch_base;
                                vmsg.duration = voices[j]->grain_end_phase / m_sr;
                                vmsg.gain = voices[j]->graingain;
                                vmsg.azimuth0degrees = voices[j]->used_azi0;
                                vmsg.azimuth1degrees = voices[j]->used_azi1;
                                vmsg.elevation0degrees = voices[j]->used_ele0;
                                vmsg.elevation1degrees = voices[j]->used_ele1;
                                visualizer_fifo.push(vmsg);
                            }
                            wasfound = true;
                            // std::cout << "grain " << graincount << " started on voice " << j
                            //           << "\n";
                            ++graincount;
                            break;
                        }
                    }
                    if (!wasfound)
                    {
                        ++missedgrains;
                    }
                    ++p.event_index;
                    if (p.event_index >= p.cloud->events.size())
                    {
                        ev = nullptr;
                        p.active = false;
                        std::cout << playposframes << " cloudplayer " << i << " reached end\n";
                    }
                    else
                        ev = &p.cloud->events[p.event_index];
                }
                if (p.event_index >= p.cloud->events.size())
                {
                    // p.event_index = 0;
                    // p.active = true;
                    // p.start_time = playposframes / m_sr;
                }
            }
            ++i;
        }
    }
    void advanceFullEventList()
    {
        GrainEvent *ev = nullptr;
        if (evindex < events.size())
            ev = &events[evindex];
        while (ev && std::floor(ev->time_position * m_sr) < playposframes + granul_block_size)
        {
            bool wasfound = false;
            for (int j = 0; j < voices.size(); ++j)
            {
                if (!voices[j]->active)
                {
                    // std::print("starting voice {} for event {}\n", j, evindex);
                    voices[j]->grainid = graincount;
                    voices[j]->start(*ev);
                    wasfound = true;
                    ++graincount;
                    break;
                }
            }
            if (!wasfound)
            {
                ++missedgrains;
            }
            ++evindex;
            if (evindex >= events.size())
                ev = nullptr;
            else
                ev = &events[evindex];
        }
    }
    void advanceAutoGeneratedGrains(float taillen)
    {
        generate_grain();
        GrainEvent *ev = nullptr;
        if (scheduledIndex < scheduledGrains.size())
            ev = &scheduledGrains[scheduledIndex];
        while (ev && std::floor(ev->time_position * m_sr) < playposframes + granul_block_size)
        {
            bool voicewasfound = false;
            for (int j = 0; j < voices.size(); ++j)
            {
                if (!voices[j]->active)
                {
                    // std::print("starting voice {} for scheduled event {}\n", j, evindex);
                    // the grain oscillators and/or fx may produce lopsided waveforms
                    // so have this to counteract
                    if (graincount % 2 == 0)
                        voices[j]->polarity_gain = 1.0f;
                    else
                        voices[j]->polarity_gain = -1.0f;
                    // std::cout << graincount << " polarity gain " << voices[j]->polarity_gain
                    //          << std::endl;
                    voices[j]->grainid = graincount;
                    voices[j]->doambnormalization = true;
                    voices[j]->tail_len = taillen;
                    voices[j]->tail_fade_len = std::clamp(taillen * 0.5, 0.002, 1.0);
                    voices[j]->start(*ev);
                    voicewasfound = true;
                    if (gatherGrainVisData)
                    {
                        GrainVisualizerMessage vmsg;
                        vmsg.timepos = ev->time_position;
                        vmsg.pitch = voices[j]->pitch_base;
                        vmsg.duration = voices[j]->grain_end_phase / m_sr;
                        vmsg.gain = voices[j]->graingain;
                        vmsg.azimuth0degrees = voices[j]->used_azi0;
                        vmsg.azimuth1degrees = voices[j]->used_azi1;
                        vmsg.elevation0degrees = voices[j]->used_ele0;
                        vmsg.elevation1degrees = voices[j]->used_ele1;

                        visualizer_fifo.push(vmsg);
                    }
                    modulatedOscType = ev->generator_type;
                    ++graincount;
                    break;
                }
            }
            // flag scheduled event for removal
            ev->time_position = -1.0;
            if (!voicewasfound)
            {
                ++missedgrains;
            }
            ++scheduledIndex;
            if (scheduledIndex >= scheduledGrains.size())
                ev = nullptr;
            else
                ev = &scheduledGrains[scheduledIndex];
        }
    }
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
                voices[j]->process(voiceout, granul_block_size);
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
    void process_block(std::span<float> outputbuffer)
    {
        assert(outputbuffer.size() == granul_block_size * 64);
        // this will be contended only infrequently,
        // but we should not generally depend on being able to use this lock...
        std::lock_guard<choc::threading::SpinLock> locker(spinLock);
        set_ambisonics_order(1 + *idtoparvalptr[PAR_AMBORDER]);

        float taillen = *idtoparvalptr[PAR_GRAINTAIL];
        taillen = 0.002 + 0.998 * std::pow(taillen, 3.0);
        handleStepSequencerMessages();
        bool self_generate = true;
        // if (events.size() == 0)
        //     self_generate = true;
        //  bool doambcoeffsnormalization = *idtoparvalptr[PAR_AMBUSENORMALIZATION];
        process_modulations();
        auxenvwarpmodulated =
            modmatrix.m.getTargetValue(GranulatorModConfig::TargetIdentifier{PAR_AUXENVTIMEWARP});
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
            masterHighPassFilter.set_cutoff_mapped(
                0, modmatrix.m.getTargetValue(
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
    }
};
