#include "xap_slider.h"

void XapSlider::paint(juce::Graphics &g)
{
    if (m_style == SS_Knob)
    {
        paintKnob(g);
        return;
    }
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::darkgrey);
    g.setFont(m_font.withHeight(getHeight() * 0.5f));
    if (!isEnabled())
    {
        g.setColour(juce::Colours::lightgrey);
        g.drawRect(2, 0, getWidth() - 4, getHeight());
        g.setColour(juce::Colours::grey);
        g.drawText("NOT AVAILABLE", 0, 0, getWidth(), getHeight(), juce::Justification::centred);
        return;
    }
    if (!m_err_msg.isEmpty())
    {

        g.setColour(juce::Colours::red);
        g.drawText(m_err_msg, 0, 0, getWidth(), getHeight(), juce::Justification::centred);
        return;
    }
    if (m_pardesc.displayScale == ParamDesc::UNORDERED_MAP && m_pardesc.discreteValues.size() == 2)
    {
        if (m_value >= 0.5)
            g.setColour(juce::Colours::green);
        else
            g.setColour(juce::Colours::red);
        g.fillRect(3, 1, getWidth() - 5, getHeight() - 1);
        g.setColour(juce::Colours::lightgrey);
        g.drawRect(2, 0, getWidth() - 4, getHeight());
        g.setColour(juce::Colours::white);
        g.drawText(m_labeltxt, 5, 0, getWidth() - 10, getHeight(),
                   juce::Justification::centredLeft);
        return;
    }
    double xforvalue = juce::jmap<double>(m_value, m_min_value, m_max_value, 2.0, getWidth() - 4.0);
    if (m_pardesc.displayScale != ParamDesc::UNORDERED_MAP)
    {
        if (hasKeyboardFocus(false))
            g.setColour(juce::Colours::cyan);
        else
            g.setColour(juce::Colours::lightgrey);
        double h = getHeight() - 4;
        g.fillEllipse(xforvalue - h / 2, 2, h, h);
        g.setColour(juce::Colours::lightgrey);
        g.drawRect(2, 0, getWidth() - 4, getHeight());
    }

    g.setColour(juce::Colours::white);

    g.drawText(m_labeltxt, 5, 0, getWidth() - 10, getHeight(), juce::Justification::centredLeft);
    auto partext = getFormattedParamText();
    // if (partext)
    {
        if (m_pardesc.displayScale != ParamDesc::UNORDERED_MAP)
        {
            g.drawText(partext, 5, 0, getWidth() - 10, getHeight(),
                       juce::Justification::centredRight);
        }
        else
        {
            g.setColour(juce::Colours::grey);
            int midx = getWidth() / 2;
            g.fillRect(midx, 1, getWidth() - midx, getHeight() - 2);
            if (hasKeyboardFocus(false))
                g.setColour(juce::Colours::cyan);
            else
                g.setColour(juce::Colours::white);
            g.drawText(partext, midx + 2, 0, getWidth() - midx - 2, getHeight(),
                       juce::Justification::centredLeft);
        }
    }
}

void XapSlider::paintKnob(juce::Graphics &g)
{
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::white);
    float texth = 25.0;
    
    if (!m_mousedown)
    {
        g.setFont(m_font.withHeight(texth * 0.8f));
        g.drawFittedText(m_pardesc.name, 1, 0, getWidth() - 2, texth, juce::Justification::centred,
                         2);
        // g.drawText(m_pardesc.name, 0, 0, getWidth(), 20, juce::Justification::centred);
    }
    else
    {
        g.drawText(getFormattedParamText(), 1, 0, getWidth() - 2, texth,
                   juce::Justification::centred);
    }

    if (hasKeyboardFocus(false))
        g.setColour(juce::Colours::cyan);
    else
        g.setColour(juce::Colours::lightgrey);
    float circleCentX = getWidth() / 2.0;
    float circleCentY = (getHeight() - texth) / 2.0 + texth;
    float circleH = getHeight() - texth - 5.0;
    g.fillEllipse(circleCentX - (circleH / 2.0), texth, circleH, circleH);
    float anglerange = 140.0;
    float angle =
        juce::jmap<float>(m_value, m_min_value, m_max_value, -anglerange, anglerange) - 90.0;
    float rads = juce::degreesToRadians(angle);
    float x = circleCentX + (circleH / 2.0) * std::cos(rads);
    float y = circleCentY + (circleH / 2.0) * std::sin(rads);
    g.setColour(juce::Colours::green);
    g.fillEllipse(x - 4.0, y - 4.0, 8.0, 8.0);
}
