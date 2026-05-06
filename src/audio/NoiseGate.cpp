#include "NoiseGate.h"

#include <cmath>

float NoiseGate::timeMsToAlpha(double sr, float timeMs)
{
    using namespace NoiseGateSpecs;
    if (sr <= 0.0 || timeMs <= 0.0f)
        return 1.0f;

    const float tauSec = timeMs * 0.001f;
    return 1.0f - std::exp(-1.0f / (tauSec * (float)sr));
}

void NoiseGate::prepare(double sr, int /*samplesPerBlock*/)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    coeffAttack = timeMsToAlpha(sampleRate, NoiseGateSpecs::kAttackMs);
    reset();
}

void NoiseGate::reset()
{
    currentGain = 1.0f;
}

void NoiseGate::setParameters(bool enabled, float thresholdDb, float releaseMs, float depthDb)
{
    using namespace NoiseGateSpecs;

    // Publish derived params first with relaxed ordering, then release-store
    // paramEnabled. The audio thread acquire-loads paramEnabled in processBlock,
    // so when it sees enabled=true the threshold/release/depth values stored
    // here are guaranteed visible. (Updates while already enabled can still
    // briefly mix old/new params across two fields — acceptable for slow-moving
    // UI changes; a SeqLock or versioned struct would be needed to fully avoid.)
    const float tDb = juce::jlimit(kThresholdDbMin, kThresholdDbMax, thresholdDb);
    paramThresholdDb.store(tDb, std::memory_order_relaxed);
    const float tLin = std::pow(10.0f, tDb / 20.0f);
    paramThresholdLinear.store(juce::jmax(kAmpFloor, tLin), std::memory_order_relaxed);

    const float rMs = juce::jlimit(kReleaseMsMin, kReleaseMsMax, releaseMs);
    paramReleaseMs.store(rMs, std::memory_order_relaxed);

    const float dDb = juce::jlimit(kDepthDbMin, kDepthDbMax, depthDb);
    paramDepthDb.store(dDb, std::memory_order_relaxed);
    const float dLin = std::pow(10.0f, dDb / 20.0f);
    paramDepthLinear.store(juce::jlimit(kAmpFloor, 1.0f, dLin), std::memory_order_relaxed);

    paramEnabled.store(enabled, std::memory_order_release);
}

void NoiseGate::processBlock(juce::AudioBuffer<float>& buffer)
{
    // Acquire-load pairs with the release-store in setParameters so that when
    // we see enabled=true the threshold/release/depth values are fully visible.
    if (!paramEnabled.load(std::memory_order_acquire))
    {
        // Park currentGain at unity while disabled so re-enabling the gate
        // doesn't inherit a previously-closed envelope (which would otherwise
        // produce an unintended fade-in until the detector opens). Audio-thread
        // only writes this; UI thread never touches it.
        currentGain = 1.0f;
        return;
    }

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return;

    const float thresh = paramThresholdLinear.load(std::memory_order_relaxed);
    const float depthLin = paramDepthLinear.load(std::memory_order_relaxed);
    const float releaseMs = paramReleaseMs.load(std::memory_order_relaxed);

    const float coeffRelease = timeMsToAlpha(sampleRate, releaseMs);

    // juce::AudioBuffer maintains its own per-channel pointer table internally;
    // reuse it (no allocation, no arbitrary cap) instead of building our own.
    float* const* channelData = buffer.getArrayOfWritePointers();

    for (int i = 0; i < numSamples; ++i)
    {
        float det = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            det = juce::jmax(det, std::abs(channelData[ch][i]));

        if (det > thresh)
            currentGain += coeffAttack * (1.0f - currentGain);
        else
            currentGain += coeffRelease * (depthLin - currentGain);

        const float g = currentGain;
        for (int ch = 0; ch < numChannels; ++ch)
            channelData[ch][i] *= g;
    }
}
