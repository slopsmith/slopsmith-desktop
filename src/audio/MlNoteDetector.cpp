#include "MlNoteDetector.h"

#include <algorithm>
#include <cmath>

#if SLOPSMITH_ONNX_SUPPORT

#include <juce_audio_basics/juce_audio_basics.h>  // juce::LagrangeInterpolator
#include <onnxruntime_cxx_api.h>
#include <array>

namespace
{
// --- Basic Pitch contract (basic_pitch/constants.py; verified by the Phase 0
//     spike — see tests/spike/README.md) -------------------------------------
constexpr int kModelSampleRate = 22050;
constexpr int kAudioNSamples   = 22050 * 2 - 256;  // 43844, ~2 s window
constexpr int kFramesPerWindow = 172;
constexpr int kModelPitches    = MlNoteDetector::kNumPitches;  // 88
constexpr int kLowestMidi      = MlNoteDetector::kLowestMidi;  // 21 (A0)

// nmp.onnx tensor names (verified in Phase 0).
const char* kInputName   = "serving_default_input_2:0";
const char* kNoteOutput  = "StatefulPartitionedCall:1";  // frame/note posteriorgram
const char* kOnsetOutput = "StatefulPartitionedCall:2";  // onset posteriorgram

// Inference cadence. Hop ≈ 96 ms balances detection latency against background
// CPU: end-to-end latency ≈ hop + inference (~20-35 ms) + model onset lag.
constexpr int kHopMs       = 96;
constexpr int kHopSamples  = kModelSampleRate * kHopMs / 1000;  // ~2117

// When reading "what is sounding now" from a fresh inference, look at the
// posteriorgram frames covering roughly the last hop plus a small margin.
constexpr int   kFreshFrames        = 12;     // ~140 ms at 86 fps
constexpr float kActivityThreshold  = 0.40f;  // frame posteriorgram -> "active"

const char* noteNameFor(int midi)
{
    static const char* n[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static thread_local char buf[8];
    if (midi < 0 || midi > 127) return "?";
    std::snprintf(buf, sizeof(buf), "%s%d", n[midi % 12], midi / 12 - 1);
    return buf;
}
} // namespace

// ── Background inference thread ───────────────────────────────────────────────
class MlInferenceThread : public juce::Thread
{
public:
    explicit MlInferenceThread(std::function<void()> callback)
        : Thread("MlNoteDetector"), cb(std::move(callback)) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            cb();
            sleep(8);
        }
    }

private:
    std::function<void()> cb;
};

// ── Impl ─────────────────────────────────────────────────────────────────────
struct MlNoteDetector::Impl
{
    // Audio thread -> inference thread.
    juce::AbstractFifo fifo{ 16384 };
    std::vector<float> fifoBuffer = std::vector<float>(16384, 0.0f);

    // Resampling to 22050 Hz.
    juce::LagrangeInterpolator resampler;
    double resampleRatio = 48000.0 / kModelSampleRate;
    std::vector<float> inQueue;       // 48 kHz samples awaiting resampling

    // Rolling ~2 s window at 22050 Hz, circular.
    std::vector<float> circ = std::vector<float>(kAudioNSamples, 0.0f);
    size_t   circWrite = 0;
    uint64_t totalResampled = 0;
    int      sinceInference = 0;

    // ONNX Runtime session, guarded like NAMProcessor's model pointer.
    Ort::Env env{ ORT_LOGGING_LEVEL_WARNING, "slopsmith-mlnd" };
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::unique_ptr<Ort::Session> session;
    juce::CriticalSection sessionLock;

    // Active-pitch snapshot, written by the inference thread, read from the
    // main (N-API) thread under snapshotLock. Neither writer nor reader is the
    // audio thread, so a lock here is correct and cheap.
    juce::CriticalSection snapshotLock;
    std::array<float, kModelPitches> snapActivity{};
    std::array<float, kModelPitches> snapOnset{};

    // Reusable inference scratch — avoids per-hop allocation.
    std::vector<float> window = std::vector<float>(kAudioNSamples, 0.0f);
    std::vector<float> resampleScratch;

    std::unique_ptr<MlInferenceThread> thread;

    // Start / stop the background inference thread. prepare() and stop() use
    // these so the thread is never alive while clearAudioState() mutates the
    // FIFO / inQueue / circular buffer — otherwise a device stop→start cycle
    // would race ingest() against the buffer reset.
    void startThread()
    {
        thread = std::make_unique<MlInferenceThread>([this]()
        {
            ingest();
            runInferenceIfDue();
        });
        thread->startThread(juce::Thread::Priority::normal);
    }

    void stopThread()
    {
        if (thread)
        {
            thread->signalThreadShouldExit();
            thread->waitForThreadToExit(1000);
            thread.reset();
        }
    }

    void clearAudioState()
    {
        resampler.reset();
        inQueue.clear();
        std::fill(circ.begin(), circ.end(), 0.0f);
        circWrite = 0;
        totalResampled = 0;
        sinceInference = 0;
        fifo.reset();
    }

    // Drain the FIFO, resample to 22050 Hz, append to the rolling window.
    void ingest()
    {
        const int ready = fifo.getNumReady();
        if (ready <= 0) return;

        auto scope = fifo.read(ready);
        const size_t base = inQueue.size();
        inQueue.resize(base + (size_t) ready);
        for (int i = 0; i < scope.blockSize1; ++i)
            inQueue[base + (size_t) i] = fifoBuffer[(size_t) (scope.startIndex1 + i)];
        for (int i = 0; i < scope.blockSize2; ++i)
            inQueue[base + (size_t) scope.blockSize1 + (size_t) i]
                = fifoBuffer[(size_t) (scope.startIndex2 + i)];

        // Produce as many 22050 Hz samples as the queued input safely allows,
        // leaving a small margin for the interpolator kernel.
        const int avail = (int) inQueue.size();
        const int numOut = (int) ((double) (avail - 8) / resampleRatio);
        if (numOut <= 0) return;

        resampleScratch.resize((size_t) numOut);
        const int used = resampler.process(resampleRatio, inQueue.data(),
                                           resampleScratch.data(), numOut);
        inQueue.erase(inQueue.begin(), inQueue.begin() + juce::jmin(used, avail));

        for (int i = 0; i < numOut; ++i)
        {
            circ[circWrite] = resampleScratch[(size_t) i];
            circWrite = (circWrite + 1) % (size_t) kAudioNSamples;
        }
        totalResampled += (uint64_t) numOut;
        sinceInference += numOut;
    }

    void runInferenceIfDue()
    {
        if (sinceInference < kHopSamples) return;
        if (totalResampled < (uint64_t) kAudioNSamples) return;  // window not full yet
        sinceInference = 0;

        // try-lock: if a model swap is in progress, skip this hop rather than
        // stalling the detector (NAMProcessor's realtime-adjacent pattern).
        const juce::ScopedTryLock sl(sessionLock);
        if (! sl.isLocked() || session == nullptr) return;

        // Linearise the circular window, oldest sample first.
        for (int i = 0; i < kAudioNSamples; ++i)
            window[(size_t) i] = circ[(circWrite + (size_t) i) % (size_t) kAudioNSamples];

        try
        {
            const int64_t inShape[3] = { 1, kAudioNSamples, 1 };
            Ort::Value inTensor = Ort::Value::CreateTensor<float>(
                memInfo, window.data(), window.size(), inShape, 3);

            const char* inNames[]  = { kInputName };
            const char* outNames[] = { kNoteOutput, kOnsetOutput };
            auto out = session->Run(Ort::RunOptions{ nullptr },
                                    inNames, &inTensor, 1, outNames, 2);

            const float* note  = out[0].GetTensorData<float>();
            const float* onset = out[1].GetTensorData<float>();
            const auto shape = out[0].GetTensorTypeAndShapeInfo().GetShape();
            const int frames  = shape.size() >= 2 ? (int) shape[1] : kFramesPerWindow;
            const int pitches = shape.size() >= 3 ? (int) shape[2] : kModelPitches;
            if (pitches != kModelPitches) return;  // unexpected model

            const int firstFrame = juce::jmax(0, frames - kFreshFrames);
            std::array<float, kModelPitches> act{};
            std::array<float, kModelPitches> ons{};
            for (int p = 0; p < kModelPitches; ++p)
            {
                float a = 0.0f, o = 0.0f;
                for (int f = firstFrame; f < frames; ++f)
                {
                    a = juce::jmax(a, note[f * pitches + p]);
                    o = juce::jmax(o, onset[f * pitches + p]);
                }
                act[(size_t) p] = a;
                ons[(size_t) p] = o;
            }

            const juce::ScopedLock snap(snapshotLock);
            snapActivity = act;
            snapOnset = ons;
        }
        catch (...)
        {
            // Inference failed — leave the previous snapshot in place. The
            // engine still has the YIN fallback for callers that need it.
        }
    }
};

// ── MlNoteDetector ───────────────────────────────────────────────────────────
MlNoteDetector::MlNoteDetector() : impl(std::make_unique<Impl>()) {}

MlNoteDetector::~MlNoteDetector() { stop(); }

bool MlNoteDetector::isAvailable() const
{
    return modelLoaded.load(std::memory_order_relaxed);
}

bool MlNoteDetector::loadModel(const juce::File& modelFile)
{
    // Return value is "is the ML detector available after this call" — not
    // "did this particular attempt succeed". A failed load never tears down a
    // model that was already working, so a bad reload over a good model still
    // reports true, keeping the result consistent with isAvailable().
    if (! modelFile.existsAsFile())
        return modelLoaded.load(std::memory_order_relaxed);

    try
    {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(2);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef _WIN32
        auto newSession = std::make_unique<Ort::Session>(
            impl->env, modelFile.getFullPathName().toWideCharPointer(), opts);
#else
        auto newSession = std::make_unique<Ort::Session>(
            impl->env, modelFile.getFullPathName().toRawUTF8(), opts);
#endif
        {
            const juce::ScopedLock sl(impl->sessionLock);
            impl->session = std::move(newSession);
        }
        modelLoaded.store(true, std::memory_order_relaxed);
        // Bring the inference thread up as soon as a model is loaded — not
        // only on the first audio-device prepare(). This keeps isAvailable()
        // and the engine state honest: once a model is loaded the detector is
        // genuinely live (it idles on an empty FIFO until audio starts).
        // loadModel() runs once at startup, before any audio device begins,
        // so it never races prepare()'s stop/clear/start.
        if (! impl->thread)
            impl->startThread();
        return true;
    }
    catch (...)
    {
        // Build failed — the previous session (if any) is untouched, so
        // report whatever availability state still holds.
        return modelLoaded.load(std::memory_order_relaxed);
    }
}

void MlNoteDetector::prepare(double sampleRate, int /*blockSize*/)
{
    // Stop the inference thread before mutating the shared buffers so a device
    // stop→start cycle can't race clearAudioState() against ingest() /
    // runInferenceIfDue() on the FIFO, inQueue and circular window.
    impl->stopThread();
    impl->resampleRatio = (sampleRate > 0.0 ? sampleRate : 48000.0) / kModelSampleRate;
    impl->clearAudioState();
    // Only run the inference thread when there's a model to feed it —
    // otherwise it would resample the input for nothing. loadModel() starts
    // the thread itself if a model arrives after prepare().
    if (modelLoaded.load(std::memory_order_relaxed))
        impl->startThread();
}

void MlNoteDetector::stop()
{
    impl->stopThread();
}

void MlNoteDetector::pushSamples(const float* data, int numSamples)
{
    if (numSamples <= 0) return;
    // Lock-free write; if the FIFO is full (inference stalled) the oldest
    // unread samples are simply not overwritten — we drop the newest instead,
    // which never blocks or allocates on the audio thread.
    const int free = impl->fifo.getFreeSpace();
    const int n = juce::jmin(numSamples, free);
    if (n <= 0) return;

    auto scope = impl->fifo.write(n);
    for (int i = 0; i < scope.blockSize1; ++i)
        impl->fifoBuffer[(size_t) (scope.startIndex1 + i)] = data[i];
    for (int i = 0; i < scope.blockSize2; ++i)
        impl->fifoBuffer[(size_t) (scope.startIndex2 + i)] = data[scope.blockSize1 + i];
}

std::vector<MlNoteDetector::ActiveNote> MlNoteDetector::getActiveNotes() const
{
    std::vector<ActiveNote> notes;
    {
        const juce::ScopedLock sl(impl->snapshotLock);
        for (int p = 0; p < kNumPitches; ++p)
        {
            if (impl->snapActivity[(size_t) p] >= kActivityThreshold)
                notes.push_back({ kLowestMidi + p,
                                  impl->snapActivity[(size_t) p],
                                  impl->snapOnset[(size_t) p] });
        }
    }
    std::sort(notes.begin(), notes.end(),
              [](const ActiveNote& a, const ActiveNote& b)
              { return a.confidence > b.confidence; });
    return notes;
}

MlNoteDetector::ActiveNote MlNoteDetector::getDominantNote() const
{
    ActiveNote best;
    const juce::ScopedLock sl(impl->snapshotLock);
    for (int p = 0; p < kNumPitches; ++p)
    {
        const float a = impl->snapActivity[(size_t) p];
        if (a >= kActivityThreshold && a > best.confidence)
            best = { kLowestMidi + p, a, impl->snapOnset[(size_t) p] };
    }
    return best;
}

bool MlNoteDetector::isPitchActive(int midi, float* confidenceOut) const
{
    const int p = midi - kLowestMidi;
    if (p < 0 || p >= kNumPitches) return false;
    const juce::ScopedLock sl(impl->snapshotLock);
    const float a = impl->snapActivity[(size_t) p];
    if (confidenceOut != nullptr) *confidenceOut = a;
    return a >= kActivityThreshold;
}

#else // !SLOPSMITH_ONNX_SUPPORT — inert stub, YIN fallback covers detection.

struct MlNoteDetector::Impl {};

MlNoteDetector::MlNoteDetector() = default;
MlNoteDetector::~MlNoteDetector() = default;
bool MlNoteDetector::isAvailable() const { return false; }
bool MlNoteDetector::loadModel(const juce::File&) { return false; }
void MlNoteDetector::prepare(double, int) {}
void MlNoteDetector::stop() {}
void MlNoteDetector::pushSamples(const float*, int) {}
std::vector<MlNoteDetector::ActiveNote> MlNoteDetector::getActiveNotes() const { return {}; }
MlNoteDetector::ActiveNote MlNoteDetector::getDominantNote() const { return {}; }
bool MlNoteDetector::isPitchActive(int, float*) const { return false; }

#endif
