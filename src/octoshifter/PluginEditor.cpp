#include "PluginProcessor.h"
#include "clap/id.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "modcurves.h"
#include "sst/filters++/api.h"
#include "sst/filters++/enums.h"
#include "sst/filters++/enums_to_string.h"
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

OctoShifterProcessorEditor::OctoShifterProcessorEditor(OctoShifterProcessor &p)
    : AudioProcessorEditor(&p), processorRef(p),
      pitchQualityDrop(XapSlider::SS_HorizontalSlider,
                       *p.par_id_to_metadata[OctoShifterProcessor::PAR_PITCHQUALITY]),
      outputModeDrop(XapSlider::SS_HorizontalSlider,
                     *p.par_id_to_metadata[OctoShifterProcessor::PAR_OUTPUTMODE]),
      spatializationTabs(juce::TabbedButtonBar::Orientation::TabsAtTop)
{
    addAndMakeVisible(spatializationTabs);
    spatializationTabs.setTabBarDepth(16);
    multiSliders.push_back(
        std::make_unique<MultiSlider>(p, "ENABLED", OctoShifterProcessor::PAR_ENABLED, 8));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "INPUT VOLUME", OctoShifterProcessor::PAR_INPUT_GAIN, 8,
        OctoShifterProcessor::PAR_INVOLUME_LFOAMOUNTS, PitchShifter::MT_INVOLUME));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "PITCH", OctoShifterProcessor::PAR_PITCH, 8, OctoShifterProcessor::PAR_PITCHLFOAMOUNTS,
        PitchShifter::MT_PITCH));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "FORMANT", OctoShifterProcessor::PAR_FORMANT, 8,
        OctoShifterProcessor::PAR_FORMANT_LFOAMOUNTS, PitchShifter::MT_FORMANT));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "DELAY TIME", OctoShifterProcessor::PAR_DELAYTIME, 8, CLAP_INVALID_ID, -1));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "DELAY FEEDBACK", OctoShifterProcessor::PAR_DELAYFEEDBACK, 8,
        OctoShifterProcessor::PAR_DELAYFB_LFOAMOUNTS, PitchShifter::MT_DELAYFEEDBACK));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "FILTER CUTOFF", OctoShifterProcessor::PAR_FILTERCUTOFF, 8,
        OctoShifterProcessor::PAR_CUTOFF0LFOAMOUNTS, PitchShifter::MT_F0CUTOFF));
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "FILTER RESONANCE", OctoShifterProcessor::PAR_FILTERRESON, 8,
        OctoShifterProcessor::PAR_RESON0_LFOAMOUNTS, PitchShifter::MT_F0RESON));

    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "PAN/AZIMUTH", OctoShifterProcessor::PAR_AZIMUTH, 8,
        OctoShifterProcessor::PAR_AZIMUTHLFOAMOUNTS, PitchShifter::MT_AZI));
    spatializationTabs.addTab("PAN/AZIMUTH", juce::Colours::grey, multiSliders.back().get(), false);
    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "ELEVATION", OctoShifterProcessor::PAR_ELEVATION, 8,
        OctoShifterProcessor::PAR_ELEVATION_LFOAMOUNTS, PitchShifter::MT_ELEVATION));
    spatializationTabs.addTab("ELEVATION", juce::Colours::grey, multiSliders.back().get(), false);

    multiSliders.push_back(std::make_unique<MultiSlider>(
        p, "OUTPUT VOLUME", OctoShifterProcessor::PAR_OUTPUT_GAIN, 8,
        OctoShifterProcessor::PAR_OUTVOLUME_LFOAMOUNTS, PitchShifter::MT_OUTVOLUME));
    for (auto &c : multiSliders)
    {
        if (c->groupname != "PAN/AZIMUTH" && c->groupname != "ELEVATION")
            addAndMakeVisible(*c);
    }
    addAndMakeVisible(optionsMenuButton);
    optionsMenuButton.setButtonText("OPTIONS");
    optionsMenuButton.onClick = [this]() {
        juce::PopupMenu menu;
        menu.addItem(
            "Formant compensation enabled", true, processorRef.formant_compensation,
            [this]() { processorRef.formant_compensation = !processorRef.formant_compensation; });
        menu.addItem("DC blocker enabled", true, processorRef.dc_blocker_enabled, [this]() {
            processorRef.dc_blocker_enabled = !processorRef.dc_blocker_enabled;
        });
        menu.showMenuAsync({});
    };
    addAndMakeVisible(infoLabel);
    add_xapslider(p, *this, pitchQualityDrop);
    add_xapslider(p, *this, outputModeDrop);
    for (int i = 0; i < OctoShifterProcessor::numLFOs; ++i)
    {
        auto c = std::make_unique<LFOComponent>(p, i);
        addAndMakeVisible(*c);
        lfo_comps.push_back(std::move(c));
    }

    visitAll<XapSlider>(*this, [this](XapSlider *slid) {
        paridtoslidermap[slid->getParameterMetaData().id] = slid;
    });

    addAndMakeVisible(filterPicker.showButton);
    // filterPicker.addToDesktop(juce::ComponentPeer::StyleFlags::windowIsResizable |
    //                           juce::ComponentPeer::StyleFlags::windowHasTitleBar);
    auto filtmodes = sst::filtersplusplus::Filter::availableModels();
    int64_t id = 0;
    for (auto &mo : filtmodes)
    {
        if (mo == sst::filtersplusplus::FilterModel::Comb ||
            mo == sst::filtersplusplus::FilterModel::CutoffWarp)
            continue;
        auto confs = sst::filtersplusplus::Filter::availableModelConfigurations(mo);
        auto moname = sst::filtersplusplus::toString(mo);
        for (auto &co : confs)
        {
            filterPicker.add_entry(moname, id, co.toString());
            FilterModeMessage msg;
            msg.model = mo;
            msg.config = co;
            id_to_filter_mode[id] = msg;
            ++id;
        }
    }
    filterPicker.OnSelected = [this](int64_t id) {
        auto msg = id_to_filter_mode[id];
        processorRef.filter_mode_from_gui_fifo.push(msg);
        update_filter_button(id);
    };
    startTimerHz(10);
    setSize(1100, 700);
    vblankAttachment = std::make_unique<juce::VBlankAttachment>(this, [this]() {
        if (paint_throttle_counter == 0)
        {
            update_components_with_modulation();
        }
        ++paint_throttle_counter;
        if (paint_throttle_counter == 1)
            paint_throttle_counter = 0;
    });
    p.gui_visible = true;
}

OctoShifterProcessorEditor::~OctoShifterProcessorEditor() { processorRef.gui_visible = false; }

void OctoShifterProcessorEditor::update_components_with_modulation()
{
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
            c->repaint();
        }
    }
}

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
    FilterModeMessage fmsg;
    while (processorRef.filter_mode_to_gui_fifo.pop(fmsg))
    {
        for (auto &e : id_to_filter_mode)
        {
            if (e.second.model == fmsg.model && e.second.config == fmsg.config)
            {
                filterPicker.set_selected_ID(e.first);
                update_filter_button(e.first);
                break;
            }
        }
    }
}

void OctoShifterProcessorEditor::update_filter_button(int64_t id)
{
    for (auto &e : filterPicker.categories)
    {
        for (auto &i : e.items)
        {
            if (i.id == id)
            {
                std::string buttext = e.text + " : " + i.text;
                filterPicker.showButton.setButtonText(buttext);
                break;
            }
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
    for (auto &c : multiSliders)
    {
        if (c->groupname != "PAN/AZIMUTH" && c->groupname != "ELEVATION")
        {
            multiflex.items.add(juce::FlexItem(*c).withFlex(1.0).withMargin(3));
        }
    }
    multiflex.items.add(juce::FlexItem(spatializationTabs).withFlex(1).withMargin(3));
    juce::FlexBox miscflex;
    miscflex.flexDirection = juce::FlexBox::Direction::row;
    miscflex.items.add(
        juce::FlexItem(optionsMenuButton).withFlex(0.3).withMaxHeight(25).withMargin(2));
    miscflex.items.add(
        juce::FlexItem(pitchQualityDrop).withFlex(1).withMaxHeight(25).withMargin(2));
    miscflex.items.add(juce::FlexItem(outputModeDrop).withFlex(1).withMaxHeight(25).withMargin(2));
    miscflex.items.add(
        juce::FlexItem(filterPicker.showButton).withFlex(1).withMaxHeight(25).withMargin(2));
    miscflex.items.add(juce::FlexItem(infoLabel).withFlex(0.7).withMaxHeight(25));

    flex.items.add(juce::FlexItem(miscflex).withFlex(0.4).withMargin(1));
    for (auto &e : multiflex.items)
    {
        e = e.withMinWidth(500);
    }
    flex.items.add(juce::FlexItem(multiflex).withFlex(8));
    juce::FlexBox lfoflex;
    lfoflex.flexDirection = juce::FlexBox::Direction::row;
    for (auto &c : lfo_comps)
    {
        lfoflex.items.add(juce::FlexItem(*c).withFlex(1.0).withMargin(1));
    }
    flex.items.add(juce::FlexItem(lfoflex).withFlex(1.0));
    flex.performLayout(getLocalBounds());
}
void MultiSlider::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::white);

    g.setFont(12.0);
    // auto polar = pref.par_id_to_metadata[firstpar]->getPolarity();
    juce::String txt = groupname;
    // if (polar == pmd::Polarity::BIPOLAR)
    //     txt += " (BIPOLAR)";
    g.drawText(txt, 2, 1, getWidth(), 16, juce::Justification::centredLeft);

    int parstodraw = paraminfos.size();
    float barw = (float)sliders_area.getWidth() / parstodraw;
    for (int i = 0; i < parstodraw; ++i)
    {
        float xcor = sliders_area.getX() + barw * i;
        float v = paraminfos[i].value;
        uint32_t pid = paraminfos[i].id;
        if (pid != CLAP_INVALID_ID)
        {
            auto partxt = pref.par_id_to_metadata[pid]->valueToString(v);
            if (partxt)
            {
                g.setColour(juce::Colours::white);
                g.drawText(*partxt, xcor, getHeight() - 18, barw, 18,
                           juce::Justification::centredBottom);
            }
        }
        float pmin = paraminfos[i].minval;
        float pmax = paraminfos[i].maxval;
        if (paraminfos[i].lfoindex >= 0)
        {
            g.setColour(juce::Colours::white);
            g.drawText("LFO" + juce::String(paraminfos[i].lfoindex + 1) + " AMT", xcor, 1, barw, 16,
                       juce::Justification::centred);
        }
        float ycor =
            juce::jmap<float>(v, pmin, pmax, sliders_area.getBottom(), sliders_area.getY());
        g.setColour(juce::Colours::yellow);
        g.drawLine(xcor, ycor, xcor + barw - 1.0, ycor, 2.0);
        if (paraminfos[i].lfoindex == -1)
        {
            g.setColour(juce::Colours::lightgrey);
            v = v + paraminfos[i].modulatedvalue;
            v = std::clamp(v, pmin, pmax);
            ycor = juce::jmap<float>(v, pmin, pmax, sliders_area.getBottom(), sliders_area.getY());
            g.drawLine(xcor, ycor, xcor + barw - 1.0, ycor, 2.0);
        }
    }
}
void MultiSlider::mouseDown(const juce::MouseEvent &ev)
{
    int paramindex = (float)(paraminfos.size()) / getWidth() * ev.x;
    if (paramindex < 0 || paramindex >= paraminfos.size())
        return;
    if (ev.mods.isRightButtonDown())
    {
        if (paraminfos[paramindex].lfoindex == -1)
        {
            juce::PopupMenu menu;
            for (int i = 0; i < OctoShifterProcessor::numLFOs; ++i)
            {
                juce::PopupMenu curvemenu;
                auto curveinfos = get_curve_metadata();
                for (auto &c : curveinfos)
                {
                    curvemenu.addItem(c.groupname + " : " + c.name, true, false,
                                      [this, c, paramindex, i]() {
                                          CurveMessage msg;
                                          msg.shifter_index = paramindex;
                                          msg.curveid = c.id;
                                          msg.lfoindex = i;
                                          msg.shifter_param_target = shifter_modulation_target;
                                          pref.curve_from_gui_fifo.push(msg);
                                      });
                }
                menu.addSubMenu("LFO " + juce::String(i + 1) + " curve", curvemenu);
            }
            // menu.addSectionHeader("LFO " + juce::String(paraminfos[paramindex].lfoindex + 1) +
            //                       " modulation curve");

            menu.showMenuAsync({});
            return;
        }
        if (paraminfos[paramindex].lfoindex == -1)
        {
            juce::PopupMenu menu;
            menu.addSectionHeader("Per shifter modulation pattern");
            for (int i = 0; i < OctoShifterProcessor::num_modulation_patterns; ++i)
            {
                std::string txt;
                for (int j = 0; j < 8; ++j)
                {
                    txt += fmt::format("{} ", pref.modulation_patterns[i][j]);
                }
                int curpat = pref.modtarget_to_modpattern[shifter_modulation_target];
                menu.addItem(txt, true, i == curpat, [this, i]() {
                    pref.modtarget_to_modpattern[shifter_modulation_target] = i;
                });
            }
            menu.showMenuAsync({});
            return;
        }
    }

    draggedindex = -1;

    if (paramindex >= 0 && paramindex < paraminfos.size())
    {
        if (ev.y > sliders_area.getBottom() - 5)
        {
            show_value_editor(paramindex, ev.x);
            return;
        }
        draggedindex = paramindex;
        if (!paraminfos[paramindex].notify_only_on_mouse_up)
            post_param_event(draggedindex, parval_from_mouse_y(draggedindex, ev.y));
        repaint();
    }
}
