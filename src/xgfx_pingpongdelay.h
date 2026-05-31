#include "xenfxbase.h"
#include <corecrt_math.h>
#include <vector>
#include <cassert>
#include <algorithm>
#include "../Common/xap_utils.h"

class XPingPongFX : public XenFXBase
{
  public:
    XPingPongFX() {}
    void prepare(double samplerate, size_t maxblocksize) override
    {
        sample_rate = samplerate;
        delayBuf.resize(maxDelayLen * samplerate * 2);
        std::fill(delayBuf.begin(), delayBuf.end(), 0.0f);
        set_parameter(0, 0.1);
    }
    void reset() override { std::fill(delayBuf.begin(), delayBuf.end(), 0.0f); }
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
        if (index == 0)
            return delayLen;
        return 0.0f;
    }
    void set_parameter(size_t paramindex, float value) override
    {
        if (paramindex == 0)
        {
            value = value * value * value;
            delayLen = 0.001 + 0.999 * value;
            int delsamples = delayLen * sample_rate;
            delsamples = std::clamp(delsamples, 1, (int)(maxDelayLen * sample_rate) - 1);
            if (delsamples != delayLenSamples)
            {
                delayLenSamples = delsamples;
                delay_write_pos = (delayLen * sample_rate) - 1;
                delay_read_pos = 0;
            }
        }
        if (paramindex == 1)
        {
            feedback = std::clamp(value, 0.0f, 1.0f);
            feedback = xenakios::mapvalue(value, 0.0f, 1.0f, -0.999f, 0.999f);
        }
        if (paramindex == 2)
        {
            wetmix = std::clamp(value, 0.0f, 1.0f);
        }
    }
    void process(float **inbuffer, float **outbuffer, size_t num_frames) override
    {
        assert(sample_rate > 0.0);
        assert(delayLen > 0.0);
        assert(delayLenSamples > 0);
        for (int i = 0; i < num_frames; ++i)
        {
            float insamples[2] = {inbuffer[0][i], inbuffer[1][i]};
            delayBuf[delay_write_pos * 2 + 0] = insamples[0] + feedback * feedbacks[0];
            delayBuf[delay_write_pos * 2 + 1] = feedback * feedbacks[1];
            float delayouts[2] = {delayBuf[delay_read_pos * 2 + 0],
                                  delayBuf[delay_read_pos * 2 + 1]};
            outbuffer[0][i] = insamples[0] * (1.0 - wetmix) + delayouts[0] * wetmix;
            outbuffer[1][i] = insamples[1] * (1.0 - wetmix) + delayouts[1] * wetmix;
            feedbacks[0] = delayouts[1];
            feedbacks[1] = delayouts[0];
            ++delay_read_pos;
            if (delay_read_pos >= delayLenSamples)
                delay_read_pos = 0;
            ++delay_write_pos;
            if (delay_write_pos >= delayLenSamples)
                delay_write_pos = 0;
        }
    }
    static constexpr float maxDelayLen = 1.0;
    float delayLen = 0.0;
    float feedback = 0.5;
    float wetmix = 1.0;
    int delayLenSamples = 0;
    float sample_rate = 0.0f;
    alignas(16) int delay_read_pos = 0;
    alignas(16) int delay_write_pos = 0;
    alignas(16) float feedbacks[2] = {0.0f, 0.0f};

    std::vector<float> delayBuf;
};
