#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_audio_formats/juce_audio_formats.h"
#include "juce_dsp/juce_dsp.h"
#include <span>
#include <fstream>
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
            double f = (k == 0) ? freqBins[1] * 0.5 : freqBins[k];
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
    juce::File infile(R"(E:\MusicAudio\sourcesamples\OvenNarinaa\ovinarina02.wav)");
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

int main() { test_spectral_descriptors(); }
