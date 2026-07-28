#pragma once

#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "juce_dsp/juce_dsp.h"

class XenAudioVisualizerComponent : public juce::Component, public juce::Timer
{
  public:
    enum
    {
        fftOrder = 11,
        fftSize = 1 << fftOrder
    };
    juce::dsp::FFT forwardFFT;
    juce::dsp::WindowingFunction<float> window;
    juce::Image spectrogramImage;

    alignas(16) float fifo[fftSize];
    alignas(16) float fftData[2 * fftSize];
    alignas(16) int fifoIndex = 0;
    alignas(16) std::atomic<bool> nextFFTBlockReady = false;
    float sampleRate = 0.0f;
    XenAudioVisualizerComponent()
        : forwardFFT(fftOrder), window(fftSize, juce::dsp::WindowingFunction<float>::hann),
          spectrogramImage(juce::Image::RGB, 512, 512, true)
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
    void drawNextLineOfSpectrogram();
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
                window.multiplyWithWindowingTable(fftData, fftSize);
                nextFFTBlockReady = true;
            }

            fifoIndex = 0;
        }

        fifo[fifoIndex++] = sample;
    }
};