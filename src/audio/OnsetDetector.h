#pragma once

// OnsetDetector — spectral-flux pick-attack detection.
//
// Consumes a contiguous mono input stream and emits an Onset for each detected
// pick attack, tagged with the monotonic input-ring sample index where the
// attack occurred. NoteVerifier uses it to time picked notes precisely: the
// harmonic-comb's 85 ms analysis window is far too coarse to place an onset,
// so timing comes from this short-hop (~5 ms) flux detector instead, and the
// comb is left to do only what it is good at — pitch.
//
// Algorithm: a 1024-sample STFT on a 256-sample hop; spectral flux is the sum
// of positive magnitude changes between consecutive frames over a guitar band.
// A flux frame that is a local maximum above an adaptive threshold (trailing
// mean × k) and far enough from the previous onset is reported as an attack.

#include <juce_dsp/juce_dsp.h>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

class OnsetDetector
{
public:
    struct Onset
    {
        uint64_t sampleIndex = 0;  // monotonic input-ring index of the attack
        float strength = 0.0f;     // spectral-flux peak value (diagnostics)
    };

    OnsetDetector();

    // Configure for a sample rate. Safe to call again on a device restart.
    void prepare(double sampleRate);

    // Drop all history — call on chart change / seek so stale flux state can't
    // fabricate an onset. Does not change the configured sample rate.
    void reset();

    // Feed contiguous samples whose first element sits at monotonic index
    // `firstSampleIndex`. Detected onsets are appended to `out`. A gap in the
    // index sequence (samples lost) resyncs and clears flux history.
    void process(const float* samples, size_t n, uint64_t firstSampleIndex,
                 std::vector<Onset>& out);

private:
    void processFrame(uint64_t frameEndIndex, std::vector<Onset>& out);

    static constexpr int kWindow = 1024;  // STFT window
    static constexpr int kHop    = 256;   // hop — onset time resolution
    static constexpr int kFftOrder = 10;  // 2^10 == kWindow

    double sampleRate = 48000.0;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<juce::dsp::Complex<float>> fftIn, fftOut;
    std::vector<float> hann;       // precomputed window
    std::vector<float> mag;        // current-frame magnitudes
    std::vector<float> prevMag;    // previous-frame magnitudes

    // Sample-by-sample sliding history; a frame is taken every kHop samples.
    std::vector<float> hist;       // kWindow-long ring
    size_t histPos = 0;            // next write slot in `hist`
    int    sinceHop = 0;           // samples since the last frame
    int    primingCount = 0;       // samples seen, capped at kWindow

    // Monotonic index bookkeeping (the caller's input-ring index space).
    uint64_t nextIndex = 0;
    bool indexInit = false;

    // Flux peak-picking — keeps the last three frames' flux (A,B,C) so the
    // middle (B) can be confirmed a local maximum.
    int framesSeen = 0;
    float fluxA = 0.0f, fluxB = 0.0f;
    uint64_t frameEndA = 0, frameEndB = 0;
    std::deque<float> fluxHist;    // trailing flux for the adaptive threshold
    uint64_t lastOnsetIndex = 0;
    bool haveOnset = false;

    int loBin = 1, hiBin = 64;     // flux band, set from the sample rate
    int onsetBackdateSamples = 2400;  // onset calibration in samples, per rate
};
