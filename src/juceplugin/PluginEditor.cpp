#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "containers/choc_Value.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"
#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "modulecomponents.h"
#include "text/choc_Files.h"
#include "text/choc_JSON.h"
#include "xap_slider.h"
#include <exception>
#include <memory>

void init_step_sequencer_js();
void deinit_step_sequencer_js();
choc::value::Value get_js_info(std::string jscode);
void cancel_js();
std::vector<float> generate_from_js(std::string jscode, std::vector<float> currentsteps,
                                    int startstep, int endstep, std::vector<float> params);
choc::value::Value perform_js(std::string jscode, choc::value::ValueView info);

inline void updateAllFonts(juce::Component &parent, const juce::Font &newFont)
{
    for (auto *child : parent.getChildren())
    {
        if (auto *drop = dynamic_cast<DropDownComponent *>(child))
        {
            drop->myfont = newFont;
        }
        if (auto *xaps = dynamic_cast<XapSlider *>(child))
        {
            xaps->m_font = newFont;
        }
        // Keep digging deeper
        updateAllFonts(*child, newFont);
    }
}

AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor &p)
    : juce::AudioProcessorEditor(p), processorRef(p), mainPage(p), modulationPage(p), dashPage(p),
      mainTabs(juce::TabbedButtonBar::Orientation::TabsAtTop)
{
    mainTabs.addTab("MAIN", juce::Colours::grey, &mainPage, false);
    mainTabs.addTab("MODULATION", juce::Colours::grey, &modulationPage, false);
    mainTabs.addTab("DASHBOARD", juce::Colours::grey, &dashPage, false);
    dashPage.dashBoardComponent.OnMacroKnobsLoadRequested = [this]() {
        processorRef.loadMacroKnobs(processorRef.macroKnobsPath);
    };
    mainTabs.setCurrentTabIndex(0);
    addAndMakeVisible(mainTabs);
    for (int i = 0; i < 8; ++i)
    {
        auto lfoc = modulationPage.lfocomps[i].get();
        addChildSlidersFrom(*lfoc);
    }

    for (auto &e : mainPage.insertComponents)
    {
        addChildSlidersFrom(*e);
    }
    addChildSlidersFrom(mainPage.spatModuleComponent);
    addChildSlidersFrom(mainPage.volumeModuleComponent);
    addChildSlidersFrom(mainPage.oscModuleComponent);
    addChildSlidersFrom(mainPage.timeModuleComponent);
    addChildSlidersFrom(mainPage.stackModuleComponent);
    addChildSlidersFrom(mainPage.mainOutModuleComponent);
#if JUCE_MAC
    setScaleFactor(0.80);
#else
    // setScaleFactor(0.90);
#endif

    for (auto &c : mainPage.insertComponents)
    {
        c->OnInsertTypeChanged = [this]() {
            for (auto &modrow : modulationPage.modRowComps)
            {
                modrow->initDestinationDrop();
            }
        };
    }
    setSize(1500, 720);
    startTimer(50);
}

void AudioPluginAudioProcessorEditor::addChildSlidersFrom(juce::Component &c)
{
    for (auto &c : c.getChildren())
    {
        if (auto knob = dynamic_cast<XapSlider *>(c))
        {
            idToSlider[knob->getParameterMetaData().id] = knob;
        }
    }
}

AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor() {}

void AudioPluginAudioProcessorEditor::timerCallback()
{
    mainPage.volumeModuleComponent.envcomp.updateIfNeeded();
    mainPage.oscModuleComponent.pitchEnvelopeComponent.updateIfNeeded();

    for (auto &c : modulationPage.stepcomps)
    {
        c->updateGUI();
    }
    ParameterMessage parmsg;
    while (processorRef.params_to_gui_fifo.pop(parmsg))
    {
        auto it = idToSlider.find(parmsg.id);
        if (it != idToSlider.end())
        {
            auto xs = it->second;
            xs->setValue(parmsg.value);
        }
    }
    ThreadMessage msg;
    while (processorRef.to_gui_fifo.pop(msg))
    {
        if (msg.opcode == ThreadMessage::OP_STEPSEQUENCER)
        {
            mainPage.oscModuleComponent.pitchEnvelopeComponent.repaint();
        }
        if (msg.opcode == ThreadMessage::OP_FILTERTYPE)
        {
            for (auto &e : mainPage.insertComponents.front()->filterInfoMap)
            {
                if (e.second.mainmode == msg.insertmainmode && e.second.awtype == msg.awtype &&
                    e.second.sstmodel == msg.filtermodel && e.second.sstconfig == msg.filterconfig)
                {
                    mainPage.insertComponents[msg.filterindex]->insertDrop.setSelectedId(e.first);
                    break;
                }
            }
            for (auto &e : mainPage.insertComponents)
            {
                e->updateInsertMetadatas();
            }
        }
        if (msg.opcode == ThreadMessage::OP_MODROUTING &&
            msg.modslot < modulationPage.modRowComps.size())
        {
            modulationPage.modRowComps[msg.modslot]->sourceDrop.setSelectedId(msg.modsource);

            modulationPage.modRowComps[msg.modslot]->viaDrop.setSelectedId(msg.modvia);

            modulationPage.modRowComps[msg.modslot]->depthSlider.setValue(msg.depth, false);
            modulationPage.modRowComps[msg.modslot]->destDrop.setSelectedId(msg.moddest);

            modulationPage.modRowComps[msg.modslot]->setTarget(msg.moddest);
            modulationPage.modRowComps[msg.modslot]->curveDrop.setSelectedId(msg.modcurve);
        }
    }
}

void AudioPluginAudioProcessorEditor::resized()
{
    mainTabs.setBounds(0, 0, getWidth(), getHeight());
}

MainPageComponent::MainPageComponent(AudioPluginAudioProcessor &p)
    : processorRef(p), oscModuleComponent(p), timeModuleComponent(p), spatModuleComponent(p),
      volumeModuleComponent(p), stackModuleComponent(p), mainOutModuleComponent(p)
{
    /*
    addAndMakeVisible(testTree);
    testRootItem.containsSubItems = true;
    std::map<std::string, MyTreeItem *> groups;
    for (auto &e : p.granulator.modSources)
    {
        if (!e.groupname.empty() && groups.count(e.groupname) == 0)
        {
            auto item = new MyTreeItem;
            item->itemText = e.groupname;
            item->containsSubItems = true;
            testRootItem.addSubItem(item, -1);
            groups[e.groupname] = item;
        }

        {
            auto item = new MyTreeItem;
            item->itemText = e.name;
            item->containsSubItems = false;
            if (groups.count(e.groupname))
            {
                auto gitem = groups[e.groupname];
                gitem->addSubItem(item, -1);
            }
            else
            {
                testRootItem.addSubItem(item);
            }
        }
    }
    testTree.setRootItem(&testRootItem);
    */
    mainOutModuleComponent.perfComponent.RequestData = [this](int &maxvoices, int &usedvoices,
                                                              float &cpu) {
        maxvoices = processorRef.granulator.voices.size();
        usedvoices = processorRef.granulator.numVoicesUsed;
        cpu = processorRef.perfMeasurer.getLoadAsProportion();
    };
    init_step_sequencer_js();

    addAndMakeVisible(oscModuleComponent);
    addAndMakeVisible(spatModuleComponent);
    addAndMakeVisible(mainOutModuleComponent);
    addAndMakeVisible(volumeModuleComponent);

    for (int i = 0; i < 2; ++i)
    {
        auto insertComp = std::make_unique<InsertModuleComponent>(processorRef, i);
        addAndMakeVisible(*insertComp);
        insertComponents.push_back(std::move(insertComp));
    }

    addAndMakeVisible(stackModuleComponent);
    addAndMakeVisible(timeModuleComponent);

    recordButton = std::make_unique<juce::TextButton>();
    recordButton->setButtonText("Record");
    recordButton->onClick = [this]() {
        if (processorRef.isRecording)
            processorRef.stopRecording();
        else
            processorRef.startRecording();
        if (processorRef.isRecording)
            recordButton->setButtonText("Stop");
        else
            recordButton->setButtonText("Record");
    };
    // mainParamsComponent.addHeaderComponent(recordButton.get());
    // mainParamsComponent.addHeaderComponent(perfcomp.get());

    auto &idtomd = processorRef.granulator.idtoparmetadata;

    // addAndMakeVisible(insertsTabs);

    // setLookAndFeel(&lnf);
    // updateAllFonts(*this, lnf.myFont);

    setSize(1500, 930);
}

MainPageComponent::~MainPageComponent()
{
    setLookAndFeel(nullptr);
    deinit_step_sequencer_js();
}

void MainPageComponent::paint(juce::Graphics &g) { g.fillAll(juce::Colours::darkgrey); }

void MainPageComponent::resized()
{
    oscModuleComponent.setBounds(0, 0, 700, 225);
    volumeModuleComponent.setBounds(0, oscModuleComponent.getBottom() + 1, 700, 150);
    timeModuleComponent.setBounds(oscModuleComponent.getRight() + 2, 0, 300, 125);

    spatModuleComponent.setBounds(0, volumeModuleComponent.getBottom() + 2, 600, 125);
    mainOutModuleComponent.setBounds(spatModuleComponent.getRight() + 2,
                                     volumeModuleComponent.getBottom() + 2, 500, 125);
    insertComponents[0]->setBounds(0, spatModuleComponent.getBottom() + 2, getWidth() / 2 - 4, 125);
    insertComponents[1]->setBounds(insertComponents[0]->getRight() + 1,
                                   spatModuleComponent.getBottom() + 2, getWidth() / 2 - 4, 125);

    stackModuleComponent.setBounds(timeModuleComponent.getRight() + 2, 0, 490, 125);
    // testTree.setBounds(getWidth() - 299, timeModuleComponent.getBottom() + 2, 300, 300);
}

void StepSeqComponent::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colours::black);
    auto &msrc = gr->stepModSources[sindex];
    int maxstepstodraw = (getWidth() - graphxpos) / 16;
    int stepstodraw = std::min<int>(maxstepstodraw, msrc.numactivesteps);
    for (int i = 0; i < maxstepstodraw; ++i)
    {
        float xcor = graphxpos + i * 16.0;
        float v = msrc.steps[i];
        if (msrc.unipolar)
            v = (v + 1.0) * 0.5;
        if (i == playingStep)
            g.setColour(juce::Colours::white);
        else
        {
            if (i >= msrc.loopstartstep && i < msrc.loopstartstep + msrc.looplen)
                g.setColour(juce::Colours::green);
            else
                g.setColour(juce::Colours::darkgreen.darker());
        }

        if (v < 0.0)
        {
            float h = juce::jmap<float>(v, -1.0, 0.0, getHeight() / 2.0, 0.0);
            g.fillRect(xcor, getHeight() / 2.0, 15.0, h);
        }
        else
        {

            float h = juce::jmap<float>(v, 0.0, 1.0, 0.0, getHeight() / 2.0);
            g.fillRect(xcor, getHeight() / 2.0 - h, 15.0, h);
        }
    }
    g.setColour(juce::Colours::white);
    g.drawRect(graphxpos + editRange.getStart() * 16.0, 1.0f, editRange.getLength() * 16.0,
               (float)getHeight() - 2.0f, 2.0f);
    g.setColour(juce::Colours::cyan);
    float xcor = graphxpos + (msrc.loopstartstep + msrc.loopoffset) * 16.0;
    g.drawLine(xcor, 0, xcor, getHeight(), 3.0f);
    g.setColour(juce::Colours::black);
    g.drawLine(float(graphxpos), getHeight() / 2.0f, getWidth(), getHeight() / 2.0f);
    juce::String txt;
    txt << editRange.getStart() << " " << editRange.getEnd();
    if (autoSetLoop)
        txt << "\nLOOP FOLLOWS SELECTION";
    juce::GenericScopedLock locker(spinlock);
    if (!js_error.empty())
        txt << "\nJavaScript error : " << js_error;
    g.setColour(juce::Colours::white);
    g.drawMultiLineText(txt, graphxpos + 2, 20, getWidth() - graphxpos - 3);
}

bool StepSeqComponent::keyPressed(const juce::KeyPress &ev)
{
    auto &msrc = gr->stepModSources[sindex];
    int curstart = msrc.loopstartstep;
    int curlooplen = msrc.looplen;

    int actiontaken = 0;
    auto incdecstep = [this, &msrc](float step) {
        int index = editRange.getStart();
        float v = msrc.steps[index];
        v = std::clamp(v + step, -1.0f, 1.0f);
        gr->fifo.push({StepModSource::Message::OP_SETSTEP, sindex, v, index});
    };
    if (ev.getKeyCode() == 'R')
    {
        runJSInThread();
        return true;
        scriptParamsEditor.setVisible(true);
        scriptParamsEditor.setBounds(graphxpos + 1, 1, 150, 25);
        scriptParamsEditor.grabKeyboardFocus();
        scriptParamsEditor.onReturnKey = [this]() {
            scriptParamsEditor.setVisible(false);
            runJSInThread();
        };
        scriptParamsEditor.onEscapeKey = [this]() { scriptParamsEditor.setVisible(false); };
    }
    else if (ev.getKeyCode() == 'Y')
    {
        autoSetLoop = !autoSetLoop;
        actiontaken = 1;
    }
    else if (ev.getKeyCode() == 'P')
    {
        runExternalProgram();
        actiontaken = 2;
    }
    else if (ev.getKeyCode() == 'T')
    {
        incdecstep(0.1f);
        actiontaken = 2;
    }
    else if (ev.getKeyCode() == 'G')
    {
        incdecstep(-0.1f);
        actiontaken = 2;
    }
    else if (ev.getKeyCode() == 'Z')
    {
        int curoff = msrc.loopoffset;
        curoff = (curoff + 1) % msrc.looplen;
        gr->fifo.push({StepModSource::Message::OP_OFFSET, sindex, 0.0f, curoff});
        actiontaken = 2;
    }
    else if (ev.getKeyCode() == 'E')
    {
        setLoopFromSelection();
        actiontaken = 2;
    }
    else if (ev.getKeyCode() == 'D' && ev.getModifiers() == juce::ModifierKeys::noModifiers)
    {
        for (int i = 0; i < editRange.getLength(); ++i)
        {
            int index = editRange.getStart() + i;
            gr->fifo.push({StepModSource::Message::OP_SETSTEP, sindex,
                           rng.nextFloatInRange(-1.0f, 1.0f), index});
        }
        actiontaken = 2;
    }
    else if (ev.getKeyCode() == 'D' && ev.getModifiers().isShiftDown())
    {
        if (editRange.getLength() > 1)
        {
            for (int i = 0; i < editRange.getLength(); ++i)
            {
                int index = editRange.getStart() + i;
                float v = -1.0 + 2.0 / (editRange.getLength() - 1) * i;
                gr->fifo.push({StepModSource::Message::OP_SETSTEP, sindex, v, index});
            }
        }
        else
        {
            gr->fifo.push({StepModSource::Message::OP_SETSTEP, sindex, 0.0f, editRange.getStart()});
        }

        actiontaken = 2;
    }
    else if (ev.getKeyCode() == 'Q' && ev.getModifiers() == juce::ModifierKeys::noModifiers)
    {
        editRange = editRange.movedToStartAt(editRange.getStart() + 1);
        actiontaken = 1;
    }
    else if (ev.getKeyCode() == 'Q' && ev.getModifiers().isShiftDown())
    {
        if (editRange.getLength() > 1)
            editRange.setStart(editRange.getStart() + 1);
        actiontaken = 1;
    }
    else if (ev.getKeyCode() == 'A' && ev.getModifiers() == juce::ModifierKeys::noModifiers)
    {
        editRange = editRange.movedToStartAt(editRange.getStart() - 1);
        actiontaken = 1;
    }
    else if (ev.getKeyCode() == 'A' && ev.getModifiers().isShiftDown())
    {
        if (editRange.getStart() > 0)
            editRange.setStart(editRange.getStart() - 1);
        actiontaken = 1;
    }

    else if (ev.getKeyCode() == 'W')
    {
        editRange = editRange.withLength(editRange.getLength() + 1);
        actiontaken = 1;
    }
    else if (ev.getKeyCode() == 'S')
    {
        editRange = editRange.withLength(editRange.getLength() - 1);
        actiontaken = 1;
    }
    editRange = juce::Range<int>{0, 4096}.constrainRange(editRange);
    if (editRange.getLength() < 1)
        editRange.setLength(1);
    if (actiontaken == 1 && autoSetLoop)
    {
        setLoopFromSelection();
    }
    return actiontaken > 0;
}

StepSeqComponent::StepSeqComponent(int seqindex, ToneGranulator *g, juce::ThreadPool *tp)
    : gr(g), sindex(seqindex), threadPool(tp)
{
    addChildComponent(scriptParamsEditor);
    rng.seed(11400714819323198485ULL, 17 + sindex * 31);
    editRange.setStart(0);
    editRange.setLength(g->stepModSources[sindex].looplen);
    setWantsKeyboardFocus(true);

    addAndMakeVisible(cancelButton);
    cancelButton.setButtonText("Stop JS script");
    cancelButton.onClick = [this]() {
        cancel_js();
        cancelButton.setVisible(false);
    };
    cancelButton.setVisible(false);

    addAndMakeVisible(unipolarBut);
    unipolarBut.setButtonText("Unipolar");
    unipolarBut.onClick = [this]() {
        gr->fifo.push(
            {StepModSource::Message::OP_UNIPOLAR, sindex, 0.0f, unipolarBut.getToggleState()});
    };
    addAndMakeVisible(par0Slider);
    par0Slider.setRange(0.0, 1.0);
    par0Slider.setNumDecimalPlacesToDisplay(2);
    par0Slider.onDragEnd = [this]() { runExternalProgram(); };
}

void StepSeqComponent::runJSInThread()
{
    js_error = "";
    auto jscode = choc::file::loadFileAsString(R"(C:\develop\nephos\src\generatesteps.js)");
    auto info = get_js_info(jscode);
    if (!info.hasObjectMember("error"))
    {
        if (!jsSettingsComponent)
            jsSettingsComponent = std::make_unique<JSEntryComponent>(info);
        jsSettingsComponent->OnRun = [this, jscode]() {
            auto ob = choc::value::createObject("");
            for (auto &ed : jsSettingsComponent->editors)
            {
                auto id = jsSettingsComponent->editorToProperty[ed.get()];
                ob.setMember(id, ed->getText().getDoubleValue());
            }
            try
            {
                auto r = perform_js(jscode, ob);
                // DBG(choc::json::toString(r));
                if (r.hasObjectMember("steps"))
                {
                    auto arr = r["steps"];
                    for (int i = 0; i < arr.size(); ++i)
                    {
                        float val = arr[i].getWithDefault(0.0);
                        gr->fifo.push({StepModSource::Message::OP_SETSTEP, sindex, val, (int)i});
                    }
                }
            }
            catch (std::exception &ex)
            {
                DBG(ex.what());
            }
        };
        jsSettingsComponent->OnHide = [this]() { jsSettingsComponent->setVisible(false); };
        getParentComponent()->getParentComponent()->addAndMakeVisible(*jsSettingsComponent);
        int h = jsSettingsComponent->labels.size() * 30 + 60;
        jsSettingsComponent->setBounds(0, 0, 500, h);
        return;
    }
    else
    {
        js_error = info["error"].getWithDefault("");
        repaint();
        return;
    }
    auto tokens = juce::StringArray::fromTokens(scriptParamsEditor.getText(), true);
    if (tokens.size() < 1 || tokens.size() > 1024)
    {
        js_error = "There must be more than 0 and less 1024 tokens in parameters";
        return;
    }
    js_error = "";
    js_status.store(1);
    cancelButton.setVisible(true);
    std::vector<float> params;
    params.reserve(tokens.size());
    for (int i = 0; i < tokens.size(); ++i)
    {
        params.push_back(tokens[i].getFloatValue());
    }

    threadPool->addJob([this, params]() {
        try
        {
            auto jscode = choc::file::loadFileAsString(R"(C:\develop\nephos\src\generatesteps.js)");
            auto steps = gr->stepModSources[sindex].steps;
            steps =
                generate_from_js(jscode, steps, editRange.getStart(), editRange.getEnd(), params);
            for (size_t i = 0; i < steps.size(); ++i)
            {
                gr->fifo.push({StepModSource::Message::OP_SETSTEP, sindex, steps[i],
                               (int)i + editRange.getStart()});
            }
        }
        catch (std::exception &ex)
        {
            DBG(ex.what());
            juce::GenericScopedLock locker(spinlock);
            js_error = ex.what();
        }
        js_status.store(0);
        juce::MessageManager::callAsync([this]() { cancelButton.setVisible(false); });
    });
}

void StepSeqComponent::runExternalProgram()
{
    threadPool->addJob([this]() {
        juce::ChildProcess cp;
        double t0 = juce::Time::getMillisecondCounterHiRes();
        cp.start(fmt::format(
            R"(python C:\develop\AudioPluginHost_mk2\Source\granularsynth\stepseq.py {} {} {})",
            sindex, editRange.getStart(), editRange.getLength()));
        auto data = cp.readAllProcessOutput();
        double t1 = juce::Time::getMillisecondCounterHiRes();
        DBG("running ext program took " << t1 - t0 << " millisecods");
        if (!data.containsIgnoreCase("error"))
        {
            auto tokens = juce::StringArray::fromTokens(data, false);
            for (int i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].isEmpty() || i == 4096 || i > editRange.getLength())
                    break;
                float v = std::clamp(tokens[i].getFloatValue(), -1.0f, 1.0f);
                // DBG(v);
                int index = editRange.getStart() + i;
                gr->fifo.push({StepModSource::Message::OP_SETSTEP, sindex, v, index});
            }
        }
        else
        {
            DBG(data);
        }
    });
}
