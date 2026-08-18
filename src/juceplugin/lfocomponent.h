#pragma once

#include "PluginProcessor.h"
#include "xap_slider.h"

struct LFOComponent : public juce::Component
{
    LFOComponent(int index, ToneGranulator *g);
    void resized();
    int lfoindex = -1;
    ToneGranulator *gr = nullptr;
    std::function<void(uint32_t, float)> stateChangedCallback;

    XapSlider rateSlider;
    XapSlider deformSlider;
    XapSlider shiftSlider;
    XapSlider warpSlider;
    XapSlider shapeSlider;
    XapSlider unipolarSlider;
    XapSlider masterSyncSlider;
};
