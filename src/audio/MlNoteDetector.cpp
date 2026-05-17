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

// Inference cadence. A 48 ms hop keeps detection latency low for live
// single-note scoring of fast material; inference itself is ~30 ms so the
// background thread stays under one core. (Onset *timing* no longer depends
// on the hop — it is back-dated from the posteriorgram frame, see below.)
constexpr int kHopMs       = 48;
constexpr int kHopSamples  = kModelSampleRate * kHopMs / 1000;  // ~1058

// One posteriorgram frame in ms (FFT hop 256 @ 22050 Hz ≈ 11.6 ms). Used to
// back-date a detected onset from its frame index to a wall-clock time.
constexpr double kFrameMs = 256.0 * 1000.0 / kModelSampleRate;

// When reading "what is sounding now" from a fresh inference, look at the
// posteriorgram frames covering roughly the last hop plus a small margin.
constexpr int   kFreshFrames        = 12;     // ~140 ms at 86 fps
constexpr float kActivityThreshold  = 0.40f;  // frame posteriorgram -> "active"
constexpr float kOnsetThreshold     = 0.50f;  // onset posteriorgram rising edge
// A pitch is reported active for this long after its onset even once its
// sustained level has decayed — so fast notes aren't missed between polls.
constexpr float kRecentOnsetMs      = 200.0f;
// Two onset edges on the same pitch must be at least this far apart to count
// as distinct notes. Below the chug rate of fast palm muting (~10/s), above
// the per-inference jitter of re-detecting the same onset.
constexpr double kMinOnsetGapMs     = 45.0;

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
    // Per-pitch onset tracking, owned by the inference thread (no lock — only
    // runInferenceIfDue() touches these). onsetTimeMs is the monotonic ms of
    // the last *distinct* onset; onsetSeq counts distinct onsets; onsetConf is
    // the note posteriorgram at that onset.
    std::array<double, kModelPitches> onsetTimeMs{};
    std::array<int, kModelPitches>    onsetSeq{};
    std::array<float, kModelPitches>  onsetConf{};

    juce::CriticalSection snapshotLock;
    std::array<float, kModelPitches>  snapActivity{};      // sustained note level
    std::array<double, kModelPitches> snapOnsetTimeMs{};   // monotonic ms of last onset; 0 = none
    std::array<int, kModelPitches>    snapOnsetSeq{};      // distinct-onset counter
    std::array<float, kModelPitches>  snapOnsetConf{};     // note posteriorgram at that onset

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
        onsetTimeMs.fill(0.0);
        onsetSeq.fill(0);
        onsetConf.fill(0.0f);
        // Also clear the published snapshot, under its lock: otherwise after a
        // device restart detectNotes()/getActiveNotes()/getDominantNote() keep
        // reporting the previous run's pitches until the next ~2 s inference
        // window fills and publishes fresh data.
        {
            const juce::ScopedLock sl(snapshotLock);
            snapActivity.fill(0.0f);
            snapOnsetTimeMs.fill(0.0);
            snapOnsetSeq.fill(0);
            snapOnsetConf.fill(0.0f);
        }
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

        // Linearise the circular window, oldest sample first. Capture the
        // wall-clock time of the window's last sample ("now") so detected
        // onsets can be back-dated from their posteriorgram frame index.
        const double tWindowEnd = juce::Time::getMillisecondCounterHiRes();
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
            // Require the note tensor to be exactly rank-3 [1, frames, 88]
            // with a positive frame count before indexing it — a fallback to
            // expected dims would let an unexpected-shape model slip through
            // and the flat indexing below would read past the tensor.
            const auto shape = out[0].GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() != 3) return;  // unexpected note-output rank
            const int frames  = (int) shape[1];
            const int pitches = (int) shape[2];
            if (frames <= 0 || pitches != kModelPitches) return;
            // The onset head is indexed below with the note tensor's
            // frames/pitches — confirm out[1] is rank-3 and matches, or an
            // incompatible model's smaller onset tensor is read out of bounds.
            const auto onsetShape = out[1].GetTensorTypeAndShapeInfo().GetShape();
            if (onsetShape.size() != 3
                || (int) onsetShape[1] != frames
                || (int) onsetShape[2] != pitches) return;

            const int firstFrame = juce::jmax(0, frames - kFreshFrames);
            std::array<float, kModelPitches> act{};
            for (int p = 0; p < kModelPitches; ++p)
            {
                // Sustained level: peak note posteriorgram over the fresh frames.
                float a = 0.0f;
                for (int f = firstFrame; f < frames; ++f)
                    a = juce::jmax(a, note[f * pitches + p]);
                act[(size_t) p] = a;

                // Most recent onset: latest rising edge of the onset
                // posteriorgram anywhere in the window, back-dated to a
                // wall-clock time from its frame offset to the window end.
                for (int f = frames - 1; f >= 1; --f)
                {
                    if (onset[f * pitches + p] >= kOnsetThreshold
                        && onset[(f - 1) * pitches + p] < kOnsetThreshold)
                    {
                        const double t = tWindowEnd - (double) (frames - 1 - f) * kFrameMs;
                        // Advance the onset counter only for a genuinely newer
                        // onset — the same physical onset re-detected on the
                        // next inference computes ~the same time and must not
                        // be counted twice.
                        if (t > onsetTimeMs[(size_t) p] + kMinOnsetGapMs)
                        {
                            onsetTimeMs[(size_t) p] = t;
                            onsetSeq[(size_t) p] += 1;
                            onsetConf[(size_t) p] = note[f * pitches + p];
                        }
                        break;
                    }
                }
            }

            const juce::ScopedLock snap(snapshotLock);
            snapActivity = act;
            snapOnsetTimeMs = onsetTimeMs;
            snapOnsetSeq = onsetSeq;
            snapOnsetConf = onsetConf;
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
    // Thread is joined — no race. Clear buffers and the published snapshot so
    // a stopped detector reports nothing stale.
    impl->clearAudioState();
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
    const double now = juce::Time::getMillisecondCounterHiRes();
    {
        const juce::ScopedLock sl(impl->snapshotLock);
        for (int p = 0; p < kNumPitches; ++p)
        {
            const float  level = impl->snapActivity[(size_t) p];
            const double ot    = impl->snapOnsetTimeMs[(size_t) p];
            const float  ageMs = (ot > 0.0) ? (float) (now - ot) : 1.0e9f;
            const bool   recentOnset = ageMs >= 0.0f && ageMs <= kRecentOnsetMs;

            // Report a pitch that is either sustained above the level
            // threshold or freshly onset — the latter keeps fast notes,
            // whose sustained level decays between polls, from being missed.
            if (level >= kActivityThreshold || recentOnset)
            {
                ActiveNote n;
                n.midi = kLowestMidi + p;
                n.confidence = juce::jmax(level,
                    recentOnset ? impl->snapOnsetConf[(size_t) p] : 0.0f);
                n.onsetAgeMs = ageMs;
                n.onsetSeq = impl->snapOnsetSeq[(size_t) p];
                notes.push_back(n);
            }
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
    const double now = juce::Time::getMillisecondCounterHiRes();
    const juce::ScopedLock sl(impl->snapshotLock);
    for (int p = 0; p < kNumPitches; ++p)
    {
        // Use the same "active" definition as getActiveNotes()/isPitchActive()
        // — sustained level OR a fresh onset — so the ML-backed
        // getPitchDetection path doesn't miss fast/decaying notes the rest of
        // the ML API still reports.
        const float  level = impl->snapActivity[(size_t) p];
        const double ot    = impl->snapOnsetTimeMs[(size_t) p];
        const float  ageMs = (ot > 0.0) ? (float) (now - ot) : 1.0e9f;
        const bool   recentOnset = ageMs >= 0.0f && ageMs <= kRecentOnsetMs;
        if (level < kActivityThreshold && ! recentOnset) continue;
        const float conf = juce::jmax(level,
            recentOnset ? impl->snapOnsetConf[(size_t) p] : 0.0f);
        if (conf > best.confidence)
        {
            best.midi = kLowestMidi + p;
            best.confidence = conf;
            best.onsetAgeMs = ageMs;
        }
    }
    return best;
}

bool MlNoteDetector::isPitchActive(int midi, float* confidenceOut) const
{
    const int p = midi - kLowestMidi;
    if (p < 0 || p >= kNumPitches) return false;
    const double now = juce::Time::getMillisecondCounterHiRes();
    const juce::ScopedLock sl(impl->snapshotLock);
    // Match getActiveNotes()'s definition of "active" exactly: sustained
    // level OR a fresh onset. Checking the level alone would miss a
    // freshly-struck fast/decaying note that getActiveNotes() — and hence
    // detectNotes() — still reports, leaving the chord-scoring path
    // (scoreChordWithMl) inconsistent with the detectNotes path.
    const float  level = impl->snapActivity[(size_t) p];
    const double ot    = impl->snapOnsetTimeMs[(size_t) p];
    const float  ageMs = (ot > 0.0) ? (float) (now - ot) : 1.0e9f;
    const bool   recentOnset = ageMs >= 0.0f && ageMs <= kRecentOnsetMs;
    if (confidenceOut != nullptr)
        *confidenceOut = juce::jmax(level,
            recentOnset ? impl->snapOnsetConf[(size_t) p] : 0.0f);
    return level >= kActivityThreshold || recentOnset;
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
