#pragma once

#include "PluginProcessor.h"
#include "../juceplugin/xap_slider.h"
#include "clap/id.h"
#include "juce_events/juce_events.h"
#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include <stdio.h>
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

class MultiSlider : public juce::Component
{
  public:
    AudioPluginAudioProcessor &pref;
    uint32_t firstpar = CLAP_INVALID_ID;
    uint32_t numparams = 0;
    float parminval = 0.0f;
    float parmaxval = 1.0f;
    float pardefaultval = 0.0f;
    int draggedindex = -1;
    juce::String groupname;
    std::array<float, 256> values = {0.0f};
    MultiSlider(AudioPluginAudioProcessor &p, juce::String name, uint32_t parstartid,
                uint32_t numpars)
        : pref(p), firstpar(parstartid), numparams(numpars), groupname(name)
    {
        int numvalidated = 0;
        for (int i = 0; i < numpars; ++i)
        {
            auto it = pref.par_id_to_metadata.find(parstartid + i);
            if (it != pref.par_id_to_metadata.end())
            {
                ++numvalidated;
                parminval = it->second->minVal;
                parmaxval = it->second->maxVal;
                pardefaultval = it->second->defaultVal;
                values[i] = *pref.par_id_to_value_pointer[it->first];
            }
        }
        assert(numvalidated == numpars);
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(juce::Colours::black);
        g.setColour(juce::Colours::lightgrey);
        g.setFont(40.0);
        g.drawText(groupname, 0, 0, getWidth(), getHeight(), juce::Justification::centred);
        g.setFont(12.0);
        float barw = (float)getWidth() / numparams;
        for (int i = 0; i < numparams; ++i)
        {
            float xcor = barw * i;
            float v = values[i];
            auto partxt = pref.par_id_to_metadata[firstpar + i]->valueToString(v);
            if (partxt)
            {
                g.setColour(juce::Colours::white);
                g.drawText(*partxt, xcor, getHeight() - 20, barw, 20, juce::Justification::centred);
            }
            float ycor = juce::jmap<float>(v, parminval, parmaxval, getHeight(), 0.0);
            g.setColour(juce::Colours::yellow);
            g.drawLine(xcor, ycor, xcor + barw - 1.0, ycor, 2.0);
        }
    }
    void mouseDoubleClick(const juce::MouseEvent &ev) override
    {
        int shifterindex = (float)numparams / getWidth() * ev.x;
        if (shifterindex >= 0 && shifterindex < numparams)
        {
            post_param_event(shifterindex, pardefaultval);
            repaint();
        }
    }
    void mouseDown(const juce::MouseEvent &ev) override
    {
        draggedindex = -1;
        int shifterindex = (float)numparams / getWidth() * ev.x;
        if (shifterindex >= 0 && shifterindex < numparams)
        {
            draggedindex = shifterindex;
            post_param_event(draggedindex, parval_from_mouse_y(ev.y));
            repaint();
        }
    }
    float parval_from_mouse_y(float y)
    {
        float v = juce::jmap<float>(y, 0.0, getHeight(), parmaxval, parminval);
        v = std::clamp(v, parminval, parmaxval);
        return v;
    }
    // our own index, not the clap id!
    void post_param_event(int parindex, float val)
    {
        ParamChangeMessage msg;
        msg.id = firstpar + parindex;
        msg.val = val;
        pref.params_from_gui_fifo.push(msg);
        values[parindex] = val;
    }
    void mouseDrag(const juce::MouseEvent &ev) override
    {
        if (draggedindex >= 0 && draggedindex < numparams)
        {
            post_param_event(draggedindex, parval_from_mouse_y(ev.y));
            repaint();
        }
    }
    void update() { repaint(); }
};

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
          delayKnob(
              XapSlider::SS_Knob,
              *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_DELAYTIME + shifter_index]),
          pitchKnob(XapSlider::SS_Knob,
                    *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_PITCH + shifter_index]),
          formantKnob(
              XapSlider::SS_Knob,
              *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_FORMANT + shifter_index]),
          cutoffKnob(
              XapSlider::SS_Knob,
              *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_FILTERCUTOFF + shifter_index])
    {
        addAndMakeVisible(lab);
        lab.setText(juce::String(shifter_index + 1), juce::dontSendNotification);
        add_xapslider(p, *this, enabledButton);
        add_xapslider(p, *this, delayKnob);
        add_xapslider(p, *this, inputGainKnob);
        add_xapslider(p, *this, pitchKnob);
        add_xapslider(p, *this, formantKnob);
        add_xapslider(p, *this, cutoffKnob);
    }
    void resized() override
    {
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        // flex.items.add(juce::FlexItem(lab).withFlex(0.1));
        flex.items.add(juce::FlexItem(enabledButton).withFlex(0.5).withMaxHeight(25));
        flex.items.add(juce::FlexItem(inputGainKnob).withFlex(1));
        flex.items.add(juce::FlexItem(delayKnob).withFlex(1));
        flex.items.add(juce::FlexItem(pitchKnob).withFlex(1));
        flex.items.add(juce::FlexItem(formantKnob).withFlex(1));
        flex.items.add(juce::FlexItem(cutoffKnob).withFlex(1));
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
    XapSlider delayKnob;
    XapSlider cutoffKnob;
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
    MultiSlider pitchMulti;
    MultiSlider inVolumeMulti;
    MultiSlider cutoffMulti;
    MultiSlider aziMulti;
    std::unordered_map<uint32_t, XapSlider *> paridtoslidermap;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessorEditor)
};
