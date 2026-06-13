
#include "audio/choc_SampleBuffers.h"
#include "grainfx.h"
#include "granularsynth.h"
#include "audio/choc_AudioFileFormat_WAV.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include "audio/choc_AudioFileFormat.h"
#include "../Common/xap_breakpoint_envelope.h"
#include "sst/basic-blocks/dsp/EllipticBlepOscillators.h"
#include "sst/basic-blocks/dsp/SpecialFunctions.h"
#include "sst/filters++/enums.h"
#include "sst/filters++/model_config.h"
#include "sst/filters/FastTiltNoiseFilter.h"
#include "sst/filters/FilterConfiguration.h"

inline int test_nephos_render()
{
    auto g = std::make_unique<ToneGranulator>();
    double sr = 44100.0;
    g->prepare(sr, 0, 0.002, 0.002);
    events_t events;
    events.reserve(500);
    xenakios::Xoroshiro128Plus rng;
    g->set_aux_envelope_interpolation_mode(0);
    for (int i = 0; i < 500; ++i)
    {
        GrainEvent e;
        e.time_position = rng.nextFloatInRange(0.0f, 29.5);
        e.pitch_semitones = rng.nextFloatInRange(-24.0f, 24.0f);
        e.duration = 0.5;
        e.generator_type = 0;
        e.azimuth = rng.nextFloatInRange(-180.0f, 180.0f);
        // e.modamounts[GrainEvent::MD_PITCH] = 0.0;
        if (rng.nextFloat() < 0.1)
        {
            e.duration = 0.95;
            e.elevation = 90.0;
            e.generator_type = 4;
            assert(e.modamounts[GrainEvent::MD_PITCH] == 0.0f);
            e.modamounts[GrainEvent::MD_PITCH] = 12.0;
            if (rng.nextFloat() < 0.5)
                e.modamounts[GrainEvent::MD_PITCH] = -12.0;
        }
        else
            assert(e.modamounts[GrainEvent::MD_PITCH] == 0.0f);
        assert(e.modamounts[GrainEvent::MD_AZI] == 0.0f);
        assert(e.modamounts[GrainEvent::MD_ELE] == 0.0f);
        assert(e.modamounts[GrainEvent::MD_FIL0FREQ] == 0.0f);
        assert(e.modamounts[GrainEvent::MD_FIL0RESO] == 0.0f);
        e.volume = 1.0;
        events.push_back(e);
    }
    g->set_event_list(events);
    const unsigned int amborder = 3;
    unsigned int numambchans = ambisonicOrderNumChannels(amborder);
    g->set_ambisonics_order(amborder);
    choc::audio::AudioFileProperties props;
    props.bitDepth = choc::audio::BitDepth::float32;
    props.sampleRate = sr;
    props.numChannels = numambchans;
    choc::audio::WAVAudioFileFormat<true> wavformat;
    auto writer = wavformat.createWriter(R"(nephos01.wav)", props);
    if (!writer)
        return 1;
    alignas(32) float outbuffer[64 * granul_block_size];
    choc::buffer::ChannelArrayBuffer<float> diskbuffer{numambchans, granul_block_size};
    int outlen = sr * 30.0;
    int outcount = 0;
    xenakios::Envelope pitchenv;
    pitchenv.addPoint({0.0, 0.0});
    pitchenv.addPoint({5.0, 12.0});
    pitchenv.sortPoints();
    *g->idtoparvalptr[ToneGranulator::PAR_DENSITY] = 6.0;
    *g->idtoparvalptr[ToneGranulator::PAR_DURATION] = 0.6;
    auto start = std::chrono::high_resolution_clock::now();
    while (outcount < outlen)
    {
        double tpos = outcount / sr;
        auto pitch = pitchenv.getValueAtPosition(tpos);
        *g->idtoparvalptr[ToneGranulator::PAR_PITCH] = pitch;
        g->process_block(std::span<float>{outbuffer, granul_block_size * 64});
        int nchs = g->num_out_chans;
        for (int i = 0; i < nchs; ++i)
        {
            for (int j = 0; j < granul_block_size; ++j)
            {
                diskbuffer.getSample(i, j) = outbuffer[j * nchs + i];
            }
        }
        writer->appendFrames(diskbuffer.getView());
        outcount += granul_block_size;
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "Time: " << duration.count() << " ms\n";
    return 0;
}

inline void test_colored_noise()
{
    struct Host
    {
        float sr = 44100.0f;
        float getSampleRateInv() { return 1.0 / sr; }
        static float dbToLinear(float db) { return xenakios::decibelsToGain(db); }
    };
    Host hc;
    sst::filters::FastTiltNoiseFilter<Host> tilt_filter{hc};
    alignas(32) float startnoise[16];
    xenakios::Xoroshiro128Plus rng;
    float noisegain = 1.0;
    for (int i = 0; i < 16; ++i)
        startnoise[i] = rng.nextFloatInRange(-noisegain, noisegain);
    tilt_filter.init(startnoise, -0.0f);
    xenakios::Envelope env{{{0.0, -3.0}, {10.0, -0.0}, {20.0, 2.0}}};
    // tilt_filter.setCoeff(-6.0);
    choc::audio::AudioFileProperties props;
    props.bitDepth = choc::audio::BitDepth::float32;
    props.sampleRate = hc.sr;
    props.numChannels = 1;
    choc::audio::WAVAudioFileFormat<true> wavformat;
    auto writer = wavformat.createWriter(R"(colornoise01.wav)", props);
    if (!writer)
        return;
    unsigned int outlen = hc.sr * 20.0;
    const unsigned int blockSize = 8;
    choc::buffer::ChannelArrayBuffer<float> diskbuffer{1, outlen + blockSize};
    diskbuffer.clear();
    int clipped = 0;
    float maxsample = 0.0f;
    float compengain = 1.0f;

    int outpos = 0;
    while (outpos < outlen)
    {
        double tpos = outpos / hc.sr;
        float filtgain = env.getValueAtPosition(tpos);
        tilt_filter.setCoeffForBlock<blockSize>(filtgain);
        if (filtgain <= 0.0f)
            compengain = 1.0f;
        else
        {
            compengain = -std::abs(filtgain * 10.0f);
            compengain = xenakios::decibelsToGain(compengain);
        }
        compengain = 1.0f;
        alignas(32) float input[blockSize];
        for (int i = 0; i < blockSize; ++i)
            input[i] = rng.nextFloatInRange(-noisegain, noisegain);
        alignas(32) float output[blockSize];
        tilt_filter.processBlock<blockSize>(input, output);
        for (int i = 0; i < blockSize; ++i)
        {
            float sample = output[i] * compengain;
            diskbuffer.getSample(0, outpos + i) = sample;
            maxsample = std::max(std::abs(sample), maxsample);
            if (sample < -1.0f || sample > 1.0f)
                ++clipped;
        }
        outpos += blockSize;
    }
    if (maxsample > 0.0f)
    {
        choc::buffer::applyGain(diskbuffer, 1.0 / maxsample);
    }
    writer->appendFrames(diskbuffer.getView());
    std::cout << clipped << " samples clipped, max sample " << maxsample << "\n";
}

struct Effect
{
    void process(float &left, float &right) {}
};

struct ChainStep
{
    int8_t fxIndices[4]; // fx to apply in sequence, -1 = end
    int8_t count;        // number of valid fx in this chain
};

struct ExecutionPlan
{
    ChainStep chains[4];
    int8_t chainCount;
};

ExecutionPlan buildPlan(const int8_t matrix[4][4])
{
    ExecutionPlan plan{};
    plan.chainCount = 0;

    for (int i = 0; i < 4; ++i)
    {
        ChainStep &step = plan.chains[plan.chainCount];
        step.count = 0;

        for (int j = 0; j < 4; ++j)
        {
            int8_t idx = matrix[i][j];
            if (idx >= 0 && idx < 4)
            {
                step.fxIndices[step.count++] = idx;
            }
        }

        // Only include chains that actually do something
        if (step.count > 0)
        {
            ++plan.chainCount;
        }
    }
    return plan;
}

inline void test_routing(std::vector<std::tuple<int, int, int>> routings)
{
    int8_t matrix[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            matrix[i][j] = -1;
    for (auto &e : routings)
    {
        int fx = std::clamp(std::get<0>(e), 0, 3);
        int fxx = std::clamp(std::get<1>(e), 0, 3);
        int fxy = std::clamp(std::get<2>(e), 0, 3);
        matrix[fxy][fxx] = fx;
    }
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            std::cout << fmt::format("{:3}", matrix[i][j]);
        }
        std::cout << "\n";
    }

    GrainInsertFX fx[4];

    float inputsignal = 0.0;
    // Build once when matrix changes
    ExecutionPlan plan = buildPlan(matrix);
    double sr = 44100.0;
    for (int i = 0; i < 4; ++i)
    {
        fx[i].prepareInstance(sr, 1);
    }
    GrainInsertFX::ModeInfo mode;
    mode.mainmode = GrainInsertFX::GFXAIRWINDOWS;
    mode.awtype = 3; // ringmod
    fx[0].setMode(mode);
    fx[0].paramvalues[0] = 0.65;
    fx[0].paramvalues[3] = 1.0; // drywet

    mode.awtype = 2; // kwoodroom
    fx[1].setMode(mode);
    fx[1].paramvalues[0] = 0.9; // regen
    fx[1].paramvalues[5] = 0.6; // drywet

    mode.awtype = 11; // glitchshifter
    fx[2].setMode(mode);
    fx[2].paramvalues[0] = 0.7;

    mode.mainmode = GrainInsertFX::GFXSSTFILTER;
    mode.sstmodel = sst::filtersplusplus::FilterModel::CytomicSVF;
    mode.sstconfig = sst::filtersplusplus::ModelConfig{sst::filtersplusplus::Passband::LP};
    fx[3].setMode(mode);
    fx[3].paramvalues[1] = 0.8;
    xenakios::Envelope cutoffenv{{{0.0, 50.0}, {5.0, -12.0}, {10.0, 50.0}}};
    xenakios::Envelope oscpitchenv{{{0.0, 60.0}, {3.0, 60.5}, {5.5, 24.0}, {10.0, 84.0}}};
    unsigned int outlen = 10.0 * sr;
    choc::buffer::ChannelArrayBuffer<float> outbuf{2, outlen};
    outbuf.clear();
    choc::audio::AudioFileProperties props;
    props.bitDepth = choc::audio::BitDepth::float32;
    props.numChannels = 2;
    props.sampleRate = sr;
    choc::audio::WAVAudioFileFormat<true> wavformat;
    auto writer = wavformat.createWriter("matrixout.wav", props);
    sst::basic_blocks::dsp::EBSaw<> oscillator;
    oscillator.setSampleRate(sr);

    for (int samplecounter = 0; samplecounter < outlen; ++samplecounter)
    {
        if (samplecounter % 32 == 0)
        {
            double tpos = samplecounter / sr;
            fx[3].paramvalues[0] = cutoffenv.getValueAtPosition(tpos);
            for (int i = 0; i < 4; ++i)
            {
                fx[i].prepareBlock();
            }
            float pitch = oscpitchenv.getValueAtPosition(tpos);
            float hz = 440.0 * std::pow(2.0, (1.0 / 12) * (pitch - 69));
            oscillator.setFrequency(hz);
        }

        float outputsignal_left = 0.0f;
        float outputsignal_right = 0.0f;
        float gain = std::fmod(1.0 / sr * 2.0 * samplecounter, 1.0);
        gain = gain * gain * gain;
        gain = sst::basic_blocks::dsp::blackman(samplecounter % 11025, 11025);
        inputsignal = 0.5 * gain * oscillator.step();
        for (int i = 0; i < plan.chainCount; ++i)
        {
            const ChainStep &chain = plan.chains[i];
            float l = inputsignal, r = inputsignal;

            for (int j = 0; j < chain.count; ++j)
            {
                fx[chain.fxIndices[j]].processStereo(l, r);
            }

            outputsignal_left += l;
            outputsignal_right += r;
        }
        if (plan.chainCount == 0)
        {
            outputsignal_left = inputsignal;
            outputsignal_right = inputsignal;
        }
        outbuf.getSample(0, samplecounter) = outputsignal_left;
        outbuf.getSample(1, samplecounter) = outputsignal_right;
        if (samplecounter % 32 == 0)
        {
            for (int i = 0; i < 4; ++i)
            {
                fx[i].concludeBlock();
            }
        }
    }
    writer->appendFrames(outbuf.getView());
}

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

struct DegradeEngine
{
    enum DistortMode
    {
        DM_CLIP,
        DM_FOLD,
        DM_WRAP
    };
    float sr = 0.0f;
    float inputgain = 1.0f;
    float bias = 0.0f;
    float outputgain = 1.0f;
    DistortMode dmode{DM_FOLD};
    void prepare(double samplerate, int blocksize) { sr = samplerate; }
    void process(float &sample)
    {
        sample += bias;
        sample *= inputgain;
        if (dmode == DM_FOLD)
        {
            sample = reflect_value_no_loop(-1.0f, sample, 1.0f);
        }
        else if (dmode == DM_CLIP)
        {
            sample = std::clamp(sample, -1.0f, 1.0f);
        }
        sample *= outputgain;
    }
};

inline float linpol(std::span<float> table, float inputvalue)
{
    int ind0 = inputvalue;
    int ind1 = ind0 + 1;
    if (ind1 >= table.size())
        ind1 = table.size() - 1;
    float frac = inputvalue - (int)inputvalue;
    float y0 = table[ind0];
    float y1 = table[ind1];
    return y0 + (y1 - y0) * frac;
}

inline void test_degrade()
{
    std::array<DegradeEngine, 2> engines;
    float sr = 44100.0f;
    for (auto &e : engines)
    {
        e.prepare(sr, granul_block_size);
        e.outputgain = 0.75;
    }
    std::array<float, 512> inputgainseps;
    xenakios::Xoroshiro128Plus rng;
    float x = 0.0f;
    for (auto &e : inputgainseps)
    {
        e = x;
        x += rng.nextHypCos(0.0, 2.0);
        x = std::clamp(x, -12.0f, 12.0f);
    }
    unsigned int numoutframes = 5.0 * sr;
    choc::buffer::ChannelArrayBuffer<float> buf(2, numoutframes);
    buf.clear();
    choc::audio::AudioFileProperties props;
    props.numChannels = 2;
    props.bitDepth = choc::audio::BitDepth::float32;
    props.sampleRate = sr;
    choc::audio::WAVAudioFileFormat<true> wavformat;
    auto writer = wavformat.createWriter("degrade.wav", props);
    xenakios::Envelope inputgain_env{{{0.0, -12.0}, {2.5, 48.0}, {5.0, 0.0}}};
    xenakios::Envelope channelsep_env{{{0.0, 0.0}, {2.5, 1.0}, {5.0, 0.0}}};
    xenakios::Envelope bias_env{{{0.0, 0.0}, {4.0, 12.0}, {5.0, 0.0}}};
    for (int i = 0; i < numoutframes; ++i)
    {
        if (i % 32 == 0)
        {
            double tpos = i / sr;
            auto ingain = inputgain_env.getValueAtPosition(tpos);
            // ingain = 12.0;
            float ingainsep = 0.0f; // channelsep_env.getValueAtPosition(tpos);
            ingainsep = linpol(inputgainseps, ingainsep * (inputgainseps.size() + 0));
            float ingainleft = std::clamp(ingain + ingainsep, -96.0, 96.0);
            engines[0].inputgain = xenakios::decibelsToGain(ingainleft);
            float ingainright = std::clamp(ingain - ingainsep, -96.0, 96.0);
            engines[1].inputgain = xenakios::decibelsToGain(ingainright);
            float bias = bias_env.getValueAtPosition(tpos);
            engines[0].bias = xenakios::decibelsToGain(bias);
            engines[1].bias = xenakios::decibelsToGain(bias);
        }
        float sample = std::sin(2 * M_PI / sr * i * 64.0);
        float outsample = sample;
        engines[0].process(outsample);
        buf.getSample(0, i) = outsample;
        outsample = sample;
        engines[1].process(outsample);
        buf.getSample(1, i) = outsample;
    }
    writer->appendFrames(buf.getView());
}

int main(int argc, char **argv)
{
    test_degrade();
    // test_nephos_render();
    return 0;
    if (argc > 1)
    {
        std::vector<std::tuple<int, int, int>> routings;
        if (std::string(argv[1]) != "-")
        {
            for (int i = 1; i < argc; ++i)
            {
                std::string token = argv[i];
                if (token.size() == 3)
                {
                    int fxindex = token[0] - '0';
                    int fxx = token[1] - '0';
                    int fxy = token[2] - '0';
                    routings.emplace_back(fxindex, fxx, fxy);
                }
            }
            for (auto &e : routings)
            {
                std::cout << fmt::format("{} -> {},{}\n", std::get<0>(e), std::get<1>(e),
                                         std::get<2>(e));
            }
        }
        test_routing(routings);
    }

    // test_colored_noise();
    return 0;
}
