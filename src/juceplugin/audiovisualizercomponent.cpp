#include "audiovisualizercomponent.h"
#include "juce_audio_basics/juce_audio_basics.h"

void XenAudioVisualizerComponent::timerCallback()
{
    jassert(sampleRate > 0.0f);
    drawNextLinesOfSpectrogram();
    repaint();
}

void XenAudioVisualizerComponent::drawNextLinesOfSpectrogram()
{
    auto rightHandEdge = spectrogramImage.getWidth() - 1;
    auto imageHeight = spectrogramImage.getHeight();
    const auto binWidth = (float)sampleRate / (float)fftSize; // Hz per FFT bin
    const auto minFreq = binWidth;                            // skip the DC bin
    const auto maxFreq = (float)sampleRate * 0.5f;            // Nyquist

    std::array<float, fftSize> temp;
    double t0 = juce::Time::getMillisecondCounterHiRes();
    if (mainfifo.pop(temp))
    {
        // first, shuffle our image leftwards by 1 pixel..
        spectrogramImage.moveImageSection(0, 0, 1, 0, rightHandEdge, imageHeight);
        juce::Image::BitmapData bitmap{
            spectrogramImage, rightHandEdge, 0, 1, imageHeight, juce::Image::BitmapData::writeOnly};
        std::array<float, 2 * fftSize> localFFTData;
        for (int i = 0; i < fftSize; ++i)
        {
            localFFTData[i] = temp[i];
        }
        forwardFFT.performFrequencyOnlyForwardTransform(localFFTData.data());
        for (auto y = 1; y < imageHeight; ++y)
        {
            // top of image = high frequency, bottom = low frequency
            auto proportionY = 1.0f - (float)y / (float)imageHeight;
            auto freq = minFreq * std::pow(maxFreq / minFreq, proportionY);
            auto fftDataIndex = juce::jlimit(0, fftSize / 2, (int)std::round(freq / binWidth));
            const float fixedMinLevel = -70.0f;
            const float fixedMaxLevel = 70.0f;
            float leveldb = juce::Decibels::gainToDecibels(localFFTData[fftDataIndex]);
            auto level = juce::jlimit(
                0.0f, 1.0f, juce::jmap(leveldb, fixedMinLevel, fixedMaxLevel, 0.0f, 1.0f));
            bitmap.setPixelColour(0, y, magmaColour(level));
        }
    }
    double t1 = juce::Time::getMillisecondCounterHiRes();
    // DBG("popping fifo took " + juce::String(t1 - t0, 1) + " milliseconds");
}
