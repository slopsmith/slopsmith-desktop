#pragma once

#include <array>
#include <atomic>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

// Tone Polish — fixed 3-band mastering EQ on the live guitar bus:
//   • High-pass        @  80 Hz
//   • Low shelf  −3 dB @ 180 Hz
//   • Peak/bell  −0.5 dB @ 200 Hz, Q = 1
//
// Three cascaded IIR biquads per channel. Zero added latency. Insertion
// point in AudioEngine: between chainOutputGain and the backing-track
// mix, so this only colours the guitar — the backing track and master
// gain stage stay bit-untouched. Always enabled by default; renderer
// exposes a per-preset toggle that can disable it.

namespace TonePolishSpecs
{
inline constexpr float kHighPassHz   = 80.0f;
inline constexpr float kLowShelfHz   = 180.0f;
inline constexpr float kLowShelfDb   = -3.0f;
inline constexpr float kPeakHz       = 200.0f;
inline constexpr float kPeakQ        = 1.0f;
inline constexpr float kPeakDb       = -0.5f;
}

class TonePolish
{
public:
    TonePolish() = default;

    // Maximum channel count we pre-allocate filter state for. Any realistic
    // audio device on Windows/macOS/Linux is well under this; we cap rather
    // than grow on the audio thread so processBlock() never allocates.
    static constexpr int kMaxChannels = 8;

    void prepare(double sampleRate);
    void reset();

    // UI / IPC thread — flips the atomic only. Audio thread reads on each
    // block. When disabled, processBlock() is a single load + early-return.
    void setEnabled(bool enabled);
    bool isEnabled() const { return paramEnabled.load(std::memory_order_seq_cst); }

    void processBlock(juce::AudioBuffer<float>& buffer);

private:
    void updateCoefficients();

    std::atomic<bool> paramEnabled{true};
    // Set to true at the end of prepare() and cleared in the constructor so
    // processBlock() is a no-op until DSP state is fully initialised.
    std::atomic<bool> paramPrepared{false};
    // Set by setEnabled(true) on the UI thread so processBlock() clears stale
    // IIR delay-line state before the first enabled block, preventing clicks
    // at the bypass re-enable boundary.
    std::atomic<bool> paramNeedsReset{false};

    double sampleRate = 48000.0;

    // Per channel: HPF, low-shelf, peak. Filters are stateful (one Direct
    // Form II Transposed delay line per instance) so each channel needs
    // its own trio. Coefficients are shared across channels via the JUCE
    // reference-counted Coefficients<float> object so updateCoefficients()
    // touches one allocation, not numChannels.
    std::vector<std::array<juce::dsp::IIR::Filter<float>, 3>> filters;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TonePolish)
};
