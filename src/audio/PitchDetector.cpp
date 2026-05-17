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

// Designs one 2nd-order Butterworth low-pass section (RBJ cookbook).
void PitchDetector::designLowpass(Biquad& bq, double cutoffHz, double sampleRate, double q)
{
    const double w0    = juce::MathConstants<double>::twoPi * cutoffHz / sampleRate;
    const double cosw0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);

    const double a0 =  1.0 + alpha;
    bq.b0 = ((1.0 - cosw0) * 0.5) / a0;
    bq.b1 =  (1.0 - cosw0)        / a0;
    bq.b2 = ((1.0 - cosw0) * 0.5) / a0;
    bq.a1 = (-2.0 * cosw0)        / a0;
    bq.a2 =  (1.0 - alpha)        / a0;
    bq.reset();
}

void PitchDetector::prepare(double sampleRate, int /*blockSize*/)
{
    // Join the detection thread before touching any shared state.  stop()
    // signals and waits unconditionally for the thread to exit and resets it,
    // so all mutations below are safe even if prepare() is called mid-stream
    // (e.g. after a sample-rate change).
    stop();

    // Decimate the device stream down to ~8 kHz for detection so YIN's
    // O(N^2) difference function stays bounded regardless of device rate.
    decimationFactor = std::max(1, (int)std::floor(sampleRate / targetInternalRate));
    internalRate     = sampleRate / decimationFactor;
    decimPhase       = 0;

    // Window spans >2 periods of the lowest note of interest (25 Hz) at the
    // internal rate, so YIN can resolve B0 / drop tunings (~640 samples).
    analysisSize = 2 * ((int)std::ceil(internalRate / 25.0) + 1);
    analysisBuffer.assign((size_t)analysisSize, 0.0f);
    analysisWritePos = 0;

    // Anti-aliasing low-pass below the post-decimation Nyquist (internalRate/2),
    // run at the device rate before decimation.  4th-order Butterworth via two
    // biquads with the standard section Q values.  The cutoff sits just above
    // the detectable range (2000 Hz) and well below Nyquist for alias rejection.
    const double cutoff = std::min(2200.0, internalRate * 0.45);
    designLowpass(aaFilter[0], cutoff, sampleRate, 0.54119610);
    designLowpass(aaFilter[1], cutoff, sampleRate, 1.30656296);

    // Drop any samples queued before this (re)configure so the restarted
    // detector never analyses stale audio from the previous run.
    fifo.reset();

    thread = std::make_unique<PitchDetectionThread>([this]() { detectionThread(); });
    thread->startThread(juce::Thread::Priority::normal);
}

void PitchDetector::stop()
{
    if (thread)
    {
        // Wait unconditionally for the thread to exit before destroying it.
        // The bounded internal rate keeps a YIN pass well under a millisecond,
        // so this returns promptly; a genuine hang surfaces rather than racing
        // into a use-after-free on a still-running thread.
        thread->stopThread(-1);
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
    // Read all available device-rate samples from the FIFO.
    auto scope = fifo.read(fifo.getNumReady());

    if (scope.blockSize1 + scope.blockSize2 == 0)
        return; // no new data

    // Anti-alias filter at the device rate, then decimate into the
    // internal-rate analysis buffer (keep every decimationFactor-th sample).
    auto consume = [this](float raw)
    {
        const float filtered = aaFilter[1].process(aaFilter[0].process(raw));
        if (++decimPhase >= decimationFactor)
        {
            decimPhase = 0;
            analysisBuffer[(size_t)analysisWritePos] = filtered;
            analysisWritePos = (analysisWritePos + 1) % analysisSize;
        }
    };

    for (int i = 0; i < scope.blockSize1; ++i)
        consume(fifoBuffer[(size_t)(scope.startIndex1 + i)]);

    for (int i = 0; i < scope.blockSize2; ++i)
        consume(fifoBuffer[(size_t)(scope.startIndex2 + i)]);

    // Run YIN on the decimated analysis buffer (oldest sample first).
    std::vector<float> window(analysisSize);
    for (int i = 0; i < analysisSize; ++i)
        window[(size_t)i] = analysisBuffer[(size_t)((analysisWritePos + i) % analysisSize)];

    float freq = yinDetect(window.data(), analysisSize, (float)internalRate);

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
    // reaching ~31 Hz (B0). Detection runs on a decimated ~8 kHz internal
    // stream whose window spans >2 periods of 25 Hz, so a 25 Hz floor is
    // resolvable; it covers extended-range bass tunings (B0 ~31 Hz, drop-A
    // ~27.5 Hz) and rejects sub-bass artefacts.
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
