#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <vector>

// Polyphonic ML note detector — Spotify Basic Pitch run via ONNX Runtime.
//
// Mirrors PitchDetector's threading contract: the audio thread pushes samples
// through a lock-free FIFO; a background thread resamples to 22050 Hz, runs
// inference on a rolling ~2 s window, and publishes an active-pitch snapshot
// that any non-audio thread can poll.
//
// When ONNX Runtime is not available at build time (SLOPSMITH_ONNX_SUPPORT=0)
// every method is an inert no-op and isAvailable() returns false — callers
// fall back to the YIN PitchDetector / ChordScorer (Constitution VII).
//
// The ONNX Runtime headers are confined to MlNoteDetector.cpp via a PIMPL, so
// including this header does not pull <onnxruntime_cxx_api.h> into the engine.
class MlNoteDetector
{
public:
    MlNoteDetector();
    ~MlNoteDetector();

    static constexpr int kNumPitches = 88;  // Basic Pitch: MIDI 21..108
    static constexpr int kLowestMidi = 21;  // A0

    // Load the Basic Pitch ONNX model. Thread-safe; the live model pointer is
    // swapped under a lock once the new session is built (NAMProcessor pattern).
    // Returns "is the ML detector available after this call" — NOT whether
    // this particular load succeeded. A missing/invalid file never tears down
    // an already-loaded model, so it can still return true; returns false when
    // no model is loaded or ONNX support is compiled out.
    bool loadModel(const juce::File& modelFile);
    bool hasModel() const { return modelLoaded.load(std::memory_order_relaxed); }

    // True only when ONNX support is compiled in AND a model is loaded
    // (and its output contract has not been demoted).
    bool isAvailable() const;

    // isAvailable() AND the detector has published at least one inference
    // snapshot — gate engine-side ML routing on this so the cold-start
    // window after an audio start/restart uses the YIN fallback.
    bool isReady() const;

    void prepare(double sampleRate, int blockSize);
    void stop();

    // Audio thread — lock-free, no allocation.
    void pushSamples(const float* data, int numSamples);

    // A currently-sounding pitch from the latest inference.
    struct ActiveNote
    {
        int   midi = -1;
        float confidence = 0.0f;    // note posteriorgram, 0..1
        // Milliseconds since this pitch's most recent detected onset, measured
        // when getActiveNotes()/getDominantNote() is called. Lets the caller
        // back-date a detection to the true onset rather than poll time.
        // >= 1e6 means no recent onset (sustained / decaying tail).
        float onsetAgeMs = 1.0e9f;
        // Monotonic per-pitch onset counter — increments once per distinct
        // detected onset. A change since the last poll means a NEW note was
        // struck on this pitch, letting the caller treat onsets as discrete
        // events instead of polling "is this pitch active".
        int   onsetSeq = 0;
    };

    // Pitches that are sounding now — either sustained above the activity
    // threshold or freshly onset within the last ~200 ms. Polled from the
    // main (N-API) thread.
    std::vector<ActiveNote> getActiveNotes() const;

    // Highest-confidence active pitch; midi < 0 when nothing is sounding.
    ActiveNote getDominantNote() const;

    // Whether a specific MIDI pitch is currently active. Optionally returns the
    // pitch's confidence. Used by the ML-backed scoreChord path.
    bool isPitchActive(int midi, float* confidenceOut = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<bool> modelLoaded{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MlNoteDetector)
};
