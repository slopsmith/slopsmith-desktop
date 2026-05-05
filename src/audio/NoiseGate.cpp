#include "NoiseGate.h"

#include <cmath>

float NoiseGate::timeMsToAlpha(double sr, float timeMs)
{
    using namespace NoiseGateTuning;
    if (sr <= 0.0 || timeMs <= 0.0f)
        return 1.0f;

    const float tauSec = timeMs * 0.001f;
    // First-order step toward target: y += α·(x−y); τ ≈ −Δt/ln(1−α) with Δt = 1/Fs
    return 1.0f - std::exp(-1.0f / (tauSec * (float)sr));
}

void NoiseGate::prepare(double sr, int /*samplesPerBlock*/)
{
    const double srUse = sr > 0.0 ? sr : 48000.0;

    using namespace NoiseGateTuning;
    coeffAttack = timeMsToAlpha(srUse, kAttackMs);
    coeffReleaseFast = timeMsToAlpha(srUse, kReleaseFastMs);
    coeffReleaseSlow = timeMsToAlpha(srUse, kReleaseSlowMs);
    coeffGainReductionSmooth = timeMsToAlpha(srUse, kGainReductionSmoothMs);

    reset();
}

void NoiseGate::reset()
{
    envCurrentDb = NoiseGateTuning::kGainReductionDbClamp;
    gainReductionDb = 0.0f;
}

void NoiseGate::setParameters(bool enabled,
                              float thresholdLinear,
                              int holdSamples,
                              float attack,
                              float release)
{
    paramEnabled.store(enabled, std::memory_order_relaxed);

    const float tl = std::max(NoiseGateTuning::kMinThresholdLinear, thresholdLinear);
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

    const float threshLin = paramThresholdLinear.load(std::memory_order_relaxed);
    const float thresholdDb = 20.0f * std::log10(juce::jmax(threshLin, NoiseGateTuning::kMinThresholdLinear));

    using namespace NoiseGateTuning;

    for (int i = 0; i < numSamples; ++i)
    {
        // Linked sidechain: peak magnitude across channels (Decimator-style stereo linking).
        float det = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            det = juce::jmax(det, std::abs(buffer.getSample(ch, i)));

        const float inputDb = 20.0f * std::log10(det + kDbFloorLinear);

        // ── Adaptive envelope follower (dB domain) ───────────────────────────
        if (inputDb > envCurrentDb)
        {
            envCurrentDb += coeffAttack * (inputDb - envCurrentDb);
        }
        else
        {
            const float deltaDb = envCurrentDb - inputDb; // > 0 in release
            const float releaseCoeff = (deltaDb > kAbruptDropDb) ? coeffReleaseFast : coeffReleaseSlow;
            envCurrentDb += releaseCoeff * (inputDb - envCurrentDb);
        }

        // ── Downward expansion → target gain reduction (dB) ─────────────────
        float targetGainDb = 0.0f;
        if (envCurrentDb < thresholdDb)
            targetGainDb = (envCurrentDb - thresholdDb) * kExpansionRatio;

        targetGainDb = juce::jmax(targetGainDb, kGainReductionDbClamp);

        gainReductionDb += coeffGainReductionSmooth * (targetGainDb - gainReductionDb);

        // ── Linear application ─────────────────────────────────────────────
        float linGain = std::pow(10.0f, gainReductionDb / 20.0f);
        if (linGain < 1.0e-6f)
            linGain = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.setSample(ch, i, buffer.getSample(ch, i) * linGain);
    }
}
