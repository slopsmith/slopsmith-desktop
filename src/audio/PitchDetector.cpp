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
    currentSampleRate = sampleRate;
    analysisWritePos = 0;
    std::fill(analysisBuffer.begin(), analysisBuffer.end(), 0.0f);

    if (!running.load())
    {
        running.store(true);
        thread = std::make_unique<PitchDetectionThread>([this]() { detectionThread(); });
        thread->startThread(juce::Thread::Priority::normal);
    }
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

    std::vector<float> yinBuffer((size_t)halfLen);

    // Difference function
    float runningSum = 0.0f;
    yinBuffer[0] = 1.0f;

    for (int tau = 1; tau < halfLen; ++tau)
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

    // Absolute threshold
    int tau = 2;
    while (tau < halfLen)
    {
        if (yinBuffer[(size_t)tau] < threshold)
        {
            while (tau + 1 < halfLen && yinBuffer[(size_t)(tau + 1)] < yinBuffer[(size_t)tau])
                ++tau;
            break;
        }
        ++tau;
    }

    if (tau == halfLen) return -1.0f; // no pitch detected

    // Parabolic interpolation for sub-sample accuracy
    float s0 = tau > 0 ? yinBuffer[(size_t)(tau - 1)] : yinBuffer[(size_t)tau];
    float s1 = yinBuffer[(size_t)tau];
    float s2 = (tau + 1 < halfLen) ? yinBuffer[(size_t)(tau + 1)] : yinBuffer[(size_t)tau];

    float denom = 2.0f * (s0 - 2.0f * s1 + s2);
    float betterTau = (std::abs(denom) > 1e-9f)
        ? (float)tau + (s0 - s2) / denom
        : (float)tau;

    float freq = sampleRate / betterTau;

    // Sanity check: guitar range is ~80 Hz (E2) to ~1320 Hz (E6), bass is ~40 Hz (E1) to ~330 Hz (E5)
    if (freq < 20.0f || freq > 2000.0f) return -1.0f;

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
