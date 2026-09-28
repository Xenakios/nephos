#include "dropdowncomponent.h"
#include "juce_graphics/juce_graphics.h"

void GalleryPicker::resized()
{
    float itemh = 20.0f;
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
            r = r.reduced(1.0f);
            it.rect = r;
            xoffs += cellw;
        }
        yoffs += itemh + 1.0f;
    }
}

void GalleryPicker::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colours::black);
    for (size_t i = 0; i < categories.size(); ++i)
    {
        if (!categories[i].text.empty() && !categories[i].rect.isEmpty())
        {
            auto r = categories[i].rect;
            // g.setColour(juce::Colours::darkgrey);
            // g.fillRect(r);
            g.setColour(juce::Colours::yellow);
            g.drawText(categories[i].text, r, juce::Justification::centredRight);
        }
        for (int j = 0; j < categories[i].items.size(); ++j)
        {
            const auto &it = categories[i].items[j];
            if (!it.rect.isEmpty())
            {
                auto r = it.rect;
                g.setColour(juce::Colours::grey);
                if (it.id == selectedID)
                {
                    g.fillRoundedRectangle(r, 4.0f);
                    g.setColour(juce::Colours::white);
                }
                else
                {
                    g.drawRoundedRectangle(r, 4.0f, 1.0f);
                    g.setColour(juce::Colours::white.darker());
                }
                g.drawText(it.text, r, juce::Justification::centred);
            }
        }
    }
}