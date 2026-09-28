#include "dropdowncomponent.h"

void GalleryPicker::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colours::black);
    float itemh = 20.0f;
    float yoffs = 1.0f;
    float cellw = 140.0f;
    for (size_t i = 0; i < categories.size(); ++i)
    {
        if (!categories[i].text.empty() && !categories[i].rect.isEmpty())
        {
            auto r = categories[i].rect;
            // g.setColour(juce::Colours::darkgrey);
            // g.fillRect(r);
            g.setColour(juce::Colours::white);
            g.drawText(categories[i].text, r, juce::Justification::centred);
            yoffs += itemh;
        }
        float xoffs = 1.0f;
        for (int j = 0; j < categories[i].items.size(); ++j)
        {
            if (xoffs + cellw >= getWidth())
            {
                xoffs = 1.0f;
                yoffs += itemh;
            }
            const auto &it = categories[i].items[j];
            if (!it.rect.isEmpty())
            {
                auto r = it.rect;
                r = r.reduced(2.0f);
                g.setColour(juce::Colours::darkgrey);
                g.fillRect(r);
                g.setColour(juce::Colours::yellow);
                g.drawText(it.text, r, juce::Justification::centred);
            }
            // juce::Rectangle<float> r{xoffs, yoffs, cellw, itemh};

            xoffs += cellw;
        }
        yoffs += itemh;
    }
}