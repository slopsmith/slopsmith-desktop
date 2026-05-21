#pragma once

// ChordScorer — native port of the notedetect plugin's polyphonic
// chord-scoring math (slopsmith-plugin-notedetect/screen.js). Constitution
// II requires audio analysis to live in JUCE, not renderer JS; the
// renderer calls in over IPC (`audio:scoreChord`) and consumes the
// returned result object.
//
// The math is a direct C++ translation of the JS originals
// (`_ndFftMagnitude`, `_ndStringBandHz`, `_ndBandEnergy`, `_ndTotalEnergy`,
// `_ndConstraintCheckString`, `_ndScoreChord`). The custom radix-2
// Cooley-Tukey in JS is replaced with `juce::dsp::FFT`; the rest of the
// helpers are line-for-line translations. Behavioural parity with the
// browser path is the target — bin-for-bin floating-point identity is
// not, since the JS FFT and `juce::dsp::FFT` evaluate the butterflies
// in different orders and may also use vectorised intrinsics natively.

#include <juce_dsp/juce_dsp.h>
#include <memory>
#include <string>
#include <vector>

class ChordScorer
{
public:
    // Standard-tuning MIDI base for the supported (arrangement, stringCount)
    // pairs. Lifted from screen.js `_ND_TUNING_*` constants verbatim so the
    // open-string MIDI values match between the native and JS paths.
    // Returns `nullptr` for unsupported pairs (e.g. "guitar" + 5-string,
    // or any unknown arrangement string) — caller is expected to fail
    // the request rather than guess a fallback tuning.
    static const std::vector<int>* standardMidiFor(const std::string& arrangement, int stringCount);

    // Hard upper bound on the FFT size we will ever build. The 3 Hz
    // bin-width floor in scoreChord() implies fftSize ≈ nextPow2(SR/3),
    // which is 16384 at 48 kHz, 32768 at 96 kHz, 65536 at 192 kHz —
    // already the largest realistic audio-interface rate. Bounding this
    // here protects the addon against caller-controlled `numSamples`
    // forcing pathological reallocations of the FFT plan and scratch
    // buffers over IPC.
    static constexpr int kMaxFftSize = 65536;

    // One chord-note in the request payload. Mirrors the chart-note shape
    // the JS chord scorer consumes from `matchNotes()`: `s` = string
    // index, `f` = fret, plus optional technique flags that adjust
    // per-string thresholds.
    struct Note
    {
        int string = 0;
        int fret = 0;
        bool hammerOn = false;     // ho — no pick attack, lower energy threshold
        bool pullOff = false;      // po — same
        bool bend = false;         // b  — pitch moving, widen pitch window
        bool slide = false;        // sl — same
        bool harmonic = false;     // hm — energy-only check, skip pitch
    };

    // Per-note scoring result. Same field names as the JS shape so the
    // N-API wrapper can map straight through and the renderer-side
    // consumer is identical to the browser path.
    struct NoteResult
    {
        int string = 0;
        int fret = 0;
        bool hit = false;
        float bandEnergy = 0.0f;
        // centsDiff is the absolute pitch deviation; centsError is signed
        // (positive = sharp). Both are valid only when the band-energy
        // threshold passed AND pitch-check was requested; otherwise
        // hasCents is false and the renderer treats them as null.
        bool hasCents = false;
        float centsDiff = 0.0f;
        float centsError = 0.0f;
    };

    struct Request
    {
        int numSamples = 4096;          // window read out of the engine's input ring
        std::string arrangement = "guitar"; // "guitar" | "bass"
        int stringCount = 6;
        std::vector<int> tuningOffsets; // size == stringCount, semitones per string
        int capo = 0;
        float pitchCheckCents = 0.0f;   // 0 = energy-only chord check
        float minHitRatio = 0.6f;
        // Force the DSP band-energy scorer even when an ML model is
        // loaded. The ML path is onset-driven and silently drops notes
        // the detector never fires an onset for; the renderer sets this
        // to verify a chart note purely from spectral energy at its
        // expected fundamental (the Rocksmith-style check).
        bool bypassMl = false;
        // Harmonic-comb verification. The default per-note check sums energy
        // across a whole string's frequency band and divides by the total
        // spectrum — a metric a bright or broadband signal dilutes to ~1-3%,
        // below threshold, so correctly-played notes are rejected. With this
        // set, each note is instead scored by the energy at its EXPECTED
        // harmonics (f, 2f, 3f, 4f, 5f) relative to the off-harmonic spectral
        // floor between them. That is the Rocksmith-style targeted check:
        // robust to brightness/distortion because distortion adds energy AT
        // the harmonics, and free of whole-spectrum dilution.
        bool harmonicVerify = false;
        // Minimum harmonic-to-floor ratio for a note to count as present.
        // Tunable over IPC so the renderer can calibrate without a rebuild.
        float harmonicSnr = 3.0f;
        std::vector<Note> notes;
    };

    struct Result
    {
        float score = 0.0f;             // hitStrings / totalStrings
        int hitStrings = 0;
        int totalStrings = 0;
        bool isHit = false;
        std::vector<NoteResult> results;
    };

    ChordScorer() = default;

    // Score a chord against `buffer` (numSamples mono floats). Audio is
    // not stored; the caller (AudioEngine) snapshots its input ring and
    // passes the pointer in. The FFT plan, the complex scratch buffer
    // and the magnitude buffer are reused across calls, so the
    // FFT/peak-pick path itself is allocation-free in steady state. The
    // returned `Result` still allocates its `results` vector (one entry
    // per requested note) — that's a tiny per-call cost the IPC layer
    // pays anyway when serialising the response.
    Result scoreChord(const float* buffer, int numSamples, double sampleRate, const Request& req);

private:
    // Lazily-built FFT for whatever size the current sampleRate dictates.
    // The JS version targets ~3 Hz bin width, so the size depends on
    // sampleRate; we rebuild only when that derived size changes.
    void ensureFft(int fftSize);
    void computeMagnitudes(const float* buffer, int numSamples);

    int currentFftSize = 0;
    int currentFftOrder = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    // FFT scratch as a vector of complex bins, length fftSize. Storing
    // it as std::complex<float> (which juce::dsp::Complex aliases) lets
    // us pass &scratch[0] to fft->perform() without reinterpret_cast'ing
    // a float buffer through a stricter aliasing boundary — the C++
    // standard only guarantees that a std::complex<T> is layout-
    // compatible with T[2] in one direction (complex → T[2]), not the
    // other, so a float* → complex* cast is undefined.
    std::vector<juce::dsp::Complex<float>> fftScratch;
    // Output buffer for fft->perform(). JUCE's FFT::perform is documented
    // out-of-place (juce_FFT.h: "Performs an out-of-place FFT"). Aliasing
    // input and output silently corrupts the result on the Ooura fallback
    // engine that ships on Linux/Windows builds — radix decomposition
    // reads input positions and writes output positions in overlapping
    // iteration patterns, so the same memory gets read after write and
    // intermediate values cascade through butterflies into ~1e27-magnitude
    // garbage bins. Keep a distinct output buffer of the same size.
    std::vector<juce::dsp::Complex<float>> fftOutScratch;
    // Magnitude spectrum, length fftSize/2 + 1 (Nyquist-inclusive).
    std::vector<float> magnitudes;
    double lastBinHz = 0.0;
};
