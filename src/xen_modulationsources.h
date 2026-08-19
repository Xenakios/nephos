#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include "../Common/xap_utils.h"
#include "containers/choc_Value.h"
#include "sst/basic-blocks/dsp/Interpolators.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

inline float smoothstep(float y0, float y1, float mu)
{
    float t = mu * mu * (3.0f - 2.0f * mu);
    return y0 + (y1 - y0) * t;
}

struct SimpleEnvelope
{
    static constexpr int maxnumsteps = 16;

  private:
    alignas(32) std::array<float, maxnumsteps + 5> steps;

  public:
    auto get_all_steps() const { return steps; }
    float get_step(size_t index) const
    {
        assert(index < maxnumsteps);
        return steps[index];
    }
    void set_step(size_t index, float value)
    {
        assert(index < maxnumsteps);
        steps[index] = value;
        if (index == maxnumsteps - 1)
        {
            for (size_t i = 0; i < 5; ++i)
                steps[maxnumsteps + i] = value;
        }
    }
    enum InterpolationMode
    {
        IM_NONE,
        IM_LINEAR,
        IM_SIGMOID,
        IM_SPLINE,
        IM_CUBIC
    };
    int interpmode = IM_SPLINE;
    choc::value::Value getState()
    {
        auto result = choc::value::createObject("stepenvstate");
        result.setMember("interpmode", interpmode);
        auto auxenvsteps = choc::value::createEmptyArray();
        for (auto &v : steps)
        {
            auxenvsteps.addArrayElement(v);
        }
        result.setMember("steps", auxenvsteps);
        return result;
    }
    // should implement this
    void setState(choc::value::ValueView state) {}
    SimpleEnvelope() { std::fill(steps.begin(), steps.end(), 0.0f); }
    float get_value(float xpos, float xwarp) const
    {
        xpos = std::clamp(xpos, 0.0f, 1.0f);
        if (xwarp < 0.0f)
        {
            float ex = xenakios::mapvalue(xwarp, -1.0f, 0.0f, 4.0f, 1.0f);
            xpos = std::pow(xpos, ex);
        }
        else
        {
            float ex = xenakios::mapvalue(xwarp, 0.0f, 1.0f, 1.0f, 4.0f);
            xpos = 1.0f - std::pow(1.0f - xpos, ex);
        }
        xpos *= maxnumsteps;
        int index = xpos;
        float y0 = steps[index];
        if (interpmode == IM_NONE)
            return y0;
        float y1 = steps[index + 1];
        float mu = xpos - index;
        if (interpmode == IM_LINEAR)
            return y0 + (y1 - y0) * mu;
        if (interpmode == IM_SIGMOID)
            return smoothstep(y0, y1, mu);
        float y2 = steps[index + 2];
        if (interpmode == IM_SPLINE)
            return sst::basic_blocks::dsp::quad_bspline(y0, y1, y2, mu);
        float y3 = index < 1 ? steps[0] : steps[index - 1];
        return sst::basic_blocks::dsp::cubic_ipol(y3, y0, y1, y2, mu);
    }
};

class MidiNoteModSource
{
  public:
    struct Message
    {
        uint8_t note = 60;
        uint8_t velo = 127;
        uint8_t aftertouch = 0;
    };
    std::vector<Message> current_messages;
    int curstep = 0;
    bool sustain = false;
    MidiNoteModSource() { current_messages.reserve(128); }
    std::string getDebugString()
    {
        if (current_messages.empty())
            return "NO MESSAGES";
        std::string result;
        // for (auto &e : current_messages)
        //     result += std::format("{} {} {} ", e.note, e.velo, e.aftertouch);
        return result;
    }
    void set_sustain(bool newsustain)
    {
        if (!newsustain)
        {
            current_messages.clear();
            curstep = 0;
        }
        sustain = newsustain;
    }
    void activate_note(uint8_t note, uint8_t velo)
    {
        if (current_messages.size() < current_messages.capacity())
        {
            current_messages.emplace_back(note, velo);
        }
    }
    void deactivate_note(uint8_t note)
    {
        if (sustain)
            return;
        std::erase_if(current_messages, [note](Message &msg) { return msg.note == note; });
        if (curstep >= current_messages.size())
            curstep = current_messages.size() - 1;
        if (curstep < 0)
            curstep = 0;
    }
    float next()
    {
        if (current_messages.size() == 0)
            return 0.0f;
        float result = current_messages[curstep].note;
        result = xenakios::mapvalue(result, 0.0f, 127.0f, -1.0f, 1.0f);
        ++curstep;
        if (curstep == current_messages.size())
            curstep = 0;
        return result;
    }
};

class StepModSource
{
  public:
    enum PlayMode
    {
        PM_FORWARDLOOP,
        PM_REVERSELOOP,
        PM_FWREVLOOP,
        PM_RANDOM,
        PM_SHUFFLERANDOM,
        PM_RANDOMWALK1,
        NUMPLAYMODES
    };
    static constexpr size_t maxSteps = 4096;
    int curstep = 0;
    int looppos = 0;
    int laststep = 0;
    int playdirection = 1;
    PlayMode playmode = PM_FORWARDLOOP;
    std::atomic<int> curstepforgui;
    int numactivesteps = 0;
    int loopstartstep = 0;
    int looplen = 1;
    int loopoffset = 0;
    std::atomic<bool> unipolar{false};
    std::vector<float> steps;
    xenakios::Xoroshiro128Plus rng;
    void reset()
    {
        curstep = 0;
        looppos = 0;
        laststep = 0;
        playdirection = 1;
    }
    struct Message
    {
        enum Opcode
        {
            OP_NOOP,
            OP_NUMSTEPS,
            OP_LOOPSTART,
            OP_LOOPLEN,
            OP_SETSTEP,
            OP_UNIPOLAR,
            OP_OFFSET,
            OP_PLAYMODE
        };
        Opcode opcode = OP_NOOP;
        uint32_t dest = 0;
        float fval0 = 0.0f;
        int ival0 = 0;
    };
    static std::string getPlayModeName(int m)
    {
        if (m == PM_FORWARDLOOP)
            return "Forward";
        if (m == PM_REVERSELOOP)
            return "Reverse";
        if (m == PM_FWREVLOOP)
            return "Forward/Reverse";
        if (m == PM_RANDOM)
            return "Random";
        if (m == PM_SHUFFLERANDOM)
            return "Random no repeat";
        if (m == PM_RANDOMWALK1)
            return "Random walk type 1";
        return "Unknown";
    }
    StepModSource()
    {
        rng.seed(98765, 334466);
        steps.resize(maxSteps);
    }

    float next()
    {
        if (steps.empty())
            return 0.0f;
        float result = 0.0f;
        if (playmode == PM_FORWARDLOOP || playmode == PM_REVERSELOOP || playmode == PM_FWREVLOOP)
        {
            result = steps[curstep];
            curstepforgui = curstep;
            looppos = looppos + playdirection;
            if (playmode == PM_FORWARDLOOP)
            {
                if (looppos >= looplen)
                    looppos = 0;
            }
            else if (playmode == PM_REVERSELOOP)
            {
                if (looppos < 0)
                    looppos = looplen - 1;
            }
            else if (playmode == PM_FWREVLOOP)
            {
                if (looppos >= looplen)
                {
                    playdirection = -1;
                    looppos = looplen - 2;
                }
                if (looppos < 0)
                {
                    playdirection = 1;
                    looppos = 1;
                }
            }

            looppos = std::clamp(looppos, 0, numactivesteps - 1);
            curstep = loopstartstep + ((looppos + loopoffset) % looplen);
        }
        else if (playmode == PM_RANDOM)
        {
            int index = rng.nextInt32InRange(loopstartstep, loopstartstep + looplen);
            result = steps[index];
            curstepforgui = index;
        }
        else if (playmode == PM_RANDOMWALK1)
        {
            int newstep = laststep;
            if (rng.nextFloat() < 0.5)
                newstep -= 1;
            else
                newstep += 1;
            if (newstep < 0)
                newstep = looplen - 1;
            if (newstep >= looplen)
                newstep = 0;
            result = steps[loopstartstep + newstep];
            laststep = newstep;
            curstepforgui = loopstartstep + newstep;
        }
        else if (playmode == PM_SHUFFLERANDOM)
        {
            int sanity = 0;
            int foundindex = 0;
            while (true)
            {
                foundindex = rng.nextInt32InRange(loopstartstep, loopstartstep + looplen);
                if (foundindex != laststep)
                {
                    result = steps[foundindex];
                    break;
                }
                ++sanity;
                if (sanity == 16)
                    break;
            }
            curstepforgui = foundindex;
            laststep = foundindex;
        }

        if (unipolar.load())
            result = (result + 1.0f) * 0.5f;
        return result;
    }
};

struct TriggeredRandomSource
{
    xenakios::Xoroshiro128Plus rng;
    enum Distribution
    {
        D_NONE,
        D_BERNOUILLI,
        D_UNIFORM,
        D_HYPCOS,
        D_CAUCHY,
        D_ARCSIN,
        D_DISCRETE8,
        D_END
    };
    enum Limiting
    {
        L_CLIP,
        L_FOLD,
        L_WRAP
    };
    struct DistributionInfo
    {
        Distribution d;
        std::string name;
    };
    static std::vector<DistributionInfo> get_distributions()
    {
        std::vector<DistributionInfo> result;
        result.emplace_back(D_BERNOUILLI, "BERNOUILLI");
        result.emplace_back(D_UNIFORM, "UNIFORM");
        result.emplace_back(D_HYPCOS, "HYPCOS");
        result.emplace_back(D_CAUCHY, "CAUCHY");
        result.emplace_back(D_DISCRETE8, "8 DISCRETE STEPS");
        return result;
    }
    std::array<float, 8> parameter_values = {0.0f};
    size_t num_params = 0;
    using PMD = sst::basic_blocks::params::ParamMetaData;
    std::array<PMD, 8> param_metadatas;
    Distribution rand_dist = D_NONE;
    Limiting limit_mode = L_CLIP;
    TriggeredRandomSource(uint64_t seed) : rng(seed, 12345)
    {
        for (auto &pmd : param_metadatas)
        {
            pmd = PMD().withName("NO PARAMETER");
        }
        set_distribution(D_BERNOUILLI);
    }
    void set_distribution(Distribution d)
    {
        if (rand_dist == d)
            return;
        rand_dist = d;
        if (rand_dist == D_UNIFORM)
        {
            num_params = 0;
        }
        if (rand_dist == D_DISCRETE8)
        {
            num_params = 8;
            for (size_t i = 0; i < 8; ++i)
            {
                parameter_values[i] = 0.5f;
                param_metadatas[i] = PMD()
                                         .withName(fmt::format("P {}", i + 1))
                                         .asFloat()
                                         .withRange(0.0f, 1.0f)
                                         .withDefault(0.5)
                                         .withLinearScaleFormatting("");
            }
        }
        if (rand_dist == D_BERNOUILLI)
        {
            num_params = 1;
            parameter_values[0] = 0.5f;
            param_metadatas[0] = PMD()
                                     .withName("Probability")
                                     .asFloat()
                                     .withRange(0.0f, 1.0f)
                                     .withDefault(0.5)
                                     .withLinearScaleFormatting("");
        }
        if (rand_dist == D_HYPCOS || rand_dist == D_CAUCHY)
        {
            num_params = 2;
            parameter_values[0] = 0.0f;
            parameter_values[1] = 0.1f;
            param_metadatas[0] = PMD()
                                     .withName("Center")
                                     .asFloat()
                                     .withRange(-1.0f, 1.0f)
                                     .withDefault(0.0)
                                     .withLinearScaleFormatting("");
            param_metadatas[1] = PMD()
                                     .withName("Spread")
                                     .asFloat()
                                     .withRange(0.0f, 1.0f)
                                     .withDefault(0.1)
                                     .withLinearScaleFormatting("");
        }
    }
    choc::value::Value get_state()
    {
        auto result = choc::value::createObject("trngstate");
        result.setMember("distribution", (int64_t)rand_dist);
        result.setMember("paramvalues", choc::value::createArray(parameter_values));
        return result;
    }
    void set_state(choc::value::ValueView state)
    {
        if (state.hasObjectMember("distribution"))
            set_distribution((Distribution)state["distribution"].getWithDefault((int)D_BERNOUILLI));
        if (state.hasObjectMember("paramvalues"))
        {
            auto arr = state["paramvalues"];
            for (int i = 0; i < arr.size(); ++i)
            {
                if (i < parameter_values.size())
                    parameter_values[i] = arr[i].getWithDefault(param_metadatas[i].defaultVal);
            }
        }
    }
    float next()
    {
        float result = 0.0f;
        if (rand_dist == D_BERNOUILLI)
        {
            if (rng.nextFloat() < parameter_values[0])
                result = 1.0f;
            else
                result = -1.0f;
            return result;
        }
        else if (rand_dist == D_UNIFORM)
        {
            return -1.0f + 2.0f * rng.nextFloat();
        }
        else if (rand_dist == D_HYPCOS)
        {
            result = rng.nextHypCos(parameter_values[0], parameter_values[1]);
        }
        else if (rand_dist == D_CAUCHY)
        {
            float norm = std::clamp(parameter_values[1], 0.0f, 1.0f);
            norm = norm * norm;
            result = rng.nextCauchy(parameter_values[0], norm);
        }
        else if (rand_dist == D_DISCRETE8)
        {
            float sum = 0.0f;
            for (auto &e : parameter_values)
                sum += e;
            if (sum > 0.0f)
            {
                float z = rng.nextFloat(); // Assumes range is [0.0, 1.0)
                float cumulative = 0.0f;

                for (int i = 0; i < 8; ++i)
                {
                    cumulative +=
                        parameter_values[i] / sum; // Add normalized weight to running total
                    if (z < cumulative)
                    {
                        result = -1.0f + (2.0f / 7.0f) * i; // Fixed floating-point division
                        break;
                    }
                }
            }
        }
        if (limit_mode == L_CLIP)
            result = std::clamp(result, -1.0f, 1.0f);
        return result;
    }
};
