#include "modcurves.h"
#include "../xen_modulationsources.h"

template <typename T> inline int sgn(T val) { return (T(0) < val) - (val < T(0)); }

std::function<float(float)> getCurveOperator(CurveIdentifier id)
{
    if (id >= CURVE_STEPS1 && id < CURVE_STEPS1 + 16)
    {
        return [id](auto x) {
            const int numsteps = id - CURVE_STEPS1 + 1;
            x = (x + 1.0f) * 0.5;
            x = std::round(x * numsteps) / numsteps;
            return -1.0f + 2.0f * x;
        };
    }
    switch (id)
    {
    case CURVE_CONST_ZERO:
        return [](auto x) { return 0.0f; };
    case CURVE_CONST_MINUS100:
        return [](auto x) { return -1.0f; };
    case CURVE_CONST_MINUS50:
        return [](auto x) { return -0.5f; };
    case CURVE_CONST_50:
        return [](auto x) { return 0.5f; };
    case CURVE_CONST_100:
        return [](auto x) { return 1.0f; };
    case CURVE_LINEAR:
        return [](auto x) { return x; };
    case CURVE_SQUARE:
        return [](auto x) { return std::abs(x) * x; };
    case CURVE_CUBE:
        return [](auto x) { return x * x * x; };
    case CURVE_ABS:
        return [](auto x) { return std::abs(x); };
    case CURVE_TOPOWER16:
        return [](auto x) {
            x = std::clamp(x, -1.0f, 1.0f);
            return std::pow(x, 16) * sgn(x);
        };
    case CURVE_EXPSIN1:
        return [](auto x) { return expsin(x, 1, 8.0f); };
    case CURVE_EXPSIN2:
        return [](auto x) { return expsin(x, 2, 12.0f); };
    case CURVE_XOR1:
        return [](auto x) { return xor_curve(x, 13107); };
    case CURVE_XOR2:
        return [](auto x) { return xor_curve(x, 43690); };
    case CURVE_XOR3:
        return [](auto x) { return xor_curve(x, 25027); };
    case CURVE_XOR4:
        return [](auto x) { return xor_curve(x, 10001); };
    case CURVE_XOR5:
        return [](auto x) { return xor_curve(x, 0x0F0F); };
    case CURVE_XOR6:
        return [](auto x) { return xor_curve(x, 0x5555); };
    case CURVE_XOR7:
        return [](auto x) { return xor_curve(x, 0x203F); };
    case CURVE_XOR8:
        return [](auto x) { return xor_curve(x, 0xef00); };
    case CURVE_BITMIRROR:
        return [](auto x) { return bit_reversal_curve(x); };
    case CURVE_UNIPOLARTOBIPOLAR:
        return [](auto x) { return std::clamp(-1.0f + 2.0f * x, -1.0f, 1.0f); };
    case CURVE_BIPOLARTOUNIPOLAR:
        return [](auto x) { return std::clamp((x + 1.0f) * 0.5f, 0.0f, 1.0f); };
    case CURVE_HARMONICSERIES3OCTAVES:
        return [](auto x) { return harmseries(x, 3); };
    case CURVE_HARMONICSERIES4OCTAVES:
        return [](auto x) { return harmseries(x, 4); };
    case CURVE_HARMONICSERIES5OCTAVES:
        return [](auto x) { return harmseries(x, 5); };
    case CURVE_PEAKING1:
        return [](auto x) { return peaking_curve(x, 0.2f); };
    case CURVE_PEAKING2:
        return [](auto x) { return peaking_curve(x, 0.5f); };
    case CURVE_PEAKING3:
        return [](auto x) { return peaking_curve(x, 1.0f); };
    case CURVE_PEAKING4:
        return [](auto x) { return peaking_curve(x, 2.0f); };
    case CURVE_PEAKING5:
        return [](auto x) { return peaking_curve(x, 3.0f); };
    case CURVE_PEAKING6:
        return [](auto x) { return peaking_curve(x, 4.0f); };
    case CURVE_POPCORN:
        return [](auto x) { return std::floor(std::tanh(x * 5.0) * 10.0) / 10.0; };
    case CURVE_SIGMOID1:
        return [](auto x) { return smoothstep(-1.0f, 1.0f, (x + 1.0f) * 0.5f); };
    case CURVE_BUTTERFLY:
        return [](auto x) {
            if (x != 0.0f)
                return std::sin(x * 10.0f) * std::cos(1.0f / x + 0.001f);
            return 0.0f;
        };
    };

    return [](auto x) { return x; };
}
