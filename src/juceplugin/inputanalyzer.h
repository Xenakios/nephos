#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "juce_dsp/juce_dsp.h"
#include "clap/id.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"

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

class SpectralModulationAnalyzer
{
  public:
    struct Mode
    {
        int fftOrder;
        int hopSize;
        const char *label;
    };
    static constexpr std::array<Mode, 5> modes{{
        {10, 512, "1024 / 512"},
        {11, 512, "2048 / 512"},
        {12, 512, "4096 / 512"},
        {13, 512, "8192 / 512"},
        {14, 1024, "16384 / 1024"},
    }};
    static constexpr int defaultModeIdx = 1;
    int lastModeIdx = -1;
    int fftOrder = 0; // 2048-point FFT
    int fftSize = 0;
    int hopSize = 0;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;
    juce::dsp::BallisticsFilter<float> envFollower;

    // Circular input buffer, sized generously (a few frames' worth)
    juce::AudioBuffer<float> circularBuffer;
    int circularBufferWritePos = 0;
    int samplesSinceLastAnalysis = 0;
    int circularBufferSize = 0; // headroom

    std::vector<float> fftWorkBuffer; // fftSize * 2, scratch for FFT
    SpectralDescriptors descriptors;

    float latestCentroid{0.0f};
    float latestSpread{0.0f};
    float latestRMS{0.0f};
    // ... etc for other descriptors

    SpectralModulationAnalyzer() { fifo_from_gui.reset(256); }
    void applyMode(int m)
    {
        if (m == lastModeIdx)
            return;
        m = std::clamp(m, 0, 4);
        lastModeIdx = m;
        auto temporder = modes[m].fftOrder;
        auto tempsize = 1 << temporder;
        fftSize = tempsize;
        hopSize = modes[m].hopSize;
        fft = std::make_unique<juce::dsp::FFT>(temporder);
        window = std::make_unique<juce::dsp::WindowingFunction<float>>(
            tempsize, juce::dsp::WindowingFunction<float>::hann);
        circularBufferSize = fftSize * 4;
        circularBuffer.setSize(1, circularBufferSize);
        circularBuffer.clear();
        circularBufferWritePos = 0;
        samplesSinceLastAnalysis = 0;
        fftWorkBuffer.assign(fftSize * 2, 0.0f);
        descriptors.prepare(fftSize, sampleRate);
        latestRMS = 0.0f;
        latestCentroid = 0.0f;
        latestSpread = 0.0f;
    }
    float sampleRate = 0.0f;
    struct ExpansionParams
    {
        float downThreshold = -40.0f; // dB level where downward expansion begins
        float downRatio = 2.0f;       // Ratio below downThreshold (e.g. 2:1)
        float downKneeWidth = 6.0f;   // Soft knee width in dB

        float upThreshold = -10.0f; // dB level where upward expansion begins
        float upRatio = 1.5f;       // Ratio above upThreshold (e.g. 1.5:1)
        float upKneeWidth = 6.0f;   // Soft knee width in dB

        float ceilingDb = 0.0f;   // Hard max ceiling limit (0 dBFS)
        float ceilingKnee = 3.0f; // Soft ceiling transition knee in dB
        float floorDb = -100.0f;  // Floor level (-100 dBFS)
    };
    ExpansionParams expanderParams;
    enum PARAMS
    {
        PAR_ATTACK = 1,
        PAR_RELEASE = 2,
        PAR_DOWNTHRESHOLD = 3,
        PAR_UPTHRESHOLD = 4,
    };
    struct Message
    {
        enum Opcode
        {
            OP_NONE,
            OP_CHANGEPARAM,
            OP_CHANGEFFTMODE
        };
        Opcode opcode = OP_NONE;
        uint32_t parid = CLAP_INVALID_ID;
        int fftmode = CLAP_INVALID_ID;
        double val = 0.0f;
    };
    choc::fifo::SingleReaderSingleWriterFIFO<Message> fifo_from_gui;
    static inline float smoothstep(float x) noexcept
    {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }
    static float envelopeExpand(float db, const ExpansionParams &p) noexcept
    {
        if (db <= p.floorDb)
            return p.floorDb;

        float gainChangeDb = 0.0f;

        // --- 1. Downward Expansion ---
        const float halfDownKnee = p.downKneeWidth * 0.5f;
        const float downKneeStart = p.downThreshold + halfDownKnee;
        const float downKneeEnd = p.downThreshold - halfDownKnee;

        if (db < downKneeStart)
        {
            const float fullDelta = (1.0f - p.downRatio) * (p.downThreshold - db);

            if (p.downKneeWidth > 0.001f && db > downKneeEnd)
            {
                const float t = (downKneeStart - db) / p.downKneeWidth;
                gainChangeDb += fullDelta * smoothstep(t);
            }
            else
            {
                gainChangeDb += fullDelta;
            }
        }

        // --- 2. Upward Expansion ---
        const float halfUpKnee = p.upKneeWidth * 0.5f;
        const float upKneeStart = p.upThreshold - halfUpKnee;
        const float upKneeEnd = p.upThreshold + halfUpKnee;

        if (db > upKneeStart)
        {
            const float fullDelta = (p.upRatio - 1.0f) * (db - p.upThreshold);

            if (p.upKneeWidth > 0.001f && db < upKneeEnd)
            {
                const float t = (db - upKneeStart) / p.upKneeWidth;
                gainChangeDb += fullDelta * smoothstep(t);
            }
            else
            {
                gainChangeDb += fullDelta;
            }
        }

        // Unclamped output level
        float targetDb = db + gainChangeDb;

        // --- 3. Soft Ceiling / Limiting at 0 dBFS ---
        const float ceilingStart = p.ceilingDb - p.ceilingKnee;

        if (targetDb > ceilingStart)
        {
            if (targetDb >= p.ceilingDb)
            {
                // Hard clamp at ceiling
                targetDb = p.ceilingDb;
            }
            else if (p.ceilingKnee > 0.001f)
            {
                // Smoothly ease output into 0 dBFS using hyperbolic tangent (tanh) soft knee
                const float overshoot = targetDb - ceilingStart;
                const float normalizedOvershoot = overshoot / p.ceilingKnee;

                // Compresses the top knee range smoothly toward 0 dBFS
                targetDb = ceilingStart + p.ceilingKnee * std::tanh(normalizedOvershoot);
            }
        }

        // Final safety floor clamp
        return std::max(p.floorDb, targetDb);
    }
    juce::AudioBuffer<float> envfoloutputbuffer;
    void prepareToPlay(double sampleRate_, int samplesPerBlock)
    {
        envfoloutputbuffer.setSize(2, samplesPerBlock);
        envfoloutputbuffer.clear();
        sampleRate = sampleRate_;
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate_;
        spec.maximumBlockSize = samplesPerBlock;
        spec.numChannels = 2;
        envFollower.prepare(spec);
        auto mode = lastModeIdx;
        if (mode == -1)
            mode = defaultModeIdx;
        applyMode(mode);
    }
    void processBlock(juce::AudioBuffer<float> &buffer)
    {
        Message msg;
        while (fifo_from_gui.pop(msg))
        {
            if (msg.opcode == Message::OP_CHANGEPARAM)
            {
                if (msg.parid == PAR_ATTACK)
                    envFollower.setAttackTime(msg.val);
                else if (msg.parid == PAR_RELEASE)
                    envFollower.setReleaseTime(msg.val);
                else if (msg.parid == PAR_DOWNTHRESHOLD)
                    expanderParams.downThreshold = msg.val;
                else if (msg.parid == PAR_UPTHRESHOLD)
                    expanderParams.upThreshold = msg.val;
            }
            if (msg.opcode == Message::OP_CHANGEFFTMODE)
            {
                // we got rid of the CriticalSection use, but applyMode
                // will allocate and deallocate heap memory...
                // however since the fft mode will be changed only infrequently,
                // i suppose this will be fine...
                applyMode(msg.fftmode);
            }
        }
        const int numSamples = buffer.getNumSamples();
        const float *in = buffer.getReadPointer(0); // mono/first channel for analysis

        juce::dsp::AudioBlock<float> oblock{envfoloutputbuffer.getArrayOfWritePointers(), 2,
                                            (size_t)buffer.getNumSamples()};
        juce::dsp::ProcessContextNonReplacing<float> ctx{buffer, oblock};
        envFollower.process(ctx);
        latestRMS = envfoloutputbuffer.getSample(0, 0);
        for (int i = 0; i < numSamples; ++i)
        {
            // latestRMS = envFollower.processSample(0, in[i]);
            // Write incoming sample into circular buffer
            circularBuffer.setSample(0, circularBufferWritePos, in[i]);
            circularBufferWritePos = (circularBufferWritePos + 1) % circularBufferSize;
            ++samplesSinceLastAnalysis;

            // Once we've accumulated a full hop of new samples, analyze
            if (samplesSinceLastAnalysis >= hopSize)
            {
                samplesSinceLastAnalysis = 0;
                analyzeFrame();
            }
        }
    }

    void analyzeFrame()
    {
        std::fill(fftWorkBuffer.begin(), fftWorkBuffer.end(), 0.0f);

        // Read the last fftSize samples out of the circular buffer, in order
        int readPos = (circularBufferWritePos - fftSize + circularBufferSize) % circularBufferSize;
        double levelsum = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            float sample = circularBuffer.getSample(0, readPos);
            // levelsum += sample * sample;
            fftWorkBuffer[i] = sample;
            readPos = (readPos + 1) % circularBufferSize;
        }
        // if (levelsum > 0.0)
        //     latestRMS = std::sqrt(levelsum / fftSize);
        window->multiplyWithWindowingTable(fftWorkBuffer.data(), fftSize);
        fft->performFrequencyOnlyForwardTransform(fftWorkBuffer.data());

        descriptors.processFrame(fftWorkBuffer.data());

        // Publish results for the GUI/message thread to read
        latestCentroid = descriptors.getCentroid();
        latestSpread = descriptors.getSpread();
        // ... etc
    }
};
