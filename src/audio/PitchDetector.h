#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

// Real-time pitch detector using the YIN algorithm.
// Audio samples are pushed from the audio thread via a lock-free FIFO.
// A background thread runs YIN detection and updates atomic results
// that can be polled from any thread.
class PitchDetector
{
public:
    PitchDetector();
    ~PitchDetector();

    void prepare(double sampleRate, int blockSize);
    void stop();

    // Called from audio thread — must be lock-free
    void pushSamples(const float* data, int numSamples);

    // Detection result — read from any thread
    struct Detection
    {
        float frequency = -1.0f;   // Hz, -1 if no pitch detected
        float confidence = 0.0f;   // 0-1
        int midiNote = -1;         // nearest MIDI note, -1 if none
        float cents = 0.0f;        // deviation from nearest MIDI note in cents
        juce::String noteName;     // e.g. "A4", "E2"
    };

    Detection getLatestDetection() const;

    // Tuning reference (default 440 Hz)
    void setTuningReference(float hz) { tuningRef.store(hz); }
    float getTuningReference() const { return tuningRef.load(); }

private:
    void detectionThread();

    // YIN algorithm
    float yinDetect(const float* buffer, int length, float sampleRate) const;

    // MIDI note helpers
    static int frequencyToMidi(float freq, float tuningRef);
    static float midiToFrequency(int midi, float tuningRef);
    static juce::String midiToNoteName(int midi);

    // Lock-free FIFO for audio thread -> detection thread
    juce::AbstractFifo fifo{4096};
    std::vector<float> fifoBuffer;

    // Analysis buffer — sized at prepare() time so the minimum detectable
    // frequency stays below 25 Hz at every device sample rate:
    //   analysisSize = 2 * (ceil(sampleRate / 25) + 1)
    // e.g. 3530 at 44.1 kHz, 3842 at 48 kHz, 7682 at 96 kHz, 15 362 at 192 kHz.
    // Default is 4096 so 44.1/48 kHz is covered before prepare() is first called.
    int analysisSize = 4096;
    std::vector<float> analysisBuffer;
    int analysisWritePos = 0;

    // Results (atomic struct via padding)
    std::atomic<float> detectedFreq{-1.0f};
    std::atomic<float> detectedConfidence{0.0f};
    std::atomic<int> detectedMidi{-1};
    std::atomic<float> detectedCents{0.0f};

    std::atomic<float> tuningRef{440.0f};
    double currentSampleRate = 48000.0;

    // Background thread
    std::unique_ptr<juce::Thread> thread;
    std::atomic<bool> running{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchDetector)
};
