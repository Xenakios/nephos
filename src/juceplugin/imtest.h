#include "PluginProcessor.h"
#include "clap/id.h"
#include "inputanalyzer.h"
#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"
#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include <cstdint>
#include <optional>

struct IMMouse
{
    bool is_down = false;
    bool is_dragging = false;
    float x = 0.0f;
    float delta_x = 0.0f;
    float previous_x = 0.0f;
    float y = 0.0f;
    float delta_y = 0.0f;
    float previous_y = 0.0f;
    float drag_start_x = 0.0f;
    float drag_start_y = 0.0f;
};

struct MiniParamResult
{
    std::optional<float> value;
    bool was_clicked = false;
};

inline MiniParamResult IMMiniParam(juce::Graphics &g, juce::Rectangle<float> bounds,
                                   const IMMouse &mouse, const std::string &text, float curval)
{
    g.setColour(juce::Colours::grey);
    g.fillRect(bounds);
    g.setColour(juce::Colours::white);
    g.setFont(bounds.getHeight() - 1.0f);
    MiniParamResult result;
    if (mouse.is_dragging)
    {
        float delta = mouse.y - mouse.previous_y;
        curval = curval + delta * 0.01f;
        curval = std::clamp(curval, -1.0f, 1.0f);
        result.value = curval;
    }
    else if (mouse.is_down && bounds.contains(mouse.x, mouse.y))
    {
        result.was_clicked = true;
        g.drawText(juce::String(curval, 2), bounds, juce::Justification::centred);
    }
    else
        g.drawText(text, bounds, juce::Justification::centred);
    return result;
}

inline bool IMSelectEnvelopeButton(juce::Graphics &g, juce::Rectangle<float> bounds, int env_index,
                                   int cur_index, const IMMouse &mouse)
{
    if (cur_index == env_index)
        g.setColour(juce::Colours::grey);
    else
        g.setColour(juce::Colours::darkgrey);
    g.fillRect(bounds);
    g.setColour(juce::Colours::white);
    g.drawText(juce::String(env_index), bounds, juce::Justification::centred);
    return mouse.is_down && bounds.contains(mouse.x, mouse.y);
}

inline std::optional<std::pair<int, float>> IMSimpleEnvelopeSteps(juce::Graphics &g,
                                                                  juce::Rectangle<float> bounds,
                                                                  IMMouse mouse,
                                                                  SimpleEnvelope &envelope)
{
    auto numsteps = SimpleEnvelope::maxnumsteps;

    int step_under_mouse = numsteps / bounds.getWidth() * mouse.x;

    float stepw = bounds.getWidth() / (numsteps - 0);
    for (int i = 0; i < numsteps; ++i)
    {
        float xcor = stepw * i;
        float y = envelope.get_step(i);
        g.setColour(juce::Colours::white);
        y = juce::jmap(y, -1.0f, 1.0f, bounds.getBottom(), bounds.getY());
        g.drawLine(xcor, y, xcor + stepw, y, 2.0f);
        if (i == step_under_mouse && bounds.contains(mouse.x, mouse.y))
        {
            g.setColour(juce::Colours::yellow.withAlpha(0.5f));
            g.fillRect(xcor, bounds.getY(), stepw, bounds.getHeight());
        }
    }
    if (mouse.is_down && bounds.contains(mouse.x, mouse.y))
    {

        if (step_under_mouse >= 0 && step_under_mouse < numsteps)
        {
            float newy = juce::jmap(mouse.y, bounds.getY(), bounds.getBottom(), 1.0f, -1.0f);
            return std::make_pair(step_under_mouse, newy);
        }
    }
    return {};
}

class IMTestComponent : public juce::Component
{
  public:
    AudioPluginAudioProcessor &processorRef;
    std::unique_ptr<juce::VBlankAttachment> vblankAttachment;
    IMMouse mouse;
    int selected_envelope = 0;
    float par_start_value = 0.0f;
    uint32_t target_param = CLAP_INVALID_ID;
    IMTestComponent(AudioPluginAudioProcessor &p) : processorRef(p)
    {
        vblankAttachment = std::make_unique<juce::VBlankAttachment>(this, [this]() { repaint(); });
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(juce::Colours::black);

        for (int i = 0; i < 4; ++i)
        {
            if (IMSelectEnvelopeButton(g, {1.0f + i * 15, 1.0f, 14.0f, 14.0f}, i + 1,
                                       selected_envelope, mouse))
                selected_envelope = i;
        }
        const std::array<std::string, 2> ops{"XSHIFT", "XWARP"};
        const std::array<uint32_t, 2> pars{ToneGranulator::PAR_AUXENVTIMESHIFT,
                                           ToneGranulator::PAR_AUXENVTIMEWARP};
        for (int i = 0; i < ops.size(); ++i)
        {
            uint32_t parid = pars[i] + selected_envelope;
            float curval = *processorRef.granulator.idtoparvalptr[parid];
            auto miniresult = IMMiniParam(g, {61.0f + i * 45.0f, 1.0f, 43.0f, 14.0f}, mouse, ops[i],
                                          par_start_value);
            if (miniresult.was_clicked)
            {
                target_param = parid;
                par_start_value = curval;
            }
            if (miniresult.value && target_param != CLAP_INVALID_ID)
            {
                ParameterMessage msg;
                msg.id = target_param;
                msg.value = *miniresult.value;
                processorRef.params_from_gui_fifo.push(msg);
            }
        }
        auto edited =
            IMSimpleEnvelopeSteps(g, {1.0f, 16.0f, 200.0f, 200.0f}, mouse,
                                  processorRef.granulator.voiceaux_envelopes[selected_envelope]);
        if (edited)
        {
            StepModSource::Message msg;
            msg.opcode = StepModSource::Message::OP_SETSTEP;
            msg.fval0 = edited->second;
            msg.dest = 1000 + selected_envelope;
            msg.ival0 = edited->first;
            processorRef.granulator.fifo.push(msg);
        }
        mouse.is_dragging = false;
        // g.setColour(juce::Colours::white);
        // g.setFont(30.0f);
        // g.drawText(juce::String(selected_envelope + 1), 1, 16, 100, 100,
        //            juce::Justification::centred);
    }
    void mouseDown(const juce::MouseEvent &ev) override
    {
        mouse.is_down = true;
        mouse.is_dragging = false;
        mouse.x = ev.position.x;
        mouse.y = ev.position.y;
        mouse.delta_x = 0.0f;
        mouse.delta_y = 0.0f;
        mouse.previous_x = mouse.x;
        mouse.previous_y = mouse.y;
    }
    void mouseMove(const juce::MouseEvent &ev) override
    {
        mouse.x = ev.position.x;
        mouse.y = ev.position.y;
        mouse.delta_x = ev.position.x - ev.mouseDownPosition.x;
        mouse.delta_y = ev.position.y - ev.mouseDownPosition.y;
    }
    void mouseDrag(const juce::MouseEvent &ev) override
    {
        mouse.previous_x = mouse.x;
        mouse.previous_y = mouse.y;
        mouse.x = ev.position.x;
        mouse.y = ev.position.y;
        mouse.delta_x = ev.position.x - ev.mouseDownPosition.x;
        mouse.delta_y = ev.position.y - ev.mouseDownPosition.y;
        mouse.is_dragging = true;
    }
    void mouseUp(const juce::MouseEvent &ev) override
    {
        mouse.is_down = false;
        mouse.is_dragging = false;
        mouse.x = ev.position.x;
        mouse.y = ev.position.y;
    }
};