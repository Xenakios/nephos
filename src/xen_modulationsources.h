#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include "../Common/xap_utils.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

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
        D_HYPCOS,
        D_CAUCHY,
        D_ARCSIN
    };
    enum Limiting
    {
        L_CLIP,
        L_FOLD,
        L_WRAP
    };
    std::array<float, 4> parameter_values = {0.0f};
    size_t num_params = 0;
    using PMD = sst::basic_blocks::params::ParamMetaData;
    std::array<PMD, 4> param_metadatas;
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
        if (rand_dist == D_BERNOUILLI)
        {
            num_params = 1;
            parameter_values[0] = 0.5f;
            param_metadatas[0] =
                PMD().withName("Probability").asFloat().withRange(0.0f, 1.0f).withDefault(0.5);
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
    float next()
    {
        float result = 0.0f;
        if (rand_dist == D_BERNOUILLI)
        {
            if (rng.nextFloat() < parameter_values[0])
                result = -1.0f;
            else
                result = 1.0f;
            return result;
        }
        else if (rand_dist == D_HYPCOS)
        {
            result = rng.nextHypCos(parameter_values[0], parameter_values[1]);
        }
        else if (rand_dist == D_CAUCHY)
        {
            result = rng.nextCauchy(parameter_values[0], parameter_values[1]);
        }
        if (limit_mode == L_CLIP)
            result = std::clamp(result,-1.0f,1.0f);
        return result;
    }
};
