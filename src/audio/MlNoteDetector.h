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
    // Returns false if the file is missing/invalid or ONNX support is disabled.
    bool loadModel(const juce::File& modelFile);
    bool hasModel() const { return modelLoaded.load(std::memory_order_relaxed); }

    // True only when ONNX support is compiled in AND a model is loaded.
    bool isAvailable() const;

    void prepare(double sampleRate, int blockSize);
    void stop();

    // Audio thread — lock-free, no allocation.
    void pushSamples(const float* data, int numSamples);

    // A currently-sounding pitch from the latest inference.
    struct ActiveNote
    {
        int   midi = -1;
        float confidence = 0.0f;  // frame/note posteriorgram, 0..1
        float onset = 0.0f;       // onset posteriorgram, 0..1
    };

    // Pitches whose frame posteriorgram clears the activity threshold,
    // highest-confidence first. Polled from the main (N-API) thread.
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
