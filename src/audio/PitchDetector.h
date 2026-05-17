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

    // YIN algorithm — non-const: reuses the yinBuffer scratch member.
    float yinDetect(const float* buffer, int length, float sampleRate);

    // MIDI note helpers
    static int frequencyToMidi(float freq, float tuningRef);
    static float midiToFrequency(int midi, float tuningRef);
    static juce::String midiToNoteName(int midi);

    // ── Anti-aliasing decimation ────────────────────────────────────────────
    // Detection runs at a bounded internal rate (~8 kHz) so YIN's O(N^2) cost
    // stays constant regardless of the device sample rate.  Incoming audio is
    // low-pass filtered and decimated before analysis.
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0; // Direct Form II transposed state

        inline float process(float x) noexcept
        {
            const double in = (double)x;
            const double y  = b0 * in + z1;
            z1 = b1 * in - a1 * y + z2;
            z2 = b2 * in - a2 * y;
            return (float)y;
        }

        void reset() noexcept { z1 = z2 = 0.0; }
    };

    // Designs one 2nd-order RBJ-cookbook low-pass biquad into 'bq'.  The
    // cascade is Butterworth only by virtue of the section Q values passed.
    static void designLowpass(Biquad& bq, double cutoffHz, double sampleRate, double q);

    // Lock-free FIFO for audio thread -> detection thread
    juce::AbstractFifo fifo{4096};
    std::vector<float> fifoBuffer;

    // Detection runs on decimated audio at ~8 kHz so YIN's cost is bounded.
    // decimationFactor and internalRate are computed in prepare(); aaFilter is
    // a 4th-order Butterworth-response low-pass (two cascaded biquads) at the
    // device rate before decimation.  prepare() writes these while the
    // detection thread is joined (stopped), and the thread only reads them
    // while running — so no concurrent access occurs.
    static constexpr double targetInternalRate = 8000.0;
    int decimationFactor = 1;
    double internalRate = 48000.0;
    Biquad aaFilter[2];
    int decimPhase = 0;

    // Analysis buffer — holds the most recent decimated (internal-rate)
    // samples, spanning >2 periods of the lowest note of interest (25 Hz).
    // Sized at prepare() time as 2 * (ceil(internalRate / 25) + 1); ~640
    // samples regardless of device rate.  The initialiser is just a
    // placeholder — detection only runs after prepare() has resized it.
    int analysisSize = 1024;
    std::vector<float> analysisBuffer;
    int analysisWritePos = 0;

    // Count of decimated samples written since prepare(), capped at
    // analysisSize.  YIN is gated on this so it never analyses a window that
    // still contains startup silence (which could publish spurious notes).
    int analysisSamplesPrimed = 0;

    // Detection-thread scratch buffers, sized in prepare() so the detection
    // loop stays allocation-free.  Touched only by the detection thread.
    std::vector<float> windowBuffer; // rearranged contiguous analysis window
    std::vector<float> yinBuffer;    // YIN CMNDF working buffer

    // Results (atomic struct via padding)
    std::atomic<float> detectedFreq{-1.0f};
    std::atomic<float> detectedConfidence{0.0f};
    std::atomic<int> detectedMidi{-1};
    std::atomic<float> detectedCents{0.0f};

    std::atomic<float> tuningRef{440.0f};

    // Background thread
    std::unique_ptr<juce::Thread> thread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchDetector)
};
