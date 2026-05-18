
#include "audio/choc_SampleBuffers.h"
#include "granularsynth.h"
#include "audio/choc_AudioFileFormat_WAV.h"
#include <chrono>
#include <random>
#include "audio/choc_AudioFileFormat.h"
#include "../Common/xap_breakpoint_envelope.h"
#include "sst/filters/FastTiltNoiseFilter.h"

inline int test_nephos_render()
{
    auto g = std::make_unique<ToneGranulator>();
    double sr = 44100.0;
    g->prepare(sr, {}, 0, 0.002, 0.002);

    unsigned int amborder = 7;
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
    int outlen = sr * 10.0;
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

int main()
{
    test_colored_noise();
    return 0;
}
