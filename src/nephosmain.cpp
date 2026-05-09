#include "audio/choc_AudioFileFormat.h"
#include "audio/choc_SampleBuffers.h"
#include "granularsynth.h"
#include <print>
#include "audio/choc_AudioFileFormat_WAV.h"
#include "../Common/xap_breakpoint_envelope.h"
#include <chrono>

int main()
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
