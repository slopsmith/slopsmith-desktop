#include "NoiseGate.h"

#include <cmath>

void NoiseGate::prepare(double /*sampleRate*/, int /*samplesPerBlock*/)
{
    reset();
}

void NoiseGate::reset()
{
    currentHold = 0;
    gateGain = 1.0f;
}

void NoiseGate::setParameters(bool enabled,
                              float thresholdLinear,
                              int holdSamples,
                              float attack,
                              float release)
{
    paramEnabled.store(enabled, std::memory_order_relaxed);

    const float tl = std::max(0.0f, thresholdLinear);
    paramThresholdLinear.store(tl, std::memory_order_relaxed);

    const int hs = std::max(0, holdSamples);
    paramHoldSamples.store(hs, std::memory_order_relaxed);

    const float att = juce::jlimit(0.0f, 1.0f, attack);
    const float rel = juce::jlimit(0.0f, 1.0f, release);
    paramAttack.store(att, std::memory_order_relaxed);
    paramRelease.store(rel, std::memory_order_relaxed);
}

void NoiseGate::processBlock(juce::AudioBuffer<float>& buffer)
{
    if (!paramEnabled.load(std::memory_order_relaxed))
        return;

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return;

    double sumSq = 0.0;
    const double n = (double)(numChannels * numSamples);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* r = buffer.getReadPointer(ch);
        for (int i = 0; i < numSamples; ++i)
        {
            const double s = (double)r[i];
            sumSq += s * s;
        }
    }

    const float rms = (float)std::sqrt(sumSq / juce::jmax(1.0, n));

    const float thresh = paramThresholdLinear.load(std::memory_order_relaxed);
    const int holdMax = paramHoldSamples.load(std::memory_order_relaxed);
    const float attackCoeff = paramAttack.load(std::memory_order_relaxed);
    const float releaseCoeff = paramRelease.load(std::memory_order_relaxed);

    // Renderer / worklet parity: treat RMS at threshold as open (screen.js: rms >= thresholdLinear).
    if (rms >= thresh)
    {
        currentHold = holdMax;
        gateGain = gateGain + (1.0f - gateGain) * attackCoeff;
    }
    else if (currentHold > 0)
    {
        currentHold -= numSamples;
        if (currentHold < 0)
            currentHold = 0;
        gateGain = gateGain + (1.0f - gateGain) * attackCoeff;
    }
    else
    {
        gateGain = gateGain + (0.0f - gateGain) * releaseCoeff;
    }

    if (gateGain < 0.0001f)
        gateGain = 0.0f;

    buffer.applyGain(gateGain);
}
