#pragma once

#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "juce_dsp/juce_dsp.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"

class XenAudioVisualizerComponent : public juce::Component, public juce::Timer
{
  public:
    static constexpr auto fftOrder = 10;
    static constexpr auto fftSize = 1 << fftOrder;
    static constexpr auto hopSize = fftSize / 1; // 75% overlap — try /2 for 50% first
    static const int scrollamount = 4;
    choc::fifo::SingleReaderSingleWriterFIFO<std::array<float, fftSize>> mainfifo;
    std::vector<float> ringBuffer{std::vector<float>(fftSize, 0.0f)};
    int ringBufferIndex = 0;
    int samplesSinceLastFFT = 0;

    alignas(16) std::array<float, 2 * fftSize> fftDataSnapshot;
    juce::dsp::FFT forwardFFT;
    juce::dsp::WindowingFunction<float> window;
    juce::Image spectrogramImage;

    float sampleRate = 0.0f;
    XenAudioVisualizerComponent()
        : forwardFFT(fftOrder), window(fftSize, juce::dsp::WindowingFunction<float>::hann),
          spectrogramImage(juce::Image::RGB, 1024, 1024, true)
    {
        mainfifo.reset(32);
        setOpaque(true);
        startTimerHz(60);
    }
    void setSampleRate(float sr) { sampleRate = sr; }
    void timerCallback() override;
    void drawNextLinesOfSpectrogram();
    void paint(juce::Graphics &g) override
    {
        g.fillAll(juce::Colours::black);

        g.setOpacity(1.0f);
        // g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImage(spectrogramImage, getLocalBounds().toFloat());
        g.setColour(juce::Colours::yellow);
        g.drawText(juce::String(mainfifo.getUsedSlots()), 1, 1, 100, 25,
                   juce::Justification::centredLeft);
    }
    static juce::Colour magmaColour(float level) noexcept
    {
        static const std::array<juce::Colour, 5> stops{
            juce::Colour::fromRGB(0, 0, 4),      // near black
            juce::Colour::fromRGB(81, 18, 124),  // purple
            juce::Colour::fromRGB(183, 55, 121), // magenta
            juce::Colour::fromRGB(252, 137, 97), // orange
            juce::Colour::fromRGB(252, 253, 191) // pale yellow
        };

        level = juce::jlimit(0.0f, 1.0f, level);
        auto scaled = level * (float)(stops.size() - 1);
        auto idx = (int)scaled;
        auto frac = scaled - (float)idx;

        if (idx >= (int)stops.size() - 1)
            return stops.back();

        return stops[(size_t)idx].interpolatedWith(stops[(size_t)idx + 1], frac);
    }
    void pushNextSampleIntoFifo(float sample) noexcept
    {
        ringBuffer[(size_t)ringBufferIndex] = sample;
        ringBufferIndex = (ringBufferIndex + 1) % fftSize;
        if (++samplesSinceLastFFT >= hopSize)
        {
            samplesSinceLastFFT = 0;
            std::array<float, fftSize> localData{};
            for (int i = 0; i < fftSize; ++i)
                localData[(size_t)i] = ringBuffer[(size_t)((ringBufferIndex + i) % fftSize)];
            window.multiplyWithWindowingTable(localData.data(), fftSize);
            mainfifo.push(localData);
        }
    }
};