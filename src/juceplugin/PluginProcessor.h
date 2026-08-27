#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include <unordered_map>
#include "../granularsynth.h"
#include "clap/id.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"
#include "containers/choc_SingleReaderMultipleWriterFIFO.h"
#include "containers/choc_Value.h"
#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_core/juce_core.h"
#include "juce_dsp/juce_dsp.h"
#include "threading/choc_SpinLock.h"
#include "xap_slider.h"
#include "audiovisualizercomponent.h"
#include "inputanalyzer.h"
#include "sqlite_helpers.h"

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
        OP_PARAMREMOTE,
        OP_RESET_MODULATORS,
        OP_RANDOMSOURCES,
        OP_TUNING
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

struct RemoteControlMessage
{
    uint32_t chan = 1;
    uint32_t src = CLAP_INVALID_ID;
    float value = 0.0f;
};

struct MIDIBinding
{
    uint32_t midichan = 1;
    uint32_t midicc = CLAP_INVALID_ID;
    uint32_t target_param = CLAP_INVALID_ID;

    std::pair<float, float> par_range{0.0f, 0.0f};
    std::function<float(float)> mapfunction;
    int mapfunctionid = 0;
    enum NonParamAction
    {
        NPA_NONE,
        NPA_GRAINMANUALTRIG,
        NPA_RESETMODULATORS,
        NPA_LOADSNAP0108,
        NPA_LOADSNAP0916,
        NPA_LOADPREVSNAP,
        NPA_LOADNEXTSNAP
    };
    NonParamAction npa = NPA_NONE;
};

enum StateIgnoreFlags
{
    SIF_MASTERVOLUME = 1 << 0,
    SIF_AMBISONICORDER = 1 << 1,
    SIF_MODULATIONROUTINGS = 1 << 2,
    SIF_DASHBOARDSETTING = 1 << 3,
    SIF_MIDIBINDINGS = 1 << 4
};

class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
  public:
    static constexpr size_t maxNumSnapshots = 64;
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
    void handleMIDICCMessage(int channel, int ccnumber, float ccvalue);
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
    // This is multiple writer because remote messages may be coming from GUI macro knobs or MIDI in
    // the audio thread. In the future we may also handle OSC messages, which
    // would arrive in yet another thread.
    choc::fifo::SingleReaderMultipleWriterFIFO<RemoteControlMessage> rc_fifo;
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

    void loadPreset(int64_t presetID);
    void loadSnapShot(int index);
    void saveSnapShot(int index, choc::value::ValueView state);
    std::unordered_map<int, int64_t> snapIndexToPresetID;
    int64_t factoryResetID = -1;

    std::vector<MacroKnobBinding> macroBindings;

    std::vector<MIDIBinding> midiBindings;
    std::atomic<uint32_t> midiLearnParam{CLAP_INVALID_ID};
    std::atomic<uint32_t> midiLearnAction{MIDIBinding::NPA_NONE};
    void removeMIDIAssignmentForParam(uint32_t parid);
    void removeMidiAssignmentForAction(uint32_t action);
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
    std::atomic<bool> modulationAnalyzerEnabled{false};
    // can be resetted back to false from the GUI
    std::atomic<bool> corruptAudioDetected{false};
    // for testing
    std::atomic<bool> corruptAudioOnPurpose{false};
    void processRemoteControlMessages();
    // the sqlite file path obviously has to be eventually dynamically generated or something
#ifdef JUCE_MAC
    SqliteDb presetsDataBase{
        R"(/Users/teemu/codeprojects/2026/nephos/granulatorpresets/presets.dat)"};
#else
    SqliteDb presetsDataBase{R"(C:\develop\nephos\granulatorpresets\presets.dat)"};
#endif

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
