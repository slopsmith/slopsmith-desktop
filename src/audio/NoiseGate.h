#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

// Analog-style downward expander / noise gate inspired by ISP Decimator-style tracking:
// sample-accurate envelope in dB, adaptive release (fast after abrupt drops, slow on tails),
// steep expansion below threshold, smoothed gain reduction. Stereo channels share one sidechain.

namespace NoiseGateTuning
{
// Envelope follower — attack toward peaks (lets pick transients through).
inline constexpr float kAttackMs = 0.75f; // between 0.5 ms and 1 ms

// Adaptive release (“time vector”): fast path after abrupt level drops, slow path on sustained decay.
inline constexpr float kReleaseFastMs = 15.0f;  // ~10–20 ms for staccato / palm mutes
inline constexpr float kReleaseSlowMs = 750.0f; // ~500–1000 ms for ringing tails

// If the envelope sits this far above the instantaneous input dB, treat the drop as abrupt → fast release.
inline constexpr float kAbruptDropDb = 8.0f;

// Downward expansion below threshold (ratio applies to dB distance below threshold → gain reduction in dB).
inline constexpr float kExpansionRatio = 14.0f; // ~1:14 region; tweak toward 10–20 as desired

// Extra smoothing on gain-reduction target → applied gain (reduces zipper / grain).
inline constexpr float kGainReductionSmoothMs = 2.5f;

inline constexpr float kDbFloorLinear = 1.0e-9f; // rectifier + log floor (matches SDD 1e-9)
inline constexpr float kMinThresholdLinear = 1.0e-9f;
inline constexpr float kGainReductionDbClamp = -120.0f; // floor before pow (numerical hygiene)
}

class NoiseGate
{
public:
    NoiseGate() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();

    // Called from the UI / IPC thread only — updates atomics (audio-thread safe reads).
    // holdSamples / attack / release are retained for API compatibility; the adaptive DSP uses
    // hard-coded timings from NoiseGateTuning (Decimator-style behaviour).
    void setParameters(bool enabled,
                       float thresholdLinear,
                       int holdSamples,
                       float attack,
                       float release);

    void processBlock(juce::AudioBuffer<float>& buffer);

private:
    std::atomic<bool> paramEnabled{false};
    std::atomic<float> paramThresholdLinear{1.0e-6f};
    std::atomic<int> paramHoldSamples{4800};
    std::atomic<float> paramAttack{0.005f};
    std::atomic<float> paramRelease{0.05f};

    // Audio-thread DSP state
    float envCurrentDb = NoiseGateTuning::kGainReductionDbClamp;
    float gainReductionDb = 0.0f;

    // Per-sample smoothing coefficients: alpha = 1 - exp(-dt/tau), dt = 1/Fs
    float coeffAttack = 1.0f;
    float coeffReleaseFast = 1.0f;
    float coeffReleaseSlow = 1.0f;
    float coeffGainReductionSmooth = 1.0f;

    static float timeMsToAlpha(double sr, float timeMs);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoiseGate)
};
