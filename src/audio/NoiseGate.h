#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

// AmpliTube-style noise gate: threshold (dBFS), user release time, depth floor (dB attenuation
// when closed). Fixed ~1 ms attack. Sample-accurate envelope; stereo-linked detector.

namespace NoiseGateSpecs
{
/** Fast attack kept internal so picking transients pass (typical commercial gate UX). */
inline constexpr float kAttackMs = 1.0f;

inline constexpr float kReleaseMsMin = 5.0f;
inline constexpr float kReleaseMsMax = 2000.0f;

inline constexpr float kThresholdDbMin = -96.0f;
inline constexpr float kThresholdDbMax = 0.0f;

inline constexpr float kDepthDbMin = -100.0f;
inline constexpr float kDepthDbMax = 0.0f;

inline constexpr float kAmpFloor = 1.0e-9f;
}

class NoiseGate
{
public:
    NoiseGate() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();

    // UI / IPC thread — updates atomics only (audio thread reads with relaxed ordering).
    void setParameters(bool enabled, float thresholdDb, float releaseMs, float depthDb);

    void processBlock(juce::AudioBuffer<float>& buffer);

private:
    std::atomic<bool> paramEnabled{false};
    std::atomic<float> paramThresholdDb{-60.0f};
    std::atomic<float> paramReleaseMs{100.0f};
    std::atomic<float> paramDepthDb{-60.0f};

    /** Derived from thresholdDb each setParameters() — amp comparison without log10 per sample. */
    std::atomic<float> paramThresholdLinear{1.0e-3f};
    /** Derived from depthDb — minimum multiplier when gate fully closed (not necessarily silence). */
    std::atomic<float> paramDepthLinear{1.0e-3f};

    float currentGain = 1.0f;

    float coeffAttack = 1.0f;
    double sampleRate = 48000.0;

    static float timeMsToAlpha(double sr, float timeMs);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoiseGate)
};
