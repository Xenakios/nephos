#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "juce_graphics/juce_graphics.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

using ParamDesc = sst::basic_blocks::params::ParamMetaData;

// this is really no longer just a slider, but also a knob and
// a drop down selector component, but we are keeping the name as XapSlider
// for now...maybe rename it later to XapGenericParameterComponent or something

class XapSlider : public juce::Component
{
  public:
    enum RemoteControlStatus
    {
        RCS_NONE,
        RCS_MIDI,
        RCS_MODULATED
    };
    enum Style
    {
        SS_HorizontalSlider,
        SS_VerticalSlider,
        SS_Knob
    };

  private:
    double m_value = 0.0;
    double m_modulation_amt = 0.0;
    double m_min_value = 0.0;
    double m_max_value = 1.0;
    double m_default_value = 0.0;
    juce::String m_labeltxt;
    bool m_mousedown = false;
    bool was_started_in_fine_mode = false;
    double m_drag_start_pos = 0.0;
    juce::Point<int> mouseDragPos;
    ParamDesc m_pardesc;
    bool m_is_bipolar = false;
    std::vector<double> m_snap_positions;
    std::vector<std::pair<juce::KeyPress, double>> keypress_to_step;
    double m_param_step = 0.0;
    ParamDesc::FeatureState *m_fstate = nullptr;
    RemoteControlStatus remoteStatus{RCS_NONE};
    Style m_style;

  public:
    juce::Font m_font{juce::FontOptions{}};
    XapSlider(Style sty, ParamDesc pdesc, ParamDesc::FeatureState *fstate = nullptr)
        : m_pardesc(pdesc), m_fstate(fstate), m_style(sty)
    {
        keypress_to_step.reserve(8);
        setParameterMetaData(pdesc, true);
        setWantsKeyboardFocus(true);
        addChildComponent(m_ed);
    }
    void setRemoteControlMode(RemoteControlStatus s);
    RemoteControlStatus getRemoteControlMode() const { return remoteStatus; }
    const ParamDesc &getParameterMetaData() const { return m_pardesc; }
    void setParameterMetaData(ParamDesc md, bool updateCurrentValue);
    void setModulationDisplayDepth(float d, std::string units);
    void enablementChanged() override { repaint(); }
    void mouseWheelMove(const juce::MouseEvent &event,
                        const juce::MouseWheelDetails &wheel) override;
    bool keyPressed(const juce::KeyPress &key) override;

    juce::TextEditor m_ed;
    void focusGained(juce::Component::FocusChangeType cause) override { repaint(); }
    void focusLost(juce::Component::FocusChangeType cause) override { repaint(); }
    void paintKnob(juce::Graphics &g);

    std::string getFormattedParamText();
    void paint(juce::Graphics &g) override;
    void mouseDoubleClick(const juce::MouseEvent &event) override;
    std::optional<std::string> valueToString(float v);
    void showTextEditor();
    juce::String m_err_msg;
    float dropdownXpercent = 0.5f;

    void mouseDown(const juce::MouseEvent &ev) override;
    void mouseDrag(const juce::MouseEvent &ev) override;
    void mouseUp(const juce::MouseEvent &ev) override;

    void setValue(double v, bool notify = false);
    double getValue() { return m_value; }
    void setModulationAmount(double amt)
    {
        m_modulation_amt = amt;
        repaint();
    }
    std::function<void()> OnValueChanged;
    void addMenuItemsCallback(std::function<void(juce::PopupMenu &)> f)
    {
        MenuAddCallbacks.push_back(f);
    }
    std::vector<std::function<void(juce::PopupMenu &)>> MenuAddCallbacks;
};
