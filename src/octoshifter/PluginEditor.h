#pragma once

#include "PluginProcessor.h"
#include "../juceplugin/xap_slider.h"
#include "clap/id.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"
#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include <stdio.h>
#include <unordered_map>
#include "../juceplugin/dropdowncomponent.h"

inline void add_xapslider(OctoShifterProcessor &proc, juce::Component &parentcomp, XapSlider &slid)
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
    OctoShifterProcessor &pref;
    struct ParamInfo
    {
        uint32_t id = CLAP_INVALID_ID;
        int lfoindex = -1;
        float minval = 0.0f;
        float maxval = 1.0f;
        float defaultval = 0.0f;
        float value = 0.0f;
        bool notify_only_on_mouse_up = false;
        float modulatedvalue = 0.0f;
    };
    std::vector<ParamInfo> paraminfos;
    int shifter_modulation_target = -1;
    int draggedindex = -1;
    int paint_throttle_counter = 0;
    juce::String groupname;
    juce::TextEditor value_entry_editor;

    MultiSlider(OctoShifterProcessor &p, juce::String name, uint32_t parstartid, uint32_t numpars,
                uint32_t lfo0par = CLAP_INVALID_ID, int shifter_par_target = -1,
                bool notify_only_on_mouse_up = false)
        : pref(p), groupname(name)
    {
        addChildComponent(value_entry_editor);
        value_entry_editor.onEscapeKey = [this]() { value_entry_editor.setVisible(false); };
        value_entry_editor.onFocusLost = [this]() { value_entry_editor.setVisible(false); };

        shifter_modulation_target = shifter_par_target;
        paraminfos.resize(8);
        int numvalidated = 0;
        for (int i = 0; i < numpars; ++i)
        {
            auto it = pref.par_id_to_metadata.find(parstartid + i);
            if (it != pref.par_id_to_metadata.end())
            {
                ++numvalidated;
                paraminfos[i].id = it->first;
                paraminfos[i].minval = it->second->minVal;
                paraminfos[i].maxval = it->second->maxVal;
                paraminfos[i].defaultval = it->second->defaultVal;
                paraminfos[i].value = *pref.par_id_to_value_pointer[it->first];
                paraminfos[i].notify_only_on_mouse_up = notify_only_on_mouse_up;
            }
        }
        assert(numvalidated == numpars);
        if (lfo0par != CLAP_INVALID_ID)
        {
            ParamInfo pi{lfo0par, 0, -1.0f, 1.0f, 0.0f, *pref.par_id_to_value_pointer[lfo0par]};
            paraminfos.push_back(pi);
            ParamInfo pi2{lfo0par + 1, 1,    -1.0f,
                          1.0f,        0.0f, *pref.par_id_to_value_pointer[lfo0par + 1]};
            paraminfos.push_back(pi2);
        }
    }
    juce::Rectangle<int> sliders_area{0, 0, 10, 10};
    void resized() override
    {
        sliders_area = juce::Rectangle<int>{0, 15, getWidth(), getHeight() - 30};
    }
    void paint(juce::Graphics &g) override;
    void mouseDoubleClick(const juce::MouseEvent &ev) override
    {
        int paramindex = (float)paraminfos.size() / getWidth() * ev.x;
        if (paramindex >= 0 && paramindex < paraminfos.size())
        {
            post_param_event(paramindex, paraminfos[paramindex].defaultval);
            repaint();
        }
    }
    void show_value_editor(int paramindex, int xcor)
    {
        value_entry_editor.setVisible(true);
        value_entry_editor.setBounds(xcor, getHeight() - 20, 75, 20);
        value_entry_editor.grabKeyboardFocus();
        float v = paraminfos[paramindex].value;
        auto str = pref.par_id_to_metadata[paraminfos[paramindex].id]->valueToString(v);
        if (str)
        {
            value_entry_editor.setText(*str);
            value_entry_editor.selectAll();
            value_entry_editor.onReturnKey = [this, paramindex]() {
                std::string txt = value_entry_editor.getText().toStdString();
                std::string err;
                auto v =
                    pref.par_id_to_metadata[paraminfos[paramindex].id]->valueFromString(txt, err);
                if (v)
                {
                    post_param_event(paramindex, *v);
                    repaint();
                    value_entry_editor.setVisible(false);
                }
                else
                {
                    Log::info("error : could not convert edit text to parameter value : {}", err);
                    value_entry_editor.setVisible(false);
                }
            };
        }
    }
    void mouseDown(const juce::MouseEvent &ev) override
    {
        if (ev.mods.isRightButtonDown())
        {
            juce::PopupMenu menu;
            menu.addSectionHeader("Per shifter modulation pattern");
            for (int i = 0; i < OctoShifterProcessor::num_modulation_patterns; ++i)
            {
                std::string txt;
                for (int j = 0; j < 8; ++j)
                {
                    txt += fmt::format("{} ", pref.modulation_patterns[i][j]);
                }
                int curpat = pref.modtarget_to_modpattern[shifter_modulation_target];
                menu.addItem(txt, true, i == curpat, [this, i]() {
                    pref.modtarget_to_modpattern[shifter_modulation_target] = i;
                });
            }
            menu.showMenuAsync({});
            return;
        }
        int paramindex = (float)(paraminfos.size()) / getWidth() * ev.x;

        draggedindex = -1;

        if (paramindex >= 0 && paramindex < paraminfos.size())
        {
            if (ev.y > sliders_area.getBottom() - 5)
            {
                show_value_editor(paramindex, ev.x);
                return;
            }
            draggedindex = paramindex;
            if (!paraminfos[paramindex].notify_only_on_mouse_up)
                post_param_event(draggedindex, parval_from_mouse_y(draggedindex, ev.y));
            repaint();
        }
    }
    float parval_from_mouse_y(int i, float y)
    {
        float pmin = paraminfos[i].minval;
        float pmax = paraminfos[i].maxval;
        bool quan = true;
        if (paraminfos[i].lfoindex >= 0)
        {
            quan = false;
        }
        float v = juce::jmap<float>(y, sliders_area.getY(), sliders_area.getBottom(), pmax, pmin);
        v = std::clamp(v, pmin, pmax);
        if (quan)
            v = pref.par_id_to_metadata[paraminfos[i].id]->quantize(v);
        return v;
    }
    // our own index, not the clap id!
    void post_param_event(int parindex, float val)
    {
        ParamChangeMessage msg;
        msg.id = paraminfos[parindex].id;
        msg.val = val;
        pref.params_from_gui_fifo.push(msg);
        paraminfos[parindex].value = val;
    }
    void mouseUp(const juce::MouseEvent &ev) override
    {
        int paramindex = (float)paraminfos.size() / getWidth() * ev.x;
        if (paramindex >= 0 && paramindex < paraminfos.size())
        {
            if (paraminfos[paramindex].notify_only_on_mouse_up)
                post_param_event(paramindex, parval_from_mouse_y(paramindex, ev.y));
            repaint();
        }
    }
    void mouseDrag(const juce::MouseEvent &ev) override
    {
        if (value_entry_editor.isVisible())
            return;
        int paramindex = (float)paraminfos.size() / getWidth() * ev.x;
        if (paramindex >= 0 && paramindex < paraminfos.size())
        {
            float pval = parval_from_mouse_y(paramindex, ev.y);
            if (!paraminfos[paramindex].notify_only_on_mouse_up)
            {
                post_param_event(paramindex, pval);
            }
            else
            {
                paraminfos[paramindex].value = pval;
            }
            repaint();
        }
    }
    void update() { repaint(); }
};

class LFOComponent : public juce::Component
{
  public:
    OctoShifterProcessor &procref;
    XapSlider shapeDrop;
    XapSlider rateKnob;
    XapSlider deformKnob;
    // XapSlider deformKnob;
    XapSlider warpKnob;
    int index = -1;
    LFOComponent(OctoShifterProcessor &p, int lfoindex)
        : procref(p),
          shapeDrop(XapSlider::SS_HorizontalSlider,
                    *p.par_id_to_metadata[OctoShifterProcessor::PAR_LFOSHAPES + lfoindex]),
          rateKnob(XapSlider::SS_Knob,
                   *p.par_id_to_metadata[OctoShifterProcessor::PAR_LFORATES + lfoindex]),
          deformKnob(XapSlider::SS_Knob,
                     *p.par_id_to_metadata[OctoShifterProcessor::PAR_LFODEFORMS + lfoindex]),
          warpKnob(XapSlider::SS_Knob,
                   *p.par_id_to_metadata[OctoShifterProcessor::PAR_LFOWARPS + lfoindex])
    {
        add_xapslider(p, *this, shapeDrop);
        add_xapslider(p, *this, rateKnob);
        add_xapslider(p, *this, deformKnob);
        add_xapslider(p, *this, warpKnob);
    }
    void resized() override
    {
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        flex.items.add(juce::FlexItem(shapeDrop).withFlex(1.0).withMaxHeight(25));
        flex.items.add(juce::FlexItem(rateKnob).withFlex(1.0));
        flex.items.add(juce::FlexItem(deformKnob).withFlex(1.0));
        flex.items.add(juce::FlexItem(warpKnob).withFlex(1.0));
        for (auto &e : flex.items)
        {
            e = e.withMargin(2);
        }
        flex.performLayout(getLocalBounds());
    }
};

class ShifterComponent : public juce::Component
{
  public:
    OctoShifterProcessor &procRef;
    ShifterComponent(OctoShifterProcessor &p, int shifter_index)
        : procRef(p),
          enabledButton(XapSlider::SS_HorizontalSlider,
                        *p.par_id_to_metadata[OctoShifterProcessor::PAR_ENABLED + shifter_index]),
          inputGainKnob(
              XapSlider::SS_Knob,
              *p.par_id_to_metadata[OctoShifterProcessor::PAR_INPUT_GAIN + shifter_index]),
          delayKnob(XapSlider::SS_Knob,
                    *p.par_id_to_metadata[OctoShifterProcessor::PAR_DELAYTIME + shifter_index]),
          pitchKnob(XapSlider::SS_Knob,
                    *p.par_id_to_metadata[OctoShifterProcessor::PAR_PITCH + shifter_index]),
          formantKnob(XapSlider::SS_Knob,
                      *p.par_id_to_metadata[OctoShifterProcessor::PAR_FORMANT + shifter_index]),
          cutoffKnob(XapSlider::SS_Knob,
                     *p.par_id_to_metadata[OctoShifterProcessor::PAR_FILTERCUTOFF + shifter_index])
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

class OctoShifterProcessorEditor final : public juce::AudioProcessorEditor, public juce::Timer
{
  public:
    explicit OctoShifterProcessorEditor(OctoShifterProcessor &);
    ~OctoShifterProcessorEditor() override;

    //==============================================================================
    void paint(juce::Graphics &) override;
    void resized() override;
    void timerCallback() override;

  private:
    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    OctoShifterProcessor &processorRef;
    std::vector<std::unique_ptr<ShifterComponent>> shiftercomps;
    std::vector<std::unique_ptr<LFOComponent>> lfo_comps;

    std::vector<std::unique_ptr<MultiSlider>> multiSliders;
    std::unordered_map<uint32_t, XapSlider *> paridtoslidermap;
    juce::Label infoLabel;
    XapSlider pitchQualityDrop;
    XapSlider outputModeDrop;
    GalleryPicker filterPicker;
    std::unordered_map<int64_t, FilterModeMessage> id_to_filter_mode;
    std::unique_ptr<juce::VBlankAttachment> vblankAttachment;
    int paint_throttle_counter = 0;
    void update_components_with_modulation();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OctoShifterProcessorEditor)
};
