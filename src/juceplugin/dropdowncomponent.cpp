#include "dropdowncomponent.h"
#include "juce_graphics/juce_graphics.h"

void GalleryPicker::resized() { update_layout(); }
bool GalleryPicker::keyPressed(const juce::KeyPress &ev)
{
    if (ev.getKeyCode() == juce::KeyPress::escapeKey)
    {
        setVisible(false);
        return true;
    }
    return false;
}
void GalleryPicker::mouseDown(const juce::MouseEvent &ev)
{
    int64_t id = -1;
    for (auto &cate : categories)
    {
        for (auto &it : cate.items)
        {
            if (it.rect.contains(ev.position))
            {
                id = it.id;
                break;
            }
        }
        if (id != -1)
            break;
    }
    if (id != -1 && OnSelected)
    {
        selectedID = id;
        OnSelected(id);
        repaint();
    }
}

GalleryPicker::GalleryPicker()
{
    setWantsKeyboardFocus(true);
    setOpaque(true);
    showButton.onClick = [this]() {
        // nasty but will have to do for now
        jassert(showButton.getParentComponent());
        parentToCover = showButton.getParentComponent()->getParentComponent();
        jassert(parentToCover);
        if (first_show)
        {
            first_show = false;
            parentToCover->addChildComponent(this);
        }
        setBounds(0, 0, parentToCover->getWidth(), parentToCover->getHeight());
        setVisible(!isVisible());
        toFront(true);
    };
}
void GalleryPicker::update_layout()
{
    float yoffs = 1.0f;
    for (size_t i = 0; i < categories.size(); ++i)
    {
        if (!categories[i].text.empty())
        {
            juce::Rectangle<float> r{1.0f, yoffs, cellw, itemh};
            categories[i].rect = r;
            // yoffs += itemh;
        }
        float xoffs = cellw;
        for (int j = 0; j < categories[i].items.size(); ++j)
        {
            if (xoffs + cellw >= getWidth())
            {
                xoffs = 1.0f;
                yoffs += itemh;
            }
            auto &it = categories[i].items[j];
            juce::Rectangle<float> r{xoffs, yoffs, cellw, itemh};
            r = r.reduced(2.0f);
            it.rect = r;
            xoffs += cellw;
        }
        yoffs += itemh + 1.0f;
    }
}
std::optional<std::string> GalleryPicker::get_text_from_id(int64_t id)
{
    for (auto &c : categories)
    {
        for (auto &it : c.items)
        {
            if (it.id == id)
            {
                return it.text;
            }
        }
    }
    return {};
}

void GalleryPicker::paint(juce::Graphics &g)
{
    ++paintcount;
    g.fillAll(juce::Colours::black);
    // g.setColour(juce::Colours::magenta);
    // g.drawRect(g.getClipBounds());
    for (size_t i = 0; i < categories.size(); ++i)
    {
        if (!categories[i].text.empty() && !categories[i].rect.isEmpty())
        {
            auto r = categories[i].rect;
            r.reduce(2.0f, 2.0f);
            g.setColour(juce::Colours::orange.darker());
            g.fillRoundedRectangle(r, 4.0f);
            g.setColour(juce::Colours::black);
            g.drawText(categories[i].text, r, juce::Justification::centred);
        }
        for (int j = 0; j < categories[i].items.size(); ++j)
        {
            const auto &it = categories[i].items[j];
            if (!it.rect.isEmpty())
            {
                auto r = it.rect;
                g.setColour(juce::Colours::grey);
                juce::Colour textcol;
                if (it.id == selectedID)
                {
                    g.fillRoundedRectangle(r, 4.0f);
                    textcol = juce::Colours::white;
                }
                else
                {
                    g.drawRoundedRectangle(r, 4.0f, 1.0f);
                    textcol = juce::Colours::white.darker();
                }
                if (!has_thumbs)
                {
                    g.setColour(textcol);
                    g.drawText(it.text, r, juce::Justification::centred);
                }
                else
                {
                    juce::Rectangle<float> thumbarea{r.getX(), r.getY(), r.getWidth(),
                                                     r.getHeight() - 0.0f};
                    if (DrawThumb)
                    {
                        g.saveState();
                        DrawThumb(it.id, g, thumbarea);
                        g.restoreState();
                    }
                    g.setColour(textcol);
                    g.drawText(it.text, r, juce::Justification::centredBottom);
                }
            }
        }
    }
    g.setColour(juce::Colours::white);
    g.drawText(juce::String(paintcount), getLocalBounds(), juce::Justification::topRight);
}
