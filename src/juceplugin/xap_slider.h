#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "sst/basic-blocks/params/ParamMetadata.h"

using ParamDesc = sst::basic_blocks::params::ParamMetaData;

// this is really no longer just a slider, but also a knob and
// a drop down selector component, but we are keeping the name as XapSlider
// for now...maybe rename it later to XapGenericParameterComponent or something

class XapSlider : public juce::Component
{
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

  public:
    enum Style
    {
        SS_HorizontalSlider,
        SS_VerticalSlider,
        SS_Knob
    };
    Style m_style;
    XapSlider(Style sty, ParamDesc pdesc, ParamDesc::FeatureState *fstate = nullptr)
        : m_pardesc(pdesc), m_fstate(fstate), m_style(sty)
    {
        keypress_to_step.reserve(8);
        setParameterMetaData(pdesc, true);
        setWantsKeyboardFocus(true);
        addChildComponent(m_ed);
    }
    const ParamDesc &getParameterMetaData() const { return m_pardesc; }
    void setParameterMetaData(ParamDesc md, bool updateCurrentValue)
    {
        m_pardesc = md;
        if (updateCurrentValue)
            m_value = m_pardesc.defaultVal;
        m_default_value = m_pardesc.defaultVal;
        m_min_value = m_pardesc.minVal;
        m_max_value = m_pardesc.maxVal;
        if (m_min_value < 0.0)
            m_is_bipolar = true;
        m_labeltxt = m_pardesc.name;
        m_modulation_amt = 0.0;
        m_snap_positions.resize(9);
        for (int i = 0; i < 9; ++i)
            m_snap_positions[i] = m_min_value + (m_max_value - m_min_value) / 8 * i;
        m_param_step = (m_max_value - m_min_value) / 64;
        if (m_pardesc.type == ParamDesc::BOOL || m_pardesc.type == ParamDesc::INT)
            m_param_step = 1;
        keypress_to_step.clear();
        keypress_to_step.emplace_back(
            juce::KeyPress(juce::KeyPress::leftKey, juce::ModifierKeys::noModifiers, 0),
            -m_param_step);
        keypress_to_step.emplace_back(
            juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::noModifiers, 0),
            m_param_step);
        keypress_to_step.emplace_back(
            juce::KeyPress(juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier, 0),
            -m_param_step * 0.1);
        keypress_to_step.emplace_back(
            juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0),
            m_param_step * 0.1);
        repaint();
    }
    void setModulationDisplayDepth(float d, std::string units)
    {
        m_pardesc = m_pardesc.withLinearScaleFormatting(units, d);
        repaint();
    }
    void enablementChanged() override { repaint(); }
    void mouseWheelMove(const juce::MouseEvent &event,
                        const juce::MouseWheelDetails &wheel) override;
    bool keyPressed(const juce::KeyPress &key) override;

    juce::Font m_font;
    juce::TextEditor m_ed;
    void focusGained(juce::Component::FocusChangeType cause) override { repaint(); }
    void focusLost(juce::Component::FocusChangeType cause) override { repaint(); }
    void paintKnob(juce::Graphics &g);

    std::string getFormattedParamText()
    {
        auto val = m_value;
        if (m_pardesc.type == ParamDesc::INT)
            val = std::floor(m_value);
        auto partext = valueToString(val);
        if (partext)
        {
            // if (m_pardesc.displayScale != ParamDesc::UNORDERED_MAP)
            {
                return *partext;
            }
        }
        return "no FMT, bug";
    }
    void paint(juce::Graphics &g) override;
    void mouseDoubleClick(const juce::MouseEvent &event) override
    {
        if (!isEnabled())
            return;
        setValue(m_default_value, true);
    }
    std::optional<std::string> valueToString(float v)
    {
        ParamDesc::FeatureState fs;
        if (m_fstate)
            fs = *m_fstate;
        return m_pardesc.valueToString(v, fs);
    }
    void showTextEditor()
    {
        m_ed.setVisible(true);
        m_ed.grabKeyboardFocus();
        m_ed.setBounds(getWidth() / 2, 0, 80, getHeight());
        auto txt = valueToString(m_value);
        if (txt)
            m_ed.setText(*txt);
        else
            m_ed.setText(juce::String(m_value));
        m_ed.selectAll();
        m_ed.onEscapeKey = [this]() { m_ed.setVisible(false); };
        m_ed.onReturnKey = [this]() {
            std::string err;
            ParamDesc::FeatureState fs;
            if (m_fstate)
                fs = *m_fstate;
            auto v = m_pardesc.valueFromString(m_ed.getText().toStdString(), err, fs);
            if (v)
            {
                setValue(*v, true);
            }
            else
            {
                m_err_msg = err;
                repaint();
                juce::Timer::callAfterDelay(3000, [this]() {
                    m_err_msg = "";
                    repaint();
                });
            }
            m_ed.setVisible(false);
        };
    }
    juce::String m_err_msg;
    float dropdownXpercent = 0.5f;

    void mouseDown(const juce::MouseEvent &ev) override;
    void mouseDrag(const juce::MouseEvent &ev) override;
    void mouseUp(const juce::MouseEvent &ev) override;

    void setValue(double v, bool notify = false)
    {
        if (v == m_value)
            return;
        m_value = juce::jlimit(m_min_value, m_max_value, v);
        if (notify && OnValueChanged)
            OnValueChanged();
        repaint();
    }
    double getValue() { return m_value; }
    void setModulationAmount(double amt)
    {
        m_modulation_amt = amt;
        repaint();
    }
    std::function<void()> OnValueChanged;
};
