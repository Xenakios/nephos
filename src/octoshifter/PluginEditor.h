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
    uint32_t lfo0_amt_par = CLAP_INVALID_ID;
    float parminval = 0.0f;
    float parmaxval = 1.0f;
    float pardefaultval = 0.0f;
    int draggedindex = -1;
    juce::String groupname;
    std::array<float, 256> values = {0.0f};
    MultiSlider(AudioPluginAudioProcessor &p, juce::String name, uint32_t parstartid,
                uint32_t numpars, uint32_t lfo0par = CLAP_INVALID_ID)
        : pref(p), firstpar(parstartid), numparams(numpars), lfo0_amt_par(lfo0par), groupname(name)
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
        if (lfo0_amt_par != CLAP_INVALID_ID)
        {
            values[numpars] = *pref.par_id_to_value_pointer[lfo0_amt_par];
        }
        assert(numvalidated == numpars);
    }
    uint32_t param_id_from_index(int index)
    {
        if (index < 0 || index >= num_all_params())
            return CLAP_INVALID_ID;
        uint32_t pid = firstpar + index;
        if (index == numparams)
        {
            pid = lfo0_amt_par;
        }
        return pid;
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(juce::Colours::black);
        g.setColour(juce::Colours::lightgrey);
        g.setFont(40.0);
        auto polar = pref.par_id_to_metadata[firstpar]->getPolarity();
        juce::String txt = groupname;
        // if (polar == pmd::Polarity::BIPOLAR)
        //     txt += " (BIPOLAR)";
        g.drawText(txt, 0, 0, getWidth(), getHeight(), juce::Justification::centred);
        g.setFont(12.0);
        int parstodraw = num_all_params();
        float barw = (float)getWidth() / parstodraw;
        for (int i = 0; i < parstodraw; ++i)
        {
            float xcor = barw * i;
            float v = values[i];
            uint32_t pid = param_id_from_index(i);
            if (pid != CLAP_INVALID_ID)
            {
                auto partxt = pref.par_id_to_metadata[pid]->valueToString(v);
                if (partxt)
                {
                    g.setColour(juce::Colours::white);
                    g.drawText(*partxt, xcor, getHeight() - 20, barw, 20,
                               juce::Justification::centred);
                }
            }
            float pmin = parminval;
            float pmax = parmaxval;
            if (i == numparams)
            {
                pmin = -1.0;
                pmax = 1.0;
                g.setColour(juce::Colours::white);
                g.drawText("LFO1 AMT", xcor, 1, barw, 20, juce::Justification::centred);
            }
            float ycor = juce::jmap<float>(v, pmin, pmax, getHeight(), 0.0);
            g.setColour(juce::Colours::yellow);
            g.drawLine(xcor, ycor, xcor + barw - 1.0, ycor, 2.0);
        }
    }
    void mouseDoubleClick(const juce::MouseEvent &ev) override
    {
        int shifterindex = (float)num_all_params() / getWidth() * ev.x;
        if (shifterindex >= 0 && shifterindex < num_all_params())
        {
            if (shifterindex < numparams)
                post_param_event(shifterindex, pardefaultval);
            else
                post_param_event(shifterindex, 0.0);
            repaint();
        }
    }
    int num_all_params()
    {
        if (lfo0_amt_par != CLAP_INVALID_ID)
            return numparams + 1;
        return numparams;
    }
    void mouseDown(const juce::MouseEvent &ev) override
    {
        draggedindex = -1;
        int shifterindex = (float)(num_all_params()) / getWidth() * ev.x;
        if (shifterindex >= 0 && shifterindex < num_all_params())
        {
            draggedindex = shifterindex;
            post_param_event(draggedindex, parval_from_mouse_y(draggedindex, ev.y));
            repaint();
        }
    }
    float parval_from_mouse_y(int i, float y)
    {
        float pmin = parminval;
        float pmax = parmaxval;
        bool quan = true;
        if (i == numparams)
        {
            pmin = -1.0;
            pmax = 1.0;
            quan = false;
        }
        float v = juce::jmap<float>(y, 0.0, getHeight(), pmax, pmin);
        v = std::clamp(v, pmin, pmax);
        if (quan)
            v = pref.par_id_to_metadata[firstpar]->quantize(v);
        return v;
    }
    // our own index, not the clap id!
    void post_param_event(int parindex, float val)
    {
        ParamChangeMessage msg;
        msg.id = firstpar + parindex;
        if (parindex == numparams)
        {
            msg.id = lfo0_amt_par;
        }
        msg.val = val;
        pref.params_from_gui_fifo.push(msg);
        values[parindex] = val;
    }
    void mouseDrag(const juce::MouseEvent &ev) override
    {
        int shifterindex = (float)num_all_params() / getWidth() * ev.x;
        if (shifterindex >= 0 && shifterindex < num_all_params())
        {
            post_param_event(shifterindex, parval_from_mouse_y(shifterindex, ev.y));
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

    MultiSlider enableMulti;
    MultiSlider pitchMulti;
    MultiSlider formantMulti;
    MultiSlider delayTimeMulti;
    MultiSlider delayFeedbackMulti;
    MultiSlider inVolumeMulti;
    MultiSlider cutoffMulti;
    MultiSlider resonMulti;
    MultiSlider aziMulti;
    std::unordered_map<uint32_t, XapSlider *> paridtoslidermap;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessorEditor)
};
