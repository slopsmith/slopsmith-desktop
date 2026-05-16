#include "PitchDetector.h"
#include <cmath>

// Background thread for YIN detection
class PitchDetectionThread : public juce::Thread
{
public:
    PitchDetectionThread(std::function<void()> callback)
        : Thread("PitchDetector"), cb(std::move(callback)) {}

    void run() override
    {
        while (!threadShouldExit())
        {
            cb();
            sleep(10); // ~100Hz detection rate
        }
    }

private:
    std::function<void()> cb;
};

PitchDetector::PitchDetector()
{
    fifoBuffer.resize(4096, 0.0f);
    analysisBuffer.resize(analysisSize, 0.0f);
}

PitchDetector::~PitchDetector()
{
    stop();
}

void PitchDetector::prepare(double sampleRate, int /*blockSize*/)
{
    // Join the detection thread before touching any shared state.  stop() signals,
    // waits for the thread to exit, and resets 'thread' and 'running', so all
    // mutations below are safe even if prepare() is called mid-stream (e.g. after
    // a sample-rate change).
    stop();

    currentSampleRate = sampleRate;

    // Recompute window so halfLen-1 >= sampleRate/25 Hz at any device rate.
    analysisSize = 2 * ((int)std::ceil(sampleRate / 25.0) + 1);
    analysisBuffer.assign((size_t)analysisSize, 0.0f);
    analysisWritePos = 0;

    // Unconditional restart — stop() always leaves running==false / thread==null.
    running.store(true);
    thread = std::make_unique<PitchDetectionThread>([this]() { detectionThread(); });
    thread->startThread(juce::Thread::Priority::normal);
}

void PitchDetector::stop()
{
    running.store(false);
    if (thread)
    {
        thread->signalThreadShouldExit();
        thread->waitForThreadToExit(1000);
        thread.reset();
    }
}

void PitchDetector::pushSamples(const float* data, int numSamples)
{
    auto scope = fifo.write(numSamples);

    for (int i = 0; i < scope.blockSize1; ++i)
        fifoBuffer[(size_t)(scope.startIndex1 + i)] = data[i];

    for (int i = 0; i < scope.blockSize2; ++i)
        fifoBuffer[(size_t)(scope.startIndex2 + i)] = data[scope.blockSize1 + i];
}

PitchDetector::Detection PitchDetector::getLatestDetection() const
{
    Detection d;
    d.frequency = detectedFreq.load();
    d.confidence = detectedConfidence.load();
    d.midiNote = detectedMidi.load();
    d.cents = detectedCents.load();
    if (d.midiNote >= 0)
        d.noteName = midiToNoteName(d.midiNote);
    return d;
}

void PitchDetector::detectionThread()
{
    // Read available samples from FIFO into analysis buffer
    auto scope = fifo.read(fifo.getNumReady());

    for (int i = 0; i < scope.blockSize1; ++i)
    {
        analysisBuffer[(size_t)analysisWritePos] = fifoBuffer[(size_t)(scope.startIndex1 + i)];
        analysisWritePos = (analysisWritePos + 1) % analysisSize;
    }

    for (int i = 0; i < scope.blockSize2; ++i)
    {
        analysisBuffer[(size_t)analysisWritePos] = fifoBuffer[(size_t)(scope.startIndex2 + i)];
        analysisWritePos = (analysisWritePos + 1) % analysisSize;
    }

    if (scope.blockSize1 + scope.blockSize2 == 0)
        return; // no new data

    // Run YIN on the analysis buffer (arranged as a contiguous window)
    // Rearrange: put oldest samples first
    std::vector<float> window(analysisSize);
    for (int i = 0; i < analysisSize; ++i)
        window[(size_t)i] = analysisBuffer[(size_t)((analysisWritePos + i) % analysisSize)];

    float freq = yinDetect(window.data(), analysisSize, (float)currentSampleRate);

    if (freq > 0.0f)
    {
        float ref = tuningRef.load();
        int midi = frequencyToMidi(freq, ref);
        float nearestFreq = midiToFrequency(midi, ref);
        float cents = 1200.0f * std::log2(freq / nearestFreq);

        detectedFreq.store(freq);
        detectedConfidence.store(1.0f); // YIN confidence is implicit from threshold
        detectedMidi.store(midi);
        detectedCents.store(cents);
    }
    else
    {
        detectedFreq.store(-1.0f);
        detectedConfidence.store(0.0f);
        detectedMidi.store(-1);
        detectedCents.store(0.0f);
    }
}

// ── YIN Algorithm ─────────────────────────────────────────────────────────────
// Ported from Slopsmith's note_detect plugin JavaScript implementation.

float PitchDetector::yinDetect(const float* buffer, int length, float sampleRate) const
{
    const float threshold = 0.15f;
    const int halfLen = length / 2;

    // Restrict tau to the frequency range we care about.  This avoids scanning
    // taus that correspond to undetectable or out-of-range pitches and keeps
    // computation proportional to the useful search window.
    //   tauMin → highest pitch of interest (2000 Hz)
    //   tauMax → lowest pitch of interest (25 Hz covers B0/drop tunings)
    //            capped at halfLen-1, which is the true YIN limit.
    const int tauMin = std::max(2, (int)std::ceil(sampleRate / 2000.0f));
    const int tauMax = std::min(halfLen - 1, (int)std::ceil(sampleRate / 25.0f));

    std::vector<float> yinBuffer((size_t)(tauMax + 1));

    // Difference function — compute only up to tauMax
    float runningSum = 0.0f;
    yinBuffer[0] = 1.0f;

    for (int tau = 1; tau <= tauMax; ++tau)
    {
        float sum = 0.0f;
        for (int i = 0; i < halfLen; ++i)
        {
            float delta = buffer[i] - buffer[i + tau];
            sum += delta * delta;
        }
        yinBuffer[(size_t)tau] = sum;
        runningSum += sum;

        // Cumulative mean normalized difference
        if (runningSum > 0.0f)
            yinBuffer[(size_t)tau] *= (float)tau / runningSum;
    }

    // Silent or DC-constant frame: all difference values are zero, every
    // CMNDF entry stays 0, and the threshold test (0 < 0.15) would accept the
    // very first tau — producing a spurious pitch.  Bail out early.
    if (runningSum < 1e-10f) return -1.0f;

    // Absolute threshold — search only within the detectable pitch range
    int tau = tauMin;
    while (tau <= tauMax)
    {
        if (yinBuffer[(size_t)tau] < threshold)
        {
            while (tau + 1 <= tauMax && yinBuffer[(size_t)(tau + 1)] < yinBuffer[(size_t)tau])
                ++tau;
            break;
        }
        ++tau;
    }

    if (tau > tauMax) return -1.0f; // no pitch detected

    // Parabolic interpolation for sub-sample accuracy
    float s0 = tau > 0 ? yinBuffer[(size_t)(tau - 1)] : yinBuffer[(size_t)tau];
    float s1 = yinBuffer[(size_t)tau];
    float s2 = (tau + 1 <= tauMax) ? yinBuffer[(size_t)(tau + 1)] : yinBuffer[(size_t)tau];

    float denom = 2.0f * (s0 - 2.0f * s1 + s2);
    float betterTau = (std::abs(denom) > 1e-9f)
        ? (float)tau + (s0 - s2) / denom
        : (float)tau;

    float freq = sampleRate / betterTau;

    // Sanity check: guitar range is ~80 Hz (E2) to ~1320 Hz (E6);
    // bass is ~41 Hz (E1) to ~330 Hz (E4), with some extended-range basses
    // reaching ~31 Hz (B0). The analysis window is derived from the current
    // sample rate, so the detector's minimum resolvable frequency is determined
    // by the configured tauMax / window length rather than a fixed
    // analysisSize=4096. A 25 Hz floor remains consistent with that bound,
    // covers extended-range bass tunings (B0 ~31 Hz, drop-A ~27.5 Hz),
    // and rejects sub-bass artefacts.
    if (freq < 25.0f || freq > 2000.0f) return -1.0f;

    return freq;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

int PitchDetector::frequencyToMidi(float freq, float tuningRef)
{
    if (freq <= 0.0f) return -1;
    return (int)std::round(69.0f + 12.0f * std::log2(freq / tuningRef));
}

float PitchDetector::midiToFrequency(int midi, float tuningRef)
{
    return tuningRef * std::pow(2.0f, (float)(midi - 69) / 12.0f);
}

juce::String PitchDetector::midiToNoteName(int midi)
{
    static const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (midi < 0 || midi > 127) return "?";
    int note = midi % 12;
    int octave = (midi / 12) - 1;
    return juce::String(noteNames[note]) + juce::String(octave);
}
