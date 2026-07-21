#include "xap_slider.h"
#include "juce_graphics/juce_graphics.h"

void XapSlider::mouseWheelMove(const juce::MouseEvent &event, const juce::MouseWheelDetails &wheel)
{
    if (!isEnabled() || m_ed.isVisible())
        return;
    double delta = 0.0;
    if (wheel.isSmooth)
    {
        delta = wheel.deltaY * m_param_step * 10.0f;
    }
    else
    {
        if (wheel.deltaY < 0)
            delta = -m_param_step;
        else
            delta = m_param_step;
    }
    if (event.mods.isShiftDown())
    {
        if (event.mods.isCommandDown())
            delta *= 2.0;
        else
            delta *= 0.1;
    }
    if (wheel.isReversed)
        delta = -delta;
    setValue(m_value + delta, true);
}

bool XapSlider::keyPressed(const juce::KeyPress &key)
{
    if (!isEnabled())
        return false;
    auto c = key.getTextCharacter();
    std::optional<double> val;
    if (c >= '1' && c <= '9')
    {
        int slot = c - 49;
        val = m_snap_positions[slot];
    }
    if (c == '0')
        val = m_default_value;

    for (auto &e : keypress_to_step)
    {
        if (e.first == key)
        {
            val = m_value + e.second;
            break;
        }
    }
    if (val)
    {
        setValue(*val, true);
        return true;
    }
    return false;
}

void XapSlider::mouseDown(const juce::MouseEvent &ev)
{
    if (!isEnabled())
        return;
    if (m_pardesc.displayScale == ParamDesc::UNORDERED_MAP && m_pardesc.discreteValues.size() == 2)
    {
        if (m_value >= 0.5)
            setValue(0.0, true);
        else
            setValue(1.0, true);
        return;
    }
    if (m_pardesc.displayScale == ParamDesc::UNORDERED_MAP && m_style != SS_Knob)
    {
        juce::PopupMenu menu;
        // we could cache the ordered entries but that would likely introduce
        // cache invalidation problems at some point...
        std::vector<std::pair<int, std::string>> ordered_entries;
        for (auto &e : m_pardesc.discreteValues)
        {
            ordered_entries.push_back(e);
        }
        std::sort(ordered_entries.begin(), ordered_entries.end(),
                  [](auto &lhs, auto &rhs) { return lhs.first < rhs.first; });
        for (auto &e : ordered_entries)
        {
            menu.addItem(e.second, true, e.first == (int)m_value,
                         [e, this]() { setValue(e.first, true); });
        }
        menu.showMenuAsync(
            juce::PopupMenu::Options{}.withTargetComponent(this).withMousePosition());
        return;
    }
    if (ev.mods.isMiddleButtonDown())
    {
        showTextEditor();
        return;
    }
    if (ev.mods.isRightButtonDown())
    {
        juce::PopupMenu menu;
        menu.addItem("Edit value...", [this]() { showTextEditor(); });
        if (m_fstate)
        {
            menu.addItem("Extend range", true, m_fstate->isExtended, [this]() {
                m_fstate->isExtended = !m_fstate->isExtended;
                repaint();
            });
        }
        if (OnAddContextMenuItems)
        {
            OnAddContextMenuItems(menu);
        }
        /*
        juce::PopupMenu storemenu;
        for (int i = 0; i < m_snap_positions.size(); ++i)
        {
            storemenu.addItem(juce::String(i),
                              [i, this]() { m_snap_positions[i] = getValue(); });
        }
        menu.addSubMenu("Store to", storemenu);
        juce::PopupMenu loadmenu;
        for (int i = 0; i < m_snap_positions.size(); ++i)
        {
            loadmenu.addItem(juce::String(i),
                             [i, this]() { setValue(m_snap_positions[i], true); });
        }
        menu.addSubMenu("Load from", loadmenu);
        */
        menu.showMenuAsync({});
        return;
    }
    m_mousedown = true;
    was_started_in_fine_mode = ev.mods.isShiftDown();
    m_drag_start_pos = m_pardesc.naturalToNormalized01(m_value);
    mouseDragPos = ev.getPosition();
    repaint();
}

void XapSlider::mouseDrag(const juce::MouseEvent &ev)
{
    if (!isEnabled())
        return;
    if (m_style == SS_Knob)
    {
        ev.source.enableUnboundedMouseMovement(true);
        // setMouseCursor (juce::MouseCursor::NoCursor);
        float delta = (ev.y - mouseDragPos.y) * 0.005;
        if (was_started_in_fine_mode)
            delta *= 0.1;
        float newvalnormalized = juce::jlimit<float>(0.0f, 1.0f, m_drag_start_pos - delta);
        m_value = m_pardesc.normalized01ToNatural(newvalnormalized);
        if (OnValueChanged)
            OnValueChanged();
        repaint();
        return;
    }
    m_value = juce::jmap<double>(ev.x, 2, getWidth() - 4, m_min_value, m_max_value);
    m_value = juce::jlimit(m_min_value, m_max_value, m_value);

    if (OnValueChanged)
        OnValueChanged();
    repaint();
    return;
    double diff = juce::jmap<double>(ev.getDistanceFromDragStartX(), 2, getWidth() - 4, m_min_value,
                                     m_max_value);
    m_value = m_drag_start_pos + diff;
    m_value = juce::jlimit(m_min_value, m_max_value, m_value);
    repaint();
}

void XapSlider::mouseUp(const juce::MouseEvent &ev)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
    if (!isEnabled())
        return;
    m_mousedown = false;
    repaint();
}
void XapSlider::setValue(double v, bool notify)
{
    if (v == m_value)
        return;
    m_value = juce::jlimit(m_min_value, m_max_value, v);
    if (notify && OnValueChanged)
        OnValueChanged();
    repaint();
}

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
            g.setColour(juce::Colours::darkgrey);
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
            int midx = getWidth() * dropdownXpercent;
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
void XapSlider::showTextEditor()
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

void XapSlider::paintKnob(juce::Graphics &g)
{
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::white);
    float texth = 25.0;

    if (!m_mousedown)
    {
        std::string display_name = m_pardesc.name;
        // if (m_pardesc.shortName != m_pardesc.name)
        //    display_name = m_pardesc.shortName;
        g.setFont(m_font.withHeight(texth * 0.8f));
        g.drawFittedText(display_name, 1, 0, getWidth() - 2, texth, juce::Justification::centred,
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
