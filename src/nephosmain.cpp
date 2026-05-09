#include "granularsynth.h"
#include <print>

int main()
{
    auto g = std::make_unique<ToneGranulator>();
    g->prepare(44100, {}, 0, 0.02, 0.02);
    return 0;
}
