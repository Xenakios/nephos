#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include "../granularsynth.h"
#include "clap/id.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"
#include "juce_audio_basics/juce_audio_basics.h"
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
        OP_PARAMREMOTE
    };
    OpCode opcode = OP_NOOP;
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

class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
  public:
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
    void processMidiMessages(juce::MidiBuffer &midiMessages);
    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override { return true; };

    const juce::String getName() const override;

    bool acceptsMidi() const override { return true; };
    bool producesMidi() const override { return false; };
    bool isMidiEffect() const override { return false; };
    double getTailLengthSeconds() const override { return 0.0; };

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String &newName) override;

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
