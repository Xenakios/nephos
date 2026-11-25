#include "xenfxbase.h"
#include <vector>
#include <cassert>
#include <algorithm>
#include "../Common/xap_utils.h"

class XPingPongFX : public XenFXBase
{
  public:
    XPingPongFX() {}
    void clear_delay_line() { std::fill(delayBuf.begin(), delayBuf.end(), 0.0f); }
    void prepare(double samplerate, size_t maxblocksize) override
    {
        sample_rate = samplerate;
        delayBuf.resize(maxDelayLen * samplerate * 2);
        clear_delay_line();
    }
    void reset() override { clear_delay_line(); }
    size_t num_params() override { return 3; }
    std::string get_param_name(size_t paramindex) override
    {
        if (paramindex == 0)
            return "Delay";
        if (paramindex == 1)
            return "Feedback";
        if (paramindex == 2)
            return "Mix";
        return "BUG";
    }
    float get_parameter(size_t index) override
    {
        if (index >= 0 && index < 3)
        {
            return normalizedParamValues[index];
        }
        return 0.0f;
    }
    float m_delay_secs = 0.1f;
    void set_parameter(size_t paramindex, float value) override
    {
        value = std::clamp(value, 0.0f, 1.0f);
        if (paramindex == 0)
        {
            normalizedParamValues[0] = value;
            float delaylen = value * value * value;
            delaylen = 0.001 + 0.999 * delaylen;
            m_delay_secs = delaylen;
        }
        if (paramindex == 1)
        {
            normalizedParamValues[1] = value;
            feedback = value * value;
            feedback = std::clamp(feedback, 0.0f, 0.999f);
        }
        if (paramindex == 2)
        {
            normalizedParamValues[2] = value;
            const float pidiv = M_PI / 2;
            wetmixcoeffs[0] = std::cos(pidiv * value);
            wetmixcoeffs[1] = std::sin(pidiv * value);
        }
    }
    void read_delay(float delaysecs, float *out)
    {
        const int maxdelaypos = maxDelayLen * sample_rate;
        int delaysamples = delaysecs * sample_rate;
        if (delaysamples < 2)
            delaysamples = 2;
        int read_pos = delay_write_pos - delaysamples;
        if (read_pos < 0)
            read_pos += maxdelaypos;
        assert(read_pos >= 0 && read_pos < maxdelaypos);
        out[0] = delayBuf[read_pos * 2 + 0];
        out[1] = delayBuf[read_pos * 2 + 1];
    }
    void process(float **inbuffer, float **outbuffer, size_t num_frames) override
    {
        assert(sample_rate > 0.0);
        for (int i = 0; i < num_frames; ++i)
        {
            float insamples[2] = {inbuffer[0][i], inbuffer[1][i]};
            delayBuf[delay_write_pos * 2 + 0] = insamples[0] + feedback * feedbacks[0];
            delayBuf[delay_write_pos * 2 + 1] = feedback * feedbacks[1];
            float delayouts[2] = {0.0f, 0.0f};
            read_delay(m_delay_secs, delayouts);
            outbuffer[0][i] = insamples[0] * wetmixcoeffs[0] + delayouts[0] * wetmixcoeffs[1];
            outbuffer[1][i] = insamples[1] * wetmixcoeffs[0] + delayouts[1] * wetmixcoeffs[1];
            feedbacks[0] = delayouts[1];
            feedbacks[1] = delayouts[0];
            ++delay_write_pos;
            if (delay_write_pos >= maxDelayLen * sample_rate)
                delay_write_pos = 0;
        }
    }
    static constexpr float maxDelayLen = 1.0;
    float normalizedParamValues[3] = {0.2f, 0.5f, 0.5f};
    float feedback = 0.5;
    float wetmixcoeffs[2] = {0.0f, 0.0f};
    float sample_rate = 0.0f;

    alignas(16) int delay_write_pos = 0;
    alignas(16) float feedbacks[2] = {0.0f, 0.0f};
    std::vector<float> delayBuf;
};
