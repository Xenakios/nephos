#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "audiovisualizercomponent.h"
#include "clap/id.h"
#include "containers/choc_Value.h"
#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_audio_utils/juce_audio_utils.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"
#include "sqlite_helpers.h"
#include "text/choc_Files.h"
#include "text/choc_JSON.h"
#include <cmath>
#include <cstdint>
#include <exception>
#include <float.h>
#include "../../Common/xap_breakpoint_envelope.h"

void AudioPluginAudioProcessor::setMidiAssignmentMappingCurve(uint32_t parid, int curveid)
{
    for (size_t i = 0; i < midiBindings.size(); ++i)
    {
        const auto &b = midiBindings[i];
        if (b.target_param == parid)
        {
            ThreadMessage msg;
            msg.opcode = ThreadMessage::OP_MIDILEARNCURVE;
            msg.modslot = i;
            msg.modcurve = curveid;
            from_gui_fifo.push(msg);
        }
    }
}

void AudioPluginAudioProcessor::initMidiBindings() { midiBindings.reserve(64); }

void AudioPluginAudioProcessor::setMidiAssignmentParameterRange(uint32_t parid,
                                                                std::optional<float> minval,
                                                                std::optional<float> maxval)
{
    for (size_t i = 0; i < midiBindings.size(); ++i)
    {
        const auto &b = midiBindings[i];
        if (b.target_param != parid)
            continue;
        ThreadMessage msg;
        msg.opcode = ThreadMessage::OP_MIDILEARNRANGE;
        msg.depth = minval.value_or(b.par_range.first);
        msg.modcurvepar0 = maxval.value_or(b.par_range.second);
        msg.modslot = i;
        from_gui_fifo.push(msg);
    }
}

void AudioPluginAudioProcessor::removeMidiAssignmentForAction(uint32_t action)
{
    ThreadMessage msg;
    msg.opcode = ThreadMessage::OP_UNLEARNMIDI;
    msg.awtype = 1;
    msg.parid = action;
    from_gui_fifo.push(msg);
}

void AudioPluginAudioProcessor::removeMIDIAssignmentForParam(uint32_t parid)
{
    ThreadMessage msg;
    msg.opcode = ThreadMessage::OP_UNLEARNMIDI;
    msg.parid = parid;
    from_gui_fifo.push(msg);
}

AudioPluginAudioProcessor::AudioPluginAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::ambisonic(3), true)),
      avisComponent(2)
{
    // init_clouds(granulator);
    try
    {
        presetsInitSchema(presetsDataBase);
    }
    catch (std::exception &ex)
    {
        DBG(ex.what());
    }
    auto snaps = listPresets(presetsDataBase);
    for (auto &e : snaps)
    {
        if (e.category == "snapshot")
        {
            try
            {
                int index = std::stoi(e.name);
                snapIndexToPresetID[index] = e.id;
            }
            catch (std::exception &ex)
            {
                DBG(ex.what());
            }
        }
        else if (e.category == "Factory Presets" && e.name == "Factory Reset")
        {
            factoryResetID = e.id;
        }
    }
    rc_fifo.reset(512);
    initMidiBindings();
    macroBindings.resize(16);
#ifdef JUCE_MAC
    macroKnobsPath = R"(/Users/teemu/codeprojects/2026/nephos/src/macroknobs.json)";
    presetsPath = R"(/Users/teemu/codeprojects/2026/nephos/granulatorpresets/)";
#else
    macroKnobsPath = R"(C:\develop\nephos\src\macroknobs.json)";
    presetsPath = R"(C:\develop\nephos\granulatorpresets\)";
#endif
    loadMacroKnobs(macroKnobsPath);

    snapshots.resize(maxNumSnapshots);

    for (int i = 0; i < maxNumSnapshots; ++i)
    {
        try
        {
            auto fname = fmt::format("{}{}.json", presetsPath, i + 1);
            if (std::filesystem::exists(fname))
            {
                auto jsontxt = choc::file::loadFileAsString(fname);
                auto state = choc::json::parseValue(jsontxt);
                // state.setMember(StateIgnoreStrings::masterVolume, true);
                state.setMember("state_ignore_flags", (int64_t)SIF_MASTERVOLUME |
                                                          SIF_DASHBOARDSETTING |
                                                          SIF_AMBISONICORDER | SIF_MIDIBINDINGS);
                snapshots[i] = state;
            }
        }
        catch (std::exception &ex)
        {
            DBG(i << " error loading state : " << ex.what());
        }
    }
    for (int i = 0; i < 8; ++i)
    {
        macroMidiMappings[21 + i] = i;
        macroMidiMappings[41 + i] = 8 + i;
    }

    buffer_adapter.reset(1024);
    from_gui_fifo.reset(1024);
    params_from_gui_fifo.reset(2048);
    params_to_gui_fifo.reset(2048);
    to_gui_fifo.reset(1024);
    dirtyStateParam =
        new juce::AudioParameterFloat({"dirtystateparam", 1}, "Internal parameter", 0.0, 1.0, 0.0);
    addParameter(dirtyStateParam);
    for (int i = 0; i < 16; ++i)
    {
        juce::String id = "MACRO" + juce::String(i);
        juce::String name = "MACRO " + juce::String(i);
        auto par = new juce::AudioParameterFloat({id, 1}, name, -1.0, 1.0, 0.0);
        addParameter(par);
    }
    // to be decided if we want to actually have the full mirrored paraneters
    /*
    for (int i = 0; i < granulator.parmetadatas.size(); ++i)
    {
        const auto &pmd = granulator.parmetadatas[i];
        juce::String id(pmd.id);
        if (pmd.type == ToneGranulator::pmd::FLOAT)
        {
            auto par = new juce::AudioParameterFloat({id, 1}, pmd.name, pmd.minVal, pmd.maxVal,
                                                     pmd.defaultVal);
            addParameter(par);
            jucepartoindex[(juce::AudioProcessorParameter *)par] = pmd.id;
        }
        if (pmd.type == ToneGranulator::pmd::INT && pmd.discreteValues.size())
        {
            juce::StringArray choices;
            for (auto &e : pmd.discreteValues)
            {
                choices.add(e.second);
            }
            auto par = new juce::AudioParameterChoice({id, 1}, pmd.name, choices, 0);
            addParameter(par);
            jucepartoindex[(juce::AudioProcessorParameter *)par] = pmd.id;
        }
    }
    */
    try
    {
        if (factoryResetID == -1)
            factoryResetID =
                insertPreset(presetsDataBase, "Factory Reset", "Factory Presets", getState());
        else
            updatePreset(presetsDataBase, factoryResetID, "Factory Reset", "Factory Presets",
                         getState());
    }
    catch (std::exception &ex)
    {
        DBG(ex.what());
    }
}

AudioPluginAudioProcessor::~AudioPluginAudioProcessor() {}

void AudioPluginAudioProcessor::saveSnapShot(int index, choc::value::ValueView state)
{
    if (index >= 0 && index < maxNumSnapshots)
    {
        std::lock_guard<choc::threading::SpinLock> locker(stateLock);
        snapshots[index] = state;
        try
        {
            auto found =
                findPresetWithNameCategory(presetsDataBase, std::to_string(index), "snapshot");
            if (found >= 0)
            {
                updatePreset(presetsDataBase, found, std::to_string(index), "snapshot", getState());
            }
            else
            {
                auto inserted =
                    insertPreset(presetsDataBase, std::to_string(index), "snapshot", getState());
                snapIndexToPresetID[index] = inserted;
            }
        }
        catch (std::exception &ex)
        {
            DBG(ex.what());
        }
    }
}

void AudioPluginAudioProcessor::loadPreset(int64_t presetID)
{
    try
    {
        auto dbstate = presetsLoadPreset(presetsDataBase, presetID);
        if (dbstate)
        {
            if (dbstate->data.size() < 1)
                return;
            choc::value::InputData idata{(const uint8_t *)dbstate->data.data(),
                                         (const uint8_t *)dbstate->data.data() +
                                             dbstate->data.size()};
            auto state = choc::value::Value::deserialise(idata);
            state.setMember("state_ignore_flags", (int64_t)SIF_AMBISONICORDER | SIF_MASTERVOLUME |
                                                      SIF_DASHBOARDSETTING | SIF_MIDIBINDINGS);
            setState(state.getView());
        }
        else
        {
            DBG("preset with ID " << presetID << " does not exist");
        }
    }
    catch (std::exception &ex)
    {
        DBG(ex.what());
    }
}

void AudioPluginAudioProcessor::loadSnapShot(int index)
{
    if (index >= 0 && index < maxNumSnapshots)
    {
        auto it = snapIndexToPresetID.find(index);
        if (it != snapIndexToPresetID.end())
        {
            loadPreset(it->second);
            granulator.currentSnapShot = index;
        }

        return;
        auto &state = snapshots[index];
        if (!state.isVoid())
        {
            setState(state);
        }
        granulator.currentSnapShot = index;
    }
}

void AudioPluginAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    DBG("prepareToPlay " << sampleRate << " Hz, " << samplesPerBlock << " max samples per block");
    modulationAnalyzer.prepareToPlay(sampleRate, samplesPerBlock);
    int avisnumchannels = getTotalNumOutputChannels();
    if (getTotalNumOutputChannels() > 2)
        avisnumchannels = 1;
    visualizerAudioBuffer.setSize(avisnumchannels, samplesPerBlock);
    visualizerAudioBuffer.clear();
    avisComponent.setNumChannels(avisnumchannels);
    perfMeasurer.reset(sampleRate, samplesPerBlock);
    // workBuffer.resize(samplesPerBlock * 64);
    workBuffer.resize(granul_block_size * 64);
    granulator.prepare(sampleRate, GranulatorVoice::FR_ALLSERIAL, 0.002f, 0.002f);
    currentSampleRate = sampleRate;
    if (baconSpectrum)
    {
        baconSpectrum->setHostSampleRate(sampleRate);
    }
}

void AudioPluginAudioProcessor::releaseResources() {}

juce::String getBusesLayoutDescription(const juce::AudioProcessor::BusesLayout &layout)
{
    juce::String description;

    auto appendBuses = [&description](const juce::Array<juce::AudioChannelSet> &buses,
                                      juce::String type) {
        description << type << " Buses (" << buses.size() << "):\n";

        if (buses.isEmpty())
        {
            description << "  None\n";
            return;
        }

        for (int i = 0; i < buses.size(); ++i)
        {
            const auto &bus = buses.getReference(i);
            description << "  Bus " << i << ": "
                        << bus.getDescription() // e.g., "Stereo" or "5.1 Surround"
                        << " [" << bus.size() << " channels]\n";
        }
    };

    appendBuses(layout.inputBuses, "Input");
    description << "\n";
    appendBuses(layout.outputBuses, "Output");

    return description;
}

bool AudioPluginAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
    // DBG("Host requested : " << getBusesLayoutDescription(layouts));
    return true;
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::discreteChannels(16))
        return false;

    // This checks if the input layout matches the output layout

    return true;
}

void AudioPluginAudioProcessor::setStateDirtyHack()
{
    dirtyStateParam->beginChangeGesture();
    dirtyStateParam->setValueNotifyingHost(*dirtyStateParam == 0.0f ? 0.001f : 0.0f);
    dirtyStateParam->endChangeGesture();
}

void AudioPluginAudioProcessor::handleMIDICCMessage(int channel, int ccnumber, float ccvalue)
{
    // DBG("handling midi message " << channel << " " << ccnumber << " " << ccvalue);
    auto &mm = granulator.modmatrix;
    uint32_t ccnum = ccnumber;
    if (midiLearnAction != MIDIBinding::NPA_NONE)
    {
        DBG("learning message " << channel << " " << ccnumber << " to trigger action "
                                << (int)midiLearnAction);
        std::erase_if(midiBindings,
                      [this](const MIDIBinding &b) { return b.npa == midiLearnAction; });
        MIDIBinding b;
        b.midichan = channel;
        b.midicc = ccnumber;
        b.npa = (MIDIBinding::NonParamAction)midiLearnAction.load();
        midiBindings.push_back(b);
        midiLearnAction = 0;
        midiLearnParam = CLAP_INVALID_ID;
        return;
    }
    if (midiLearnParam == CLAP_INVALID_ID && midiLearnAction == 0)
    {
        // not in learn mode, so scan for matches
        for (const auto &binding : midiBindings)
        {
            if ((binding.npa == MIDIBinding::NPA_LOADSNAP0108 ||
                 binding.npa == MIDIBinding::NPA_LOADSNAP0916) &&
                binding.midichan == channel && ccvalue >= 64)
            {
                if (ccnum >= binding.midicc && ccnum < binding.midicc + 8)
                {
                    int snaptoload = ccnum - binding.midicc;
                    if (binding.npa == MIDIBinding::NPA_LOADSNAP0916)
                        snaptoload += 8;
                    // DBG("going to load snapshot " << snaptoload << " triggered by CC "
                    //                               << (int)ccnum);
                    loadSnapShot(snaptoload);
                }
            }
            if (binding.npa == MIDIBinding::NPA_LOADNEXTSNAP ||
                binding.npa == MIDIBinding::NPA_LOADPREVSNAP && binding.midichan == channel &&
                    ccvalue >= 64)
            {
                if (!snapshots.empty())
                {
                    const int count = static_cast<int>(snapshots.size());
                    int nextsnap = granulator.currentSnapShot;
                    if (binding.npa == MIDIBinding::NPA_LOADNEXTSNAP)
                        nextsnap = (nextsnap + 1) % count;
                    else if (binding.npa == MIDIBinding::NPA_LOADPREVSNAP)
                        nextsnap = (nextsnap - 1 + count) % count;
                    loadSnapShot(nextsnap);
                }
            }
            if (binding.target_param != CLAP_INVALID_ID && binding.midichan == channel &&
                binding.midicc == ccnum)
            {
                auto md = granulator.idtoparmetadata[binding.target_param];
                float minval = std::clamp(binding.par_range.first, md->minVal, md->maxVal);
                float maxval = std::clamp(binding.par_range.second, md->minVal, md->maxVal);
                float val = juce::jmap<float>(ccvalue, 0.0f, 1.0f, -1.0f, 1.0f);
                if (binding.mapfunction)
                    val = binding.mapfunction(val);
                val = juce::jmap<float>(val, -1.0f, 1.0f, minval, maxval);
                *granulator.idtoparvalptr[binding.target_param] = val;
                ParameterMessage msg;
                msg.id = binding.target_param;
                msg.value = val;
                params_to_gui_fifo.push(msg);
            }
        }
    }
    else
    {
        auto it = granulator.idtoparmetadata.find(midiLearnParam);
        if (it != granulator.idtoparmetadata.end())
        {
            std::erase_if(midiBindings, [this](const MIDIBinding &b) {
                return b.target_param == midiLearnParam;
            });
            auto md = it->second;
            DBG("learning message " << channel << " " << ccnumber << " to set parameter "
                                    << md->name);
            midiBindings.emplace_back(
                MIDIBinding{(uint32_t)channel, ccnum, midiLearnParam, {md->minVal, md->maxVal}});
            midiLearnParam = CLAP_INVALID_ID;
            midiLearnAction = 0;
            ThreadMessage msg;
            msg.opcode = ThreadMessage::OP_PARAMREMOTE;
            to_gui_fifo.push(msg);
        }
    }
    return;
    auto dmit = macroMidiMappings.find(ccnum);
    // if (dmit != macroMidiMappings.end())
    if (false)
    {
        float val = juce::jmap<float>(ccvalue, 0, 127, -1.0f, 1.0f);
        handleMacroKnob(dmit->second, val, true);
        const auto &pmd = granulator.idtoparmetadata[dmit->second];

        // *granulator.idtoparvalptr[dmit->second] = val;
        ParameterMessage msg;
        msg.id = dmit->second;
        msg.value = val;
        // params_to_gui_fifo.push(msg);
    }
    /*
    auto it = granulator.midiCCMap.find(ccnum);
    if (it != granulator.midiCCMap.end())
    {
        granulator.modSourceValues[it->second] =
            juce::jmap<float>(msg.getControllerValue(), 0, 127, 0.0, 1.0);
    }
    */
}

void AudioPluginAudioProcessor::processMidiMessages(juce::MidiBuffer &midiMessages)
{
    for (const auto mm : midiMessages)
    {
        const auto msg = mm.getMessage();
        if (msg.isController())
        {
            RemoteControlMessage rcmsg;
            rcmsg.chan = msg.getChannel();
            rcmsg.src = msg.getControllerNumber();
            rcmsg.value = juce::jmap<float>(msg.getControllerValue(), 0, 127, 0.0f, 1.0f);
            rc_fifo.push(rcmsg);
            // handleMIDICCMessage(msg.getChannel(), msg.getControllerNumber(),
            //                     msg.getControllerValue());
        }
        if (msg.isSustainPedalOn())
        {
            granulator.midiNoteModSource.set_sustain(true);
        }
        if (msg.isSustainPedalOff())
        {
            granulator.midiNoteModSource.set_sustain(false);
        }
        const bool midinotesaremodulation = true;
        if (midinotesaremodulation)
        {
            if (msg.isNoteOn())
            {
                granulator.midiNoteModSource.activate_note(msg.getNoteNumber(), msg.getVelocity());
                DBG(granulator.midiNoteModSource.getDebugString());
            }
            if (msg.isNoteOff())
            {
                granulator.midiNoteModSource.deactivate_note(msg.getNoteNumber());
                DBG(granulator.midiNoteModSource.getDebugString());
            }
        }
        else
        {
            int notenumber = msg.getNoteNumber();
            if (msg.isNoteOn())
            {
                granulator.start_cloud(notenumber - 60, notenumber);
            }
            if (msg.isNoteOff())
            {
                granulator.stop_cloud(notenumber);
            }
            if (msg.isAftertouch())
            {
                float at = juce::jmap<float>(msg.getAfterTouchValue(), 0, 127, 0.0f, 1.0f);
                granulator.handle_cloud_aftertouch(msg.getNoteNumber(), at);
            }
        }
    }
}

void AudioPluginAudioProcessor::processRemoteControlMessages()
{
    RemoteControlMessage msg;
    while (rc_fifo.pop(msg))
    {
        handleMIDICCMessage(msg.chan, msg.src, msg.value);
    }
}

void AudioPluginAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                             juce::MidiBuffer &midiMessages)
{
    juce::AudioProcessLoadMeasurer::ScopedTimer perftimer(perfMeasurer, buffer.getNumSamples());
    double cpu_bench_t0 = juce::Time::getMillisecondCounterHiRes();

    {
        std::lock_guard<choc::threading::SpinLock> locker(stateLock);
        if (!pendingState.isVoid())
        {
            double t0 = juce::Time::getMillisecondCounterHiRes();
            changeStateImpl(pendingState);
            pendingState = choc::value::Value();
            granulator.reset_lfos();
            granulator.reset_step_sequencers();
            sendExtraStatesToGUI();
            double t1 = juce::Time::getMillisecondCounterHiRes();
            DBG("state change took " << t1 - t0 << " milliseconds");
        }
    }
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());
    keyboardState.processNextMidiBuffer(midiMessages, 0, buffer.getNumSamples(), true);
    if (modulationAnalyzerEnabled.load())
        modulationAnalyzer.processBlock(buffer);
    float modcentroid = std::clamp(modulationAnalyzer.latestCentroid, -4.0f, 4.0f);
    granulator.modSourceValues[ToneGranulator::AA_CENTROID] =
        juce::jmap<float>(modcentroid, -4.0f, 4.0f, -1.0f, 1.0f);
    float modspread = std::clamp(modulationAnalyzer.latestSpread, 0.0f, 5.0f);
    granulator.modSourceValues[ToneGranulator::AA_SPEAD] =
        juce::jmap<float>(modspread, 0.0f, 5.0f, 0.0f, 1.0f);
    processMidiMessages(midiMessages);
    processRemoteControlMessages();
    bool statechanged = false;
    ThreadMessage msg;
    while (from_gui_fifo.pop(msg))
    {
        if (msg.opcode == ThreadMessage::OP_RESET_MODULATORS)
        {
            granulator.reset_lfos();
            granulator.reset_step_sequencers();
        }
        if (msg.opcode == ThreadMessage::OP_MIDILEARNRANGE)
        {
            midiBindings[msg.modslot].par_range.first = msg.depth;
            midiBindings[msg.modslot].par_range.second = msg.modcurvepar0;
        }
        if (msg.opcode == ThreadMessage::OP_MIDILEARNCURVE)
        {
            midiBindings[msg.modslot].mapfunction = GranulatorModConfig::getCurveOperator(
                GranulatorModConfig::CurveIdentifier{msg.modcurve});
            midiBindings[msg.modslot].mapfunctionid = msg.modcurve;
        }
        if (msg.opcode == ThreadMessage::OP_UNLEARNMIDI)
        {
            if (msg.awtype == 1)
            {
                std::erase_if(midiBindings, [action = msg.parid](const MIDIBinding &b) {
                    return b.npa == action;
                });
            }
            if (msg.awtype == 0 && msg.parid != CLAP_INVALID_ID)
            {
                std::erase_if(midiBindings, [parid = msg.parid](const MIDIBinding &b) {
                    return b.target_param == parid;
                });
            }
            if (msg.awtype == 0 && msg.parid == CLAP_INVALID_ID)
            {
                midiBindings.clear();
            }
            ThreadMessage tmsg;
            tmsg.opcode = ThreadMessage::OP_PARAMREMOTE;
            to_gui_fifo.push(tmsg);
        }
        if (msg.opcode == ThreadMessage::OP_FILTERTYPE && msg.filterindex >= 0 &&
            msg.filterindex < 2)
        {
            granulator.set_filter(msg.filterindex, msg.insertmainmode, msg.awtype, msg.filtermodel,
                                  msg.filterconfig);
            for (size_t i = 0; i < GranulatorVoice::maxParamsPerInsert; ++i)
            {
                ParameterMessage omsg;
                int parid = ToneGranulator::PAR_INSERTAFIRST + 32 * msg.filterindex + i;
                omsg.id = parid;
                omsg.value = *granulator.idtoparvalptr[parid];
                params_to_gui_fifo.push(omsg);
            }
            statechanged = true;
        }

        auto &mm = granulator.modmatrix;
        if (msg.opcode == ThreadMessage::OP_MODROUTING)
        {
            jassert(msg.moddest >= 1);
            auto it = granulator.modRanges.find(msg.moddest);
            if (it != granulator.modRanges.end())
                msg.depth *= it->second;

            mm.rt.updateActiveAt(msg.modslot, true);
            // DBG(msg.modslot << " " << msg.modsource << " " << msg.depth << " " <<
            // msg.moddest);
            mm.rt.updateRoutingAt(msg.modslot,
                                  GranulatorModConfig::SourceIdentifier{(uint32_t)msg.modsource},
                                  GranulatorModConfig::SourceIdentifier{(uint32_t)msg.modvia},
                                  GranulatorModConfig::MyCurve{msg.modcurve, msg.modcurvepar0},
                                  GranulatorModConfig::TargetIdentifier{msg.moddest}, msg.depth);
            if (msg.modvia == 0)
            {
                mm.rt.routes[msg.modslot].sourceVia = std::nullopt;
            }
            mm.m.prepare(mm.rt, granulator.m_sr, granul_block_size);
            ThreadMessage tmsg;
            tmsg.opcode = ThreadMessage::OP_PARAMREMOTE;
            to_gui_fifo.push(tmsg);
            statechanged = true;
        }
    }
    if (statechanged)
    {
        setStateDirtyHack();
        // updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
    }

    ParameterMessage parmsg;
    while (params_from_gui_fifo.pop(parmsg))
    {
        if (parmsg.id > 0)
        {
            *granulator.idtoparvalptr[parmsg.id] = parmsg.value;
            setStateDirtyHack();
        }
    }

    const auto &pars = getParameters();
    for (int i = 0; i < 16; ++i)
    {
        // auto rpar = dynamic_cast<juce::AudioParameterFloat *>(pars[i + 1]);
        // granulator.modSourceValues[ToneGranulator::HOSTPARAMSTART + i] = *rpar;
    }
    alignas(16) std::array<float, ambisonicOrderNumChannels(maxAmbiSonicOrder)> adapter_block;
    std::fill(adapter_block.begin(), adapter_block.end(), 0.0f);
    int procnumoutchs = 0;
    int opos = 0;
    int numoutsamples = buffer.getNumSamples();

    while (buffer_adapter.getUsedSlots() < numoutsamples)
    {
        // ok so this is a bit dodgy, can't be bothered to do input side
        // buffering that adapts to the granulator internal block size
        const int readPos = std::min(opos, numoutsamples - 1);
        float modlevel = modulationAnalyzer.envfoloutputbuffer.getSample(0, readPos);

        opos += granul_block_size;
        modlevel = juce::Decibels::gainToDecibels(modlevel);
        modlevel =
            SpectralModulationAnalyzer::envelopeExpand(modlevel, modulationAnalyzer.expanderParams);
        granulator.modSourceValues[ToneGranulator::AA_LEVEL] =
            juce::jmap<float>(modlevel, -100.0f, 0.0f, 0.0f, 1.0f);

        std::span<float> procspan{workBuffer};
        granulator.process_block(procspan);
        procnumoutchs = granulator.num_out_chans;
        for (int j = 0; j < granul_block_size; ++j)
        {
            for (int i = 0; i < procnumoutchs; ++i)
            {
                float gosa = workBuffer[j * procnumoutchs + i];
                adapter_block[i] = gosa;
            }
            buffer_adapter.push(adapter_block);
        }
    }

    procnumoutchs = granulator.num_out_chans;
    buffer.clear();
    auto channelDatas = buffer.getArrayOfWritePointers();
    bool pushAnalysisData = false;
    if (baconSpectrum && baconSpectrum->visibleAtomic.load())
        pushAnalysisData = true;
    if (totalNumOutputChannels == 2)
    {
        // super simple decode for stereo monitoring, we should probably just bite
        // the bullet and get rid of this completely
        const float midGain = 1.414f;
        for (int j = 0; j < buffer.getNumSamples(); ++j)
        {
            buffer_adapter.pop(adapter_block);
            float m = adapter_block[0];
            float s = adapter_block[1];
            if (corruptAudioOnPurpose && j == 31)
            {
                s = std::nanf("");
            }
            if (std::isfinite(m) && std::isfinite(s))
            {
                m = m * midGain;
                channelDatas[0][j] = std::clamp((m + s) * 0.5f, -1.0f, 1.0f);
                channelDatas[1][j] = std::clamp((m - s) * 0.5f, -1.0f, 1.0f);
                if (pushAnalysisData)
                    baconSpectrum->pushSample(m);
            }
            else
            {
                channelDatas[0][j] = 0.0f;
                channelDatas[1][j] = 0.0f;
                if (pushAnalysisData)
                    baconSpectrum->pushSample(0.0f);
                corruptAudioDetected = true;
            }
        }
        if (corruptAudioDetected)
        {
            buffer.clear();
        }
        // for convenience stereo output, visualize the MS decoded stereo
        avisComponent.pushBuffer(buffer);
    }
    if (totalNumOutputChannels > 2)
    {
        for (int j = 0; j < buffer.getNumSamples(); ++j)
        {
            buffer_adapter.pop(adapter_block);
            for (int i = 0; i < procnumoutchs; ++i)
            {
                // float s = workBuffer[j * procnumoutchs + i];
                float s = adapter_block[i];
                if (i < totalNumOutputChannels)
                {
                    if (std::isfinite(s))
                        channelDatas[i][j] = std::clamp(s, -1.0f, 1.0f);
                    else
                    {
                        channelDatas[i][j] = 0.0f;
                        corruptAudioDetected = true;
                    }
                }
            }
            // if (!corrupt_audio_detected && pushAnalysisData)
            //     baconSpectrum->pushSample(adapter_block[0]);
            // else
            //     baconSpectrum->pushSample(0.0f);
        }
        if (corruptAudioDetected)
        {
            buffer.clear();
        }
        // for ambisonic output, just show the W channel because with increasing ambisonic orders
        // the number of channels explodes and we have just a tiny waveform visualizer at the moment
        jassert(visualizerAudioBuffer.getNumChannels() == 1);
        visualizerAudioBuffer.copyFrom(0, 0, buffer, 0, 0, buffer.getNumSamples());
        avisComponent.pushBuffer(visualizerAudioBuffer);
    }

    jassert(buffer.getNumSamples() > 0);
    double cpu_bench_t1 = juce::Time::getMillisecondCounterHiRes();
    double elapsed_secs = (cpu_bench_t1 - cpu_bench_t0) / 1000.0;
    double max_secs = buffer.getNumSamples() / getSampleRate();
    cpu_load.store(elapsed_secs / max_secs);
}

//==============================================================================

juce::AudioProcessorEditor *AudioPluginAudioProcessor::createEditor()
{
    sendExtraStatesToGUI();
    // return new juce::GenericAudioProcessorEditor(*this);
    return new AudioPluginAudioProcessorEditor(*this);
}

choc::value::Value AudioPluginAudioProcessor::getState()
{
    auto rootstate = choc::value::createObject("state");
    auto mainparams = choc::value::createObject("params");
    const auto &pmds = granulator.parmetadatas;
    for (int i = 0; i < pmds.size(); ++i)
    {
        std::string id = std::to_string(pmds[i].id);
        float v = *granulator.idtoparvalptr[pmds[i].id];
        mainparams.setMember(id, v);
    }
    rootstate.setMember("params", mainparams);
    rootstate.setMember("gvs_timespan", granulator.gvsettings.timespantoshow);

    auto tuningstate = choc::value::createObject("tuning");
    tuningstate.setMember("scala_file", granulator.currentScalaFile);
    rootstate.setMember("tuning", tuningstate);

    auto filterstates = choc::value::createEmptyArray();
    for (int i = 0; i < 2; ++i)
    {
        auto filterstate = choc::value::createObject("filterstate");
        filterstate.setMember("mainmode", (int64_t)granulator.currentInsertConfs[i].mainmode);
        filterstate.setMember("awtype", (int64_t)granulator.currentInsertConfs[i].awtype);
        filterstate.setMember("model", (int64_t)granulator.currentInsertConfs[i].sstmodel);
        filterstate.setMember("pt", (int64_t)granulator.currentInsertConfs[i].sstconfig.pt);
        filterstate.setMember("st", (int64_t)granulator.currentInsertConfs[i].sstconfig.st);
        filterstate.setMember("dt", (int64_t)granulator.currentInsertConfs[i].sstconfig.dt);
        filterstate.setMember("mt", (int64_t)granulator.currentInsertConfs[i].sstconfig.mt);
        filterstates.addArrayElement(filterstate);
    }
    rootstate.setMember("filterstates", filterstates);

    auto rngstates = choc::value::createEmptyArray();
    for (int i = 0; i < granulator.randomModSources.size(); ++i)
    {
        auto rngstate = granulator.randomModSources[i].get_state();
        rngstates.addArrayElement(rngstate);
    }
    rootstate.setMember("trigrandstates", rngstates);

    auto stepseqstates = choc::value::createEmptyArray();
    for (size_t i = 0; i < granulator.stepModSources.size(); ++i)
    {
        auto &ss = granulator.stepModSources[i];
        auto seqstate = choc::value::createObject("seqstate");
        auto seqsteps = choc::value::createEmptyArray();
        for (size_t j = 0; j < 128; ++j)
        {
            seqsteps.addArrayElement(ss.steps[j]);
        }
        seqstate.setMember("steps", seqsteps);
        seqstate.setMember("startstep", ss.loopstartstep);
        seqstate.setMember("looplen", ss.looplen);
        seqstate.setMember("playmode", (int)ss.playmode);
        stepseqstates.addArrayElement(seqstate);
    }
    rootstate.setMember("stepseqstates", stepseqstates);

    auto osctypemap = choc::value::createEmptyArray();
    for (int i = 0; i < granulator.osctypemapping.size(); ++i)
    {
        osctypemap.addArrayElement(granulator.osctypemapping[i]);
    }
    rootstate.setMember("osctypemapping", osctypemap);

    auto auxenvstates = choc::value::createEmptyArray();
    for (int i = 0; i < granulator.voiceaux_envelopes.size(); ++i)
    {
        auto auxenvstate = granulator.voiceaux_envelopes[i].get_state();
        auxenvstates.addArrayElement(auxenvstate);
    }
    rootstate.setMember("auxenvstates", auxenvstates);

    auto grainmodroutings = choc::value::createEmptyArray();
    for (int i = 0; i < GrainEvent::max_grain_mod_slots; ++i)
    {
        auto grainmodrouting = choc::value::createObject("routing");
        grainmodrouting.setMember("source",
                                  (int64_t)granulator.voices[0]->modulation_slots[i].source_id);
        grainmodrouting.setMember("destination",
                                  (int64_t)granulator.voices[0]->modulation_slots[i].target_id);
        grainmodroutings.addArrayElement(grainmodrouting);
    }
    rootstate.setMember("grainmodroutings", grainmodroutings);
    auto modroutings = choc::value::createEmptyArray();
    auto &mm = granulator.modmatrix;
    for (int i = 0; i < GranulatorModConfig::FixedMatrixSize; ++i)
    {
        if (mm.rt.routes[i].active)
        {
            auto routingstate = choc::value::createObject("routing");
            routingstate.setMember("slot", i);
            if (mm.rt.routes[i].source)
                routingstate.setMember("source", (int)(mm.rt.routes[i].source->src));
            if (mm.rt.routes[i].sourceVia)
                routingstate.setMember("via", (int)(mm.rt.routes[i].sourceVia->src));
            routingstate.setMember("depth", mm.rt.routes[i].depth);
            if (mm.rt.routes[i].target)
                routingstate.setMember("dest", (int)(mm.rt.routes[i].target->target));
            if (mm.rt.routes[i].curve)
                routingstate.setMember("curve", mm.rt.routes[i].curve->id);
            modroutings.addArrayElement(routingstate);
        }
    }
    rootstate.setMember("modroutings", modroutings);
    auto midibinds = choc::value::createEmptyArray();
    for (auto &b : midiBindings)
    {
        auto midibind = choc::value::createObject("midibinding");
        midibind.setMember("midicc", (int64_t)b.midicc);
        midibind.setMember("midichan", (int64_t)b.midichan);
        midibind.setMember("targetpar", (int64_t)b.target_param);
        midibind.setMember("targetaction", (int64_t)b.npa);
        midibind.setMember("parmin", b.par_range.first);
        midibind.setMember("parmax", b.par_range.second);
        midibind.setMember("curveid", b.mapfunctionid);
        midibinds.addArrayElement(midibind);
    }
    rootstate.setMember("midibindings", midibinds);
    return rootstate;
}

void AudioPluginAudioProcessor::changeStateImpl(choc::value::ValueView state)
{
    jassert(!juce::MessageManager::getInstance()->isThisTheMessageThread());
    int64_t state_ignore_flags = state["state_ignore_flags"].getWithDefault(0);
    if (state_ignore_flags & SIF_DASHBOARDSETTING)
    {
        granulator.gvsettings.timespantoshow = state["gvs_timespan"].getWithDefault(8.0);
    }
    bool ignoreMidiBindings = state_ignore_flags & SIF_MIDIBINDINGS;
    if (!ignoreMidiBindings && state.hasObjectMember("midibindings"))
    {
        auto binds = state["midibindings"];
        if (binds.size() > 0)
            midiBindings.clear();
        for (int i = 0; i < binds.size(); ++i)
        {
            auto b = binds[i];
            uint32_t cc = b["midicc"].getWithDefault(CLAP_INVALID_ID);
            uint32_t chan = b["midichan"].getWithDefault(1);
            uint32_t parid = b["targetpar"].getWithDefault(CLAP_INVALID_ID);
            uint32_t npa = b["targetaction"].getWithDefault(0);
            if (npa)
            {
                MIDIBinding binding;
                binding.midichan = chan;
                binding.midicc = cc;
                binding.npa = (MIDIBinding::NonParamAction)npa;
                midiBindings.emplace_back(binding);
            }
            else
            {
                auto it = granulator.idtoparmetadata.find(parid);
                if (it != granulator.idtoparmetadata.end())
                {
                    float parmin = b["parmin"].getWithDefault(it->second->minVal);
                    float parmax = b["parmax"].getWithDefault(it->second->maxVal);
                    int curveid = b["curveid"].getWithDefault(0);
                    MIDIBinding binding;
                    binding.midichan = chan;
                    binding.midicc = cc;
                    binding.target_param = parid;
                    binding.par_range = {parmin, parmax};
                    if (curveid > 0)
                    {
                        binding.mapfunctionid = curveid;
                        binding.mapfunction = GranulatorModConfig::getCurveOperator({curveid});
                    }
                    midiBindings.emplace_back(binding);
                }
            }
        }
    }
    if (state.hasObjectMember("tuning"))
    {
        auto tunstate = state["tuning"];
        auto scalapath = tunstate["scala_file"].getWithDefault(std::string(""));
        granulator.load_scala_file(scalapath, true);
    }
    if (state.hasObjectMember("trigrandstates"))
    {
        auto randstates = state["trigrandstates"];
        for (int i = 0; i < randstates.size(); ++i)
        {
            granulator.randomModSources[i].set_state(randstates[i]);
        }
    }
    if (state.hasObjectMember("osctypemapping"))
    {
        auto osctypemap = state["osctypemapping"];
        if (osctypemap.isArray())
        {
            for (int i = 0; i < osctypemap.size(); ++i)
            {
                if (i < granulator.osctypemapping.size())
                {
                    granulator.osctypemapping[i] = osctypemap[i].getWithDefault(i);
                }
            }
        }
    }
    if (state.hasObjectMember("grainmodroutings"))
    {
        auto grainmodroutings = state["grainmodroutings"];
        for (int i = 0; i < grainmodroutings.size(); ++i)
        {
            auto routing = grainmodroutings[i];
            uint32_t src = routing["source"].getWithDefault(CLAP_INVALID_ID);
            uint32_t dest = routing["destination"].getWithDefault(CLAP_INVALID_ID);
            granulator.set_grain_modulation_routing(i, src, dest, true);
        }
    }
    if (state.hasObjectMember("auxenvstates"))
    {
        auto auxenvstates = state["auxenvstates"];
        for (int i = 0; i < auxenvstates.size(); ++i)
        {
            if (i >= granulator.voiceaux_envelopes.size())
                break;
            auto auxenvstate = auxenvstates[i];
            granulator.voiceaux_envelopes[i].set_state(auxenvstate);
        }
    }
    if (state.hasObjectMember("stepseqstates"))
    {
        auto stepseqstate = state["stepseqstates"];
        for (size_t i = 0; i < stepseqstate.size(); ++i)
        {
            if (i >= granulator.stepModSources.size())
                break;
            auto &ss = granulator.stepModSources[i];
            auto seqstate = stepseqstate[(int)i];
            auto steps = seqstate["steps"];
            for (size_t j = 0; j < steps.size(); ++j)
            {
                if (j < 128)
                {
                    StepModSource::Message msg;
                    msg.opcode = StepModSource::Message::OP_SETSTEP;
                    msg.dest = i;
                    msg.ival0 = j;
                    msg.fval0 = steps[(int)j].getWithDefault(0.0f);
                    granulator.fifo.push(msg);
                }
            }
            StepModSource::Message msg;
            msg.opcode = StepModSource::Message::OP_LOOPSTART;
            msg.dest = i;
            msg.ival0 = seqstate["startstep"].getWithDefault(0);
            granulator.fifo.push(msg);
            msg.opcode = StepModSource::Message::OP_LOOPLEN;
            msg.ival0 = seqstate["looplen"].getWithDefault(1);
            granulator.fifo.push(msg);
            msg.opcode = StepModSource::Message::OP_PLAYMODE;
            msg.ival0 = seqstate["playmode"].getWithDefault(0);
            granulator.fifo.push(msg);
        }
    }
    if (state.hasObjectMember("filterstates"))
    {
        auto filterstates = state["filterstates"];
        for (int i = 0; i < filterstates.size(); ++i)
        {
            auto filterstate = filterstates[i];
            if (i < 2)
            {
                sfpp::FilterModel m = (sfpp::FilterModel)filterstate["model"].getWithDefault(0);
                sfpp::ModelConfig conf;
                conf.dt = (decltype(conf.dt))filterstate["dt"].getWithDefault(0);
                conf.st = (decltype(conf.st))filterstate["st"].getWithDefault(0);
                conf.mt = (decltype(conf.mt))filterstate["mt"].getWithDefault(0);
                conf.pt = (decltype(conf.pt))filterstate["pt"].getWithDefault(0);
                int mainmode = filterstate["mainmode"].getWithDefault(0);
                int awtype = filterstate["awtype"].getWithDefault(0);
                granulator.set_filter(i, mainmode, awtype, m, conf);

                ThreadMessage msg;
                msg.opcode = ThreadMessage::OP_FILTERTYPE;
                msg.insertmainmode = mainmode;
                msg.awtype = awtype;
                msg.filterindex = i;
                msg.filtermodel = m;
                msg.filterconfig = conf;
                // from_gui_fifo.push(msg);
            }
        }
    }
    if (state.hasObjectMember("params"))
    {
        auto params = state["params"];
        auto &pars = granulator.parmetadatas;
        bool ignoreMasterVolume = state_ignore_flags & SIF_MASTERVOLUME;
        bool ignoreAmbisonicOrder = state_ignore_flags & SIF_AMBISONICORDER;
        for (int i = 0; i < pars.size(); ++i)
        {
            if (ignoreMasterVolume && pars[i].id == ToneGranulator::PAR_MAINVOLUME)
                continue;
            if (ignoreAmbisonicOrder && pars[i].id == ToneGranulator::PAR_AMBORDER)
                continue;
            std::string id = std::to_string(pars[i].id);
            if (params.hasObjectMember(id))
            {
                float v = params[id].getWithDefault(pars[i].defaultVal);
                ParameterMessage parmsg;
                parmsg.id = pars[i].id;
                parmsg.value = v;
                // params_from_gui_fifo.push(parmsg);
                *granulator.idtoparvalptr[pars[i].id] = v;
            }
        }
    }
    if (state.hasObjectMember("modroutings"))
    {
        auto routings = state["modroutings"];
        auto &mm = granulator.modmatrix;
        for (int i = 0; i < GranulatorModConfig::FixedMatrixSize; ++i)
        {
            mm.rt.updateActiveAt(i, false);
        }
        for (int i = 0; i < routings.size(); ++i)
        {
            auto rstate = routings[i];
            int slot = rstate["slot"].get<int>();
            if (slot >= 0 && slot < GranulatorModConfig::FixedMatrixSize)
            {
                mm.rt.updateActiveAt(slot, true);
                uint32_t src = rstate["source"].getWithDefault(0);
                uint32_t srcvia = rstate["via"].getWithDefault(0);
                int curve = rstate["curve"].getWithDefault(1);
                float d = rstate["depth"].get<float>();
                int dest = rstate["dest"].getWithDefault(1);
                mm.rt.updateRoutingAt(slot, GranulatorModConfig::SourceIdentifier{src},
                                      GranulatorModConfig::SourceIdentifier{srcvia},
                                      GranulatorModConfig::MyCurve{curve},
                                      GranulatorModConfig::TargetIdentifier{dest}, d);
                if (srcvia == 0)
                    mm.rt.routes[slot].sourceVia = std::nullopt;
            }
        }
        mm.m.prepare(mm.rt, granulator.m_sr, granul_block_size);
    }
}

void AudioPluginAudioProcessor::setState(choc::value::ValueView state)
{
    std::lock_guard<choc::threading::SpinLock> locker(stateLock);
    pendingState = state;
}

void AudioPluginAudioProcessor::getStateInformation(juce::MemoryBlock &destData)
{
    auto state = getState();
    auto sdata = state.serialise();
    destData.append(sdata.data.data(), sdata.data.size());
}

void AudioPluginAudioProcessor::setStateInformation(const void *data, int sizeInBytes)
{
    DBG("setStateInformation");
    try
    {
        if (sizeInBytes < 1)
            return;
        choc::value::InputData idata{(const uint8_t *)data, (const uint8_t *)data + sizeInBytes};
        auto state = choc::value::Value::deserialise(idata);
        setState(state.getView());
    }
    catch (std::exception &ex)
    {
        DBG("tonegranulator error restoring state : " << ex.what());
    }

    sendExtraStatesToGUI();
}

void AudioPluginAudioProcessor::sendExtraStatesToGUI()
{
    to_gui_fifo.push(ThreadMessage{ThreadMessage::OP_STEPSEQUENCER});
    to_gui_fifo.push(ThreadMessage{ThreadMessage::OP_RANDOMSOURCES});
    to_gui_fifo.push(ThreadMessage{ThreadMessage::OP_TUNING});
    ThreadMessage msg;
    msg.opcode = ThreadMessage::OP_FILTERTYPE;
    for (int i = 0; i < granulator.currentInsertConfs.size(); ++i)
    {
        msg.filterindex = i;
        msg.insertmainmode = granulator.currentInsertConfs[i].mainmode;
        msg.awtype = granulator.currentInsertConfs[i].awtype;
        msg.filtermodel = granulator.currentInsertConfs[i].sstmodel;
        msg.filterconfig = granulator.currentInsertConfs[i].sstconfig;
        to_gui_fifo.push(msg);
    }

    for (auto &p : granulator.parmetadatas)
    {
        ParameterMessage msg;
        msg.id = p.id;
        msg.value = *granulator.idtoparvalptr[msg.id];
        params_to_gui_fifo.push(msg);
    }
    auto &mm = granulator.modmatrix;
    for (int i = 0; i < GranulatorModConfig::FixedMatrixSize; ++i)
    {
        if (mm.rt.routes[i].source && mm.rt.routes[i].target)
        {
            ThreadMessage msg;
            msg.opcode = ThreadMessage::OP_MODROUTING;
            msg.modslot = i;
            msg.modsource = mm.rt.routes[i].source->src;
            if (mm.rt.routes[i].sourceVia)
                msg.modvia = mm.rt.routes[i].sourceVia->src;
            msg.depth = mm.rt.routes[i].depth;
            msg.moddest = mm.rt.routes[i].target->target;
            if (mm.rt.routes[i].curve)
                msg.modcurve = mm.rt.routes[i].curve->id;
            auto it = granulator.modRanges.find(msg.moddest);
            if (it != granulator.modRanges.end())
                msg.depth /= it->second;
            to_gui_fifo.push(msg);
        }
    }
    to_gui_fifo.push(ThreadMessage{ThreadMessage::OP_PARAMREMOTE});
}

void AudioPluginAudioProcessor::init_clouds(ToneGranulator &g)
{
    xenakios::Xoroshiro128Plus rng;
    std::array<xenakios::Envelope, 8> lopitchlimits;
    std::array<xenakios::Envelope, 8> hipitchlimits;
    // static limits
    lopitchlimits[0] = xenakios::Envelope{{{0.0, 0.0}}};
    hipitchlimits[0] = xenakios::Envelope{{{0.0, 1.0}}};
    lopitchlimits[1] = xenakios::Envelope{{{0.0, 0.0}, {1.0, 1.0}}};
    hipitchlimits[1] = xenakios::Envelope{{{0.0, 1.0}}};
    lopitchlimits[2] = xenakios::Envelope{{{0.0, 0.0}}};
    hipitchlimits[2] = xenakios::Envelope{{{0.0, 1.0}, {1.0, 0.0}}};
    lopitchlimits[3] = xenakios::Envelope{{{0.0, 0.5}, {1.0, 0.0}}};
    hipitchlimits[3] = xenakios::Envelope{{{0.0, 0.5}, {1.0, 1.0}}};
    lopitchlimits[4] = xenakios::Envelope{{{0.0, 0.0}, {1.0, 0.5}}};
    hipitchlimits[4] = xenakios::Envelope{{{0.0, 1.0}, {1.0, 0.5}}};
    lopitchlimits[5] = xenakios::Envelope{{{0.0, 0.5}, {0.5, 0.0}, {1.0, 0.5}}};
    hipitchlimits[5] = xenakios::Envelope{{{0.0, 0.5}, {0.5, 1.0}, {1.0, 0.5}}};
    lopitchlimits[6] = xenakios::Envelope{{{0.0, 0.0}, {0.5, 0.5}, {1.0, 0.0}}};
    hipitchlimits[6] = xenakios::Envelope{{{0.0, 1.0}, {0.5, 0.5}, {1.0, 1.0}}};
    lopitchlimits[7] = xenakios::Envelope{{{0.0, 0.0}}};
    hipitchlimits[7] = xenakios::Envelope{{{0.0, 1.0}, {0.5, 0.0}, {1.0, 1.0}}};
    {
        for (int i = 0; i < 8; ++i)
        {
            Cloud c;
            double clouddur = 4.0;
            double t = 0.0;
            while (t < clouddur)
            {
                double envtimepos = 1.0 / clouddur * t;
                CloudEvent e;
                e.time_position = t;
                double pitchlo = -24.0 + 48.0 * lopitchlimits[i].getValueAtPosition(envtimepos);
                double pitchhi = -24.0 + 48.0 * hipitchlimits[i].getValueAtPosition(envtimepos);
                e.param_modulations[0] = {ToneGranulator::PAR_PITCH,
                                          rng.nextFloatInRange(pitchlo, pitchhi)};
                c.events.push_back(e);
                t += -std::log(rng.nextFloat()) * (1.0 / 32.0);
            }
            g.clouds.push_back(c);
        }
    }
    return;
    std::vector<float> rates{0.5f, 0.25f, 0.125f, 0.05f, 0.025f};
    for (auto &r : rates)
    {
        Cloud c;
        double t = 0.0;
        while (t < 1.0)
        {
            CloudEvent e;
            e.time_position = t;
            e.param_modulations[0].id = ToneGranulator::PAR_PITCH;
            e.param_modulations[0].value = rng.nextFloatInRange(-12.0f, 12.0f);
            e.param_modulations[1] = {ToneGranulator::PAR_OSCTYPE, 3};
            e.param_modulations[2] = {ToneGranulator::PAR_DURATION, 0.5};
            c.events.push_back(e);
            t += r;
        }
        g.clouds.push_back(c);
    }
    Cloud c;
    c.duration = 10.0;
    c.events.clear();
    c.duration = 1.0;
    double t = 0.0;
    int i = 0;
    while (t < 10.0)
    {
        CloudEvent e;
        e.time_position = t;
        e.param_modulations[0].id = ToneGranulator::PAR_PITCH;
        e.param_modulations[0].value = rng.nextFloatInRange(36.0, 48.0);
        e.param_modulations[1] = {ToneGranulator::PAR_OSCTYPE, 0};
        e.param_modulations[2] = {ToneGranulator::PAR_DURATION, 0.15};
        e.param_modulations[3] = {ToneGranulator::PAR_AZIMUTH, rng.nextFloatInRange(-90.0f, 90.0f)};
        c.events.push_back(e);

        t += 0.025;
    }
    g.clouds.push_back(c);

    c.events.clear();
    c.duration = 1.0;
    t = 0.0;
    while (t < 10.0)
    {
        if (i % 7 == 0 || i % 13 == 0)
        {
            CloudEvent e;
            e.time_position = t;
            e.param_modulations[0].id = ToneGranulator::PAR_PITCH;
            if (rng.nextFloat() < 0.5)
            {
                e.param_modulations[0].value = 35.0f;
                e.param_modulations[2] = {ToneGranulator::PAR_DURATION, 0.19};
            }
            else
            {
                e.param_modulations[0].value = -11.0f;
                e.param_modulations[4] = {ToneGranulator::PAR_OSC_SYNC, 1.53f};
                e.param_modulations[2] = {ToneGranulator::PAR_DURATION, 0.6};
            }

            e.param_modulations[1] = {ToneGranulator::PAR_OSCTYPE, 2};

            e.param_modulations[3] = {ToneGranulator::PAR_AZIMUTH,
                                      rng.nextFloatInRange(-90.0f, 90.0f)};
            c.events.push_back(e);
        }
        ++i;
        t = i * 0.01;
    }
    g.clouds.push_back(c);
    for (auto &e : g.clouds)
    {
        e.after_touch_dest = ToneGranulator::PAR_INSERTAFIRST + 0;
    }
}

void AudioPluginAudioProcessor::loadMacroKnobs(std::string filename)
{
    try
    {
        auto jsontxt = choc::file::loadFileAsString(filename);
        auto bindings = choc::json::parseValue(jsontxt);
        for (int i = 0; i < bindings.size(); ++i)
        {
            auto binding = bindings[i];
            auto index = binding["knob"].getWithDefault(-1);
            if (index < 0 || index >= macroBindings.size())
                continue;
            macroBindings[index].dest_type = binding["desttype"].getWithDefault(-1);
            macroBindings[index].dest = binding["dest"].getWithDefault(0);
            macroBindings[index].label = binding["name"].getWithDefault(fmt::format("M{}", i + 1));
            if (binding.hasObjectMember("min") && binding.hasObjectMember("max"))
            {
                macroBindings[index].par_range = {binding["min"].getWithDefault(-1.0f),
                                                  binding["max"].getWithDefault(1.0f)};
            }
        }
    }
    catch (std::exception &excep)
    {
        DBG(excep.what());
    }
}

void AudioPluginAudioProcessor::handleMacroKnob(int knobindex, float value, bool is_audio_thread)
{
    if (knobindex >= 0 && knobindex < macroBindings.size())
    {
        auto &mb = macroBindings[knobindex];
        if (mb.dest_type == 0)
        {
            ParameterMessage msg;
            msg.id = mb.dest;
            auto pmdit = granulator.idtoparmetadata.find(msg.id);
            if (pmdit != granulator.idtoparmetadata.end())
            {
                float val = pmdit->second->defaultVal;
                float minval = pmdit->second->minVal;
                float maxval = pmdit->second->maxVal;
                if (mb.par_range)
                {
                    minval = mb.par_range->first;
                    maxval = mb.par_range->second;
                }
                val = juce::jmap<float>(value, -1.0f, 1.0f, minval, maxval);
                msg.value = val;
                if (!is_audio_thread)
                    params_from_gui_fifo.push(msg);
                else
                    *granulator.idtoparvalptr[msg.id] = msg.value;
            }
        }
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() { return new AudioPluginAudioProcessor(); }
