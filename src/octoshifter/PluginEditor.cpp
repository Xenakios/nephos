#include "PluginProcessor.h"
#include "clap/id.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"
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
using MyProcessor = OctoShifterProcessor;
//==============================================================================
OctoShifterProcessorEditor::OctoShifterProcessorEditor(OctoShifterProcessor &p)
    : AudioProcessorEditor(&p), processorRef(p),

      enableMulti(p, "ENABLED", OctoShifterProcessor::PAR_ENABLED, 8),
      pitchMulti(p, "PITCH", MyProcessor::PAR_PITCH, 8, MyProcessor::PAR_PITCHLFOAMOUNTS),
      formantMulti(p, "FORMANT", OctoShifterProcessor::PAR_FORMANT, 8,
                   MyProcessor::PAR_FORMANT_LFOAMOUNTS),
      delayTimeMulti(p, "DELAY TIME", OctoShifterProcessor::PAR_DELAYTIME, 8, CLAP_INVALID_ID,
                     true),
      delayFeedbackMulti(p, "DELAY FEEDBACK", OctoShifterProcessor::PAR_DELAYFEEDBACK, 8,
                         OctoShifterProcessor::PAR_DELAYFB_LFOAMOUNTS),
      inVolumeMulti(p, "INPUT VOLUME", OctoShifterProcessor::PAR_INPUT_GAIN, 8,
                    OctoShifterProcessor::PAR_INVOLUME_LFOAMOUNTS),
      cutoffMulti(p, "FILTER CUTOFF", OctoShifterProcessor::PAR_FILTERCUTOFF, 8,
                  MyProcessor::PAR_CUTOFF0LFOAMOUNTS),
      resonMulti(p, "FILTER RESONANCE", OctoShifterProcessor::PAR_FILTERRESON, 8,
                 OctoShifterProcessor::PAR_RESON0_LFOAMOUNTS),
      aziMulti(p, "AZIMUTH/PAN", OctoShifterProcessor::PAR_AZIMUTH, 8,
               MyProcessor::PAR_AZIMUTHLFOAMOUNTS)
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
    addAndMakeVisible(infoLabel);

    for (int i = 0; i < OctoShifterProcessor::numLFOs; ++i)
    {
        auto c = std::make_unique<LFOComponent>(p, i);
        addAndMakeVisible(*c);
        lfo_comps.push_back(std::move(c));
    }

    visitAll<XapSlider>(*this, [this](XapSlider *slid) {
        paridtoslidermap[slid->getParameterMetaData().id] = slid;
    });
    startTimerHz(10);
    setSize(1100, 700);
}

OctoShifterProcessorEditor::~OctoShifterProcessorEditor() {}

void OctoShifterProcessorEditor::timerCallback()
{
    juce::String txt;
    txt += juce::String(processorRef.infifoavail) + " " + juce::String(processorRef.outfifoavail);
    txt += " " + juce::String(processorRef.outsilencecount);
    infoLabel.setText(txt, juce::dontSendNotification);
    ParamChangeMessage msg;
    while (processorRef.params_to_gui_fifo.pop(msg))
    {
        auto it = paridtoslidermap.find(msg.id);
        if (it != paridtoslidermap.end())
        {
            it->second->setValue(msg.val);
        }
        if (msg.id >= OctoShifterProcessor::PAR_PITCH &&
            msg.id < OctoShifterProcessor::PAR_PITCH + 8)
        {
            pitchMulti.update();
        }
    }
}

void OctoShifterProcessorEditor::paint(juce::Graphics &g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void OctoShifterProcessorEditor::resized()
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
    multiflex.items.add(juce::FlexItem(infoLabel).withFlex(1.0).withMargin(3).withMaxHeight(25));
    for (auto &e : multiflex.items)
    {
        e = e.withMinWidth(500);
    }
    flex.items.add(juce::FlexItem(multiflex).withFlex(8));
    for (auto &c : lfo_comps)
    {
        flex.items.add(juce::FlexItem(*c).withFlex(0.7).withMargin(2));
    }
    flex.performLayout(getLocalBounds());
}
