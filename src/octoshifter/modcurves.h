#pragma once

#include <cmath>
#include <algorithm>
#include <vector>
#include <string>
#include <functional>
#include <fmt/format.h>

using CurveIdentifier = int;

enum CurveTypes
{
    CURVE_LINEAR = 1,
    CURVE_SQUARE,
    CURVE_CUBE,
    CURVE_STEPS1,
    CURVE_EXPSIN1 = CURVE_STEPS1 + 16,
    CURVE_EXPSIN2,
    CURVE_XOR1,
    CURVE_XOR2,
    CURVE_XOR3,
    CURVE_XOR4,
    CURVE_XOR5,
    CURVE_XOR6,
    CURVE_XOR7,
    CURVE_XOR8,
    CURVE_BITMIRROR,
    CURVE_BIPOLARTOUNIPOLAR,
    CURVE_UNIPOLARTOBIPOLAR,
    CURVE_HARMONICSERIES3OCTAVES,
    CURVE_HARMONICSERIES4OCTAVES,
    CURVE_HARMONICSERIES5OCTAVES,
    CURVE_TOPOWER16,
    CURVE_PEAKING1,
    CURVE_PEAKING2,
    CURVE_PEAKING3,
    CURVE_PEAKING4,
    CURVE_PEAKING5,
    CURVE_PEAKING6,
    CURVE_ABS,
    CURVE_POPCORN,
    CURVE_BUTTERFLY,
    CURVE_SIGMOID1,
    CURVE_MUL_MINUS100,
    CURVE_MUL_MINUS50,
    CURVE_MUL_ZERO,
    CURVE_MUL_50,
    CURVE_MUL_100,
    NUM_CURVES
};
static float peaking_curve(float x, float y)
{
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, 0.1f, 4.0f);
    return -1.0f + 2.0f * (1.0f - std::pow(std::abs(x), y));
}
static float xor_curve(float x, uint16_t a)
{
    x = std::clamp(x, -1.0f, 1.0f);
    x = (x + 1.0f) * 0.5f;
    uint16_t ival = static_cast<uint16_t>(x * 65535.0f);
    ival = ival ^ a;
    x = ival / 65535.0f;
    return -1.0f + 2.0f * x;
}
static uint16_t reverse_bits_16(uint16_t n)
{
    n = ((n >> 1) & 0x5555) | ((n << 1) & 0xAAAA); // Swap adjacent bits
    n = ((n >> 2) & 0x3333) | ((n << 2) & 0xCCCC); // Swap 2-bit groups
    n = ((n >> 4) & 0x0F0F) | ((n << 4) & 0xF0F0); // Swap nibbles
    n = ((n >> 8) & 0x00FF) | ((n << 8) & 0xFF00); // Swap bytes
    return n;
}

static float bit_reversal_curve(float x)
{
    x = std::clamp(x, -1.0f, 1.0f);
    x = (x + 1.0f) * 0.5f;

    uint16_t ival = static_cast<uint16_t>(x * 65535.0f);
    ival = reverse_bits_16(ival); // Mirror the bits

    x = ival / 65535.0f;
    return -1.0f + 2.0f * x;
}
static float expsin(float x, int ampmode, float frequency)
{
    float norm = (x + 1.0f) * 0.5f;
    float amplitude = 1.0f;
    if (ampmode == 1)
        amplitude = norm * norm;
    else if (ampmode == 2)
        amplitude = norm * norm * norm;
    return amplitude * std::sin(2 * M_PI * norm * frequency);
}
static float harmseries(float x, int octaves)
{
    int numpartials = std::pow(2, octaves);
    x = std::clamp(x, -1.0f, 1.0f);
    x = (x + 1.0f) * 0.5f;
    x = 1.0 + (numpartials - 1) * x;
    return std::log2(std::floor(x)) / octaves;
}

std::function<float(float)> getCurveOperator(CurveIdentifier id);

struct CurveMetadata
{
    int id = 0;
    std::string groupname;
    std::string name;
};
static std::vector<CurveMetadata> get_curve_metadata()
{
    std::vector<CurveMetadata> result;
    result.emplace_back(CURVE_LINEAR, "", "-Linear-");
    result.emplace_back(CURVE_ABS, "UTILITY", "Absolute value");
    result.emplace_back(CURVE_BIPOLARTOUNIPOLAR, "UTILITY", "Bipolar to Unipolar");
    result.emplace_back(CURVE_UNIPOLARTOBIPOLAR, "UTILITY", "Unipolar to Bipolar");
    result.emplace_back(CURVE_MUL_MINUS100, "UTILITY", "* -100%");
    result.emplace_back(CURVE_MUL_MINUS50, "UTILITY", "* -50%");
    result.emplace_back(CURVE_MUL_ZERO, "UTILITY", "* 0%");
    result.emplace_back(CURVE_MUL_50, "UTILITY", "* +50%");
    result.emplace_back(CURVE_MUL_100, "UTILITY", "* +100%");
    result.emplace_back(CURVE_SQUARE, "POWER", "x^2");
    result.emplace_back(CURVE_CUBE, "POWER", "x^3");
    result.emplace_back(CURVE_TOPOWER16, "POWER", "x^16");
    result.emplace_back(CURVE_SIGMOID1, "POWER", "Sigmoid");
    for (int i = 0; i < 16; ++i)
    {
        int actnumsteps = i + 2;
        result.emplace_back(CURVE_STEPS1 + i, "STEPS", fmt::format("{:02d} Steps", actnumsteps));
    }
    for (int i = 0; i < 8; ++i)
    {
        result.emplace_back(CURVE_XOR1 + i, "XOR", fmt::format("XOR {}", i + 1));
    }
    for (int i = 0; i < 4; ++i)
    {
        result.emplace_back(CURVE_PEAKING1 + i, "PEAKING", fmt::format("Peaking {}", i + 1));
    }
    result.emplace_back(CURVE_HARMONICSERIES3OCTAVES, "HARMONIC SERIES", "3 Octaves");
    result.emplace_back(CURVE_HARMONICSERIES4OCTAVES, "HARMONIC SERIES", "4 Octaves");
    result.emplace_back(CURVE_HARMONICSERIES5OCTAVES, "HARMONIC SERIES", "5 Octaves");
    result.emplace_back(CURVE_EXPSIN1, "EXP SINE", "Exp Sine 1");
    result.emplace_back(CURVE_EXPSIN2, "EXP SINE", "Exp Sine 2");
    result.emplace_back(CURVE_BITMIRROR, "STRANGE", "Bit Mirror");
    result.emplace_back(CURVE_POPCORN, "STRANGE", "Popcorn");
    result.emplace_back(CURVE_BUTTERFLY, "STRANGE", "Butterfly");
    std::sort(result.begin(), result.end(), [](auto &lhs, auto &rhs) {
        return lhs.groupname + "/" + lhs.name < rhs.groupname + "/" + rhs.name;
    });
    return result;
}