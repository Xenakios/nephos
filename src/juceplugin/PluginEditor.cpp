#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "clap/id.h"
#include "containers/choc_Value.h"
#include "juce_audio_utils/juce_audio_utils.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"
#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "modulecomponents.h"
#include "text/choc_Files.h"
#include "text/choc_JSON.h"
#include "xap_slider.h"
#include <exception>
#include <fstream>
#include <memory>

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
      imTest(p), mainTabs(juce::TabbedButtonBar::Orientation::TabsAtTop)
{
    /*
    if (!processorRef.baconSpectrum)
    {
        processorRef.baconSpectrum =
            std::make_unique<baconpaul::six_sines::ui::SpectrumAnalyzerComponent>(44100.0);
    }
    */
    addChildComponent(overlaylabel);
    mainTabs.addTab("MAIN", juce::Colours::grey, &mainPage, false);
    mainTabs.addTab("MODULATION", juce::Colours::grey, &modulationPage, false);
    mainTabs.addTab("DASHBOARD", juce::Colours::grey, &dashPage, false);
    dashPage.dashBoardComponent.OnMacroKnobsLoadRequested = [this]() {
        processorRef.loadMacroKnobs(processorRef.macroKnobsPath);
    };
    if (processorRef.baconSpectrum)
        mainTabs.addTab("ANALYSIS", juce::Colours::grey, processorRef.baconSpectrum.get(), false);
    mainTabs.addTab("IM TEST", juce::Colours::grey, &imTest, false);
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
    for (auto &c : modulationPage.modRowComps)
    {
        addChildSlidersFrom(*c);
    }
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
    for (auto &c : idToSlider)
    {
        c.second->addMenuItemsCallback([this, parid = c.first](juce::PopupMenu &menu) {
            addMidiLearnToMenu(menu, processorRef, parid);
        });
    }
    setSize(1500, 720);
    startTimerHz(20);
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

void AudioPluginAudioProcessorEditor::updateParameterRemoteStates()
{
    DBG("updating knob remote control status leds");
    for (auto &c : idToSlider)
    {
        uint32_t s = XapSlider::RCS_NONE;
        for (auto &binding : processorRef.midiBindings)
        {
            if (binding.target_param == c.first)
            {
                s |= XapSlider::RCS_MIDI;
                break;
            }
        }
        for (auto &r : processorRef.granulator.modmatrix.rt.routes)
        {
            if (r.target && r.target->target == c.first)
            {
                s |= XapSlider::RCS_MODULATED;
                break;
            }
        }
        c.second->setRemoteControlMode((XapSlider::RemoteControlStatus)s);
    }
}

void AudioPluginAudioProcessorEditor::setOverLaytext(juce::String txt, int delay_ms)
{
    overlaylabel.toFront(false);
    overlaylabel.setVisible(true);
    overlaylabel.setJustificationType(juce::Justification::centred);
    overlaylabel.setColour(juce::Label::ColourIds::backgroundColourId, juce::Colours::black);
    overlaylabel.setFont(overlaylabel.getFont().withHeight(50));
    overlaylabel.setText(txt, juce::dontSendNotification);
    juce::Timer::callAfterDelay(delay_ms, [this]() { overlaylabel.setVisible(false); });
}

void AudioPluginAudioProcessorEditor::timerCallback()
{
    mainPage.oscModuleComponent.pitchEnvelopeComponent.updateIfNeeded();

    for (auto &c : modulationPage.stepcomps)
    {
        c->updateGUI();
    }
    // mainPage.stackModuleComponent.repaint();
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
    for (auto &c : modulationPage.randComponents)
    {
        c->update_all();
    }
    ThreadMessage msg;
    while (processorRef.to_gui_fifo.pop(msg))
    {
        if (msg.opcode == ThreadMessage::OP_TUNING)
        {
            mainPage.oscModuleComponent.updateScalaDropFromPath(
                processorRef.granulator.currentScalaFile);
        }
        if (msg.opcode == ThreadMessage::OP_PARAMREMOTE)
        {
            updateParameterRemoteStates();
            // setOverLaytext("Updated remote control states", 1000);
        }
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

            modulationPage.modRowComps[msg.modslot]->destDrop.setSelectedId(msg.moddest);

            modulationPage.modRowComps[msg.modslot]->setTarget(msg.moddest);
            modulationPage.modRowComps[msg.modslot]->curveDrop.setSelectedId(msg.modcurve);
        }
    }
}

void AudioPluginAudioProcessorEditor::resized()
{
    mainTabs.setBounds(0, 0, getWidth(), getHeight());
    auto area = getLocalBounds().reduced(100, 300);
    overlaylabel.setBounds(area);
}

MainPageComponent::MainPageComponent(AudioPluginAudioProcessor &p)
    : processorRef(p), oscModuleComponent(p), timeModuleComponent(p), spatModuleComponent(p),
      volumeModuleComponent(p), stackModuleComponent(p), mainOutModuleComponent(p),
      keyboardComponent(p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    addAndMakeVisible(corruptButton);
    corruptButton.setButtonText("CORRUPT AUDIO");
    corruptButton.onClick = [this]() {
        processorRef.corruptAudioOnPurpose = true;
        juce::Timer::callAfterDelay(1000, [this]() { processorRef.corruptAudioOnPurpose = false; });
    };
    mainOutModuleComponent.perfComponent.RequestData = [this](int &maxvoices, int &usedvoices,
                                                              float &cpu) {
        maxvoices = processorRef.granulator.voices.size();
        usedvoices = processorRef.granulator.numVoicesUsed;
        cpu = processorRef.perfMeasurer.getLoadAsProportion();
    };
    init_step_sequencer_js();

    addAndMakeVisible(keyboardComponent);

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

void MainPageComponent::mouseDown(const juce::MouseEvent &ev)
{
    if (ev.mods.isRightButtonDown())
    {
        juce::PopupMenu menu;
        menu.addItem("Reset all MIDI assignments", [this]() {
            ThreadMessage msg;
            msg.opcode = ThreadMessage::OP_UNLEARNMIDI;
            msg.parid = CLAP_INVALID_ID;
            processorRef.from_gui_fifo.push(msg);
        });
        menu.addItem("Copy MIDI assignments to clipboard", [this]() {
            juce::String result;
            result << "CHAN\t" << "CC\t" << "PARID\t" << "NAME\n";
            for (auto &e : processorRef.midiBindings)
            {
                auto it = processorRef.granulator.idtoparmetadata.find(e.target_param);
                juce::String name;
                if (it != processorRef.granulator.idtoparmetadata.end())
                {
                    name = it->second->name;
                }
                if (e.npa == MIDIBinding::NPA_LOADSNAP0108)
                    name = "SNAPSHOTS 1-8";
                if (e.npa == MIDIBinding::NPA_LOADSNAP0916)
                    name = "SNAPSHOTS 9-16";
                result << (int)e.midichan << "\t" << (int)e.midicc << "\t" << (int)e.target_param
                       << "\t" << name << "\n";
            }
            juce::SystemClipboard::copyTextToClipboard(result);
        });
        menu.addItem("Reset to default state",
                     [this]() { processorRef.loadPreset("Factory Reset", "Factory Presets"); });
        menu.showMenuAsync({});
    }
}

void MainPageComponent::resized()
{
    oscModuleComponent.setBounds(0, 0, 920, 280);
    volumeModuleComponent.setBounds(0, oscModuleComponent.getBottom() + 1, 700, 150);

    timeModuleComponent.setBounds(oscModuleComponent.getRight() + 2, 0, 300, 125);

    spatModuleComponent.setBounds(0, volumeModuleComponent.getBottom() + 2, 600, 125);
    mainOutModuleComponent.setBounds(spatModuleComponent.getRight() + 2,
                                     volumeModuleComponent.getBottom() + 2, 600, 125);
    insertComponents[0]->setBounds(0, spatModuleComponent.getBottom() + 2, getWidth() / 2 - 4, 125);
    insertComponents[1]->setBounds(insertComponents[0]->getRight() + 1,
                                   spatModuleComponent.getBottom() + 2, getWidth() / 2 - 4, 125);

    stackModuleComponent.setBounds(oscModuleComponent.getRight() + 2,
                                   timeModuleComponent.getBottom() + 2, 490, 175);
    // processorRef.xenAvisComponent.setBounds(getWidth() - 501, stackModuleComponent.getBottom() +
    // 2,
    //                                         500, 250);
    //  keyboardComponent.setBounds(1, getHeight() - 50, getWidth() - 300, 49);
    //  testTree.setBounds(getWidth() - 299, timeModuleComponent.getBottom() + 2, 300, 300);
    corruptButton.setBounds(getWidth() - 200, stackModuleComponent.getBottom() + 2, 190, 25);
}

void DashPage::saveSnapShot(int index)
{
    auto state = processorRef.getState();
    std::ofstream ostream(fmt::format("{}{}.json", processorRef.presetsPath, index + 1));
    choc::json::writeAsJSON(ostream, state, true);
    processorRef.saveSnapShot(index, state);
}
void PresetsComponent::mouseDown(const juce::MouseEvent &ev)
{
    if (ev.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem("Learn MIDI CC range to load snapshots 1-8",
                     [this]() { processorRef.midiLearnAction = MIDIBinding::NPA_LOADSNAP0108; });
        menu.addItem("Learn MIDI CC range to load snapshots 9-16",
                     [this]() { processorRef.midiLearnAction = MIDIBinding::NPA_LOADSNAP0916; });
        for (auto &b : processorRef.midiBindings)
        {
            if (b.npa == MIDIBinding::NPA_LOADSNAP0108 || b.npa == MIDIBinding::NPA_LOADSNAP0916)
            {
                std::string rtxt = "1-8";
                if (b.npa == MIDIBinding::NPA_LOADSNAP0916)
                    rtxt = "9-16";
                menu.addItem(
                    fmt::format("Remove MIDI CC range {}-{} to load snapshots {}", b.midicc,
                                b.midicc + 7, rtxt),
                    [this, npa = b.npa]() { processorRef.removeMidiAssignmentForAction(npa); });
            }
        }
        menu.showMenuAsync({});
    }
}
void PresetsComponent::resized()
{
    juce::FlexBox flex;
    flex.flexDirection = juce::FlexBox::Direction::row;
    flex.flexWrap = juce::FlexBox::Wrap::wrap;
    for (auto &b : buttons)
    {
        flex.items.add(juce::FlexItem(*b).withFlex(1.0).withMinWidth(40.0f).withMaxWidth(40.0f));
    }
    flex.performLayout(getLocalBounds());
}
void PresetsComponent::updateButtonColors()
{
    for (int i = 0; i < buttons.size(); ++i)
    {
        buttons[i]->setColour(juce::TextButton::ColourIds::buttonColourId, defaultButtonColor);
        if (i == lastLoaded)
            buttons[i]->setColour(juce::TextButton::ColourIds::buttonColourId,
                                  juce::Colours::orange);
        if (i == lastSaved)
            buttons[i]->setColour(juce::TextButton::ColourIds::buttonColourId, juce::Colours::red);
    }
}
