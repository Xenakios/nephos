#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
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
#include "sqlite3.h"

struct SqliteError : std::runtime_error
{
    explicit SqliteError(const std::string &msg) : std::runtime_error(msg) {}
};

class SqliteDb
{
  public:
    explicit SqliteDb(const std::string &path)
    {
        sqlite3 *raw = nullptr;
        if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK)
        {
            std::string err = sqlite3_errmsg(raw);
            sqlite3_close(raw);
            throw SqliteError("Failed to open db: " + err);
        }
        db_.reset(raw);
        sqlite3_exec(db_.get(), "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    }

    sqlite3 *get() const { return db_.get(); }

  private:
    struct Deleter
    {
        void operator()(sqlite3 *p) const { sqlite3_close(p); }
    };
    std::unique_ptr<sqlite3, Deleter> db_;
};

class SqliteStmt
{
  public:
    SqliteStmt() = default;
    SqliteStmt(sqlite3 *db, const std::string &sql)
    {
        sqlite3_stmt *raw = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        {
            throw SqliteError("Failed to prepare: " + std::string(sqlite3_errmsg(db)));
        }
        stmt_.reset(raw);
    }

    sqlite3_stmt *get() const { return stmt_.get(); }

  private:
    struct Deleter
    {
        void operator()(sqlite3_stmt *p) const { sqlite3_finalize(p); }
    };
    std::unique_ptr<sqlite3_stmt, Deleter> stmt_;
};

inline void presetsInitSchema(SqliteDb &db)
{
    const char *sql = R"(
        CREATE TABLE IF NOT EXISTS presets (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            name        TEXT NOT NULL,
            category    TEXT,
            author      TEXT,
            tags        TEXT,
            is_factory  INTEGER DEFAULT 0,
            created_at  INTEGER NOT NULL,
            modified_at INTEGER NOT NULL,
            data_format INTEGER DEFAULT 1,
            data        BLOB NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_presets_category ON presets(category);
        CREATE INDEX IF NOT EXISTS idx_presets_name ON presets(name);
    )";
    char *errMsg = nullptr;
    if (sqlite3_exec(db.get(), sql, nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        std::string msg = errMsg ? errMsg : "unknown error";
        sqlite3_free(errMsg);
        throw SqliteError("Schema init failed: " + msg);
    }
}

inline int64_t insertOrUpdatePreset(SqliteDb &db, const std::string &name,
                                    const std::string &category, bool force_insert,
                                    choc::value::ValueView statedata)
{
    auto sdata = statedata.serialise();
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    SqliteStmt outerstmt;

    SqliteStmt stmt(db.get(), "SELECT id, name, category, data FROM presets WHERE name = ?");
    sqlite3_bind_text(stmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt.get()) != SQLITE_ROW || force_insert)
    {
        // not found, so insert
        outerstmt = SqliteStmt(db.get(), R"(
        INSERT INTO presets (name, category, created_at, modified_at, data)
        VALUES (?, ?, ?, ?, ?)
    )");
        sqlite3_bind_text(outerstmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(outerstmt.get(), 2, category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(outerstmt.get(), 3, now);
        sqlite3_bind_int64(outerstmt.get(), 4, now);
        // SQLITE_TRANSIENT tells sqlite to copy the bytes now, since `data`
        // may go out of scope before the statement executes
        sqlite3_bind_blob(outerstmt.get(), 5, sdata.data.data(),
                          static_cast<int>(sdata.data.size()), SQLITE_TRANSIENT);
    }
    else
    {
        // was already in db, so update
        outerstmt = SqliteStmt(db.get(), R"(
        UPDATE presets
        SET name = ?, category = ?, data = ?, modified_at = ?
        WHERE name = ?
    )");
        sqlite3_bind_text(outerstmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(outerstmt.get(), 2, category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(outerstmt.get(), 3, sdata.data.data(),
                          static_cast<int>(sdata.data.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(outerstmt.get(), 4, now);
        sqlite3_bind_text(outerstmt.get(), 5, name.c_str(), -1, SQLITE_TRANSIENT);
    }

    if (sqlite3_step(outerstmt.get()) != SQLITE_DONE)
    {
        throw SqliteError("Insert failed: " + std::string(sqlite3_errmsg(db.get())));
    }
    return sqlite3_last_insert_rowid(db.get());
}

struct PresetRecord
{
    int64_t id;
    std::string name;
    std::string category;
    std::vector<uint8_t> data;
};

inline std::optional<PresetRecord> presetsLoadPreset(SqliteDb &db, std::string name,
                                                     std::string category)
{
    SqliteStmt stmt(db.get(),
                    "SELECT id, name, category, data FROM presets WHERE name = ? AND category = ?");
    sqlite3_bind_text(stmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, category.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW)
    {
        return std::nullopt; // not found
    }

    PresetRecord rec;
    rec.id = sqlite3_column_int64(stmt.get(), 0);
    rec.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 1));

    if (const unsigned char *catText = sqlite3_column_text(stmt.get(), 2))
        rec.category = reinterpret_cast<const char *>(catText);

    const void *blobPtr = sqlite3_column_blob(stmt.get(), 3);
    int blobSize = sqlite3_column_bytes(stmt.get(), 3);
    rec.data.assign(static_cast<const uint8_t *>(blobPtr),
                    static_cast<const uint8_t *>(blobPtr) + blobSize);

    return rec;
}

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
    std::vector<choc::value::Value> snapshots;

    void loadPreset(std::string name, std::string category);
    void loadSnapShot(int index);
    void saveSnapShot(int index, choc::value::ValueView state);
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
    SqliteDb presetsDataBase{R"(C:\develop\nephos\granulatorpresets\presets.dat)"};

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
