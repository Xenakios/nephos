#pragma once

#include "juce_gui_basics/juce_gui_basics.h"
#include "juce_dsp/juce_dsp.h"

class XenAudioVisualizerComponent : public juce::Component, public juce::Timer
{
  public:
    enum
    {
        fftOrder = 12,
        fftSize = 1 << fftOrder
    };
    juce::dsp::FFT forwardFFT;
    juce::Image spectrogramImage;

    alignas(16) float fifo[fftSize];
    alignas(16) float fftData[2 * fftSize];
    alignas(16) int fifoIndex = 0;
    alignas(16) std::atomic<bool> nextFFTBlockReady = false;
    float sampleRate = 0.0f;
    XenAudioVisualizerComponent()
        : forwardFFT(fftOrder), spectrogramImage(juce::Image::RGB, 512, 512, true)
    {
        setOpaque(true);
        startTimerHz(25);
    }
    void setSampleRate(float sr) { sampleRate = sr; }
    void timerCallback() override
    {
        jassert(sampleRate > 0.0f);
        if (nextFFTBlockReady)
        {
            drawNextLineOfSpectrogram();
            nextFFTBlockReady = false;
            repaint();
        }
    }
    void drawNextLineOfSpectrogram()
    {
        auto rightHandEdge = spectrogramImage.getWidth() - 1;
        auto imageHeight = spectrogramImage.getHeight();

        // first, shuffle our image leftwards by 1 pixel..
        spectrogramImage.moveImageSection(0, 0, 1, 0, rightHandEdge, imageHeight);

        // then render our FFT data..
        forwardFFT.performFrequencyOnlyForwardTransform(fftData);

        auto maxLevel = juce::FloatVectorOperations::findMinAndMax(fftData, fftSize / 2);

        // --- log-frequency axis parameters ---
        const auto binWidth = (float)sampleRate / (float)fftSize; // Hz per FFT bin
        const auto minFreq = binWidth;                            // skip the DC bin
        const auto maxFreq = (float)sampleRate * 0.5f;            // Nyquist

        juce::Image::BitmapData bitmap{
            spectrogramImage, rightHandEdge, 0, 1, imageHeight, juce::Image::BitmapData::writeOnly};

        for (auto y = 1; y < imageHeight; ++y)
        {
            // top of image = high frequency, bottom = low frequency
            auto proportionY = 1.0f - (float)y / (float)imageHeight;
            auto freq = minFreq * std::pow(maxFreq / minFreq, proportionY);

            auto fftDataIndex = juce::jlimit(0, fftSize / 2, (int)std::round(freq / binWidth));

            auto level = juce::jmap(fftData[fftDataIndex], 0.0f,
                                    juce::jmax(maxLevel.getEnd(), 1e-5f), 0.0f, 1.0f);

            bitmap.setPixelColour(0, y, juce::Colour::fromHSV(level, 1.0f, level, 1.0f));
        }
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(juce::Colours::black);

        g.setOpacity(1.0f);
        g.drawImage(spectrogramImage, getLocalBounds().toFloat());
    }
    void pushNextSampleIntoFifo(float sample) noexcept
    {
        // if the fifo contains enough data, set a flag to say
        // that the next line should now be rendered..
        if (fifoIndex == fftSize)
        {
            if (!nextFFTBlockReady)
            {
                juce::zeromem(fftData, sizeof(fftData));
                memcpy(fftData, fifo, sizeof(fifo));
                nextFFTBlockReady = true;
            }

            fifoIndex = 0;
        }

        fifo[fifoIndex++] = sample;
    }
};