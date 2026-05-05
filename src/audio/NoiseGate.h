#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

// Noise gate aligned with the desktop renderer / NAM worklet semantics:
// RMS over the full buffer, threshold compare against pre-linearised amplitude,
// hold counted in samples (consumed per callback via numSamples), attack/release
// coefficients applied once per audio block (worklet "frame").
class NoiseGate
{
public:
    NoiseGate() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();

    // Called from the UI / IPC thread only — updates atomics (audio-thread safe reads).
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

    // Audio thread state only
    int currentHold = 0;
    float gateGain = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoiseGate)
};
