#pragma once

#include "PluginProcessor.h"
#include "IEM/HammerAitovGrid.h"
class DashBoardComponent : public juce::Component
{
  public:
    AudioPluginAudioProcessor &processorRef;
    ToneGranulator *gr = nullptr;
    std::vector<ToneGranulator::GrainVisualizerMessage> persisted_events;
    HammerAitovGrid haGrid;
    struct ParamEvent
    {
        double timestamp = 0.0;
        double value = 0.0;
        double cpu_usage = 0.0;
    };
    std::vector<ParamEvent> paramValuesHistory;
    juce::Path paramHistoryPath;
    double timespantoshow = 8.0;
    int throttlecounter = 0;
    float visualfadecoefficient = 1.0;
    bool showModulatorValues = false;

    juce::ColourGradient pitchGradient;
    std::unique_ptr<juce::VBlankAttachment> vblankAttachment;
    std::function<double()> GetCPULoad;
    std::function<void(void)> OnMacroKnobsLoadRequested;
    DashBoardComponent(AudioPluginAudioProcessor &p);

    void paint(juce::Graphics &g) override;
    void paintAmbisonicFieldPolar(juce::Graphics &g);
    void paintAmbisonicFieldHammerProjection(juce::Graphics &g);
    void mouseDown(const juce::MouseEvent &ev) override;
    void drawCPUGraph(juce::Graphics &g, double enginetime, juce::Rectangle<float> area);
    void updateGrainData();

    void resized() override;
};
