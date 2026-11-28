#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_audio_formats/juce_audio_formats.h"
#include "juce_dsp/juce_dsp.h"
#include "juce_graphics/juce_graphics.h"
#include <span>
#include <execution>
#include <fstream>
#include "audio/choc_AudioFileFormat_WAV.h"
#include "audio/choc_SampleBuffers.h"
#include <random>
#include "sst/basic-blocks/dsp/FollowSlewAndSmooth.h"
#include "sst/basic-blocks/dsp/FastMath.h"
#include "Tunings.h"
#include "../../Common/xen_ambisonics.h"
#include "sst/basic-blocks/dsp/EllipticBlepOscillators.h"

inline double quantize_pitch_binary(Tunings::Tuning &tuning, double sourcepitch, double threshold,
                                    int tuning_offset)
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

template <typename T> struct SplitOnePoleFilter
{
    T out = 0.0f;
    T attackCoef{};
    T releaseCoef{};
    T step(T dt, T in)
    {
        T a = (in > out) ? attackCoef : releaseCoef;
        out += a * (in - out);
        return out;
    }
    void setRiseFall(T samplerate, T r, T f)
    {
        T dt = 1.0 / samplerate;
        attackCoef = 1 - exp(-dt / r);
        releaseCoef = 1 - exp(-dt / f);
    }
};

template <typename T = float> struct TSlewLimiter
{
    T out = 0.f;
    T rise = 0.f;
    T fall = 0.f;

    void reset() { out = 0.f; }

    void setRiseFall(T rise_, T fall_)
    {
        rise = rise_;
        fall = fall_;
    }
    T process(T deltaTime, T in)
    {
        out = std::clamp(in, out - fall * deltaTime, out + rise * deltaTime);
        return out;
    }
};

/*
// magnitude spectrum X[k], frequencies f[k], k = 0..N/2

double sumMag = 0, sumMagF = 0;
for (k) { sumMag += X[k]; sumMagF += X[k] * f[k]; }
double centroid = sumMagF / sumMag;

double sumDev2 = 0;
for (k) { double d = f[k] - centroid; sumDev2 += X[k] * d * d; }
double spread = sqrt(sumDev2 / sumMag);   // "spectral deviation"

double sumDev3 = 0, sumDev4 = 0;
for (k) {
    double d = f[k] - centroid;
    sumDev3 += X[k] * d*d*d;
    sumDev4 += X[k] * d*d*d*d;
}
double skewness = (sumDev3 / sumMag) / pow(spread, 3);
double kurtosis = (sumDev4 / sumMag) / pow(spread, 4) - 3.0; // excess kurtosis

// Flatness: geometric mean / arithmetic mean
double logSum = 0;
for (k) logSum += log(X[k] + 1e-12);
double geoMean = exp(logSum / N);
double arithMean = sumMag / N;
double flatness = geoMean / arithMean;

// Rolloff (e.g. 85%)
double target = 0.85 * sumMag;
double cum = 0; int rolloffBin = 0;
for (k) { cum += X[k]; if (cum >= target) { rolloffBin = k; break; } }
double rolloff = f[rolloffBin];

// Flux (needs previous frame's spectrum X_prev[k])
double flux = 0;
for (k) { double d = X[k] - X_prev[k]; flux += d * d; }
flux = sqrt(flux);
*/

class SpectralDescriptors
{
  public:
    void prepare(int fftSizeIn, double sampleRateIn)
    {
        fftSize = fftSizeIn;
        samplerate = sampleRateIn;
        numBins = fftSize / 2 + 1;

        freqBins.resize(numBins);
        logFreqBins.resize(numBins);
        for (int k = 0; k < numBins; ++k)
        {
            freqBins[k] = (float)(k * samplerate / fftSize);
            // k=0 (DC) is undefined in log space; floor it to avoid -inf
            double f = (k == 0) ? (0.5 * samplerate / fftSize) : freqBins[k];
            logFreqBins[k] = (float)std::log2(f / reference_frequency);
        }
        prevMagnitude.assign(numBins, 0.0f);
    }
    // fftData must be the buffer you passed to performFrequencyOnlyForwardTransform,
    // sized 2 * fftSize, with the first numBins entries holding magnitudes.
    void processFrame(const float *mag)
    {
        double sumMag = 0.0, sumMagF = 0.0;
        for (int k = 1; k < numBins; ++k)
        {
            sumMag += mag[k];
            sumMagF += mag[k] * logFreqBins[k];
        }
        const double eps = 0.000001;
        logCentroid_ = sumMagF / (sumMag + eps); // now directly in octaves re 440Hz

        double sumDev2 = 0.0;
        for (int k = 1; k < numBins; ++k)
        {
            double d = logFreqBins[k] - logCentroid_;
            sumDev2 += mag[k] * d * d;
        }
        logSpread_ = std::sqrt(sumDev2 / (sumMag + eps)); // now directly in octaves
        return;
        double spread3 = spread_ * spread_ * spread_;
        double spread4 = spread3 * spread_;
        skew_ = (spread_ > eps) ? (spread3 / (sumMag + eps)) / spread3 : 0.0;
        kurt_ = (spread_ > eps) ? (spread4 / (sumMag + eps)) / spread4 - 3.0 : 0.0;

        double logSum = 0.0;
        for (int k = 0; k < numBins; ++k)
            logSum += std::log(mag[k] + eps);
        double geoMean = std::exp(logSum / numBins);
        double arithMean = sumMag / numBins;
        flat_ = geoMean / (arithMean + eps);

        double target = 0.85 * sumMag;
        double cum = 0.0;
        rolloff_ = freqBins[numBins - 1];
        for (int k = 0; k < numBins; ++k)
        {
            cum += mag[k];
            if (cum >= target)
            {
                rolloff_ = freqBins[k];
                break;
            }
        }

        double fluxSum = 0.0;
        for (int k = 0; k < numBins; ++k)
        {
            double d = mag[k] - prevMagnitude[k];
            fluxSum += d * d;
        }
        flux_ = std::sqrt(fluxSum);

        std::copy(mag, mag + numBins, prevMagnitude.begin());
    }
    static constexpr double reference_frequency = 440.0;
    double getCentroid() const { return logCentroid_; }
    double getSpread() const { return logSpread_; }
    double getSkewness() const;
    double getKurtosis() const;
    double getFlatness() const;
    double getRolloff(float percentage = 0.85f) const;
    double getFlux() const; // needs previous frame internally

  private:
    std::vector<double> freqBins;
    std::vector<float> prevMagnitude;
    std::vector<double> logFreqBins;
    double centroid_, spread_, skew_, kurt_, flat_, rolloff_, flux_ = 0.0;
    double logCentroid_ = 0.0;
    double logSpread_ = 0.0f;
    double samplerate = 0;
    size_t fftSize = 0;
    size_t numBins = 0;
};

inline void test_spectral_descriptors()
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    juce::File infile(R"(E:\MusicAudio\sourcesamples\test_signals\sweep3.wav)");
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(infile));
    const size_t fftOrder = 11;
    const size_t fftSize = 1 << fftOrder;
    juce::dsp::FFT fft(fftOrder); // e.g. order 11 = 2048 samples
    juce::dsp::WindowingFunction<float> window(fftSize, juce::dsp::WindowingFunction<float>::hann);

    juce::AudioBuffer<float> block(1, fftSize);
    int hopSize = fftSize / 2;
    SpectralDescriptors descriptors;
    descriptors.prepare(fftSize, reader->sampleRate);
    std::ofstream csvfile("descriptors.csv");
    // csvfile << "time,centroid,spread,skewness,kurtosis,flatness,rolloff,flux\n";
    csvfile << "time,rmslevel,centroid,spread\n";
    for (int pos = 0; pos + fftSize < reader->lengthInSamples; pos += hopSize)
    {
        reader->read(&block, 0, fftSize, pos, true, true);
        double level = block.getRMSLevel(0, 0, fftSize);
        level = juce::Decibels::gainToDecibels(level);
        std::vector<float> fftData(fftSize * 2, 0.0f);
        std::copy(block.getReadPointer(0), block.getReadPointer(0) + fftSize, fftData.begin());

        window.multiplyWithWindowingTable(fftData.data(), fftSize);
        fft.performFrequencyOnlyForwardTransform(fftData.data());

        descriptors.processFrame(fftData.data()); // only first fftSize/2 bins are meaningful
        csvfile << (double)pos / reader->sampleRate << "," << level << ","
                << descriptors.getCentroid() << "," << descriptors.getSpread() << "\n";
    }
}

struct ImageSynth
{
    static constexpr size_t blocksize = 64;
    struct Oscillator
    {
        sst::basic_blocks::dsp::EBTriToSaw<> ebosc;
        double phase = 0.0;
        double samplerate = 0.0;
        double phaseinc = 0.0;
        float outgain = 0.0f;
        bool active = false;
        alignas(16) float ambicoeffs[64] = {0.0f};
        alignas(32) float output_buffer[blocksize] = {0.0f};
        // TSlewLimiter<float> gainslew;
        SplitOnePoleFilter<float> gainfilter;
        void reset(double sr, double frequency_hz, double ogain, double initphase)
        {
            samplerate = sr;
            ebosc.setSampleRate(sr);
            ebosc.setInitialPhase(initphase);
            ebosc.setFrequency(frequency_hz);
            ebosc.setWidth(1.0);
            phase = initphase;
            phaseinc = (M_PI * 2) / sr * frequency_hz;
            outgain = ogain;
            // gainslew.reset();
            // gainslew.setRiseFall(1.0 / 0.001f, 1.0 / 0.1f);
            gainfilter.setRiseFall(sr, 0.001, 0.01);
        }
        void process_block_to_owned_buffer(float gain, bool &all_silence)
        {
            process_block(output_buffer, gain, all_silence);
        }
        void process_block(float *__restrict buffer, float gain, bool &all_silence)
        {
            const float cut_th = juce::Decibels::decibelsToGain(-72.0);
            float invsr = 1.0 / samplerate;
            int silence_counter = 0;
            for (int i = 0; i < blocksize; ++i)
            {
                float smoothedgain = gainfilter.step(invsr, gain);
                if (smoothedgain >= cut_th)
                {
                    float s = ebosc.step() * smoothedgain;
                    buffer[i] = s;
                }
                else
                {
                    buffer[i] = 0.0f;
                    ++silence_counter;
                }
            }
            if (silence_counter == blocksize)
            {
                all_silence = true;
            }
        }
    };
    static constexpr size_t num_oscillators = 600;
    std::array<Oscillator, num_oscillators> oscillators;
    std::array<int, num_oscillators> oscillator_indices;
    juce::Image sourceimage;
    juce::Image azimuth_image;
    std::minstd_rand0 rng;
    double samplerate = 0.0;
    double phase = 0.0;
    double out_dur = 0.0;
    enum ImageTarget
    {
        IT_VOLUME,
        IT_AZIMUTH
    };
    void set_image(juce::Image img) { sourceimage = img; }
    void load_image_from_file(juce::String path, int whichtarget)
    {
        auto temp = juce::ImageFileFormat::loadFrom(path);
        if (temp.isValid())
        {
            if (whichtarget == IT_VOLUME)
                sourceimage = temp;
            if (whichtarget == IT_AZIMUTH)
                azimuth_image = temp;
        }
        else
            std::cout << "could not load image file\n";
    }
    void prepare(double sample_rate, double outduration, Tunings::Tuning &tuning,
                 double tuning_amount)
    {
        std::iota(oscillator_indices.begin(), oscillator_indices.end(), 0);
        out_dur = outduration;
        samplerate = sample_rate;
        std::uniform_real_distribution<float> phasedist{0.0f, 1.0f};
        std::uniform_real_distribution<float> azimuthdist{-180.0f, 180.0f};
        for (int i = 0; i < oscillators.size(); ++i)
        {
            double pitch = juce::jmap<double>(i, 0, oscillators.size() - 1, 60.0, -36.0);
            double qpitch = quantize_pitch_binary(tuning, pitch + 60.0, 1.0, 0) - 60.0;
            pitch = (1.0 - tuning_amount) * pitch + tuning_amount * qpitch;
            double hz = 262.0 * std::pow(2.0, 1.0 / 12 * pitch);
            double gaindb = juce::jmap<double>(pitch, -36.0, 60.0, -12.0, -20.0);
            double gain = juce::Decibels::gainToDecibels(gaindb) * (1.0 / 300);
            oscillators[i].reset(samplerate, hz, gain, phasedist(rng));
            float azi = juce::jmap<double>(i, 0, oscillators.size() - 1, -180.0, 180.0);
            azi = degreesToRadians(azi);
            float x, y, z;
            sphericalToCartesian(azi, 0.0f, x, y, z);
            SHEval3(x, y, z, oscillators[i].ambicoeffs);
            float *__restrict acoefs = oscillators[i].ambicoeffs;
            for (int j = 0; j < 16; ++j)
                acoefs[j] *= n3d2sn3d[j];
        }
    }
    void process_block(float **__restrict buffer)
    {
        alignas(32) float sumbuf[blocksize][64] = {0.0};
        alignas(32) float voicebuf[blocksize];
        int outdursamples = out_dur * samplerate;
        int imgcolumn = (double)sourceimage.getWidth() / outdursamples * phase;
        imgcolumn = std::clamp(imgcolumn, 0, sourceimage.getWidth() - 1);
        phase += blocksize;
        std::for_each(std::execution::par, oscillator_indices.begin(), oscillator_indices.end(),
                      [&](std::size_t i) {
                          // work with index i
                      });
        for (int j = 0; j < oscillators.size(); ++j)
        {
            float gain = sourceimage.getPixelAt(imgcolumn, j).getBrightness();
            gain = gain * gain;
            bool was_silence = false;
            oscillators[j].process_block(voicebuf, gain, was_silence);
            if (!was_silence)
            {

                float azimuth = azimuth_image.getPixelAt(imgcolumn, j).getBrightness();
                azimuth = juce::jmap<float>(azimuth, 0.0f, 1.0f, -180.0f, 180.0f);
                azimuth = degreesToRadians(azimuth);
                float x, y, z;
                sphericalToCartesian(azimuth, 0.0f, x, y, z);
                alignas(32) float updatedcoeffs[16];
                SHEval3(x, y, z, updatedcoeffs);
                for (int ch = 0; ch < 16; ++ch)
                    updatedcoeffs[ch] *= n3d2sn3d[ch];
                float *__restrict acoefs = oscillators[j].ambicoeffs;
                for (int ch = 0; ch < 16; ++ch)
                {
                    float step = (updatedcoeffs[ch] - acoefs[ch]) / blocksize;
                    float a = acoefs[ch];
                    for (int i = 0; i < blocksize; ++i)
                    {
                        sumbuf[i][ch] += voicebuf[i] * a;
                        a += step;
                    }
                }
                for (int ch = 0; ch < 16; ++ch)
                {
                    acoefs[ch] = updatedcoeffs[ch];
                }
            }
        }
        for (int k = 0; k < 16; ++k)
            for (int i = 0; i < blocksize; ++i)
            {
                buffer[k][i] = sumbuf[i][k] * (1.0 / 8);
            }
    }
    ImageSynth() {}
};

inline void test_image_synth()
{
    juce::ScopedJuceInitialiser_GUI gui_init;
    auto syn = std::make_unique<ImageSynth>();
    double sr = 44100.0;
    double outlenseconds = 30.0;
    int outlensamples = outlenseconds * sr;
    auto scale = Tunings::readSCLFile(
        R"(C:\Users\teemu\Documents\nephos_data\scala_scales\Ethnic\Indian Raga Bhairavi.scl)");
    auto tuning = Tunings::Tuning(scale);
    syn->prepare(sr, outlenseconds, tuning, 0.97);
    syn->load_image_from_file(
        R"(C:\develop\nephos\src\spectral\test_images\img12_more_complex2.png)",
        ImageSynth::IT_VOLUME);
    syn->load_image_from_file(R"(C:\develop\nephos\src\spectral\test_images\azimuth_test_04.png)",
                              ImageSynth::IT_AZIMUTH);
    choc::buffer::ChannelArrayBuffer<float> outbuffer{16, ImageSynth::blocksize};
    choc::audio::AudioFileProperties props;
    props.numChannels = 16;
    props.sampleRate = sr;
    props.bitDepth = choc::audio::BitDepth::float32;
    choc::audio::WAVAudioFileFormat<true> wavformat;
    auto writer = wavformat.createWriter(
        R"(C:\develop\nephos\src\spectral\test_images\img11d_notunetest.wav)", props);
    double benchmarkaccum = 0.0;
    int benchcounter = 0;
    int outcounter = 0;
    juce::ScopedNoDenormals nodenomarmals;
    auto start = std::chrono::high_resolution_clock::now();
    while (outcounter < outlensamples)
    {
        if (outcounter / ImageSynth::blocksize % 512 == 0)
        {
            std::cout << 100.0 / outlensamples * outcounter << "%\r";
        }

        syn->process_block((float **)outbuffer.getView().data.channels);
        writer->appendFrames(outbuffer.getView());
        outcounter += ImageSynth::blocksize;
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "\n";
    double rtratio = outlenseconds / (duration.count() / 1000000.0);
    std::cout << "render took " << duration.count() / 1000000.0 << " seconds, " << rtratio
              << "x realtime\n";
}

int main()
{
    test_image_synth();
    // test_juce_file_native_handle();
    // test_spectral_descriptors();
}
