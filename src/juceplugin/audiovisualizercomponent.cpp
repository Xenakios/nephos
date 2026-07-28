#include "audiovisualizercomponent.h"
#include "juce_audio_basics/juce_audio_basics.h"

void XenAudioVisualizerComponent::drawNextLineOfSpectrogram()
{
    auto rightHandEdge = spectrogramImage.getWidth() - 1;
    auto imageHeight = spectrogramImage.getHeight();

    // first, shuffle our image leftwards by 1 pixel..
    spectrogramImage.moveImageSection(0, 0, 1, 0, rightHandEdge, imageHeight);

    // then render our FFT data..
    forwardFFT.performFrequencyOnlyForwardTransform(fftData);

    // auto maxLevel = juce::FloatVectorOperations::findMinAndMax(fftData, fftSize / 2);

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

        // auto level = juce::jmap(fftData[fftDataIndex], 0.0f,
        //                         juce::jmax(maxLevel.getEnd(), 1e-5f), 0.0f, 1.0f);
        const float fixedMinLevel = -30.0f;
        const float fixedMaxLevel = 12.00f;
        float leveldb = juce::Decibels::gainToDecibels(fftData[fftDataIndex]);
        auto level =
            juce::jlimit(0.0f, 1.0f, juce::jmap(leveldb, fixedMinLevel, fixedMaxLevel, 0.0f, 1.0f));
        bitmap.setPixelColour(0, y, juce::Colour::greyLevel(level));
    }
}
