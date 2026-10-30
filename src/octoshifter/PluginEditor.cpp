#include "PluginProcessor.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "PluginEditor.h"

template <typename CompType>
inline void visitAll(juce::Component &c, std::function<void(CompType *)> fn)
{
    for (auto *child : c.getChildren())
    {
        if (auto *d = dynamic_cast<CompType *>(child))
        {
            fn(d);
        }
        visitAll(*child, fn);
    }
}
using MyProcessor = AudioPluginAudioProcessor;
//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor &p)
    : AudioProcessorEditor(&p), processorRef(p),
      lfoShapeKnob(XapSlider::SS_HorizontalSlider,
                   *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_LFOSHAPE]),
      lfoRateKnob(XapSlider::SS_Knob,
                  *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_LFORATE]),
      lfoDeformKnob(XapSlider::SS_Knob,
                    *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_LFODEFORM]),
      lfoShiftKnob(XapSlider::SS_Knob,
                   *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_LFOSHIFT]),
      lfoWarpKnob(XapSlider::SS_Knob,
                  *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_LFOWARP]),
      lfoAmountKnob(XapSlider::SS_Knob,
                    *p.par_id_to_metadata[AudioPluginAudioProcessor::PAR_PITCHLFO0AMOUNT]),
      enableMulti(p, "ENABLED", AudioPluginAudioProcessor::PAR_ENABLED, 8),
      pitchMulti(p, "PITCH", MyProcessor::PAR_PITCH, 8, MyProcessor::PAR_PITCHLFO0AMOUNT),
      formantMulti(p, "FORMANT", AudioPluginAudioProcessor::PAR_FORMANT, 8),
      delayTimeMulti(p, "DELAY TIME", AudioPluginAudioProcessor::PAR_DELAYTIME, 8),
      delayFeedbackMulti(p, "DELAY FEEDBACK", AudioPluginAudioProcessor::PAR_DELAYFEEDBACK, 8),
      inVolumeMulti(p, "INPUT VOLUME", AudioPluginAudioProcessor::PAR_INPUT_GAIN, 8),
      cutoffMulti(p, "FILTER CUTOFF", AudioPluginAudioProcessor::PAR_FILTERCUTOFF, 8,
                  MyProcessor::PAR_CUTOFF0LFO0AMOUNT),
      resonMulti(p, "FILTER RESONANCE", AudioPluginAudioProcessor::PAR_FILTERRESON, 8),
      aziMulti(p, "AZIMUTH/PAN", AudioPluginAudioProcessor::PAR_AZIMUTH, 8,
               MyProcessor::PAR_AZIMUTHLFO0AMOUNT)
{
    addAndMakeVisible(enableMulti);
    addAndMakeVisible(pitchMulti);
    addAndMakeVisible(formantMulti);
    addAndMakeVisible(delayTimeMulti);
    addAndMakeVisible(delayFeedbackMulti);
    addAndMakeVisible(inVolumeMulti);
    addAndMakeVisible(cutoffMulti);
    addAndMakeVisible(resonMulti);
    addAndMakeVisible(aziMulti);
    for (int i = 0; i < p.numshifters; ++i)
    {
        // auto c = std::make_unique<ShifterComponent>(p, i);
        // addAndMakeVisible(*c);
        // shiftercomps.push_back(std::move(c));
    }
    add_xapslider(p, *this, lfoShapeKnob);
    add_xapslider(p, *this, lfoRateKnob);
    add_xapslider(p, *this, lfoAmountKnob);
    add_xapslider(p, *this, lfoDeformKnob);
    add_xapslider(p, *this, lfoShiftKnob);
    add_xapslider(p, *this, lfoWarpKnob);
    visitAll<XapSlider>(*this, [this](XapSlider *slid) {
        paridtoslidermap[slid->getParameterMetaData().id] = slid;
    });
    startTimerHz(10);
    setSize(1100, 700);
}

AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor() {}

void AudioPluginAudioProcessorEditor::timerCallback()
{
    ParamChangeMessage msg;
    while (processorRef.params_to_gui_fifo.pop(msg))
    {
        auto it = paridtoslidermap.find(msg.id);
        if (it != paridtoslidermap.end())
        {
            it->second->setValue(msg.val);
        }
        if (msg.id >= AudioPluginAudioProcessor::PAR_PITCH &&
            msg.id < AudioPluginAudioProcessor::PAR_PITCH + 8)
        {
            pitchMulti.update();
        }
    }
}

void AudioPluginAudioProcessorEditor::paint(juce::Graphics &g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioPluginAudioProcessorEditor::resized()
{
    juce::FlexBox flex;
    flex.flexDirection = juce::FlexBox::Direction::column;
    juce::FlexBox multiflex;
    multiflex.flexDirection = juce::FlexBox::Direction::row;
    multiflex.flexWrap = juce::FlexBox::Wrap::wrap;
    multiflex.items.add(juce::FlexItem(enableMulti).withFlex(1.0).withMargin(3));
    multiflex.items.add(juce::FlexItem(inVolumeMulti).withFlex(1.0).withMargin(3));
    multiflex.items.add(juce::FlexItem(pitchMulti).withFlex(1.0).withMargin(3));
    multiflex.items.add(juce::FlexItem(formantMulti).withFlex(1.0).withMargin(3));
    multiflex.items.add(juce::FlexItem(delayTimeMulti).withFlex(1.0).withMargin(3));
    multiflex.items.add(juce::FlexItem(delayFeedbackMulti).withFlex(1.0).withMargin(3));
    multiflex.items.add(juce::FlexItem(cutoffMulti).withFlex(1.0).withMargin(3));
    multiflex.items.add(juce::FlexItem(resonMulti).withFlex(1.0).withMargin(3));
    multiflex.items.add(juce::FlexItem(aziMulti).withFlex(1.0).withMargin(3));
    multiflex.items.add(juce::FlexItem().withFlex(1.0).withMargin(3));
    for (auto &e : multiflex.items)
    {
        e = e.withMinWidth(500);
    }
    flex.items.add(juce::FlexItem(multiflex).withFlex(8));
    for (int i = 0; i < shiftercomps.size(); ++i)
    {
        flex.items.add(juce::FlexItem(*shiftercomps[i]).withFlex(1.0));
    }
    juce::FlexBox lfoflex;
    lfoflex.flexDirection = juce::FlexBox::Direction::row;
    lfoflex.items.add(juce::FlexItem(lfoShapeKnob).withFlex(3).withMaxHeight(25));
    lfoflex.items.add(juce::FlexItem(lfoRateKnob).withFlex(1));
    lfoflex.items.add(juce::FlexItem(lfoDeformKnob).withFlex(1));
    lfoflex.items.add(juce::FlexItem(lfoShiftKnob).withFlex(1));
    lfoflex.items.add(juce::FlexItem(lfoWarpKnob).withFlex(1));
    lfoflex.items.add(juce::FlexItem(lfoAmountKnob).withFlex(1));
    for (auto &e : lfoflex.items)
    {
        e = e.withMargin(2.0);
    }
    flex.items.add(juce::FlexItem(lfoflex).withFlex(1.0));
    flex.performLayout(getLocalBounds());
}
