#pragma once

#include "PluginProcessor.h"
#include "../juceplugin/xap_slider.h"
#include "juce_events/juce_events.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include <unordered_map>

inline void add_xapslider(AudioPluginAudioProcessor &proc, juce::Component &parentcomp,
                          XapSlider &slid)
{
    parentcomp.addAndMakeVisible(slid);
    slid.OnValueChanged = [&slid, &proc]() {
        ParamChangeMessage msg;
        msg.id = slid.getParameterMetaData().id;
        msg.val = slid.getValue();
        proc.params_from_gui_fifo.push(msg);
    };
}

class ShifterComponent : public juce::Component
{
  public:
    AudioPluginAudioProcessor &procRef;
    ShifterComponent(AudioPluginAudioProcessor &p, int shifter_index)
        : procRef(p),
          enabledButton(
              XapSlider::SS_HorizontalSlider,
              *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_ENABLED + shifter_index]),
          inputGainKnob(
              XapSlider::SS_Knob,
              *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_INPUT_GAIN + shifter_index]),
          pitchKnob(XapSlider::SS_Knob,
                    *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_PITCH + shifter_index]),
          formantKnob(XapSlider::SS_Knob,
                      *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_FORMANT + shifter_index])
    {
        addAndMakeVisible(lab);
        lab.setText(juce::String(shifter_index + 1), juce::dontSendNotification);
        add_xapslider(p, *this, enabledButton);
        add_xapslider(p, *this, inputGainKnob);
        add_xapslider(p, *this, pitchKnob);
        add_xapslider(p, *this, formantKnob);
    }
    void resized() override
    {
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        flex.items.add(juce::FlexItem(lab).withFlex(0.1));
        flex.items.add(juce::FlexItem(enabledButton).withFlex(0.5).withMaxHeight(25));
        flex.items.add(juce::FlexItem(inputGainKnob).withFlex(1));
        flex.items.add(juce::FlexItem(pitchKnob).withFlex(1));
        flex.items.add(juce::FlexItem(formantKnob).withFlex(1));
        for (auto &i : flex.items)
        {
            i = i.withMargin(2.0f);
        }
        flex.performLayout(getLocalBounds());
    }
    juce::Label lab;
    XapSlider enabledButton;
    XapSlider inputGainKnob;
    XapSlider pitchKnob;
    XapSlider formantKnob;
    // XapSlider spatKnob;
};

class AudioPluginAudioProcessorEditor final : public juce::AudioProcessorEditor, public juce::Timer
{
  public:
    explicit AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor &);
    ~AudioPluginAudioProcessorEditor() override;

    //==============================================================================
    void paint(juce::Graphics &) override;
    void resized() override;
    void timerCallback() override;

  private:
    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    AudioPluginAudioProcessor &processorRef;
    std::vector<std::unique_ptr<ShifterComponent>> shiftercomps;
    XapSlider lfoShapeKnob;
    XapSlider lfoRateKnob;
    XapSlider lfoDeformKnob;
    XapSlider lfoShiftKnob;
    XapSlider lfoWarpKnob;
    XapSlider lfoAmountKnob;
    std::unordered_map<uint32_t, XapSlider *> paridtoslidermap;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessorEditor)
};
