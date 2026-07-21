#include "modulecomponents.h"
#include "clap/id.h"
#include "juce_core/juce_core.h"
#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "sst/basic-blocks/dsp/SpecialFunctions.h"
#include "text/choc_Files.h"
#include "text/choc_JSON.h"
#include <exception>
#include <stdexcept>

void GrainModulationVisualizationComponent::timerCallback()
{
    ToneGranulator::GrainEnvelopeVisMessage msg;
    while (granul->gevisfifo.pop(msg))
    {
        vismsg = msg;
    }
    repaint();
}

void GrainModulationVisualizationComponent::mouseDown(const juce::MouseEvent &ev)
{
    juce::PopupMenu menu;
    std::set<uint32_t> targets;
    for (auto &e : granul->voices[0]->modulation_slots)
    {
        if (e.target_id != CLAP_INVALID_ID)
            targets.insert(e.target_id);
    }
    menu.addSectionHeader("Modulation targets");
    for (auto &e : targets)
    {
        menu.addItem(GranulatorVoice::get_mod_target_name((GranulatorVoice::MODTARGET)e),
                     [this, e]() {
                         target_to_show = e;
                         repaint();
                     });
    }
    menu.showMenuAsync({});
}

void GrainModulationVisualizationComponent::paint(juce::Graphics &g)
{
    double t0 = juce::Time::getMillisecondCounterHiRes();
    g.fillAll(juce::Colours::black);
    path.clear();
    alignas(16) std::array<GranulatorVoice::ModSlot, GrainEvent::max_grain_mod_slots>
        modulation_slots;

    for (int i = 0; i < modulation_slots.size(); ++i)
    {
        float depth = vismsg.moddepths[i];
        modulation_slots[i] = {vismsg.modsources[i], depth, vismsg.modtargets[i]};
    }
    float sinfreq = getWidth() / 8.0;
    float curvemorph = vismsg.curvemorph;
    int curvestart = vismsg.startcurve;
    int curveend = vismsg.endcurve;
    auto &eluts = granul->eluts;
    float grainvol = vismsg.grainvolume;

    for (int i = 0; i < getWidth(); ++i)
    {
        float modvalues[30] = {0.0f};
        double normphase = 1.0 / getWidth() * i;
        GranulatorVoice::process_mod_matrix(normphase, vismsg.auxenvparams, modulation_slots,
                                            granul->voiceaux_envelopes, modvalues);
        float y = modvalues[target_to_show];
        if (target_to_show == GranulatorVoice::MT_VOLUME)
        {
            y += grainvol;
            y = std::clamp(y, 0.0f, 1.0f);
            y = y * y * y;
            float sinvalue = std::sin(2 * M_PI * normphase * sinfreq);
            y = sinvalue * y;
            float envgain = 0.0f;
            if (normphase < curvemorph)
            {
                normphase = xenakios::mapvalue<float>(normphase, 0.0f, curvemorph, 0.0f, 1.0f);
                envgain = eluts.getValueLERP<true>(curvestart, normphase);
            }
            else
            {
                normphase = xenakios::mapvalue<float>(normphase, curvemorph, 1.0f, 1.0f, 0.0f);
                envgain = eluts.getValueLERP<true>(curveend, normphase);
            }
            y *= envgain;
            y = juce::jmap(y, -1.0f, 1.0f, (float)getHeight(), 0.0f);
        }
        else
        {
            y = juce::jmap(y, -1.0f, 1.0f, (float)getHeight(), 0.0f);
        }

        if (i == 0)
            path.startNewSubPath(i, y);
        else
            path.lineTo(i, y);
    }
    g.setColour(juce::Colours::yellow);
    g.strokePath(path, juce::PathStrokeType(2.0f));
    double t1 = juce::Time::getMillisecondCounterHiRes();
    paint_elapsed_sum += t1 - t0;
    ++paintcount;
    double avg = paint_elapsed_sum / paintcount;
    int fifoslots = granul->gevisfifo.getUsedSlots();
    if (false)
    {
        g.setColour(juce::Colours::white);
        g.drawText(juce::String(avg, 2) + " ms avg " + juce::String(fifoslots), 1, 1, getWidth(),
                   25, juce::Justification::centredLeft);
    }

    if (paintcount == 50)
    {
        paintcount = 0;
        paint_elapsed_sum = 0.0;
    }
}

void GrainEnvelopeEditorComponent::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colours::black);
    for (int i = 0; i < GranulatorVoice::num_aux_envelopes; ++i)
    {
        if (i == target_envelope)
            g.setColour(juce::Colours::lightgrey);
        else
            g.setColour(juce::Colours::darkgrey);
        g.fillRect(i * 15, 0, 13, 13);
        g.setColour(juce::Colours::white);
        g.drawText(juce::String(i + 1), i * 15, 0, 13, 13, juce::Justification::centred);
    }
    juce::StringArray params{"TWARP", "TSHIFT", "VWARP"};
    for (int i = 0; i < params.size(); ++i)
    {
        g.setColour(juce::Colours::darkgrey);
        g.fillRect(i * 45, 15, 44, 13);
        g.setColour(juce::Colours::white);
        g.drawText(params[i], i * 45, 15, 44, 10, juce::Justification::centred);
    }
    if (target_param != CLAP_INVALID_ID)
    {
        float parval = *granul->idtoparvalptr[target_param];
        g.drawText(juce::String(parval, 3), 0, top_margin, 100, 15, juce::Justification::centred);
    }
    curvepath.clear();
    auto &auxenv = granul->voiceaux_envelopes[target_envelope];

    g.setColour(juce::Colours::white);
    auto numsteps = SimpleEnvelope<false>::maxnumsteps;
    for (int i = 0; i < numsteps; ++i)
    {
        float x0 = (float)getWidth() / numsteps * i;
        float x1 = (float)getWidth() / numsteps * (i + 1);
        float y = juce::jmap<float>(auxenv.steps[i], -1.1f, 1.1f, getHeight(), top_margin);
        g.drawLine(x0, y, x1, y, 2.0f);
    }
    // g.drawText("Envelope " + juce::String(target_envelope + 1), 1, 1, getWidth() - 2, 20,
    //            juce::Justification::centredTop);
    g.drawFittedText(lastError, 1, 20, getWidth() - 2, getHeight() - 2,
                     juce::Justification::topLeft, 8);
}

void GrainEnvelopeEditorComponent::transform_steps(TransformMode mode)
{
    auto numsteps = SimpleEnvelope<false>::maxnumsteps;
    auto oldsteps = granul->voiceaux_envelopes[target_envelope].steps;
    bool waschanged = false;
    if (mode == TM_Reverse)
    {
        std::reverse(oldsteps.begin(), oldsteps.begin() + numsteps);
        waschanged = true;
    }
    if (mode == TM_Mutate)
    {
        for (auto &e : oldsteps)
        {
            e = std::clamp(e + rng.nextHypCos(0.0, 0.02), -1.0, 1.0);
        }
        waschanged = true;
    }
    if (mode == TM_Sort)
    {
        std::sort(oldsteps.begin(), oldsteps.begin() + numsteps);
        waschanged = true;
    }
    if (mode == TM_ApplyEnvelope)
    {
        for (int i = 0; i < numsteps; ++i)
        {
            float x = juce::jmap<float>(i, 0, numsteps - 1, 0.0f, 1.0f);
            float y = 0.0f;
            if (x < 0.5)
                y = juce::jmap(x, 0.0f, 0.5f, 0.0f, 1.0f);
            else
                y = juce::jmap(x, 0.5f, 1.0f, 1.0f, 0.0f);
            oldsteps[i] *= y;
        }
        waschanged = true;
    }
    if (waschanged)
    {
        for (int i = 0; i < numsteps; ++i)
        {
            StepModSource::Message msg;
            msg.opcode = StepModSource::Message::OP_SETSTEP;
            msg.fval0 = oldsteps[i];
            msg.dest = 1000 + target_envelope;
            msg.ival0 = i;
            granul->fifo.push(msg);
        }
        juce::Timer::callAfterDelay(100, [this]() { repaint(); });
    }
}

void GrainEnvelopeEditorComponent::mouseDown(const juce::MouseEvent &ev)
{
    auto numsteps = SimpleEnvelope<false>::maxnumsteps;
    if (ev.mods.isRightButtonDown())
    {
        juce::PopupMenu menu;
        menu.addSectionHeader("Interpolation mode");
        juce::StringArray modes{"None", "Linear", "Spline"};
        for (int i = 0; i < modes.size(); ++i)
        {
            menu.addItem(modes[i], true,
                         granul->get_aux_envelope_interpolation_mode(target_envelope) == i,
                         [i, this]() { set_interpolation_mode(i); });
        }
        menu.addSectionHeader("Transform");
        menu.addItem("Reverse", [this]() { transform_steps(TM_Reverse); });
        menu.addItem("Mutate", [this]() { transform_steps(TM_Mutate); });
        menu.addItem("Sort", [this]() { transform_steps(TM_Sort); });
        menu.addItem("Envelope", [this]() { transform_steps(TM_ApplyEnvelope); });

        menu.addSectionHeader("Generate");
        menu.addItem("Reset to zero", [this]() { generate_steps(GM_RESET); });
        menu.addItem("Reset to max", [this]() { generate_steps(GM_MAX); });
        menu.addItem("Alt min/max", [this]() { generate_steps(GM_ALTERNATEMINMAX); });
        menu.addItem("Ramp up", [this]() { generate_steps(GM_RAMPUP); });
        menu.addItem("Ramp up/down", [this]() { generate_steps(GM_RAMPUPDOWN); });
        menu.addItem("Random Uniform", [this]() { generate_steps(GM_RANDOM); });
        menu.addItem("Paste from JSON array in clipboard",
                     [this]() { generate_steps(GM_CLIPBOARD); });
        auto presetsmenu = generate_presets_menu();
        menu.addSubMenu("Presets", presetsmenu);

        /*
        menu.addItem("Help", []() {
juce::URL("file:///C:/develop/nephos/src/nephos_help.html")
   .launchInDefaultBrowser();
});
*/
        menu.showMenuAsync(juce::PopupMenu::Options{});
    }
    else
    {
        if (ev.y < 14)
        {
            target_envelope = ev.x / 15.0f;
            target_envelope =
                std::clamp<int>(target_envelope, 0, GranulatorVoice::num_aux_envelopes - 1);
            repaint();
            return;
        }
        if (ev.y >= 15 && ev.y < top_margin)
        {
            target_param = get_param_from_x_coord(ev.x);
            if (target_param != CLAP_INVALID_ID)
                param_start_value = *granul->idtoparvalptr[target_param];
            repaint();
            return;
        }
        int stepindex = numsteps / (float)getWidth() * ev.x;
        if (stepindex >= 0 && stepindex < numsteps)
        {
            float val = juce::jmap<float>(ev.y, top_margin, getHeight(), 1.1, -1.1);
            StepModSource::Message msg;
            msg.opcode = StepModSource::Message::OP_SETSTEP;
            msg.fval0 = val;
            msg.dest = 1000 + target_envelope;
            msg.ival0 = stepindex;
            granul->fifo.push(msg);
        }
    }
    juce::Timer::callAfterDelay(100, [this]() { repaint(); });
}

void GrainEnvelopeEditorComponent::mouseDrag(const juce::MouseEvent &ev)
{
    if (target_param != CLAP_INVALID_ID)
    {
        float delta = ev.getDistanceFromDragStartY() * 0.01;
        float newval = std::clamp(param_start_value - delta, -1.0f, 1.0f);
        if (target_param >= ToneGranulator::PAR_AUXENVTIMEWARP &&
            target_param < ToneGranulator::PAR_AUXENVTIMESHIFT + 4)
        {
            ParameterMessage msg;
            msg.id = target_param;
            msg.value = newval;
            processorRef.params_from_gui_fifo.push(msg);
        }
        // DBG(newval);
        // param_start_value = newval;
    }
}
void GrainEnvelopeEditorComponent::mouseDoubleClick(const juce::MouseEvent &ev)
{
    auto pid = get_param_from_x_coord(ev.x);
    if (pid != CLAP_INVALID_ID)
    {
        ParameterMessage msg;
        msg.id = pid;
        msg.value = 0.0f;
        processorRef.params_from_gui_fifo.push(msg);
        repaint();
    }
}

juce::PopupMenu GrainEnvelopeEditorComponent::generate_presets_menu()
{
    juce::PopupMenu menu;
    for (auto &f : juce::RangedDirectoryIterator{
             juce::File(R"(C:\develop\nephos\pitchenvelopepresets)"), false})
    {
        menu.addItem(f.getFile().getFileNameWithoutExtension(), [f, this]() {
            lastError = "";
            try
            {
                auto txt =
                    choc::file::loadFileAsString(f.getFile().getFullPathName().toStdString());
                auto arr = choc::json::parse(txt);
                auto numsteps = SimpleEnvelope<false>::maxnumsteps;
                if (arr.isArray())
                {
                    for (int i = 0; i < numsteps; ++i)
                    {
                        if (i < arr.size())
                        {
                            StepModSource::Message msg;
                            msg.opcode = StepModSource::Message::OP_SETSTEP;
                            msg.fval0 = arr[i].getWithDefault(0.0f);
                            msg.dest = 1000 + target_envelope;
                            msg.ival0 = i;
                            granul->fifo.push(msg);
                        }
                    }
                    juce::Timer::callAfterDelay(100, [this]() { repaint(); });
                }
            }
            catch (std::exception &exc)
            {
                lastError = exc.what();
                repaint();
            }
        });
    }
    return menu;
}
void GrainEnvelopeEditorComponent::mouseWheelMove(const juce::MouseEvent &event,
                                                  const juce::MouseWheelDetails &wheel)
{
    auto numsteps = SimpleEnvelope<false>::maxnumsteps;
    int stepindex = numsteps / (float)getWidth() * event.x;
    if (stepindex >= 0 && stepindex < numsteps)
    {
        float delta = wheel.deltaY * 0.2;
        auto &auxenv = granul->voiceaux_envelopes[target_envelope];
        float val = std::clamp(auxenv.steps[stepindex] + delta, -1.0f, 1.0f);
        StepModSource::Message msg;
        msg.opcode = StepModSource::Message::OP_SETSTEP;
        msg.fval0 = val;
        msg.dest = 1000 + target_envelope;
        msg.ival0 = stepindex;
        granul->fifo.push(msg);
    }
    repaint();
}

void GrainEnvelopeEditorComponent::generate_steps(GenMode mode)
{
    auto numsteps = SimpleEnvelope<false>::maxnumsteps;
    lastError = "";
    if (mode == GM_CLIPBOARD)
    {
        try
        {
            auto text = juce::SystemClipboard::getTextFromClipboard().toStdString();
            auto arr = choc::json::parse(text);
            if (arr.isArray() && arr.size() == numsteps)
            {
                for (int i = 0; i < numsteps; ++i)
                {
                    StepModSource::Message msg;
                    msg.opcode = StepModSource::Message::OP_SETSTEP;
                    msg.fval0 = std::clamp(arr[i].getWithDefault(0.0f), -1.0f, 1.0f);
                    msg.dest = 1000 + target_envelope;
                    msg.ival0 = i;
                    granul->fifo.push(msg);
                }
                juce::Timer::callAfterDelay(100, [this]() { repaint(); });
            }
            else
                throw std::runtime_error("Invalid JSON data");
        }
        catch (std::exception &excep)
        {
            lastError = excep.what();
            repaint();
        }
        return;
    }

    for (int i = 0; i < numsteps; ++i)
    {
        float val = 0.0f;
        if (mode == GM_RANDOM)
            val = rng.nextFloatInRange(-1.0f, 1.0f);
        else if (mode == GM_MAX)
            val = 1.0f;
        else if (mode == GM_ALTERNATEMINMAX)
        {
            if (i % 2 == 0)
                val = -1.0f;
            else
                val = 1.0f;
        }
        else if (mode == GM_RAMPUP)
            val = juce::jmap<float>(i, 0, numsteps - 1, -1.0f, 1.0f);
        else if (mode == GM_RAMPUPDOWN)
        {
            if (i < numsteps / 2)
                val = juce::jmap<float>(i, 0, numsteps / 2 - 1, -1.0f, 1.0f);
            else
                val = juce::jmap<float>(i, numsteps / 2, numsteps - 1, 1.0f, -1.0f);
        }
        StepModSource::Message msg;
        msg.opcode = StepModSource::Message::OP_SETSTEP;
        msg.fval0 = val;
        msg.dest = 1000 + target_envelope;
        msg.ival0 = i;
        granul->fifo.push(msg);
    }
    juce::Timer::callAfterDelay(100, [this]() { repaint(); });
}
juce::Image OscTypeComponent::drawWaveImage(std::function<float(float)> func)
{
    juce::Image img{juce::Image::ARGB, 50, 50, true};
    juce::Graphics g(img);
    g.fillAll(juce::Colours::transparentWhite);
    juce::Path p;
    for (int i = 0; i < img.getWidth(); ++i)
    {
        float x = juce::jmap<float>(i, 0, img.getWidth() - 1, 0.0f, 1.0f);
        float y = (1.0 - func(x)) * 0.95 + 0.025;
        y = y * img.getHeight();
        if (i == 0)
        {
            p.startNewSubPath(0.0f, y);
        }
        else
        {
            p.lineTo(i, y);
        }
    }
    g.setColour(juce::Colours::white);
    g.strokePath(p, juce::PathStrokeType(2.0f));
    return img;
}
bool OscTypeComponent::keyPressed(const juce::KeyPress &ev)
{
    int delta = 0;
    int otype = -1;
    if (ev.getKeyCode() == juce::KeyPress::leftKey)
        delta = -1;
    if (ev.getKeyCode() == juce::KeyPress::rightKey)
        delta = 1;
    if (delta != 0)
    {
        otype = *processorRef.granulator.idtoparvalptr[ToneGranulator::PAR_OSCTYPE];
        otype += delta;
        if (otype < 0)
            otype = 6;
        if (otype > 6)
            otype = 0;
    }
    if (ev.getKeyCode() >= '1' && ev.getKeyCode() < '8')
    {
        otype = ev.getKeyCode() - '1';
        jassert(otype >= 0 && otype < 7);
    }
    if (otype >= 0 && otype < 7)
    {
        ParameterMessage msg;
        msg.id = ToneGranulator::PAR_OSCTYPE;
        msg.value = otype;
        processorRef.params_from_gui_fifo.push(msg);
        return true;
    }
    return false;
}
void OscTypeComponent::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colours::black);
    int otypebase = *processorRef.granulator.idtoparvalptr[ToneGranulator::PAR_OSCTYPE];
    float cellw = 50.0f;
    for (int i = 0; i < 7; ++i)
    {
        if (i == otypebase)
            g.setColour(juce::Colours::yellow.darker());
        else
            g.setColour(juce::Colours::darkgrey);
        juce::Rectangle<float> r{cellw * i + 2.0f, 2.0f, cellw - 4.0f, cellw - 4.0f};
        g.fillRect(r);

        g.setColour(juce::Colours::white);
        int mappedtype = processorRef.granulator.osctypemapping[i];
        if (mappedtype >= 0 && mappedtype < waveimages.size())
        {
            g.drawImage(waveimages[mappedtype], cellw * i + 2.0, 2.0f, cellw - 4.0f, cellw - 4.0f,
                        0, 0, 50, 50);
        }
        if (i == priorOscType)
        {
            g.setColour(juce::Colours::green);
            g.fillRect(cellw * i + 4.0f, 4.0f, 8.0f, 8.0f);
        }
        // g.drawFittedText(waveshortnames[mappedtype], r.getX(), r.getY(), r.getWidth(),
        //                  r.getHeight(), juce::Justification::centred, 1);
    }
}
void OscTypeComponent::mouseDown(const juce::MouseEvent &ev)
{
    int otype = ev.x / 50.0;
    if (otype >= 0 && otype < 7)
    {
        if (!ev.mods.isRightButtonDown())
        {
            ParameterMessage msg;
            msg.id = ToneGranulator::PAR_OSCTYPE;
            msg.value = otype;
            processorRef.params_from_gui_fifo.push(msg);
        }
        else
        {
            juce::PopupMenu menu;
            for (int i = 0; i < 7; ++i)
            {
                auto dr = std::make_unique<juce::DrawableImage>(waveimages[i]);
                menu.addItem(juce::PopupMenu::Item(waveshortnames[i])
                                 .setImage(std::move(dr))
                                 .setID(i + 1)
                                 .setAction([this, otype, i] {
                                     processorRef.granulator.osctypemapping[otype] = i;
                                     repaint();
                                 }));
            }
            menu.showMenuAsync(juce::PopupMenu::Options{}.withStandardItemHeight(50));
        }
    }
    repaint();
}
OscillatorModuleComponent::OscillatorModuleComponent(AudioPluginAudioProcessor &p)
    : juce::GroupComponent("", "Oscillator"), processorRef(p), oscTypeComponent(p),
      oscPitchKnob(XapSlider::SS_Knob, *p.granulator.idtoparmetadata[ToneGranulator::PAR_PITCH]),
      pitchEnvWarpKnob(XapSlider::SS_Knob,
                       *p.granulator.idtoparmetadata[ToneGranulator::PAR_AUXENVTIMEWARP]),
      oscSyncKnob(XapSlider::SS_Knob, *p.granulator.idtoparmetadata[ToneGranulator::PAR_OSC_SYNC]),
      oscPWKnob(XapSlider::SS_Knob, *p.granulator.idtoparmetadata[ToneGranulator::PAR_OSC_PW]),
      oscFMPitchKnob(XapSlider::SS_Knob,
                     *p.granulator.idtoparmetadata[ToneGranulator::PAR_FMPITCH]),
      oscFMDepthKnob(XapSlider::SS_Knob,
                     *p.granulator.idtoparmetadata[ToneGranulator::PAR_FMDEPTH]),
      oscFMFeedbackKnob(XapSlider::SS_Knob,
                        *p.granulator.idtoparmetadata[ToneGranulator::PAR_FMFEEDBACK]),
      oscNoiseModeDrop(XapSlider::SS_HorizontalSlider,
                       *p.granulator.idtoparmetadata[ToneGranulator::PAR_NOISEMODE]),
      oscNoiseCorrelationKnob(XapSlider::SS_Knob,
                              *p.granulator.idtoparmetadata[ToneGranulator::PAR_NOISECORRELATION]),
      pitchEnvelopeComponent(p), grainModComponent(&p.granulator)
{
    addAndMakeVisible(grainModComponent);
    addAndMakeVisible(oscTypeComponent);
    initSlider(p, *this, oscPitchKnob);
    for (int i = 0; i < GrainEvent::max_grain_mod_slots; ++i)
    {
        auto knob = std::make_unique<XapSlider>(
            XapSlider::SS_Knob,
            *p.granulator.idtoparmetadata[ToneGranulator::PAR_GRAINMODSLOTAMOUNT0 + i]);
        knob->OnAddContextMenuItems = [this, i](juce::PopupMenu &menu) {
            menu.addSectionHeader("Routing");
            juce::PopupMenu sourcemenu;
            auto cursource = processorRef.granulator.voices[0]->modulation_slots[i].source_id;
            sourcemenu.addItem("None", true, cursource == CLAP_INVALID_ID, [this, i] {
                processorRef.granulator.set_grain_modulation_routing(i, CLAP_INVALID_ID, {}, false);
            });
            for (int j = 0; j < 4; ++j)
            {
                sourcemenu.addItem(
                    "Envelope " + juce::String(j + 1), true, cursource == j, [this, i, j]() {
                        processorRef.granulator.set_grain_modulation_routing(i, j, {}, false);
                    });
            }
            menu.addSubMenu("Modulation source", sourcemenu);
            juce::PopupMenu targetmenu;
            auto curtarget = processorRef.granulator.voices[0]->modulation_slots[i].target_id;
            targetmenu.addItem("None", true, curtarget == CLAP_INVALID_ID, [this, i] {
                processorRef.granulator.set_grain_modulation_routing(i, {}, CLAP_INVALID_ID, false);
            });
            for (int j = 0; j < GranulatorVoice::NUMMODTARGETS; ++j)
            {
                targetmenu.addItem(
                    GranulatorVoice::get_mod_target_name((GranulatorVoice::MODTARGET)j), true,
                    curtarget == j, [this, i, j]() {
                        processorRef.granulator.set_grain_modulation_routing(i, {}, j, false);
                    });
            }
            menu.addSubMenu("Modulation target", targetmenu);
        };
        initSlider(p, *this, *knob);
        modDepthKnobs.push_back(std::move(knob));
    }

    initSlider(p, *this, pitchEnvWarpKnob);
    initSlider(p, *this, oscSyncKnob);
    initSlider(p, *this, oscPWKnob);
    initSlider(p, *this, oscFMPitchKnob);
    initSlider(p, *this, oscFMDepthKnob);
    initSlider(p, *this, oscFMFeedbackKnob);
    initSlider(p, *this, oscNoiseModeDrop);
    oscNoiseModeDrop.dropdownXpercent = 0.3;
    initSlider(p, *this, oscNoiseCorrelationKnob);
    addAndMakeVisible(pitchEnvelopeComponent);
    addChildComponent(oscTypeEditor);
    oscTypeEditor.setBounds(2, 2, 200, 25);
}
void OscillatorModuleComponent::resized()
{
    oscTypeComponent.setBounds(7, 17, 350, 50);
    oscPitchKnob.setBounds(7, oscTypeComponent.getBottom() + 1, 80, 100);

    for (int i = 0; i < modDepthKnobs.size(); ++i)
    {
        modDepthKnobs[i]->setBounds(oscPitchKnob.getRight() + 2,
                                    oscTypeComponent.getBottom() + 1 + i * 51, 80, 50);
    }

    // pitchEnvWarpKnob.setBounds(oscPitchKnob.getRight() + 2, pitchEnvKnob.getBottom() + 1, 80,
    //                            50);
    pitchEnvelopeComponent.setBounds(modDepthKnobs[0]->getRight() + 2, oscTypeComponent.getBottom(),
                                     200, 200);
    grainModComponent.setBounds(pitchEnvelopeComponent.getRight() + 2, oscTypeComponent.getBottom(),
                                200, 200);
    oscSyncKnob.setBounds(grainModComponent.getRight() + 2, oscTypeComponent.getBottom() + 1, 80,
                          50);
    oscPWKnob.setBounds(grainModComponent.getRight() + 2, oscSyncKnob.getBottom() + 1, 80, 50);
    oscFMPitchKnob.setBounds(oscSyncKnob.getRight() + 2, oscTypeComponent.getBottom() + 1, 80, 50);
    oscFMDepthKnob.setBounds(oscFMPitchKnob.getRight() + 2, oscTypeComponent.getBottom() + 1, 80,
                             50);
    oscFMFeedbackKnob.setBounds(oscFMDepthKnob.getRight() + 2, oscTypeComponent.getBottom() + 1, 80,
                                50);
    oscNoiseModeDrop.setBounds(oscSyncKnob.getRight() + 2, oscFMPitchKnob.getBottom() + 1, 250, 25);
    oscNoiseCorrelationKnob.setBounds(oscSyncKnob.getRight() + 2, oscNoiseModeDrop.getBottom() + 1,
                                      80, 50);
}
void StackingModuleComponent::resized()
{
    juce::FlexBox flex;
    flex.flexDirection = juce::FlexBox::Direction::row;
    flex.items.add(juce::FlexItem(countKnob).withFlex(1.0).withMargin(2));
    flex.items.add(juce::FlexItem(lengthKnob).withFlex(1.0).withMargin(2));
    flex.items.add(juce::FlexItem(warpKnob).withFlex(1.0).withMargin(2));
    flex.items.add(juce::FlexItem(pitchRandomKnob).withFlex(1.0).withMargin(2));
    flex.items.add(juce::FlexItem(spatRandomKnob).withFlex(1.0).withMargin(2));
    flex.performLayout(juce::Rectangle<int>(7, 17, getWidth() - 14, getHeight() - 28));
}
