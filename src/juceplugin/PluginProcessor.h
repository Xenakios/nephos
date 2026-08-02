#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include "../granularsynth.h"
#include "clap/id.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"
#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_core/juce_core.h"
#include "juce_dsp/juce_dsp.h"
#include "threading/choc_SpinLock.h"
#include "xap_slider.h"
#include "audiovisualizercomponent.h"

inline bool is_debug()
{
#ifdef JUCE_DEBUG
    return true;
#else
    return false;
#endif
}

struct ParameterMessage
{
    uint32_t id = 0;
    float value = 0.0f;
};

struct ThreadMessage
{
    enum OpCode
    {
        OP_NOOP,
        OP_MODROUTING,
        OP_FILTERTYPE,
        OP_STEPSEQUENCER,
        OP_UNLEARNMIDI,
        OP_MIDILEARNRANGE,
        OP_MIDILEARNCURVE,
        OP_PARAMREMOTE
    };
    OpCode opcode = OP_NOOP;
    // note that some of the fields have multiple meanings depending on
    // the opcode. this mess should be fixed at some point somehow...
    int16_t modslot = -1;
    int modsource = -1;
    int modvia = 0;
    int modcurve = 0;
    float modcurvepar0 = 0.0f;
    float depth = 0.0f;
    int moddest = -1;
    int16_t filterindex = -1;
    uint8_t insertmainmode = 0;
    uint8_t awtype = 0;
    sfpp::FilterModel filtermodel;
    sfpp::ModelConfig filterconfig;
    uint32_t parid = CLAP_INVALID_ID;
    uint16_t parremotestatus = XapSlider::RCS_NONE;
};

struct MacroKnobBinding
{
    int dest_type = -1;
    int dest = 0;
    std::optional<std::pair<float, float>> par_range;
    std::string label;
};

struct MIDIBinding
{
    uint32_t midichan = 1;
    uint32_t midicc = CLAP_INVALID_ID;
    uint32_t target_param = CLAP_INVALID_ID;
    std::pair<float, float> par_range{0.0f, 0.0f};
    std::function<float(float)> mapfunction;
    int mapfunctionid = 0;
};

namespace StateIgnoreStrings
{
using namespace std::literals;

static constexpr auto masterVolume = "ignore_param_mastervolume"sv;
static constexpr auto modulationRouting = "ignore_modulationrouting"sv;
static constexpr auto dashboardsettings = "ignore_dashboard"sv;
static constexpr auto ambisonicOrder = "ignore_ambiorder"sv;
static constexpr auto midiBinds = "ignore_midibindings"sv;
} // namespace StateIgnoreStrings

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
    juce::CriticalSection cs;
    SpectralModulationAnalyzer()
    {
        // envFollower.set
    }
    void applyMode(int m)
    {
        juce::ScopedLock locker(cs);
        if (m == lastModeIdx)
            return;
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
    float expander_th = -40.0f;
    static float envelopeExpand(float db, float threshold)
    {
        constexpr float floorDb = -100.0f;
        constexpr float transitionWidth = 12.0f;

        if (db >= threshold)
            return db;

        // Clamp the input signal to the transition region [threshold - 12, threshold]
        const float clampedDb = std::clamp(db, threshold - transitionWidth, threshold);

        // Map from transition region to [floorDb, threshold]
        return juce::jmap<float>(clampedDb, threshold - transitionWidth, threshold, floorDb,
                                 threshold);
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
    void setEnvelopeFollowerParameters(float atta, float rele)
    {
        juce::ScopedLock locker(cs);
        envFollower.setAttackTime(atta);
        envFollower.setReleaseTime(rele);
    }
    void processBlock(juce::AudioBuffer<float> &buffer)
    {
        // yes yes, nasty but will do for now...
        juce::ScopedLock locker(cs);
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

class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
  public:
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
    void handleMIDICCMessage(int channel, int ccnumber, int ccvalue);
    void processMidiMessages(juce::MidiBuffer &midiMessages);
    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; };

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int index) override {}
    const juce::String getProgramName(int index) override { return {}; }
    void changeProgramName(int index, const juce::String &newName) override {}

    void getStateInformation(juce::MemoryBlock &destData) override;
    void setStateInformation(const void *data, int sizeInBytes) override;
    ToneGranulator granulator;
    choc::fifo::SingleReaderSingleWriterFIFO<ThreadMessage> from_gui_fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<ParameterMessage> params_from_gui_fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<ParameterMessage> params_to_gui_fifo;
    choc::fifo::SingleReaderSingleWriterFIFO<ThreadMessage> to_gui_fifo;
    juce::AudioProcessLoadMeasurer perfMeasurer;
    std::atomic<float> cpu_load{0.0f};
    juce::ThreadPool tpool{juce::ThreadPool::Options{"granulatorworker", 1}};
    choc::value::Value getState();
    void setState(choc::value::ValueView state);
    void changeStateImpl(choc::value::ValueView state);
    void sendExtraStatesToGUI();
    std::unordered_map<uint32_t, uint32_t> macroMidiMappings;
    choc::value::Value pendingState;
    choc::threading::SpinLock stateLock;
    std::vector<choc::value::Value> snapshots;

    void loadSnapShot(int index);
    void saveSnapShot(int index, choc::value::ValueView state);
    std::vector<MacroKnobBinding> macroBindings;

    std::vector<MIDIBinding> midiBindings;

    std::atomic<uint32_t> midiLearnParam{CLAP_INVALID_ID};
    void removeMIDIAssignmentForParam(uint32_t parid);
    void setMidiAssignmentParameterRange(uint32_t parid, std::optional<float> minval,
                                         std::optional<float> maxval);
    void setMidiAssignmentMappingCurve(uint32_t parid, int curveid);
    void initMidiBindings();

    void handleMacroKnob(int knobindex, float value, bool is_audio_tread);
    void loadMacroKnobs(std::string filename);
    std::string presetsPath;
    std::string macroKnobsPath;
    double currentSampleRate = 0.0;
    // usually we would not have gui components as audioprocessor members
    // but in this case easier to just do it this way
    juce::AudioVisualiserComponent avisComponent;

    // this is bit of an antipattern to have the AudioProcessor own the component, but oh well...
    // maybe fix this later
    std::unique_ptr<baconpaul::six_sines::ui::SpectrumAnalyzerComponent> baconSpectrum;
    juce::AudioBuffer<float> visualizerAudioBuffer;
    juce::MidiKeyboardState keyboardState;
    SpectralModulationAnalyzer modulationAnalyzer;

  private:
    alignas(32) std::vector<float> workBuffer;
    alignas(32) choc::fifo::SingleReaderSingleWriterFIFO<
        std::array<float, ambisonicOrderNumChannels(maxAmbiSonicOrder)>> buffer_adapter;
    void setStateDirtyHack();
    void init_clouds(ToneGranulator &g);
    std::unordered_map<juce::AudioProcessorParameter *, int> jucepartoindex;
    juce::AudioParameterFloat *dirtyStateParam = nullptr;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
};
