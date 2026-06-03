#include "modulecomponents.h"
#include "juce_core/juce_core.h"
#include "juce_graphics/juce_graphics.h"
#include "sst/basic-blocks/dsp/SpecialFunctions.h"
#include <stdexcept>

void VolumeEnvelopeComponent::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::yellow);
    curvepath.clear();
    auto curvemorph = priormorph;
    auto curvestart = priorstartcurve;
    auto curveend = priorendcurve;
    float sinfreq = getWidth() / 8.0;
    auto &eluts = granul->eluts;
    auto &auxenv = granul->voiceaux_envelope;

    for (int i = 0; i < getWidth(); ++i)
    {
        float normx = 1.0 / getWidth() * i;
        float sinvalue = std::sin(2 * M_PI * normx * sinfreq);
        float normy = 0.0f;
        if (!auxenvmode)
        {
            if (normx < curvemorph)
            {
                normx = xenakios::mapvalue(normx, 0.0f, curvemorph, 0.0f, 1.0f);
                // normy = easing_table[curvestart].function(normx);
                normy = eluts.getValueLERP<true>(curvestart, normx);
            }
            else
            {
                normx = xenakios::mapvalue(normx, curvemorph, 1.0f, 1.0f, 0.0f);
                // normy = easing_table[curveend].function(normx);
                normy = eluts.getValueLERP<true>(curveend, normx);
            }
            normy *= sinvalue;
        }
        else
        {
            normy = priorauxamount * auxenv.get_value(normx, priorauxwarp);
            normy *= 1.0;
        }

        float ycor = xenakios::mapvalue<float>(normy, -1.1f, 1.1f, getHeight(), 0);
        if (i == 0)
            curvepath.startNewSubPath({(float)i, ycor});
        else
            curvepath.lineTo({(float)i, ycor});
    }
    g.strokePath(curvepath, juce::PathStrokeType(1.0f));
    if (auxenvmode)
    {
        g.setColour(juce::Colours::white);
        auto numsteps = SimpleEnvelope<false>::maxnumsteps;
        for (int i = 0; i < numsteps; ++i)
        {
            float x0 = (float)getWidth() / numsteps * i;
            float x1 = (float)getWidth() / numsteps * (i + 1);
            float y = juce::jmap<float>(auxenv.steps[i], -1.1f, 1.1f, getHeight(), 0);
            g.drawLine(x0, y, x1, y, 2.0f);
        }
        g.drawText("Pitch Envelope", 1, 1, getWidth() - 2, 20, juce::Justification::centredTop);
        g.drawFittedText(lastError, 1, 20, getWidth() - 2, getHeight() - 2,
                         juce::Justification::topLeft, 8);
    }
}

void VolumeEnvelopeComponent::transform_steps(TransformMode mode)
{
    auto numsteps = SimpleEnvelope<false>::maxnumsteps;
    auto oldsteps = granul->voiceaux_envelope.steps;
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
            msg.dest = 1000;
            msg.ival0 = i;
            granul->fifo.push(msg);
        }
        juce::Timer::callAfterDelay(100, [this]() { repaint(); });
    }
}

void VolumeEnvelopeComponent::mouseDown(const juce::MouseEvent &ev)
{
    if (!auxenvmode)
        return;
    auto numsteps = SimpleEnvelope<false>::maxnumsteps;
    if (ev.mods.isRightButtonDown())
    {
        juce::PopupMenu menu;
        menu.addSectionHeader("Interpolation mode");
        juce::StringArray modes{"None", "Linear", "Spline"};
        for (int i = 0; i < modes.size(); ++i)
        {
            menu.addItem(modes[i], true, granul->get_aux_envelope_interpolation_mode() == i,
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
        menu.addItem("Ramp up", [this]() { generate_steps(GM_RAMPUP); });
        menu.addItem("Random Uniform", [this]() { generate_steps(GM_RANDOM); });
        menu.addItem("Paste from JSON array in clipboard",
                     [this]() { generate_steps(GM_CLIPBOARD); });
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
        int stepindex = numsteps / (float)getWidth() * ev.x;
        if (stepindex >= 0 && stepindex < numsteps)
        {
            float val = juce::jmap<float>(ev.y, 0, getHeight(), 1.1, -1.1);
            StepModSource::Message msg;
            msg.opcode = StepModSource::Message::OP_SETSTEP;
            msg.fval0 = val;
            msg.dest = 1000;
            msg.ival0 = stepindex;
            granul->fifo.push(msg);
        }
    }
    juce::Timer::callAfterDelay(100, [this]() { repaint(); });
}

void VolumeEnvelopeComponent::generate_steps(GenMode mode)
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
                    msg.dest = 1000;
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
        else if (mode == GM_RAMPUP)
            val = juce::jmap<float>(i, 0, numsteps - 1, -1.0f, 1.0f);
        StepModSource::Message msg;
        msg.opcode = StepModSource::Message::OP_SETSTEP;
        msg.fval0 = val;
        msg.dest = 1000;
        msg.ival0 = i;
        granul->fifo.push(msg);
    }
    juce::Timer::callAfterDelay(100, [this]() { repaint(); });
}