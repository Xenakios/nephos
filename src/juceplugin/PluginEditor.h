#pragma once

#include "PluginProcessor.h"
#include "juce_core/juce_core.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "xap_slider.h"
#include "dashboardcomponent.h"
#include "dropdowncomponent.h"
#include "modulecomponents.h"

class MyCustomLNF : public juce::LookAndFeel_V4
{
  public:
    // Update the constructor/member to avoid that deprecation warning!
    juce::Font myFont{juce::FontOptions("Comic Sans MS", 20.0f, juce::Font::bold)};

    // This covers Labels (and many components that use Labels internally)
    juce::Font getLabelFont(juce::Label &l) override { return myFont; }

    // This covers standard TextButtons
    juce::Font getTextButtonFont(juce::TextButton &b, int buttonHeight) override { return myFont; }

    // This ensures custom typefaces are retrieved if something asks for it
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font &f) override
    {
        return myFont.getTypefacePtr();
    }
};

struct PresetsComponent : public juce::Component
{
    PresetsComponent()
    {
        for (int i = 0; i < 64; ++i)
        {
            auto but = std::make_unique<juce::TextButton>();
            but->setButtonText(juce::String(i + 1));
            but->onClick = [this, i]() {
                auto mods = juce::ModifierKeys::getCurrentModifiers();
                if (mods.isCommandDown())
                {
                    lastSaved = i;
                    if (OnSave)
                        OnSave(i);
                }
                else
                {
                    lastLoaded = i;
                    if (OnLoad)
                        OnLoad(i);
                }
                updateButtonColors();
            };
            addAndMakeVisible(*but);
            buttons.push_back(std::move(but));
        }
        defaultButtonColor =
            buttons.front()->findColour(juce::TextButton::ColourIds::buttonColourId);
    }
    void resized() override
    {
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        flex.flexWrap = juce::FlexBox::Wrap::wrap;
        for (auto &b : buttons)
        {
            flex.items.add(
                juce::FlexItem(*b).withFlex(1.0).withMinWidth(40.0f).withMaxWidth(40.0f));
        }
        flex.performLayout(getLocalBounds());
    }
    void updateButtonColors()
    {
        for (int i = 0; i < buttons.size(); ++i)
        {
            buttons[i]->setColour(juce::TextButton::ColourIds::buttonColourId, defaultButtonColor);
            if (i == lastLoaded)
                buttons[i]->setColour(juce::TextButton::ColourIds::buttonColourId,
                                      juce::Colours::orange);
            if (i == lastSaved)
                buttons[i]->setColour(juce::TextButton::ColourIds::buttonColourId,
                                      juce::Colours::red);
        }
    }
    std::function<void(int)> OnSave;
    std::function<void(int)> OnLoad;
    int lastSaved = -1;
    int lastLoaded = -1;
    juce::Colour defaultButtonColor;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
};

struct GUIParam : public juce::Component
{
    std::unique_ptr<juce::Label> parLabel;
    std::unique_ptr<juce::Slider> slider;
    std::unique_ptr<juce::ComboBox> combo;
    std::unique_ptr<juce::SliderParameterAttachment> slidAttach;
    std::unique_ptr<juce::ComboBoxParameterAttachment> choiceAttach;
    GUIParam() {}
    void resized() override
    {
        auto layout = juce::FlexBox(juce::FlexBox::Direction::row, juce::FlexBox::Wrap::noWrap,
                                    juce::FlexBox::AlignContent::spaceAround,
                                    juce::FlexBox::AlignItems::stretch,
                                    juce::FlexBox::JustifyContent::flexStart);
        layout.items.add(juce::FlexItem(*parLabel).withFlex(1.0));
        if (slider)
            layout.items.add(juce::FlexItem(*slider).withFlex(2.5));
        if (combo)
            layout.items.add(juce::FlexItem(*combo).withFlex(2.5));
        layout.performLayout(juce::Rectangle<int>{0, 0, getWidth(), getHeight()});
    }
};

struct LFOComponent : public juce::Component
{
    LFOComponent(int index, ToneGranulator *g)
        : lfoindex(index), gr(g),
          rateSlider(XapSlider::SS_Knob, *g->idtoparmetadata[ToneGranulator::PAR_LFORATES + index]),
          deformSlider(XapSlider::SS_Knob,
                       *g->idtoparmetadata[ToneGranulator::PAR_LFODEFORMS + index]),
          shiftSlider(XapSlider::SS_Knob,
                      *g->idtoparmetadata[ToneGranulator::PAR_LFOSHIFTS + index]),
          warpSlider(XapSlider::SS_Knob, *g->idtoparmetadata[ToneGranulator::PAR_LFOWARPS + index]),
          shapeSlider(XapSlider::SS_HorizontalSlider,
                      *g->idtoparmetadata[ToneGranulator::PAR_LFOSHAPES + index]),
          unipolarSlider(XapSlider::SS_HorizontalSlider,
                         *g->idtoparmetadata[ToneGranulator::PAR_LFOUNIPOLARS + index])
    {
        addAndMakeVisible(rateSlider);
        rateSlider.OnValueChanged = [this]() {
            stateChangedCallback(rateSlider.getParameterMetaData().id, rateSlider.getValue());
        };

        addAndMakeVisible(deformSlider);
        deformSlider.OnValueChanged = [this]() {
            stateChangedCallback(deformSlider.getParameterMetaData().id, deformSlider.getValue());
        };

        addAndMakeVisible(shiftSlider);
        shiftSlider.OnValueChanged = [this]() {
            stateChangedCallback(shiftSlider.getParameterMetaData().id, shiftSlider.getValue());
        };

        addAndMakeVisible(warpSlider);
        warpSlider.OnValueChanged = [this]() {
            stateChangedCallback(warpSlider.getParameterMetaData().id, warpSlider.getValue());
        };

        addAndMakeVisible(shapeSlider);
        shapeSlider.OnValueChanged = [this]() {
            stateChangedCallback(shapeSlider.getParameterMetaData().id, shapeSlider.getValue());
        };

        addAndMakeVisible(unipolarSlider);
        unipolarSlider.OnValueChanged = [this]() {
            stateChangedCallback(unipolarSlider.getParameterMetaData().id,
                                 unipolarSlider.getValue());
        };
    }
    void resized()
    {
        shapeSlider.setBounds(0, 0, 240, 25);
        unipolarSlider.setBounds(shapeSlider.getRight() + 1, 0, 100, 25);

        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;

        flex.items.add(juce::FlexItem(rateSlider).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(deformSlider).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(shiftSlider).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(warpSlider).withFlex(1.0).withMargin(2));
        flex.performLayout(juce::Rectangle<int>(0, 25, getWidth(), getHeight() - 25));
    }
    int lfoindex = -1;
    ToneGranulator *gr = nullptr;
    std::function<void(uint32_t, float)> stateChangedCallback;

    XapSlider rateSlider;
    XapSlider deformSlider;
    XapSlider shiftSlider;
    XapSlider warpSlider;
    XapSlider shapeSlider;
    XapSlider unipolarSlider;
};

struct StepSeqComponent : public juce::Component
{
    uint16_t playingStep = 0;
    xenakios::Xoroshiro128Plus rng;
    juce::Range<int> editRange{0, 8};
    void updateGUI()
    {
        playingStep = gr->stepModSources[sindex].curstepforgui;
        unipolarBut.setToggleState(gr->stepModSources[sindex].unipolar.load(),
                                   juce::dontSendNotification);
        repaint();
    }
    void runExternalProgram();
    void runJSInThread();
    std::atomic<int> js_status{0};
    juce::SpinLock spinlock;
    std::string js_error;
    juce::TextButton cancelButton;
    StepSeqComponent(int seqindex, ToneGranulator *g, juce::ThreadPool *tp);
    void mouseDown(const juce::MouseEvent &ev) override
    {
        if (!ev.mods.isRightButtonDown())
            return;
        juce::PopupMenu menu;
        menu.addSectionHeader("Play mode");
        for (int i = 0; i < StepModSource::NUMPLAYMODES; ++i)
        {
            menu.addItem(StepModSource::getPlayModeName(i), [i, this]() {
                StepModSource::Message msg;
                msg.opcode = StepModSource::Message::OP_PLAYMODE;
                msg.dest = sindex;
                msg.ival0 = i;
                gr->fifo.push(msg);
            });
        }
        menu.showMenuAsync(juce::PopupMenu::Options{});
    }
    bool keyPressed(const juce::KeyPress &ev) override;

    int graphxpos = 200;
    void resized() override
    {
        unipolarBut.setBounds(0, 1, graphxpos, 25);
        cancelButton.setBounds(0, unipolarBut.getBottom() + 1, graphxpos, 25);
    }
    void paint(juce::Graphics &g) override;

    void setLoopFromSelection()
    {
        gr->fifo.push({StepModSource::Message::OP_LOOPSTART, sindex, 0.0f, editRange.getStart()});
        gr->fifo.push({StepModSource::Message::OP_LOOPLEN, sindex, 0.0f, editRange.getLength()});
    }

    juce::ToggleButton unipolarBut;
    juce::Slider par0Slider;
    juce::TextEditor scriptParamsEditor;
    bool autoSetLoop = false;
    ToneGranulator *gr = nullptr;
    uint32_t sindex = 0;
    juce::ThreadPool *threadPool = nullptr;
};

struct ModulationRowComponent : public juce::Component
{
    void fillDropWithSources(DropDownComponent &drop, std::string roottext)
    {
        drop.rootNode.text = roottext;
        std::map<std::string, DropDownComponent::Node *> nodemap;
        drop.rootNode.children.reserve(16);
        for (int i = 0; i < gr->modSources.size(); ++i)
        {
            auto &ms = gr->modSources[i];
            if (!ms.groupname.empty())
            {
                if (nodemap.count(ms.groupname) == 0)
                {
                    drop.rootNode.children.push_back({ms.groupname, -1});
                    nodemap[ms.groupname] = &drop.rootNode.children.back();
                }
            }
        }
        for (int i = 0; i < gr->modSources.size(); ++i)
        {
            auto &ms = gr->modSources[i];
            if (ms.groupname.empty())
            {
                drop.rootNode.children.push_back({ms.name, (int)ms.id.src});
            }
            else
            {
                nodemap[ms.groupname]->children.push_back({ms.name, (int)ms.id.src});
            }
        }
        drop.setSelectedId(0);
    }
    using Node = DropDownComponent::Node;
    ModulationRowComponent(ToneGranulator *g) : gr(g)
    {
        addAndMakeVisible(sourceDrop);
        addAndMakeVisible(viaDrop);
        addAndMakeVisible(depthSlider);

        auto updatfunc = [this] {
            CallbackParams pars{false,
                                modslotindex,
                                (int)sourceDrop.selectedId,
                                (int)viaDrop.selectedId,
                                (int)curveDrop.selectedId,
                                (float)depthSlider.getValue(),
                                (uint32_t)destDrop.selectedId};
            stateChangedCallback(pars);
        };
        fillDropWithSources(sourceDrop, "Modulation source");
        sourceDrop.OnItemSelected = updatfunc;
        fillDropWithSources(viaDrop, "Modulation via source");
        viaDrop.OnItemSelected = updatfunc;

        depthSlider.OnValueChanged = [this]() {
            CallbackParams pars{true,
                                modslotindex,
                                (int)sourceDrop.selectedId,
                                (int)viaDrop.selectedId,
                                (int)curveDrop.selectedId,
                                (float)depthSlider.getValue(),
                                (uint32_t)destDrop.selectedId};
            stateChangedCallback(pars);
        };

        addAndMakeVisible(curveDrop);
        using mcf = GranulatorModConfig;

        curveDrop.rootNode.text = "Curve";
        curveDrop.rootNode.children.push_back(Node{"Linear", GranulatorModConfig::CURVE_LINEAR});
        curveDrop.rootNode.children.push_back(
            Node{"UNIPOL TO BIPOL", GranulatorModConfig::CURVE_UNIPOLARTOBIPOLAR});
        curveDrop.rootNode.children.push_back(
            Node{"BIPOL TO UNIPOL", GranulatorModConfig::CURVE_BIPOLARTOUNIPOLAR});
        curveDrop.rootNode.children.push_back(Node{"x^2", GranulatorModConfig::CURVE_SQUARE});
        curveDrop.rootNode.children.push_back(Node{"x^3", GranulatorModConfig::CURVE_CUBE});
        curveDrop.rootNode.children.push_back(Node{"x^16", GranulatorModConfig::CURVE_TOPOWER16});

        curveDrop.rootNode.children.push_back(Node{"EXPSIN 1", GranulatorModConfig::CURVE_EXPSIN1});
        curveDrop.rootNode.children.push_back(Node{"EXPSIN 2", GranulatorModConfig::CURVE_EXPSIN2});
        curveDrop.rootNode.children.push_back(
            Node{"HARMONICS 3 OCTAVES", GranulatorModConfig::CURVE_HARMONICSERIES3OCTAVES});
        curveDrop.rootNode.children.push_back(
            Node{"HARMONICS 4 OCTAVES", GranulatorModConfig::CURVE_HARMONICSERIES4OCTAVES});
        curveDrop.rootNode.children.push_back(
            Node{"HARMONICS 5 OCTAVES", GranulatorModConfig::CURVE_HARMONICSERIES5OCTAVES});
        Node peakingnode{"PEAKING"};
        for (int i = 0; i < 4; ++i)
        {
            peakingnode.children.push_back(
                Node{fmt::format("PEAKING {}", i + 1), GranulatorModConfig::CURVE_PEAKING1 + i});
        }
        curveDrop.rootNode.children.emplace_back(peakingnode);
        Node xornode{"XOR"};
        for (int i = 0; i < 4; ++i)
        {
            xornode.children.push_back(
                Node{fmt::format("XOR {}", i + 1), GranulatorModConfig::CURVE_XOR1 + i});
        }
        curveDrop.rootNode.children.emplace_back(xornode);
        Node stepnode{"STEPS"};
        for (int i = 0; i < 16; ++i)
        {
            int actnumsteps = i + 2;
            stepnode.children.push_back(
                Node{fmt::format("{} STEPS", actnumsteps), GranulatorModConfig::CURVE_STEPS1 + i});
        }
        curveDrop.rootNode.children.emplace_back(stepnode);
        curveDrop.rootNode.children.push_back(
            Node{"BIT MIRROR", GranulatorModConfig::CURVE_BITMIRROR});
        curveDrop.setSelectedId(GranulatorModConfig::CURVE_LINEAR);
        curveDrop.OnItemSelected = updatfunc;

        addAndMakeVisible(destDrop);
        initDestinationDrop();
        destDrop.setSelectedId(1);
        destDrop.OnItemSelected = [updatfunc, this]() {
            auto id = destDrop.selectedId;
            if (id > 0)
            {
                if (id > 1)
                {
                    auto pmd = gr->idtoparmetadata[destDrop.selectedId];
                    auto d = gr->modRanges[destDrop.selectedId];
                    depthSlider.setModulationDisplayDepth(d, pmd->unit);
                }
                updatfunc();
            }
        };
        addAndMakeVisible(slotLabel);
        slotLabel.setJustificationType(juce::Justification::centred);
    }
    void initDestinationDrop()
    {
        destDrop.rootNode.children.clear();
        destDrop.rootNode.text = "Modulation target";
        destDrop.rootNode.children.push_back({"No target", 1});
        std::map<std::string, Node *> nodemap;
        destDrop.rootNode.children.reserve(128);
        for (auto &pmd : gr->parmetadatas)
        {
            if (pmd.flags & CLAP_PARAM_IS_MODULATABLE && !pmd.groupName.empty())
            {
                if (nodemap.count(pmd.groupName) == 0)
                {
                    destDrop.rootNode.children.push_back({pmd.groupName, 0});
                    nodemap[pmd.groupName] = &destDrop.rootNode.children.back();
                }
            }
        }
        for (auto &pmd : gr->parmetadatas)
        {
            if (pmd.flags & CLAP_PARAM_IS_MODULATABLE)
            {
                if (pmd.groupName.empty())
                {
                    destDrop.rootNode.children.push_back({pmd.name, (int)pmd.id});
                }
                else
                {
                    nodemap[pmd.groupName]->children.push_back({pmd.name, (int)pmd.id});
                }
            }
        }
        destDrop.setSelectedId(destDrop.getSelectedId());
    }
    void setTarget(uint32_t parid)
    {
        destDrop.setSelectedId(parid);
        if (parid > 1)
        {
            auto pmd = gr->idtoparmetadata[destDrop.selectedId];
            auto d = gr->modRanges[destDrop.selectedId];
            depthSlider.setModulationDisplayDepth(d, pmd->unit);
        }
    }

    void resized() override
    {
        slotLabel.setText(juce::String(modslotindex + 1), juce::dontSendNotification);
        auto layout = juce::FlexBox(juce::FlexBox::Direction::row, juce::FlexBox::Wrap::noWrap,
                                    juce::FlexBox::AlignContent::spaceAround,
                                    juce::FlexBox::AlignItems::stretch,
                                    juce::FlexBox::JustifyContent::flexStart);
        layout.items.add(juce::FlexItem(slotLabel).withFlex(0.15));
        layout.items.add(juce::FlexItem(sourceDrop).withFlex(0.5));
        layout.items.add(juce::FlexItem(viaDrop).withFlex(0.5));
        layout.items.add(juce::FlexItem(depthSlider).withFlex(2.0));
        layout.items.add(juce::FlexItem(curveDrop).withFlex(0.5));
        layout.items.add(juce::FlexItem(destDrop).withFlex(0.5));
        layout.performLayout(juce::Rectangle<int>{0, 0, getWidth(), getHeight()});
    }
    ToneGranulator *gr = nullptr;
    struct CallbackParams
    {
        bool onlydepth = false;
        int slot = 0;
        int source = 0;
        int via = 0;
        int curve = 1;
        float depth = 0.0f;
        uint32_t target;
    };
    std::function<void(CallbackParams)> stateChangedCallback;
    int modslotindex = -1;
    juce::Label slotLabel;
    DropDownComponent sourceDrop;
    DropDownComponent viaDrop;
    XapSlider depthSlider{XapSlider::SS_HorizontalSlider,
                          ParamDesc()
                              .asFloat()
                              .withName("DEPTH")
                              .withRange(-1.0f, 1.0f)
                              .withLinearScaleFormatting("%", 100.0f)};
    DropDownComponent curveDrop;
    DropDownComponent destDrop;
};

class MainPageComponent final : public juce::Component
{
  public:
    explicit MainPageComponent(AudioPluginAudioProcessor &);
    ~MainPageComponent() override;

    //==============================================================================
    void paint(juce::Graphics &) override;
    void resized() override;

    // MyCustomLNF lnf;
    AudioPluginAudioProcessor &processorRef;
    OscillatorModuleComponent oscModuleComponent;
    MainOutputModule mainOutModuleComponent;
    SpatializationModuleComponent spatModuleComponent;
    VolumeModuleComponent volumeModuleComponent;
    TimeModuleComponent timeModuleComponent;
    StackingModuleComponent stackModuleComponent;
    std::vector<std::unique_ptr<InsertModuleComponent>> insertComponents;

    std::unique_ptr<juce::TextButton> recordButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainPageComponent)
};

class DashPage : public juce::Component
{
  public:
    DashPage(AudioPluginAudioProcessor &p) : processorRef(p), dashBoardComponent(&p.granulator)
    {
        presetsComponent.OnSave = [this](int index) { saveSnapShot(index); };
        presetsComponent.OnLoad = [this](int index) { loadSnapShot(index); };
        dashBoardComponent.GetCPULoad = [this]() {
            return processorRef.perfMeasurer.getLoadAsProportion();
        };
        addAndMakeVisible(dashBoardComponent);
        addAndMakeVisible(presetsComponent);
        for (int i = 0; i < 16; ++i)
        {
            ParamDesc pmd = ParamDesc()
                                .asFloat()
                                .withRange(-1.0, 1.0)
                                .withName(fmt::format("M{}", i + 1))
                                .withLinearScaleFormatting("");
            auto knob = std::make_unique<XapSlider>(XapSlider::SS_Knob, pmd);
            knob->OnValueChanged = [this, i, knobptr = knob.get()]() {
                processorRef.handleMacroKnob(i, knobptr->getValue(), false);
            };
            addAndMakeVisible(knob.get());
            perfSliders.push_back(std::move(knob));
        }
    }
    void loadSnapShot(int index) { processorRef.loadSnapShot(index); }
    void saveSnapShot(int index)
    {
        auto state = processorRef.getState();
        std::ofstream ostream(fmt::format("{}{}.json", processorRef.presetsPath, index + 1));
        choc::json::writeAsJSON(ostream, state, true);
        processorRef.saveSnapShot(index, state);
    }

    void resized() override
    {
        presetsComponent.setBounds(0, 0, getWidth(), 50);
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        for (auto &c : perfSliders)
        {
            flex.items.add(juce::FlexItem(*c).withFlex(1.0).withMaxHeight(70));
        }
        flex.performLayout(juce::Rectangle<int>(0, 50, getWidth(), 70));
        dashBoardComponent.setBounds(0, 121, getWidth(), getHeight() - 121);
    }
    AudioPluginAudioProcessor &processorRef;
    PresetsComponent presetsComponent;
    DashBoardComponent dashBoardComponent;
    std::vector<std::unique_ptr<XapSlider>> perfSliders;
};

class ModulationPage : public juce::Component
{
  public:
    ModulationPage(AudioPluginAudioProcessor &p)
        : processorRef(p), stepSeqTabs(juce::TabbedButtonBar::Orientation::TabsAtTop)
    {
        for (int i = 0; i < 8; ++i)
        {
            auto lfoc = std::make_unique<LFOComponent>(i, &processorRef.granulator);
            lfoc->stateChangedCallback = [this](uint32_t parid, float val) {
                ParameterMessage parmsg;
                parmsg.id = parid;
                parmsg.value = val;
                processorRef.params_from_gui_fifo.push(parmsg);
            };
            /*
            idToSlider[ToneGranulator::PAR_LFORATES + i] = &lfoc->rateSlider;
            idToSlider[ToneGranulator::PAR_LFODEFORMS + i] = &lfoc->deformSlider;
            idToSlider[ToneGranulator::PAR_LFOSHIFTS + i] = &lfoc->shiftSlider;
            idToSlider[ToneGranulator::PAR_LFOWARPS + i] = &lfoc->warpSlider;
            idToSlider[ToneGranulator::PAR_LFOSHAPES + i] = &lfoc->shapeSlider;
            idToSlider[ToneGranulator::PAR_LFOUNIPOLARS + i] = &lfoc->unipolarSlider;
            lfoTabs.addTab("LFO " + juce::String(i + 1), juce::Colours::darkgrey, lfoc.get(),
                           false);
            */
            addAndMakeVisible(*lfoc);
            lfocomps.push_back(std::move(lfoc));
        }
        for (int i = 0; i < 8; ++i)
        {
            auto stepcomp = std::make_unique<StepSeqComponent>(i, &processorRef.granulator,
                                                               &processorRef.tpool);
            stepSeqTabs.addTab("STEP SEQ " + juce::String(i + 1), juce::Colours::darkgrey,
                               stepcomp.get(), false);
            stepcomps.push_back(std::move(stepcomp));
        }
        addAndMakeVisible(stepSeqTabs);
        for (int i = 0; i < 16; ++i)
        {
            auto modcomp = std::make_unique<ModulationRowComponent>(&processorRef.granulator);
            modcomp->modslotindex = i;
            modcomp->stateChangedCallback = [this](ModulationRowComponent::CallbackParams args) {
                if (args.slot >= 0 && args.source >= 0 && args.target >= 0)
                {
                    processorRef.updateHostDisplay(
                        juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
                    ThreadMessage msg;
                    msg.modslot = args.slot;
                    msg.depth = args.depth;
                    msg.modsource = args.source;
                    msg.modvia = args.via;
                    msg.moddest = args.target;
                    msg.modcurve = args.curve;
                    msg.opcode = ThreadMessage::OP_MODROUTING;
                    if (args.onlydepth)
                    {
                        msg.opcode = ThreadMessage::OP_MODPARAM;
                    }
                    processorRef.from_gui_fifo.push(msg);
                }
            };
            addAndMakeVisible(*modcomp);
            modRowComps.push_back(std::move(modcomp));
        }
    }
    void resized() override
    {
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::column;
        flex.flexWrap = juce::FlexBox::Wrap::wrap;
        for (int i = 0; i < lfocomps.size(); ++i)
        {
            flex.items.add(
                juce::FlexItem(*lfocomps[i]).withFlex(1.0).withMargin(2.0).withMinHeight(80.0));
        }
        flex.performLayout(juce::Rectangle<int>(0, 0, getWidth(), 175));
        stepSeqTabs.setBounds(0, lfocomps.back()->getBottom() + 2, getWidth(), 120);
        juce::FlexBox modrowflex;
        modrowflex.flexDirection = juce::FlexBox::Direction::column;
        modrowflex.flexWrap = juce::FlexBox::Wrap::wrap;
        for (int i = 0; i < modRowComps.size(); ++i)
        {
            modrowflex.items.add(
                juce::FlexItem(*modRowComps[i]).withFlex(1).withMinHeight(25).withMargin(1));
        }
        int yoffs = stepSeqTabs.getBottom() + 1;
        modrowflex.performLayout(juce::Rectangle<int>{0, yoffs, getWidth(), 220});
    }
    AudioPluginAudioProcessor &processorRef;
    juce::TabbedComponent stepSeqTabs;
    std::vector<std::unique_ptr<LFOComponent>> lfocomps;
    std::vector<std::unique_ptr<StepSeqComponent>> stepcomps;
    std::vector<std::unique_ptr<ModulationRowComponent>> modRowComps;
};

class AudioPluginAudioProcessorEditor final : public juce::AudioProcessorEditor, public juce::Timer
{
  public:
    explicit AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor &);
    ~AudioPluginAudioProcessorEditor() override;
    void resized() override;
    void timerCallback() override;
    AudioPluginAudioProcessor &processorRef;
    MainPageComponent mainPage;
    ModulationPage modulationPage;
    DashPage dashPage;
    juce::TabbedComponent mainTabs;
    std::unordered_map<uint32_t, XapSlider *> idToSlider;
    void addChildSlidersFrom(juce::Component &c);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessorEditor)
};
