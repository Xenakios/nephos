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
    if (mode == TM_RotateLeft)
    {
        std::rotate(oldsteps.begin(), oldsteps.begin() + 1, oldsteps.begin() + numsteps);
        waschanged = true;
    }
    if (mode == TM_RotateRight)
    {
        std::rotate(oldsteps.begin(), oldsteps.begin() + (numsteps - 1),
                    oldsteps.begin() + numsteps);
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
        menu.addItem("Rotate left", [this]() { transform_steps(TM_RotateLeft); });
        menu.addItem("Rotate right", [this]() { transform_steps(TM_RotateRight); });
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
        if (ev.y < 15)
        {
            target_envelope = ev.x / 15.0f;
            target_envelope =
                std::clamp<int>(target_envelope, 0, GranulatorVoice::num_aux_envelopes - 1);
            repaint();
            return;
        }
        if (ev.y >= 15 && ev.y < top_margin)
        {
            int pindex = ev.x / 45.0;
            if (pindex >= 0 && pindex < 3)
            {
                if (pindex == 0)
                    target_param = ToneGranulator::PAR_AUXENVTIMEWARP + target_envelope;
                if (target_param != CLAP_INVALID_ID)
                    param_start_value = *granul->idtoparvalptr[target_param];
            }
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