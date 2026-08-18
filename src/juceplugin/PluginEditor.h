#pragma once

#include "PluginProcessor.h"
#include "audiovisualizercomponent.h"
#include "clap/id.h"
#include "containers/choc_Value.h"
#include "juce_audio_utils/juce_audio_utils.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"
#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "xap_slider.h"
#include "dashboardcomponent.h"
#include "dropdowncomponent.h"
#include "modulecomponents.h"
#include "lfocomponent.h"
#include <memory>
#include <stdio.h>
#include <unordered_map>

struct PresetsComponent : public juce::Component
{
    AudioPluginAudioProcessor &processorRef;
    PresetsComponent(AudioPluginAudioProcessor &p) : processorRef(p)
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
    void mouseDown(const juce::MouseEvent &ev) override;
    void resized() override;
    void updateButtonColors();
    std::function<void(int)> OnSave;
    std::function<void(int)> OnLoad;
    int lastSaved = -1;
    int lastLoaded = -1;
    juce::Colour defaultButtonColor;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
};

struct ModulationRowComponent : public juce::Component
{
    void fillDropWithCurves(DropDownComponent &drop, std::string roottext)
    {
        auto curves = GranulatorModConfig::get_curve_metadata();
        drop.rootNode.text = roottext;
        std::map<std::string, DropDownComponent::Node *> nodemap;
        drop.rootNode.children.reserve(16);
        for (int i = 0; i < curves.size(); ++i)
        {
            auto &md = curves[i];
            if (!md.groupname.empty())
            {
                if (nodemap.count(md.groupname) == 0)
                {
                    drop.rootNode.children.push_back({md.groupname, -1});
                    nodemap[md.groupname] = &drop.rootNode.children.back();
                }
            }
        }
        for (int i = 0; i < curves.size(); ++i)
        {
            auto &md = curves[i];
            if (md.groupname.empty())
            {
                drop.rootNode.children.push_back({md.name, (int)md.id});
            }
            else
            {
                nodemap[md.groupname]->children.push_back({md.name, (int)md.id});
            }
        }
        drop.setSelectedId(0);
    }
    void fillDropWithSources(DropDownComponent &drop, std::string roottext)
    {
        drop.rootNode.text = roottext;
        std::map<std::string, DropDownComponent::Node *> nodemap;
        drop.rootNode.children.reserve(16);
        for (int i = 0; i < gr->modSourceInfos.size(); ++i)
        {
            auto &ms = gr->modSourceInfos[i];
            if (!ms.groupname.empty())
            {
                if (nodemap.count(ms.groupname) == 0)
                {
                    drop.rootNode.children.push_back({ms.groupname, -1});
                    nodemap[ms.groupname] = &drop.rootNode.children.back();
                }
            }
        }
        for (int i = 0; i < gr->modSourceInfos.size(); ++i)
        {
            auto &ms = gr->modSourceInfos[i];
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
    AudioPluginAudioProcessor &processorRef;
    ModulationRowComponent(AudioPluginAudioProcessor &proc, int modindex)
        : processorRef(proc), gr(&proc.granulator), modslotindex(modindex),
          depthSlider(XapSlider::SS_HorizontalSlider,
                      ParamDesc()
                          .withRange(-1.0f, 1.0f)
                          .withName("DEPTH")
                          .withLinearScaleFormatting("")
                          .withID(ToneGranulator::PAR_MAINMODDEPTHSTART + modindex))
    {
        addAndMakeVisible(sourceDrop);
        addAndMakeVisible(viaDrop);
        addAndMakeVisible(depthSlider);

        auto updatfunc = [this] {
            ThreadMessage msg;
            msg.modslot = modslotindex;
            msg.depth = depthSlider.getValue();
            msg.modsource = sourceDrop.selectedId;
            msg.modvia = viaDrop.selectedId;
            msg.moddest = destDrop.selectedId;
            msg.modcurve = curveDrop.selectedId;
            msg.opcode = ThreadMessage::OP_MODROUTING;
            processorRef.from_gui_fifo.push(msg);
        };
        fillDropWithSources(sourceDrop, "Modulation source");
        sourceDrop.OnItemSelected = updatfunc;
        fillDropWithSources(viaDrop, "Modulation via source");
        viaDrop.OnItemSelected = updatfunc;
        depthSlider.OnValueChanged = [this]() {
            ParameterMessage msg;
            msg.id = ToneGranulator::PAR_MAINMODDEPTHSTART + modslotindex;
            msg.value = depthSlider.getValue();
            processorRef.params_from_gui_fifo.push(msg);
        };
        /*
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
        */
        addAndMakeVisible(curveDrop);

        using mcf = GranulatorModConfig;
        fillDropWithCurves(curveDrop, "Curve");
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

    int modslotindex = -1;
    juce::Label slotLabel;
    DropDownComponent sourceDrop;
    DropDownComponent viaDrop;

    DropDownComponent curveDrop;
    DropDownComponent destDrop;

  private:
    XapSlider depthSlider;
};

class MainPageComponent final : public juce::Component
{
  public:
    explicit MainPageComponent(AudioPluginAudioProcessor &);
    ~MainPageComponent() override;

    //==============================================================================
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;

    AudioPluginAudioProcessor &processorRef;
    OscillatorModuleComponent oscModuleComponent;
    MainOutputModule mainOutModuleComponent;
    SpatializationModuleComponent spatModuleComponent;
    VolumeModuleComponent volumeModuleComponent;
    TimeModuleComponent timeModuleComponent;
    StackingModuleComponent stackModuleComponent;
    std::vector<std::unique_ptr<InsertModuleComponent>> insertComponents;

    juce::MidiKeyboardComponent keyboardComponent;

    // juce::TreeView testTree;
    struct MyTreeItem : public juce::TreeViewItem
    {
        juce::String itemText;
        bool containsSubItems = false;
        bool is_selected = false;
        bool mightContainSubItems() override { return containsSubItems; }
        void itemClicked(const juce::MouseEvent &ev) override { is_selected = true; }
        void paintItem(juce::Graphics &g, int width, int height) override
        {
            if (is_selected)
                g.fillAll(juce::Colours::lightblue);
            g.setColour(juce::Colours::white);
            g.drawText(itemText, 0, 0, width, height, juce::Justification::centredLeft);
        }
    };
    juce::TextButton corruptButton;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainPageComponent)
};

class DashPage : public juce::Component
{
  public:
    DashPage(AudioPluginAudioProcessor &p)
        : processorRef(p), presetsComponent(p), dashBoardComponent(p)
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
    void saveSnapShot(int index);

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
        : processorRef(p), analysisComponen(p),
          stepSeqTabs(juce::TabbedButtonBar::Orientation::TabsAtTop),
          masterRateKnob(XapSlider::SS_Knob,
                         *p.granulator.idtoparmetadata[ToneGranulator::PAR_MASTERLFORATE])
    {
        addAndMakeVisible(resetModsButton);
        addAndMakeVisible(masterRateKnob);
        initSlider(p, *this, masterRateKnob);
        resetModsButton.setButtonText("RESET MODULATOR PHASES");
        resetModsButton.onClick = [this]() {
            ThreadMessage msg;
            msg.opcode = ThreadMessage::OP_RESET_MODULATORS;
            processorRef.from_gui_fifo.push(msg);
        };
        for (int i = 0; i < 8; ++i)
        {
            auto lfoc = std::make_unique<LFOComponent>(i, &processorRef.granulator);
            lfoc->stateChangedCallback = [this](uint32_t parid, float val) {
                ParameterMessage parmsg;
                parmsg.id = parid;
                parmsg.value = val;
                processorRef.params_from_gui_fifo.push(parmsg);
            };
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
        for (int i = 0; i < processorRef.granulator.randomModSources.size(); ++i)
        {
            auto rc = std::make_unique<TriggeredRandomModuleComponent>(processorRef, i);
            stepSeqTabs.addTab("RANDOM " + juce::String(i + 1), juce::Colours::darkgrey, rc.get(),
                               false);
            randComponents.push_back(std::move(rc));
        }

        stepSeqTabs.addTab("AUDIO INPUT", juce::Colours::darkgrey, &analysisComponen,
                           false);
        addAndMakeVisible(stepSeqTabs);
        for (int i = 0; i < 16; ++i)
        {
            auto modcomp = std::make_unique<ModulationRowComponent>(processorRef, i);
            modcomp->modslotindex = i;
            addAndMakeVisible(*modcomp);
            modRowComps.push_back(std::move(modcomp));
        }
    }
    void resized() override
    {
        resetModsButton.setBounds(1, 1, 200, 38);
        masterRateKnob.setBounds(resetModsButton.getRight() + 2, 1, 80, 60);
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::column;
        flex.flexWrap = juce::FlexBox::Wrap::wrap;
        for (int i = 0; i < lfocomps.size(); ++i)
        {
            flex.items.add(
                juce::FlexItem(*lfocomps[i]).withFlex(1.0).withMargin(2.0).withMinHeight(80.0));
        }
        flex.performLayout(juce::Rectangle<int>(0, 61, getWidth(), 175));
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
    AnalysisSourceComponent analysisComponen;
    std::vector<std::unique_ptr<TriggeredRandomModuleComponent>> randComponents;
    std::vector<std::unique_ptr<LFOComponent>> lfocomps;
    std::vector<std::unique_ptr<StepSeqComponent>> stepcomps;
    std::vector<std::unique_ptr<ModulationRowComponent>> modRowComps;
    juce::TextButton resetModsButton;
    XapSlider masterRateKnob;
};

class AudioPluginAudioProcessorEditor final : public juce::AudioProcessorEditor, public juce::Timer
{
  public:
    explicit AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor &);
    ~AudioPluginAudioProcessorEditor() override;
    juce::Label overlaylabel;
    void setOverLaytext(juce::String txt, int delay_ms);

    void resized() override;
    void timerCallback() override;
    void updateParameterRemoteStates();
    AudioPluginAudioProcessor &processorRef;
    MainPageComponent mainPage;
    ModulationPage modulationPage;
    DashPage dashPage;
    juce::TabbedComponent mainTabs;
    std::unordered_map<uint32_t, XapSlider *> idToSlider;
    void addChildSlidersFrom(juce::Component &c);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessorEditor)
};
