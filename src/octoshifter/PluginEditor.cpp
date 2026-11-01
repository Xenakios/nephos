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

      pitchQualityDrop(XapSlider::SS_HorizontalSlider,
                       *p.par_id_to_metadata[OctoShifterProcessor::PAR_PITCHQUALITY])
{
    p.gui_visible = true;
    multiSliders.push_back(
        std::make_unique<MultiSlider>(p, "ENABLED", OctoShifterProcessor::PAR_ENABLED, 8));
    multiSliders.push_back(
        std::make_unique<MultiSlider>(p, "INPUT VOLUME", OctoShifterProcessor::PAR_INPUT_GAIN, 8,
                                      OctoShifterProcessor::PAR_INVOLUME_LFOAMOUNTS));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "PITCH", OctoShifterProcessor::PAR_PITCH, 8, OctoShifterProcessor::PAR_PITCHLFOAMOUNTS,
        PitchShifter::MT_PITCH));
    multiSliders.push_back(
        std::make_unique<MultiSlider>(p, "FORMANT", OctoShifterProcessor::PAR_FORMANT, 8,
                                      OctoShifterProcessor::PAR_FORMANT_LFOAMOUNTS));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "DELAY TIME", OctoShifterProcessor::PAR_DELAYTIME, 8, CLAP_INVALID_ID, -1, true));
    multiSliders.push_back(
        std::make_unique<MultiSlider>(p, "DELAY FEEDBACK", OctoShifterProcessor::PAR_DELAYFEEDBACK,
                                      8, OctoShifterProcessor::PAR_DELAYFB_LFOAMOUNTS));
    multiSliders.push_back(
        std::make_unique<MultiSlider>(p, "FILTER CUTOFF", OctoShifterProcessor::PAR_FILTERCUTOFF, 8,
                                      OctoShifterProcessor::PAR_CUTOFF0LFOAMOUNTS));
    multiSliders.push_back(
        std::make_unique<MultiSlider>(p, "FILTER RESONANCE", OctoShifterProcessor::PAR_FILTERRESON,
                                      8, OctoShifterProcessor::PAR_RESON0_LFOAMOUNTS));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "PAN/AZIMUTH", OctoShifterProcessor::PAR_AZIMUTH, 8,
        OctoShifterProcessor::PAR_AZIMUTHLFOAMOUNTS, PitchShifter::MT_AZI));

    for (auto &c : multiSliders)
    {
        addAndMakeVisible(*c);
    }

    addAndMakeVisible(infoLabel);
    add_xapslider(p, *this, pitchQualityDrop);
    for (int i = 0; i < OctoShifterProcessor::numLFOs; ++i)
    {
        auto c = std::make_unique<LFOComponent>(p, i);
        addAndMakeVisible(*c);
        lfo_comps.push_back(std::move(c));
    }

    visitAll<XapSlider>(*this, [this](XapSlider *slid) {
        paridtoslidermap[slid->getParameterMetaData().id] = slid;
    });
    startTimerHz(25);
    setSize(1100, 700);
}

OctoShifterProcessorEditor::~OctoShifterProcessorEditor() { processorRef.gui_visible = false; }

void OctoShifterProcessorEditor::timerCallback()
{
    juce::String txt;
    txt += juce::String(processorRef.infifoavail) + " " + juce::String(processorRef.outfifoavail);
    txt += " " + juce::String(processorRef.outsilencecount);
    txt += " " + juce::String(processorRef.shifters[0].latency_in);
    txt += " " + juce::String(processorRef.shifters[0].latency_out);
    infoLabel.setText(txt, juce::dontSendNotification);
    ParamChangeMessage msg;
    while (processorRef.params_to_gui_fifo.pop(msg))
    {
        auto it = paridtoslidermap.find(msg.id);
        if (it != paridtoslidermap.end())
        {
            it->second->setValue(msg.val);
        }
    }
    ModulationMessage modmsg;
    while (processorRef.modulations_to_gui_fifo.pop(modmsg))
    {
        for (auto &c : multiSliders)
        {
            if (c->shifter_modulation_target >= 0)
            {
                auto it = processorRef.mod_ranges.find(c->shifter_modulation_target);
                if (it != processorRef.mod_ranges.end())
                {
                    c->paraminfos[modmsg.shifterindex].modulatedvalue =
                        modmsg.values[c->shifter_modulation_target] * it->second;
                }
            }
        }
        /*
        aziMulti.paraminfos[modmsg.shifterindex].modulatedvalue =
            modmsg.values[PitchShifter::MT_AZI];
        pitchMulti.paraminfos[modmsg.shifterindex].modulatedvalue =
            modmsg.values[PitchShifter::MT_PITCH] * 12.0;
        formantMulti.paraminfos[modmsg.shifterindex].modulatedvalue =
            modmsg.values[PitchShifter::MT_FORMANT] * 12.0;
        inVolumeMulti.paraminfos[modmsg.shifterindex].modulatedvalue =
            modmsg.values[PitchShifter::MT_INVOLUME] * 12.0;
        cutoffMulti.paraminfos[modmsg.shifterindex].modulatedvalue =
            modmsg.values[PitchShifter::MT_F0CUTOFF] * 24.0;
        */
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
    for (auto &c : multiSliders)
    {
        multiflex.items.add(juce::FlexItem(*c).withFlex(1.0).withMargin(3));
    }
    juce::FlexBox miscflex;
    miscflex.flexDirection = juce::FlexBox::Direction::row;
    miscflex.items.add(juce::FlexItem(pitchQualityDrop).withFlex(1).withMaxHeight(25));
    miscflex.items.add(juce::FlexItem(infoLabel).withFlex(1).withMaxHeight(25));
    multiflex.items.add(juce::FlexItem(miscflex).withFlex(1.0).withMargin(3));
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
