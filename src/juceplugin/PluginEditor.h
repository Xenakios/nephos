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
#include "imtest.h"

struct MacrosPresetsComponent : public juce::Component
{
    AudioPluginAudioProcessor &processorRef;
    MacrosPresetsComponent(AudioPluginAudioProcessor &p);

    void resized() override;
    void updateButtonColors();

    int lastSaved = -1;
    int lastLoaded = -1;
    juce::Colour defaultButtonColor;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    juce::TextButton menuButton;
    std::vector<std::unique_ptr<XapSlider>> perfSliders;
};

struct ModulationRowComponent : public juce::Component
{
    void fillPickerWithCurves(GalleryPicker &picker)
    {
        auto curves = GranulatorModConfig::get_curve_metadata();
        std::map<std::string, GalleryPicker::Category *> catmap;
        picker.categories.reserve(32);
        for (int i = 0; i < curves.size(); ++i)
        {
            auto &md = curves[i];
            // if (!md.groupname.empty())
            {
                if (catmap.count(md.groupname) == 0)
                {
                    GalleryPicker::Category cat;
                    cat.text = md.groupname;
                    picker.categories.push_back(cat);
                    catmap[md.groupname] = &picker.categories.back();
                }
            }
        }
        for (int i = 0; i < curves.size(); ++i)
        {
            auto &md = curves[i];

            catmap[md.groupname]->items.push_back({md.id, md.name});
        }
    }
    void fillPickerWithSources(GalleryPicker &gal)
    {
        std::map<std::string, GalleryPicker::Category *> catmap;
        gal.categories.clear();
        gal.categories.reserve(64);
        for (int i = 0; i < gr->modSourceInfos.size(); ++i)
        {
            auto &ms = gr->modSourceInfos[i];
            // if (!ms.groupname.empty())
            {
                if (catmap.count(ms.groupname) == 0)
                {
                    GalleryPicker::Category cate;
                    cate.text = ms.groupname;
                    gal.categories.push_back(cate);
                    catmap[ms.groupname] = &gal.categories.back();
                }
            }
        }
        for (int i = 0; i < gr->modSourceInfos.size(); ++i)
        {
            auto &ms = gr->modSourceInfos[i];
            // if (!ms.groupname.empty())
            {
                GalleryPicker::Item it;
                it.id = ms.id.src;
                it.text = ms.name;
                catmap[ms.groupname]->items.push_back(it);
            }
        }
    }
    using Node = DropDownComponent::Node;
    AudioPluginAudioProcessor &processorRef;
    ModulationRowComponent(AudioPluginAudioProcessor &proc, int modindex);
    void update_source(int64_t id)
    {
        sourcePicker.selectedID = id;
        auto txt = sourcePicker.get_text_from_id(id);
        if (txt)
            sourcePicker.showButton.setButtonText(*txt);
    }
    void update_via(int64_t id)
    {
        viaPicker.selectedID = id;
        auto txt = viaPicker.get_text_from_id(id);
        if (txt)
            viaPicker.showButton.setButtonText(*txt);
    }
    void update_curve(int64_t id)
    {
        curvePicker.selectedID = id;
        auto txt = curvePicker.get_text_from_id(id);
        if (txt)
            curvePicker.showButton.setButtonText(*txt);
    }
    void update_destination(int64_t id)
    {
        destPicker.selectedID = id;
        auto txt = destPicker.get_text_from_id(id);
        if (txt)
            destPicker.showButton.setButtonText(*txt);
    }
    void initDestinationPicker()
    {
        destPicker.categories.clear();
        destPicker.categories.reserve(64);
        std::map<std::string, GalleryPicker::Category *> catmap;
        destPicker.categories.emplace_back("");
        destPicker.categories.back().items.push_back(GalleryPicker::Item(1, "None"));
        for (auto &pmd : gr->parmetadatas)
        {
            if (pmd.flags & CLAP_PARAM_IS_MODULATABLE && !pmd.groupName.empty())
            {
                if (catmap.count(pmd.groupName) == 0)
                {
                    GalleryPicker::Category cat;
                    cat.text = pmd.groupName;
                    destPicker.categories.push_back(cat);
                    catmap[pmd.groupName] = &destPicker.categories.back();
                }
            }
        }
        for (auto &pmd : gr->parmetadatas)
        {
            if (pmd.flags & CLAP_PARAM_IS_MODULATABLE)
            {
                if (!pmd.groupName.empty())
                {
                    GalleryPicker::Item item;
                    item.text = pmd.name;
                    item.id = pmd.id;
                    catmap[pmd.groupName]->items.push_back(item);
                }
            }
        }
    }
    void setTarget(uint32_t parid)
    {
        destPicker.selectedID = parid;
        if (parid > 1)
        {
            auto pmd = gr->idtoparmetadata[parid];
            auto d = gr->modRanges[parid];
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
        layout.items.add(juce::FlexItem(sourcePicker.showButton).withFlex(0.5));
        layout.items.add(juce::FlexItem(viaPicker.showButton).withFlex(0.5));
        layout.items.add(juce::FlexItem(depthSlider).withFlex(2.0));
        layout.items.add(juce::FlexItem(curvePicker.showButton).withFlex(0.5));
        layout.items.add(juce::FlexItem(destPicker.showButton).withFlex(0.5));
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
    GalleryPicker sourcePicker;
    GalleryPicker viaPicker;
    GalleryPicker curvePicker;
    GalleryPicker destPicker;

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
    DashPage(AudioPluginAudioProcessor &p) : processorRef(p), dashBoardComponent(p)
    {
        dashBoardComponent.GetCPULoad = [this]() {
            return processorRef.perfMeasurer.getLoadAsProportion();
        };
        addAndMakeVisible(dashBoardComponent);
    }
    void resized() override { dashBoardComponent.setBounds(0, 0, getWidth(), getHeight()); }
    AudioPluginAudioProcessor &processorRef;
    DashBoardComponent dashBoardComponent;
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

        stepSeqTabs.addTab("AUDIO INPUT", juce::Colours::darkgrey, &analysisComponen, false);
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
    bool keyPressed(const juce::KeyPress &ev) override;
    AudioPluginAudioProcessor &processorRef;
    MacrosPresetsComponent macrosPresetsComp;
    MainPageComponent mainPage;
    ModulationPage modulationPage;
    DashPage dashPage;
    std::unique_ptr<IMTestComponent> imTest;
    juce::TabbedComponent mainTabs;
    std::unordered_map<uint32_t, XapSlider *> idToSlider;
    void addChildSlidersFrom(juce::Component &c);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessorEditor)
};
