#include "PluginProcessor.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "PluginEditor.h"

//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor &p)
    : AudioProcessorEditor(&p), processorRef(p),
      lfoShapeKnob(XapSlider::SS_HorizontalSlider,
                   *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_LFOSHAPE]),
      lfoRateKnob(XapSlider::SS_Knob,
                  *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_LFORATE]),
      lfoAmountKnob(XapSlider::SS_Knob,
                    *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_LFOAMOUNT])
{
    juce::ignoreUnused(processorRef);
    for (int i = 0; i < p.numshifters; ++i)
    {
        auto c = std::make_unique<ShifterComponent>(p, i);
        addAndMakeVisible(*c);
        shiftercomps.push_back(std::move(c));
    }
    add_xapslider(p, *this, lfoShapeKnob);
    add_xapslider(p, *this, lfoRateKnob);
    add_xapslider(p, *this, lfoAmountKnob);
    setSize(600, 500);
}

AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor() {}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint(juce::Graphics &g)
{
    // (Our component is opaque, so we must completely fill the background with a solid colour)
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    return;
    g.setColour(juce::Colours::white);
    g.setFont(15.0f);
    g.drawFittedText("Hello World!", getLocalBounds(), juce::Justification::centred, 1);
}

void AudioPluginAudioProcessorEditor::resized()
{
    juce::FlexBox flex;
    flex.flexDirection = juce::FlexBox::Direction::column;
    for (int i = 0; i < shiftercomps.size(); ++i)
    {
        flex.items.add(juce::FlexItem(*shiftercomps[i]).withFlex(1.0));
    }
    juce::FlexBox lfoflex;
    lfoflex.flexDirection = juce::FlexBox::Direction::row;
    lfoflex.items.add(juce::FlexItem(lfoShapeKnob).withFlex(3));
    lfoflex.items.add(juce::FlexItem(lfoRateKnob).withFlex(1));
    lfoflex.items.add(juce::FlexItem(lfoAmountKnob).withFlex(1));
    flex.items.add(juce::FlexItem(lfoflex).withFlex(0.75));
    flex.performLayout(getLocalBounds());
}
