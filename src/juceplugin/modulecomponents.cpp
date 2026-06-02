#include "modulecomponents.h"
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
            normy = auxenv.get_value(normx, priorauxwarp);
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
        g.drawFittedText(lastError, 1, 1, getWidth() - 2, getHeight() - 2,
                         juce::Justification::left, 8);
    }
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
                    msg.fval0 = arr[i].getWithDefault(0.0f);
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