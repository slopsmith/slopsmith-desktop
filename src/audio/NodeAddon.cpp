// Slopsmith Audio Engine — Node.js Native Addon (N-API)
// Bridges the JUCE-based C++ audio engine to Electron via node-addon-api.
// All audio processing happens in C++; JS communicates via IPC.

#include <napi.h>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <cmath>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "AudioEngine.h"
#include "VSTHost.h"

// Forward declaration — defined alongside loadVstSandboxAware further down.
// doShutdown (below) needs it to release any LoadVSTWorker / LoadPreset-
// Worker blocked on a pending async load before the message thread stops.
static void cancelAllPendingLoads();
#include "VSTTrace.h"
#include "NAMProcessor.h"
#include "IRLoader.h"
#include "Sandbox/SandboxedProcessor.h"

#include <juce_events/juce_events.h>

// engine / vstHost — shared_ptr (not unique_ptr) so worker threads can take
// a stable snapshot that keeps the object alive for the duration of their
// work, even if the message thread reassigns the global mid-operation. This
// matters most for the async VST load: createPluginInstanceAsync's JUCE
// continuation must not have VSTHost / its formatManager torn out from
// under it mid-load.
//
// snapshotEngine() / snapshotVstHost() take the global under the matching
// mutex and return a private copy. The only code permitted to touch the
// bare `engine` / `vstHost` globals is the snapshot helpers below and the
// mutex-guarded writes in Init / doShutdown — the message thread mutates
// the globals there while worker threads and the napi handlers read them.
//
// Enforced rule: every *dereference* of engine / vstHost goes through a
// local snapshot. A napi handler takes that snapshot at the top, null-
// checks it, and uses only the local — either for the rest of its body,
// or (for the handlers that hand off to an AsyncWorker — LoadVST,
// LoadNAMModel, LoadIR, LoadPreset) purely as the availability guard
// before queuing, with the worker re-snapshotting on its own thread.
// Either way a concurrent doShutdown reset can never pull the object out
// from under an in-flight dereference.
static std::shared_ptr<AudioEngine> engine;
static std::mutex engineMutex;

static std::shared_ptr<AudioEngine> snapshotEngine()
{
    std::lock_guard<std::mutex> lock(engineMutex);
    return engine;
}

static std::shared_ptr<VSTHost> vstHost;
static std::mutex vstHostMutex;

static std::shared_ptr<VSTHost> snapshotVstHost()
{
    std::lock_guard<std::mutex> lock(vstHostMutex);
    return vstHost;
}
static std::thread juceMessageThread;
static std::atomic<bool> juceRunning{false};
static std::atomic<bool> alreadyShutDown{false};

// ── JUCE Message Thread ───────────────────────────────────────────────────────
// JUCE requires a message thread for plugin loading, audio device management, etc.
// We pump it in a dedicated thread.

static void startJuceMessageThread()
{
    if (juceRunning.load()) return;
    juceRunning.store(true);

#if JUCE_MAC
    // On macOS, JUCE's MessageManager::runDispatchLoopUntil internally calls
    // `-[NSApplication _nextEventMatchingEventMask:...]`, which AppKit asserts
    // must run on the true main thread. Node.js already owns the main thread
    // (running libuv's event loop), so we can't spawn a second NS event pump
    // without hitting `nextEventMatchingMask should only be called from the
    // Main Thread!` and aborting.
    //
    // Workaround: designate Node's current thread as JUCE's message thread and
    // skip the dispatch loop. callAsync()'d callbacks will still queue; we
    // drain them from the Node thread via a libuv timer created below.
    juce::MessageManager::getInstance();
#else
    juceMessageThread = std::thread([]() {
        juce::MessageManager::getInstance();
        while (juceRunning.load())
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        }
        juce::MessageManager::deleteInstance();
    });
#endif
}

static void stopJuceMessageThread()
{
    juceRunning.store(false);
#if !JUCE_MAC
    if (juceMessageThread.joinable())
        juceMessageThread.join();
#else
    juce::MessageManager::deleteInstance();
#endif
}

// ── Helper: dispatch on JUCE message thread ───────────────────────────────────

template <typename Func>
static void dispatchOnMessageThread(Func&& func)
{
#if JUCE_MAC
    // No background message thread on macOS — execute inline on caller thread.
    // Audio device / NAM / IR init is thread-safe for our use; VST/AU plugin
    // instantiation (which genuinely requires a message thread on macOS) is
    // the one capability we give up until a proper libuv-based pump lands.
    func();
#else
    // Heap-allocate the WaitableEvent and capture by value so the queued
    // callAsync closure can outlive this stack frame. Without this, a 15 s
    // timeout (rare, but possible during shutdown when the message thread is
    // busy) leaves the lambda running on freed `done` storage — a real UAF.
    auto done = std::make_shared<juce::WaitableEvent>();
    juce::MessageManager::callAsync([func = std::forward<Func>(func), done]() mutable {
        func();
        done->signal();
    });
    done->wait(15000);
#endif
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

static Napi::Value Init(const Napi::CallbackInfo& info)
{
    auto env = info.Env();

    // Reset the shutdown latch so a JS-level init→shutdown→init cycle (e.g.
    // a test harness recreating the engine) actually runs shutdown again
    // instead of treating it as already-done.
    alreadyShutDown.store(false, std::memory_order_release);

    // Start JUCE message thread first (no-op on macOS — see startJuceMessageThread)
    startJuceMessageThread();

#if !JUCE_MAC
    // Small delay to ensure message thread is pumping
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif

    // Create engine on the JUCE message thread (or inline on macOS)
    dispatchOnMessageThread([]() {
        std::shared_ptr<AudioEngine> liveEngine;
        {
            std::lock_guard<std::mutex> lock(engineMutex);
            engine = std::make_shared<AudioEngine>();
            liveEngine = engine;
        }
        {
            std::lock_guard<std::mutex> lock(vstHostMutex);
            vstHost = std::make_shared<VSTHost>();
        }

        auto types = liveEngine->getDeviceTypes();
        fprintf(stderr, "[audio-native] Init complete. Device types: %d\n", types.size());
        for (int i = 0; i < types.size(); ++i)
            fprintf(stderr, "[audio-native]   %s: %d inputs, %d outputs\n",
                    types[i].name.toRawUTF8(),
                    types[i].inputDevices.size(),
                    types[i].outputDevices.size());
    });

    return env.Undefined();
}

static void doShutdown()
{
    // The latch is flipped at the TOP rather than the bottom so a
    // re-entrant call (e.g. env-cleanup-hook firing while a JS-level
    // shutdown is mid-flight) bails immediately rather than racing on
    // the same teardown sequence. Assumed serialisation invariants:
    //   - dispatchOnMessageThread is single-writer to engine/vstHost
    //     (both unique_ptrs touched only here or from Init);
    //   - stopJuceMessageThread is idempotent and safe to call when the
    //     thread was never started (defensive checks inside).
    // If a future caller mutates engine/vstHost between this latch and
    // the dispatch (or the dispatch's 15s wait times out), THIS call's
    // body may not finish before returning — but the re-entrant
    // cleanup-hook will then no-op via the latch and the dispatch
    // queue itself unwinds whatever's pending. Net result: at-most-
    // once execution of the gated body, even under teardown races.
    bool expected = false;
    if (!alreadyShutDown.compare_exchange_strong(expected, true)) return;

    // Release any LoadVSTWorker / LoadPresetWorker currently blocked on a
    // pending async load. Without this they'd wait forever on the
    // WaitableEvent — the createPluginInstanceAsync callback can't fire
    // once the message thread is gone. Forward-declared above; the
    // implementation lives near loadVstSandboxAware.
    cancelAllPendingLoads();

    if (juceRunning.load() || snapshotEngine() || snapshotVstHost())
    {
        dispatchOnMessageThread([]() {
            if (auto liveEngine = snapshotEngine())
                liveEngine->stopAudio();
            {
                std::lock_guard<std::mutex> lock(engineMutex);
                engine.reset();
            }
            {
                std::lock_guard<std::mutex> lock(vstHostMutex);
                vstHost.reset();
            }
        });
    }

    stopJuceMessageThread();
}

static Napi::Value Shutdown(const Napi::CallbackInfo& info)
{
    doShutdown();
    return info.Env().Undefined();
}

// ── Device Enumeration ────────────────────────────────────────────────────────

static Napi::Value GetDeviceTypes(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();

    // Device types are already scanned during init — safe to read from any thread
    auto types = liveEngine->getDeviceTypes();

    auto result = Napi::Array::New(env, types.size());

    for (int i = 0; i < types.size(); ++i)
    {
        auto obj = Napi::Object::New(env);
        obj.Set("name", types[i].name.toStdString());

        auto inputs = Napi::Array::New(env, types[i].inputDevices.size());
        for (int j = 0; j < types[i].inputDevices.size(); ++j)
            inputs.Set((uint32_t)j, types[i].inputDevices[j].toStdString());
        obj.Set("inputs", inputs);

        auto outputs = Napi::Array::New(env, types[i].outputDevices.size());
        for (int j = 0; j < types[i].outputDevices.size(); ++j)
            outputs.Set((uint32_t)j, types[i].outputDevices[j].toStdString());
        obj.Set("outputs", outputs);

        result.Set((uint32_t)i, obj);
    }

    return result;
}

static Napi::Value GetSampleRates(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return Napi::Array::New(env);

    auto rates = liveEngine->getSampleRates();
    auto result = Napi::Array::New(env, rates.size());
    for (int i = 0; i < rates.size(); ++i)
        result.Set((uint32_t)i, rates[i]);
    return result;
}

static Napi::Value GetBufferSizes(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return Napi::Array::New(env);

    auto sizes = liveEngine->getBufferSizes();
    auto result = Napi::Array::New(env, sizes.size());
    for (int i = 0; i < sizes.size(); ++i)
        result.Set((uint32_t)i, sizes[i]);
    return result;
}

static Napi::Value ProbeDeviceOptions(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    auto obj = Napi::Object::New(env);
    auto type = info.Length() > 0 && info[0].IsString() ? info[0].As<Napi::String>().Utf8Value() : "";
    auto input = info.Length() > 1 && info[1].IsString() ? info[1].As<Napi::String>().Utf8Value() : "";
    auto output = info.Length() > 2 && info[2].IsString() ? info[2].As<Napi::String>().Utf8Value() : "";
    auto ratesArray = Napi::Array::New(env);
    auto buffersArray = Napi::Array::New(env);

    obj.Set("type", type);
    obj.Set("input", input);
    obj.Set("output", output);
    obj.Set("sampleRates", ratesArray);
    obj.Set("bufferSizes", buffersArray);
    if (!liveEngine)
    {
        obj.Set("error", "Audio engine not initialized");
        return obj;
    }

    auto options = liveEngine->probeDeviceOptions(juce::String(type), juce::String(input), juce::String(output));
    obj.Set("type", options.type.toStdString());
    obj.Set("input", options.input.toStdString());
    obj.Set("output", options.output.toStdString());
    obj.Set("error", options.error.toStdString());

    ratesArray = Napi::Array::New(env, options.sampleRates.size());
    for (int i = 0; i < options.sampleRates.size(); ++i)
        ratesArray.Set((uint32_t)i, options.sampleRates[i]);
    obj.Set("sampleRates", ratesArray);

    buffersArray = Napi::Array::New(env, options.bufferSizes.size());
    for (int i = 0; i < options.bufferSizes.size(); ++i)
        buffersArray.Set((uint32_t)i, options.bufferSizes[i]);
    obj.Set("bufferSizes", buffersArray);

    return obj;
}

static Napi::Value GetCurrentDevice(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();

    auto obj = Napi::Object::New(env);
    obj.Set("type", liveEngine->getCurrentDeviceType().toStdString());
    obj.Set("input", liveEngine->getCurrentInputDevice().toStdString());
    obj.Set("output", liveEngine->getCurrentOutputDevice().toStdString());
    obj.Set("sampleRate", liveEngine->getCurrentSampleRate());
    obj.Set("blockSize", liveEngine->getCurrentBlockSize());
    obj.Set("latencyMs", liveEngine->getLatencyMs());
    return obj;
}

// ── Device Selection ──────────────────────────────────────────────────────────

static Napi::Value SetDeviceType(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1) return Napi::Boolean::New(env, false);

    auto typeName = info[0].As<Napi::String>().Utf8Value();
    bool result = liveEngine->setDeviceType(juce::String(typeName));
    return Napi::Boolean::New(env, result);
}

static Napi::Value SetDevice(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return Napi::Boolean::New(env, false);

    auto input = info.Length() > 0 && !info[0].IsNull() ? info[0].As<Napi::String>().Utf8Value() : "";
    auto output = info.Length() > 1 && !info[1].IsNull() ? info[1].As<Napi::String>().Utf8Value() : "";
    double sr = info.Length() > 2 && !info[2].IsUndefined() ? info[2].As<Napi::Number>().DoubleValue() : 48000.0;
    int bs = info.Length() > 3 && !info[3].IsUndefined() ? info[3].As<Napi::Number>().Int32Value() : 256;

    // Must run on the main thread — JUCE's ALSA backend deadlocks if called from a worker
    bool result = liveEngine->setAudioDevice(juce::String(input), juce::String(output), sr, bs);
    return Napi::Boolean::New(env, result);
}

// ── Audio Control ─────────────────────────────────────────────────────────────

static Napi::Value StartAudio(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine()) liveEngine->startAudio();
    return info.Env().Undefined();
}

static Napi::Value StopAudio(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine()) liveEngine->stopAudio();
    return info.Env().Undefined();
}

static Napi::Value IsAudioRunning(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    return Napi::Boolean::New(info.Env(), liveEngine ? liveEngine->isAudioRunning() : false);
}

// ── Gain ──────────────────────────────────────────────────────────────────────

static Napi::Value SetGain(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 2) return env.Undefined();

    auto which = info[0].As<Napi::String>().Utf8Value();
    float value = info[1].As<Napi::Number>().FloatValue();

    if (which == "input") liveEngine->setInputGain(value);
    else if (which == "output") liveEngine->setOutputGain(value);
    else if (which == "chain") liveEngine->setChainOutputGain(value);
    else if (which == "backing") liveEngine->setBackingVolume(value);

    return env.Undefined();
}

static Napi::Value SetInputChannel(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0)
        liveEngine->setInputChannel(info[0].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

static Napi::Value SetMonitorMute(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0)
        liveEngine->setMonitorMute(info[0].As<Napi::Boolean>().Value());
    return info.Env().Undefined();
}

static Napi::Value SetMonitorMuteSuppressed(const Napi::CallbackInfo& info)
{
    // IsBoolean()-guarded so a mismatched renderer build / manual caller
    // passing a non-boolean is a clean no-op rather than a hard N-API failure
    // (NAPI_DISABLE_CPP_EXCEPTIONS is enabled). Mirrors SetNoiseGate's style.
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0 && info[0].IsBoolean())
        liveEngine->setMonitorMuteSuppressed(info[0].As<Napi::Boolean>().Value());
    return info.Env().Undefined();
}

static Napi::Value SetNoiseGate(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsObject())
        return env.Undefined();

    auto o = info[0].As<Napi::Object>();

    bool enabled = false;
    if (o.Has("enabled"))
    {
        auto v = o.Get("enabled");
        if (v.IsBoolean())
            enabled = v.As<Napi::Boolean>().Value();
        else if (v.IsNumber())
            enabled = v.As<Napi::Number>().DoubleValue() != 0.0;
    }

    float thresholdDb = -60.0f;
    if (o.Has("thresholdDb") && o.Get("thresholdDb").IsNumber())
        thresholdDb = (float)o.Get("thresholdDb").As<Napi::Number>().DoubleValue();

    float releaseMs = 100.0f;
    if (o.Has("releaseMs") && o.Get("releaseMs").IsNumber())
        releaseMs = (float)o.Get("releaseMs").As<Napi::Number>().DoubleValue();

    float depthDb = -60.0f;
    if (o.Has("depthDb") && o.Get("depthDb").IsNumber())
        depthDb = (float)o.Get("depthDb").As<Napi::Number>().DoubleValue();

    liveEngine->setNoiseGate(enabled, thresholdDb, releaseMs, depthDb);
    return env.Undefined();
}

static Napi::Value SetTonePolish(const Napi::CallbackInfo& info)
{
    // Tone Polish — { enabled: bool }. Mirrors SetNoiseGate's defensive
    // shape so a mismatched renderer build / manual caller passing a
    // non-object is a clean no-op rather than a hard N-API failure
    // (NAPI_DISABLE_CPP_EXCEPTIONS).
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsObject())
        return env.Undefined();

    auto o = info[0].As<Napi::Object>();

    bool enabled = true;
    if (o.Has("enabled"))
    {
        auto v = o.Get("enabled");
        if (v.IsBoolean())
            enabled = v.As<Napi::Boolean>().Value();
        else if (v.IsNumber())
            enabled = v.As<Napi::Number>().DoubleValue() != 0.0;
    }

    liveEngine->setTonePolishEnabled(enabled);
    return env.Undefined();
}

static Napi::Value IsMonitorMuted(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    return Napi::Boolean::New(info.Env(), liveEngine ? liveEngine->isMonitorMuted() : true);
}

// ── Metering (polled — read atomics) ──────────────────────────────────────────

static Napi::Value GetLevels(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    auto liveEngine = snapshotEngine();

    if (liveEngine)
    {
        obj.Set("inputLevel", liveEngine->getInputLevel());
        obj.Set("outputLevel", liveEngine->getOutputLevel());
        obj.Set("inputPeak", liveEngine->getInputPeak());
        obj.Set("outputPeak", liveEngine->getOutputPeak());
    }
    else
    {
        obj.Set("inputLevel", 0.0);
        obj.Set("outputLevel", 0.0);
        obj.Set("inputPeak", 0.0);
        obj.Set("outputPeak", 0.0);
    }

    return obj;
}

static Napi::Value ResetPeaks(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine()) liveEngine->resetPeaks();
    return info.Env().Undefined();
}

// ── Pitch Detection (polled) ──────────────────────────────────────────────────

// Load the Basic Pitch ONNX model for the polyphonic ML note detector.
// Called once at startup by audio-bridge.ts with the bundled model path.
// Never throws. Returns "is ML note detection available after this call" —
// a model is loaded with a valid contract. A missing/invalid file does NOT
// tear down an already-loaded model, so it can still return true; it returns
// false when the engine isn't ready or ONNX support isn't compiled in, and
// the engine then keeps using the YIN PitchDetector / ChordScorer
// (Constitution VII).
static Napi::Value LoadNoteModel(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsString())
        return Napi::Boolean::New(env, false);

    const auto path = info[0].As<Napi::String>().Utf8Value();
    const bool ok = liveEngine->loadNoteModel(juce::File(juce::String(path)));
    return Napi::Boolean::New(env, ok);
}

// Whether the ML note detector is active (ONNX support compiled in AND a
// model loaded). Lets the renderer / tests tell the ML path from the YIN
// fallback without inferring it from behaviour.
static Napi::Value IsMlNoteDetection(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    // Report readiness, not just model-loaded: the engine only routes
    // getPitchDetection()/scoreChord() to ML once the detector has published
    // its first snapshot (isReady()). Reporting true during the cold-start
    // window would tell the renderer "ML active" while it's still getting the
    // YIN fallback.
    auto liveEngine = snapshotEngine();
    return Napi::Boolean::New(env,
        liveEngine && liveEngine->hasMlNoteDetector()
              && liveEngine->getMlNoteDetector().isReady());
}

// Raw polyphonic transcription from the ML note detector — the full set of
// currently-active pitches, not just the dominant one. Returns
// `{ notes: [{ midi, confidence, onsetMs, onsetSeq }], sampleRate }`, or null when the ML
// detector isn't active (no model / ONNX support) so the renderer can feature-
// detect and fall back. Never throws.
static Napi::Value DetectNotes(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    // Gate on isReady(): the contract is that callers get null whenever the
    // ML detector isn't actively producing notes. isReady() is false with no
    // model, after a device stop, and during the cold-start window before the
    // first inference publishes — so the renderer feature-detects correctly
    // and falls back instead of consuming an empty ML stream.
    auto liveEngine = snapshotEngine();
    if (!liveEngine || !liveEngine->getMlNoteDetector().isReady())
        return env.Null();

    const auto active = liveEngine->getMlNoteDetector().getActiveNotes();
    auto notesArr = Napi::Array::New(env, active.size());
    for (size_t i = 0; i < active.size(); ++i)
    {
        auto entry = Napi::Object::New(env);
        entry.Set("midi", active[i].midi);
        entry.Set("confidence", active[i].confidence);
        // Milliseconds since this pitch's onset — lets the renderer back-date
        // a detection to the true onset instead of poll time.
        entry.Set("onsetMs", active[i].onsetAgeMs);
        // Monotonic per-pitch onset counter — a change means a new note was
        // struck, so the renderer can consume onsets as discrete events.
        entry.Set("onsetSeq", active[i].onsetSeq);
        notesArr.Set((uint32_t) i, entry);
    }

    auto obj = Napi::Object::New(env);
    obj.Set("notes", notesArr);
    // Normalise the sample rate: getCurrentSampleRate() is 0 when no audio
    // device is active — hand the renderer a sane positive value so its
    // Hz/time math can't divide by zero.
    const double sr = liveEngine->getCurrentSampleRate();
    obj.Set("sampleRate", sr > 0.0 ? sr : 48000.0);
    return obj;
}

static Napi::Value GetPitchDetection(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    auto liveEngine = snapshotEngine();

    if (liveEngine)
    {
        // getActiveDetection() returns the polyphonic ML detector's dominant
        // pitch when a Basic Pitch model is loaded, else the YIN detector's
        // latest result — same shape either way, so the plugin is unchanged.
        auto det = liveEngine->getActiveDetection();
        obj.Set("frequency", det.frequency);
        obj.Set("confidence", det.confidence);
        obj.Set("midiNote", det.midiNote);
        obj.Set("cents", det.cents);
        obj.Set("noteName", det.noteName.toStdString());
    }
    else
    {
        obj.Set("frequency", -1.0);
        obj.Set("confidence", 0.0);
        obj.Set("midiNote", -1);
        obj.Set("cents", 0.0);
        obj.Set("noteName", "");
    }

    return obj;
}

// Score a polyphonic chord against the engine's most recent input
// samples. Renderer (notedetect plugin's matchNotes chord branch)
// supplies the chord context — chart notes plus tuning/arrangement
// metadata — and gets back a `{score, hitStrings, totalStrings, isHit,
// results[]}` object identical in shape to what the JS implementation
// produced. Audio never crosses the N-API boundary, which is the
// whole reason for moving the math here: constitution II says audio
// analysis lives in JUCE, and this is the missing piece.
//
// Request shape. Fields marked `required` must be present and
// internally consistent — the C++ scorer fails closed (all-miss
// result with one entry per requested note) when the validation
// invariants don't hold, rather than silently substituting defaults.
// {
//   notes: [{ s, f, ho?, po?, b?, sl?, hm? }, ...],
//                                   // required, each `s` must be in [0, stringCount)
//   arrangement?: 'guitar'|'bass',  // default 'guitar' — must be one of these two strings
//   stringCount?: number,           // default 6 — must match the (arrangement, stringCount)
//                                   //  table: bass{4,5} or guitar{6,7,8}
//   offsets: number[],              // required, length must equal stringCount.
//                                   //  Pass an array of zeros for standard tuning;
//                                   //  the default of `stringCount = 6` only works
//                                   //  if you supply 6 offsets.
//   numSamples?: number,            // analysis window (default 4096, capped at the
//                                   //  engine input-ring capacity, currently 8192)
//   capo?: number,                  // default 0
//   pitchCheckCents?: number,       // 0 = energy-only chord check (default 0)
//   minHitRatio?: number,           // default 0.6
//   bypassMl?: boolean,             // force the DSP band-energy scorer even
//                                   //  when an ML model is loaded (default false)
//   harmonicVerify?: boolean,       // score each note by harmonic-comb energy
//                                   //  (f,2f..5f vs the floor between) instead
//                                   //  of band-energy/total (default false)
//   harmonicSnr?: number,           // min harmonic-to-floor ratio for a hit
//                                   //  when harmonicVerify is set (default 3.0)
// }
static Napi::Value ScoreChord(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();

    // Hard caps on caller-controlled array lengths. The scorer's
    // (arrangement, stringCount) validation only accepts up to 8
    // strings; chord-notes have a natural ceiling at the same value
    // (one per string). 32 is a generous headroom that still bounds
    // worst-case allocations the renderer could trigger over IPC —
    // without these limits, a malformed/malicious payload claiming a
    // gigantic JS array length would force a multi-GB reserve before
    // the scorer's own validation rejected the request. A request
    // that exceeds either cap is treated as outright malformed and
    // returns the "no chord requested" failure shape (totalStrings=0);
    // every other validation failure goes through the all-miss path
    // below so results[] stays in lockstep with notes[].
    static constexpr uint32_t kMaxOffsets = 32;
    static constexpr uint32_t kMaxNotes = 32;

    auto noRequestFailure = [&env]() {
        auto failure = Napi::Object::New(env);
        failure.Set("score", 0.0);
        failure.Set("hitStrings", 0);
        failure.Set("totalStrings", 0);
        failure.Set("isHit", false);
        failure.Set("results", Napi::Array::New(env, 0));
        return failure;
    };

    if (!liveEngine || info.Length() < 1 || !info[0].IsObject())
        return noRequestFailure();

    auto reqObj = info[0].As<Napi::Object>();

    // Capture the notes array up front so every downstream failure
    // path can build a per-note all-miss result aligned 1:1 with the
    // caller's notes[]. Pre-cap check happens before we even read the
    // length into the helper to prevent a payload claiming an enormous
    // length from forcing the helper to allocate a huge results array.
    Napi::Value notesVal = reqObj.Has("notes") ? reqObj.Get("notes") : env.Null();
    if (!notesVal.IsArray()) return noRequestFailure();
    auto notesArr = notesVal.As<Napi::Array>();
    if (notesArr.Length() > kMaxNotes) return noRequestFailure();
    const uint32_t noteCount = notesArr.Length();

    // All-miss result aligned with the caller's notes[]. Walks the
    // original JS array so the per-note `s` / `f` echo back in the
    // result even when the request fails validation (lets the renderer
    // distinguish "this string missed" from "this string wasn't sent").
    // Used by every failure path below except the cap/no-notes case
    // above, which doesn't have a coherent notes[] to mirror.
    auto buildAllMiss = [&]() {
        auto resultsArr = Napi::Array::New(env, noteCount);
        for (uint32_t i = 0; i < noteCount; ++i)
        {
            int s = -1, f = -1;
            auto v = notesArr.Get(i);
            if (v.IsObject())
            {
                auto o = v.As<Napi::Object>();
                if (o.Has("s") && o.Get("s").IsNumber())
                    s = o.Get("s").As<Napi::Number>().Int32Value();
                if (o.Has("f") && o.Get("f").IsNumber())
                    f = o.Get("f").As<Napi::Number>().Int32Value();
            }
            auto entry = Napi::Object::New(env);
            entry.Set("s", s);
            entry.Set("f", f);
            entry.Set("hit", false);
            entry.Set("bandEnergy", 0.0);
            entry.Set("centsDiff", env.Null());
            entry.Set("centsError", env.Null());
            resultsArr.Set(i, entry);
        }
        auto out = Napi::Object::New(env);
        out.Set("score", 0.0);
        out.Set("hitStrings", 0);
        out.Set("totalStrings", (int) noteCount);
        out.Set("isHit", false);
        out.Set("results", resultsArr);
        return out;
    };

    ChordScorer::Request req;
    if (reqObj.Has("numSamples") && reqObj.Get("numSamples").IsNumber())
        req.numSamples = reqObj.Get("numSamples").As<Napi::Number>().Int32Value();
    if (reqObj.Has("arrangement") && reqObj.Get("arrangement").IsString())
        req.arrangement = reqObj.Get("arrangement").As<Napi::String>().Utf8Value();
    if (reqObj.Has("stringCount") && reqObj.Get("stringCount").IsNumber())
        req.stringCount = reqObj.Get("stringCount").As<Napi::Number>().Int32Value();
    if (reqObj.Has("capo") && reqObj.Get("capo").IsNumber())
        req.capo = reqObj.Get("capo").As<Napi::Number>().Int32Value();
    if (reqObj.Has("pitchCheckCents") && reqObj.Get("pitchCheckCents").IsNumber())
        req.pitchCheckCents = reqObj.Get("pitchCheckCents").As<Napi::Number>().FloatValue();
    if (reqObj.Has("minHitRatio") && reqObj.Get("minHitRatio").IsNumber())
        req.minHitRatio = reqObj.Get("minHitRatio").As<Napi::Number>().FloatValue();
    if (reqObj.Has("bypassMl") && reqObj.Get("bypassMl").IsBoolean())
        req.bypassMl = reqObj.Get("bypassMl").As<Napi::Boolean>().Value();
    if (reqObj.Has("harmonicVerify") && reqObj.Get("harmonicVerify").IsBoolean())
        req.harmonicVerify = reqObj.Get("harmonicVerify").As<Napi::Boolean>().Value();
    if (reqObj.Has("harmonicSnr") && reqObj.Get("harmonicSnr").IsNumber())
        req.harmonicSnr = reqObj.Get("harmonicSnr").As<Napi::Number>().FloatValue();

    if (reqObj.Has("offsets") && reqObj.Get("offsets").IsArray())
    {
        auto arr = reqObj.Get("offsets").As<Napi::Array>();
        if (arr.Length() > kMaxOffsets) return noRequestFailure();
        req.tuningOffsets.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
            auto v = arr.Get(i);
            // Tuning offsets materially shift expected pitch — silently
            // substituting 0 for a missing/non-numeric entry would
            // produce confidently wrong scores. Fail closed with the
            // per-note all-miss shape so the renderer sees the right
            // results[] length even when the request is malformed.
            if (!v.IsNumber()) return buildAllMiss();
            req.tuningOffsets.push_back(v.As<Napi::Number>().Int32Value());
        }
    }

    req.notes.reserve(noteCount);
    for (uint32_t i = 0; i < noteCount; ++i)
    {
        auto v = notesArr.Get(i);
        // For malformed entries (non-object, or missing/non-numeric
        // s/f) push a sentinel Note with string = -1. This keeps
        // req.notes.size() in lockstep with the incoming notes[]
        // length AND guarantees ChordScorer's range check
        // (`n.string < 0 || n.string >= stringCount`) trips on the
        // sentinel — yielding the same all-miss fail-closed result
        // the shape contract advertises, never a false hit on the
        // default low-string position.
        ChordScorer::Note n{};
        n.string = -1;
        n.fret = -1;
        if (!v.IsObject())
        {
            req.notes.push_back(n);
            continue;
        }
        auto noteObj = v.As<Napi::Object>();
        const bool hasS = noteObj.Has("s") && noteObj.Get("s").IsNumber();
        const bool hasF = noteObj.Has("f") && noteObj.Get("f").IsNumber();
        if (!hasS || !hasF)
        {
            req.notes.push_back(n);
            continue;
        }
        n.string = noteObj.Get("s").As<Napi::Number>().Int32Value();
        n.fret = noteObj.Get("f").As<Napi::Number>().Int32Value();
        // Technique flags are truthy/falsy in JS; coerce to bool
        // here so an unset value cleanly becomes false.
        auto truthy = [&noteObj](const char* key) {
            if (!noteObj.Has(key)) return false;
            auto val = noteObj.Get(key);
            return val.ToBoolean().Value();
        };
        n.hammerOn = truthy("ho");
        n.pullOff = truthy("po");
        n.bend = truthy("b");
        n.slide = truthy("sl");
        n.harmonic = truthy("hm");
        req.notes.push_back(n);
    }

    auto result = liveEngine->scoreChord(req);

    auto out = Napi::Object::New(env);
    out.Set("score", result.score);
    out.Set("hitStrings", result.hitStrings);
    out.Set("totalStrings", result.totalStrings);
    out.Set("isHit", result.isHit);
    auto resultsArr = Napi::Array::New(env, result.results.size());
    for (size_t i = 0; i < result.results.size(); ++i)
    {
        const auto& r = result.results[i];
        auto entry = Napi::Object::New(env);
        entry.Set("s", r.string);
        entry.Set("f", r.fret);
        entry.Set("hit", r.hit);
        entry.Set("bandEnergy", r.bandEnergy);
        // Mirror the JS result shape: when cents weren't measured the
        // fields are present-but-null so the renderer can distinguish
        // "no pitch check ran" (null) from "pitch check said 0"
        // (numeric 0).
        if (r.hasCents)
        {
            entry.Set("centsDiff", r.centsDiff);
            entry.Set("centsError", r.centsError);
        }
        else
        {
            entry.Set("centsDiff", env.Null());
            entry.Set("centsError", env.Null());
        }
        resultsArr.Set(i, entry);
    }
    out.Set("results", resultsArr);
    return out;
}

// Push the song's note chart into the engine for continuous, background
// verification. The notedetect plugin calls this once per arrangement load;
// the engine's NoteVerifier thread then scores each note's timing window
// against the live playhead and input ring, so the renderer no longer runs a
// per-tick scoreChord IPC loop (which starved during dense passages).
//
// Expected payload:
// {
//   arrangement?: 'guitar'|'bass',  // default 'guitar'
//   stringCount?: number,           // default 6
//   tuningOffsets: number[],        // length should equal stringCount
//   capo?: number,                  // default 0
//   pitchCheckCents?: number,       // default 0 (energy-only)
//   harmonicSnr?: number,           // default 3.0
//   timingTolerance?: number,       // seconds, default 0.1
//   notes: [{ id:string, t:number, s:number, f:number, sus:number,
//             ho?,po?,b?,sl?,hm?:boolean }, ...]
// }
// Returns true when the chart was accepted, false on a malformed payload or
// when no engine exists.
static Napi::Value SetChart(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();

    // Generous cap on the chart length — a full song's note list is well
    // under this, but it bounds the worst-case allocation a malformed payload
    // (claiming a gigantic JS array length) could force over IPC.
    static constexpr uint32_t kMaxChartNotes = 8192;

    // Rejecting a malformed chart must also drop whatever chart the verifier
    // currently holds — otherwise a failed (re)load leaves the previous
    // song's chart active and getNoteVerdicts() keeps emitting stale verdicts.
    auto reject = [&]() -> Napi::Value {
        if (liveEngine) liveEngine->clearChart();
        return Napi::Boolean::New(info.Env(), false);
    };

    if (!liveEngine || info.Length() < 1 || !info[0].IsObject())
        return reject();

    auto reqObj = info[0].As<Napi::Object>();

    NoteVerifier::ChartUpdate chart;
    if (reqObj.Has("arrangement") && reqObj.Get("arrangement").IsString())
        chart.arrangement = reqObj.Get("arrangement").As<Napi::String>().Utf8Value();
    if (reqObj.Has("stringCount") && reqObj.Get("stringCount").IsNumber())
        chart.stringCount = reqObj.Get("stringCount").As<Napi::Number>().Int32Value();
    if (reqObj.Has("capo") && reqObj.Get("capo").IsNumber())
        chart.capo = reqObj.Get("capo").As<Napi::Number>().Int32Value();
    if (reqObj.Has("pitchCheckCents") && reqObj.Get("pitchCheckCents").IsNumber())
        chart.pitchCheckCents = reqObj.Get("pitchCheckCents").As<Napi::Number>().FloatValue();
    if (reqObj.Has("harmonicSnr") && reqObj.Get("harmonicSnr").IsNumber())
        chart.harmonicSnr = reqObj.Get("harmonicSnr").As<Napi::Number>().FloatValue();
    if (reqObj.Has("timingTolerance") && reqObj.Get("timingTolerance").IsNumber())
        chart.timingTolerance = reqObj.Get("timingTolerance").As<Napi::Number>().DoubleValue();

    if (reqObj.Has("tuningOffsets") && reqObj.Get("tuningOffsets").IsArray())
    {
        auto arr = reqObj.Get("tuningOffsets").As<Napi::Array>();
        if (arr.Length() > 32) return reject();
        chart.tuningOffsets.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
            auto v = arr.Get(i);
            if (!v.IsNumber()) return reject();
            chart.tuningOffsets.push_back(v.As<Napi::Number>().Int32Value());
        }
    }

    // ChordScorer requires exactly one tuning offset per string and otherwise
    // fails every note closed. Reject the chart here so a malformed payload
    // surfaces as setChart() == false rather than a silently all-miss session
    // the caller believes loaded fine.
    if ((int) chart.tuningOffsets.size() != chart.stringCount)
        return reject();

    Napi::Value notesVal = reqObj.Has("notes") ? reqObj.Get("notes") : env.Null();
    if (!notesVal.IsArray()) return reject();
    auto notesArr = notesVal.As<Napi::Array>();
    if (notesArr.Length() > kMaxChartNotes) return reject();

    chart.notes.reserve(notesArr.Length());
    for (uint32_t i = 0; i < notesArr.Length(); ++i)
    {
        auto v = notesArr.Get(i);
        if (!v.IsObject()) return reject();
        auto noteObj = v.As<Napi::Object>();

        // Every chart note must carry all five required fields with the right
        // type. Filling defaults for a missing field would push a bogus
        // time-0 note with an empty id — that breaks verdict-by-id alignment
        // — so reject the whole chart instead.
        const bool validNote =
            noteObj.Has("id")  && noteObj.Get("id").IsString() &&
            noteObj.Has("t")   && noteObj.Get("t").IsNumber() &&
            noteObj.Has("s")   && noteObj.Get("s").IsNumber() &&
            noteObj.Has("f")   && noteObj.Get("f").IsNumber() &&
            noteObj.Has("sus") && noteObj.Get("sus").IsNumber();
        if (!validNote) return reject();

        NoteVerifier::ChartNote n{};
        n.id = noteObj.Get("id").As<Napi::String>().Utf8Value();
        n.t = noteObj.Get("t").As<Napi::Number>().DoubleValue();
        n.string = noteObj.Get("s").As<Napi::Number>().Int32Value();
        n.fret = noteObj.Get("f").As<Napi::Number>().Int32Value();
        n.sus = noteObj.Get("sus").As<Napi::Number>().DoubleValue();
        auto truthy = [&noteObj](const char* key) {
            if (!noteObj.Has(key)) return false;
            return noteObj.Get(key).ToBoolean().Value();
        };
        n.ho = truthy("ho");
        n.po = truthy("po");
        n.b  = truthy("b");
        n.sl = truthy("sl");
        n.hm = truthy("hm");
        chart.notes.push_back(std::move(n));
    }

    liveEngine->setChart(chart);
    return Napi::Boolean::New(env, true);
}

// Drain the verdicts the NoteVerifier thread has finalized since the last
// call. Returns an array of { id, detected, detectedSongTime, centsError, snr }.
//
// Optionally also pushes the renderer's playhead: getNoteVerdicts(songTime,
// playing). The plugin calls this once per detect tick, so folding the push in
// here advances the verifier's clock without a second IPC round-trip. A
// downlevel caller passing no args still just drains.
static Napi::Value GetNoteVerdicts(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    // Null (not an empty array) on a missing engine — the bridge/preload
    // contract treats null as "unsupported/unavailable" so the renderer
    // feature-detects, matching detectNotes' no-engine path.
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();

    // Push the playhead before draining so this tick's verdicts reflect it.
    // A JS NaN/Infinity passes IsNumber() — guard with isfinite so a bad
    // value can't corrupt the verifier's interpolated timing.
    if (info.Length() >= 2 && info[0].IsNumber() && info[1].IsBoolean())
    {
        const double songTime = info[0].As<Napi::Number>().DoubleValue();
        if (std::isfinite(songTime))
            liveEngine->setPlayhead(songTime, info[1].As<Napi::Boolean>().Value());
    }

    const auto verdicts = liveEngine->getNoteVerdicts();
    auto arr = Napi::Array::New(env, verdicts.size());
    for (size_t i = 0; i < verdicts.size(); ++i)
    {
        const auto& v = verdicts[i];
        auto entry = Napi::Object::New(env);
        entry.Set("id", v.id);
        entry.Set("detected", v.detected);
        entry.Set("detectedSongTime", v.detectedSongTime);
        entry.Set("centsError", v.centsError);
        entry.Set("snr", v.snr);
        arr.Set((uint32_t) i, entry);
    }
    return arr;
}

// Sample rate the audio device is running at. Notedetect's chord scorer
// needs this to map FFT bins to Hz; on the bridge path there's no
// AudioContext to read it from. Falls back to 48000 if the engine isn't
// ready (matches the historical fallback in screen.js) — and also if
// the engine is initialized but no device is currently active, which
// pins currentSampleRate to 0 internally and would otherwise propagate
// a divide-by-zero into the renderer's FFT-bin→Hz math.
static Napi::Value GetSampleRate(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    constexpr double kFallbackSampleRate = 48000.0;
    auto liveEngine = snapshotEngine();
    if (!liveEngine)
        return Napi::Number::New(env, kFallbackSampleRate);
    const double sr = liveEngine->getCurrentSampleRate();
    if (!std::isfinite(sr) || sr <= 0.0)
        return Napi::Number::New(env, kFallbackSampleRate);
    return Napi::Number::New(env, sr);
}

// ── VST Plugin Scanning ──────────────────────────────────────────────────────

class ScanPluginsWorker : public Napi::AsyncWorker
{
public:
    ScanPluginsWorker(Napi::Env env, Napi::Promise::Deferred deferred, juce::StringArray dirs)
        : Napi::AsyncWorker(env), deferred(deferred), directories(std::move(dirs)) {}

    void Execute() override
    {
        auto host = snapshotVstHost();
        if (!host) return;
        host->scanDirectories(directories, [](float, const juce::String&) {});
    }

    void OnOK() override
    {
        auto env = Env();
        auto result = Napi::Array::New(env);

        if (auto host = snapshotVstHost())
        {
            auto plugins = host->getKnownPlugins();
            for (int i = 0; i < plugins.size(); ++i)
            {
                auto obj = Napi::Object::New(env);
                obj.Set("name", plugins[i].name.toStdString());
                obj.Set("manufacturer", plugins[i].manufacturer.toStdString());
                obj.Set("category", plugins[i].category.toStdString());
                obj.Set("format", plugins[i].formatName.toStdString());
                obj.Set("path", plugins[i].fileOrIdentifier.toStdString());
                obj.Set("uid", plugins[i].uid.toStdString());
                obj.Set("isInstrument", plugins[i].isInstrument);
                result.Set((uint32_t)i, obj);
            }
        }

        deferred.Resolve(result);
    }

    void OnError(const Napi::Error& error) override
    {
        deferred.Reject(error.Value());
    }

private:
    Napi::Promise::Deferred deferred;
    juce::StringArray directories;
};

static Napi::Value ScanPlugins(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    juce::StringArray dirs;
    if (info.Length() > 0 && info[0].IsArray())
    {
        auto arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i)
            dirs.add(juce::String(arr.Get(i).As<Napi::String>().Utf8Value()));
    }
    else
    {
        dirs = VSTHost::getDefaultScanDirectories();
    }

    auto worker = new ScanPluginsWorker(env, deferred, dirs);
    worker->Queue();
    return deferred.Promise();
}

static Napi::Value GetKnownPlugins(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto result = Napi::Array::New(env);

    if (auto host = snapshotVstHost())
    {
        auto plugins = host->getKnownPlugins();
        for (int i = 0; i < plugins.size(); ++i)
        {
            auto obj = Napi::Object::New(env);
            obj.Set("name", plugins[i].name.toStdString());
            obj.Set("manufacturer", plugins[i].manufacturer.toStdString());
            obj.Set("category", plugins[i].category.toStdString());
            obj.Set("format", plugins[i].formatName.toStdString());
            obj.Set("path", plugins[i].fileOrIdentifier.toStdString());
            obj.Set("uid", plugins[i].uid.toStdString());
            obj.Set("isInstrument", plugins[i].isInstrument);
            result.Set((uint32_t)i, obj);
        }
    }

    return result;
}

static Napi::Value SavePluginList(const Napi::CallbackInfo& info)
{
    if (info.Length() == 0) return info.Env().Undefined();
    if (auto host = snapshotVstHost())
        host->savePluginList(juce::File(juce::String(info[0].As<Napi::String>().Utf8Value())));
    return info.Env().Undefined();
}

static Napi::Value LoadPluginList(const Napi::CallbackInfo& info)
{
    if (info.Length() == 0) return info.Env().Undefined();
    if (auto host = snapshotVstHost())
        host->loadPluginList(juce::File(juce::String(info[0].As<Napi::String>().Utf8Value())));
    return info.Env().Undefined();
}

// Register the plugins the renderer's VST crash guard recorded as having
// crashed the app on a previous run. shouldSandbox() then routes them through
// the out-of-process sandbox. Expects a single array-of-strings argument.
static Napi::Value SetCrashedPlugins(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    juce::StringArray paths;
    if (info.Length() > 0 && info[0].IsArray())
    {
        auto arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
            auto value = arr.Get(i);
            if (value.IsString())
                paths.add(juce::String(value.As<Napi::String>().Utf8Value()));
        }
    }
    slopsmith::sandbox::setCrashedPlugins(paths);
    return env.Undefined();
}

// ── Signal Chain Management ──────────────────────────────────────────────────

// Pending in-process loads: each LoadVSTWorker / LoadPresetWorker that's
// currently blocked on `done->wait()` registers its event here. doShutdown
// signals them all so the workers unblock and return a clean "cancelled"
// error instead of hanging forever when the JUCE message thread is about
// to be stopped (and any unfired callback would never arrive).
static std::mutex pendingLoadsMutex;
static std::set<std::shared_ptr<juce::WaitableEvent>> pendingLoads;

static void registerPendingLoad(std::shared_ptr<juce::WaitableEvent> evt)
{
    std::lock_guard<std::mutex> lock(pendingLoadsMutex);
    pendingLoads.insert(std::move(evt));
}

static void unregisterPendingLoad(const std::shared_ptr<juce::WaitableEvent>& evt)
{
    std::lock_guard<std::mutex> lock(pendingLoadsMutex);
    pendingLoads.erase(evt);
}

static void cancelAllPendingLoads()
{
    std::lock_guard<std::mutex> lock(pendingLoadsMutex);
    for (auto& evt : pendingLoads) evt->signal();
    pendingLoads.clear();
}

// Load a VST3, routing it through the out-of-process sandbox when
// shouldSandbox() says so (the filename pre-seed or the runtime crash
// blocklist), otherwise loading it in-process. The in-process load uses
// VSTHost::loadPluginAsync so the JUCE message thread keeps pumping during
// the plugin's init — critical for plugins like AmpliTube that post WM_USER
// / WM_TIMER messages to themselves while initialising. The sync
// createPluginInstance would block the pump, those self-messages would
// queue forever, and the plugin would end up half-wired (a pointer that
// only gets written by a queued message stays null, and the editor crashes
// on its first WindowProc dispatch — the AmpliTube failure signature).
//
// Threading: on !JUCE_MAC must be called from a libuv worker thread (NOT
// the JS main thread, NOT the JUCE message thread) — the done->wait below
// has to be on a thread that *isn't* the one running JUCE's pump or the
// load can't complete. On JUCE_MAC the inline sync fallback is used and
// the caller can be the Node/main thread (which is also JUCE's message
// thread there); LoadVST does exactly that, while LoadPresetWorker still
// hits this from a worker (a pre-existing macOS limitation).
//
// On a *required*-sandbox failure (the plugin matched shouldSandbox but the
// sandbox couldn't spawn) this returns nullptr with `error` set and
// `sandboxRequired` true, so the caller can choose how to surface it —
// LoadVSTWorker throws to JS, LoadPresetWorker just skips the slot.
static std::unique_ptr<juce::AudioProcessor> loadVstSandboxAware(
    const juce::String& pluginPath, double sr, int bs,
    juce::String& error, bool& sandboxRequired)
{
    sandboxRequired = false;

    juce::PluginDescription probeDesc;
    probeDesc.fileOrIdentifier = pluginPath;
    probeDesc.name = juce::File(pluginPath).getFileNameWithoutExtension();

    if (slopsmith::sandbox::shouldSandbox(probeDesc))
    {
        sandboxRequired = true;
        juce::String sandboxErr;
        auto processor = slopsmith::sandbox::tryLoadSandboxed(
            probeDesc, sr, bs, sandboxErr);
        if (!processor)
        {
            error = "sandbox load failed: "
                  + (sandboxErr.isEmpty() ? juce::String("unknown error")
                                          : sandboxErr);
            VST_TRACE("loadVstSandboxAware: sandbox path declined/failed: %s",
                      sandboxErr.toRawUTF8());
        }
        return processor;
    }

   #if JUCE_MAC
    // macOS has no separate JUCE message thread (see startJuceMessageThread /
    // dispatchOnMessageThread): the JUCE MessageManager is bound to the
    // Node/main thread, and dispatchOnMessageThread historically ran inline
    // on the caller. A callAsync + done->wait pattern would queue a callback
    // to a pump that may never run in this calling context.
    //
    // Fall back to the sync loadPlugin, executed on whichever thread called
    // in — the Node/main thread for LoadVST's JUCE_MAC branch (correct: that
    // *is* the MessageManager thread on macOS), or a libuv worker thread for
    // LoadPresetWorker (the pre-existing macOS constraint). Caveat: the
    // existing dispatchOnMessageThread block on macOS already documents
    // that "VST/AU plugin instantiation (which genuinely requires a message
    // thread on macOS) is the one capability we give up until a proper
    // libuv-based pump lands." LoadPresetWorker has called loadVstSandbox-
    // Aware on a worker thread for ages under exactly the same constraint;
    // moving LoadVST to AsyncWorker brings direct loads under the same
    // (pre-existing) limitation. The AmpliTube-class self-message problem
    // this PR targets is Windows-specific (Electron owns the OS main
    // thread, forcing JUCE's MessageManager onto a background thread that
    // createPluginInstance then blocks); macOS doesn't have that mismatch.
    auto host = snapshotVstHost();
    if (! host) { error = "vstHost not initialised"; return nullptr; }
    juce::String err;
    auto instance = host->loadPlugin(pluginPath, sr, bs, err);
    if (! instance) error = err.isNotEmpty() ? err : juce::String("load failed");
    return instance;
   #else
    // In-process: kick off createPluginInstanceAsync on the message thread,
    // block *this* (libuv worker) thread on a WaitableEvent until the load
    // callback fires. The message thread keeps pumping during the wait so
    // the plugin's self-posted init messages dispatch and its state finishes
    // wiring up before the editor is ever opened.
    //
    // All state passed across the thread hop is held by shared_ptr so it
    // outlives the lambda even on an unexpected destructor / scope exit.
    auto instance  = std::make_shared<std::unique_ptr<juce::AudioPluginInstance>>();
    auto loadError = std::make_shared<juce::String>();
    auto done      = std::make_shared<juce::WaitableEvent>();

    // Register BEFORE scheduling so a shutdown that lands between callAsync
    // and the wait below can't miss us — cancelAllPendingLoads would
    // otherwise see an empty set and the worker would block forever.
    registerPendingLoad(done);

    // Check alreadyShutDown after registering to catch the inverse race
    // (shutdown ran before we registered): if it's already set, the
    // shutdown won't see this event and we must bail ourselves.
    if (alreadyShutDown.load(std::memory_order_acquire))
    {
        unregisterPendingLoad(done);
        error = "shutdown in flight";
        return nullptr;
    }

    // Snapshot a shared_ptr to vstHost so the async load and its inner
    // continuation can keep VSTHost (and thus formatManager) alive even if
    // shutdown resets the global mid-load. The inner callback captures the
    // same hostKeeper, so JUCE retains it until createPluginInstanceAsync
    // completes; once the callback destructs, the keeper drops, and if the
    // global has been reset by then the VSTHost destructor runs safely
    // (no work in flight). The snapshot itself goes through vstHostMutex
    // so the shared_ptr copy can't race with shutdown's vstHost.reset().
    auto hostKeeper = snapshotVstHost();

    const bool scheduled = juce::MessageManager::callAsync(
        [hostKeeper, pluginPath, sr, bs, instance, loadError, done]()
        {
            // Shutdown may have fired between callAsync queueing this
            // lambda and the message thread picking it up. Bail before
            // kicking off another in-flight createPluginInstanceAsync
            // that the shutdown would otherwise have to wait on.
            if (alreadyShutDown.load(std::memory_order_acquire))
            {
                *loadError = "shutdown in flight";
                done->signal();
                return;
            }
            if (! hostKeeper)
            {
                *loadError = "vstHost not initialised";
                done->signal();
                return;
            }
            hostKeeper->loadPluginAsync(
                pluginPath, sr, bs,
                [hostKeeper, instance, loadError, done]
                (std::unique_ptr<juce::AudioPluginInstance> inst, juce::String err)
                {
                    *instance  = std::move(inst);
                    *loadError = std::move(err);
                    done->signal();
                });
        });

    if (! scheduled)
    {
        // The message queue is gone (typically: shutdown in flight). The
        // lambda will never run, so done would never signal — surface the
        // failure rather than hanging the worker forever.
        unregisterPendingLoad(done);
        error = "message manager unavailable (shutdown?)";
        return nullptr;
    }

    // No timeout: createPluginInstanceAsync is genuinely async (the message
    // thread keeps pumping), so a slow first-run plugin (e.g. one doing a
    // license check that exceeds 15 s) is allowed to take however long it
    // takes. The old 15-second timeout in dispatchOnMessageThread could
    // return early while the lambda was still running, then the lambda
    // would construct a fully-initialised plugin only for it to immediately
    // destruct because no one held a reference — running VST teardown on
    // the message thread while the user had already moved on. That race is
    // gone with this design.
    //
    // Tradeoff: this call holds a libuv threadpool worker for the duration
    // of the plugin's init. Multiple concurrent hung loads could in theory
    // starve other AsyncWorkers (fs / crypto). In practice plugin loads are
    // user-driven and serialised (LoadPresetWorker loads slots one at a
    // time), and a truly stuck load is bounded by app shutdown via
    // cancelAllPendingLoads. A proper "fire-and-forget with a TSFN
    // completion callback" model would eliminate the block entirely but
    // requires a bigger API restructure than this PR's scope.
    done->wait();
    unregisterPendingLoad(done);

    // Distinguish "shutdown cancelled us before the callback fired"
    // (instance null AND error empty) from a normal load failure (instance
    // null with error set) and a normal success.
    if (! *instance && loadError->isEmpty())
    {
        error = "load cancelled (shutdown)";
        return nullptr;
    }
    error = *loadError;
    return std::move(*instance);
   #endif
}

// AsyncWorker wrapper for LoadVST. Execute() runs on a libuv worker thread,
// so loadVstSandboxAware can block-wait on the async load without freezing
// the JS main thread or deadlocking the JUCE message thread.
class LoadVSTWorker : public Napi::AsyncWorker
{
public:
    LoadVSTWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string path)
        : Napi::AsyncWorker(env)
        , deferred_(deferred)
        , pluginPath_(std::move(path)) {}

    void Execute() override
    {
        // Snapshot engine + vstHost through their mutex-protected helpers so
        // shutdown's reset on the message thread can't race the worker's
        // dereferences below. The shared_ptr locals keep both objects alive
        // for the duration of this worker even if the globals get reset
        // mid-load. The atomic alreadyShutDown gate is the early-out: once
        // it's set, the dispatched reset is on its way and there's no point
        // continuing.
        if (alreadyShutDown.load(std::memory_order_acquire))
        {
            error_ = "shutdown in flight";
            return;
        }
        auto engineKeeper = snapshotEngine();
        auto hostSnap     = snapshotVstHost();
        if (!engineKeeper || !hostSnap)
        {
            error_ = "engine not initialised";
            return;
        }

        const auto sr = engineKeeper->getCurrentSampleRate();
        const auto bs = engineKeeper->getCurrentBlockSize();
        const auto path = juce::String(pluginPath_);
        VST_TRACE("LoadVSTWorker: path='%s' sr=%.0f bs=%d",
                  pluginPath_.c_str(), sr, bs);

        bool sandboxRequired = false;
        juce::String err;
        auto processor = loadVstSandboxAware(path, sr, bs, err, sandboxRequired);

        if (sandboxRequired && !processor)
        {
            // The plugin's on the denylist and the sandbox couldn't spawn —
            // falling back to in-process is what crashed the addon to begin
            // with. Surface as a JS exception (handled in OnOK).
            fprintf(stderr, "[LoadVST] Failed: %s\n", err.toRawUTF8());
            error_ = err;
            sandboxFailed_ = true;
            return;
        }

        if (!processor)
        {
            fprintf(stderr, "[LoadVST] Failed: %s\n", err.toRawUTF8());
            error_ = err;
            return;
        }

        // Engine may have been torn down while we were waiting on the async
        // load. The shared_ptr captures keep `processor` alive; just don't
        // touch a freed engine. The processor destructs cleanly when this
        // scope exits.
        //
        // Gate on alreadyShutDown (atomic, properly synchronised) before the
        // raw engine/vstHost pointer reads — once that flag is set, the
        // dispatched reset of engine/vstHost is on its way and any use of
        // the pointers from this worker thread is racy. The atomic check is
        // the authoritative "should I still be touching engine?" signal.
        if (alreadyShutDown.load(std::memory_order_acquire))
        {
            error_ = "engine torn down during load";
            return;
        }
        // Re-snapshot the engine — the original engineKeeper might have
        // outlived a reset on the message thread, but the AudioEngine
        // we're about to mutate must be the still-installed one. If the
        // global has been reset, the local keeps the old engine alive but
        // we shouldn't be adding slots to it any more.
        auto liveEngine = snapshotEngine();
        if (!liveEngine || !snapshotVstHost())
        {
            error_ = "engine torn down during load";
            return;
        }

        auto name = processor->getName();
        slotId_ = liveEngine->getSignalChain().addProcessor(
            std::move(processor),
            ProcessorSlot::Type::VST,
            name,
            path);
    }

    void OnOK() override
    {
        if (sandboxFailed_)
        {
            // Match the prior LoadVST throw-on-required-sandbox-failure
            // behaviour so renderers' try/catch keeps working.
            deferred_.Reject(
                Napi::Error::New(Env(), error_.toStdString()).Value());
            return;
        }
        deferred_.Resolve(Napi::Number::New(Env(), slotId_));
    }

    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::string pluginPath_;
    int slotId_ = -1;
    bool sandboxFailed_ = false;
    juce::String error_;
};

static Napi::Value LoadVST(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    if (!snapshotEngine() || !snapshotVstHost() || info.Length() < 1)
    {
        deferred.Resolve(Napi::Number::New(env, -1));
        return deferred.Promise();
    }

    auto pluginPath = info[0].As<Napi::String>().Utf8Value();

   #if JUCE_MAC
    // On macOS the JUCE MessageManager is bound to the Node/main thread.
    // Running this as an AsyncWorker would call vstHost->loadPlugin on a
    // libuv worker thread, which JUCE documents as unsupported for VST/AU
    // instantiation. Do the load synchronously on the Node/main thread
    // (same as the pre-PR LoadVST) and return a resolved Promise to match
    // the new signature. Pays the foreground-block cost the AsyncWorker
    // path was supposed to avoid, but that's the existing macOS reality —
    // dispatchOnMessageThread already runs inline there. The async-load
    // motivation (AmpliTube blocking the background JUCE message thread
    // under Electron) is a Windows-only problem.
    // Snapshot once for the whole load so the same AudioEngine is used for
    // the sr/bs reads and the addProcessor mutation, even if shutdown
    // resets the global mid-call.
    auto liveEngine = snapshotEngine();
    if (! liveEngine)
    {
        deferred.Resolve(Napi::Number::New(env, -1));
        return deferred.Promise();
    }
    juce::String error;
    bool sandboxRequired = false;
    auto processor = loadVstSandboxAware(
        juce::String(pluginPath),
        liveEngine->getCurrentSampleRate(),
        liveEngine->getCurrentBlockSize(),
        error, sandboxRequired);

    if (sandboxRequired && !processor)
    {
        fprintf(stderr, "[LoadVST] Failed: %s\n", error.toRawUTF8());
        deferred.Reject(
            Napi::Error::New(env, error.toStdString()).Value());
        return deferred.Promise();
    }

    int slotId = -1;
    if (processor)
    {
        auto name = processor->getName();
        slotId = liveEngine->getSignalChain().addProcessor(
            std::move(processor),
            ProcessorSlot::Type::VST,
            name,
            juce::String(pluginPath));
    }
    else
    {
        fprintf(stderr, "[LoadVST] Failed: %s\n", error.toRawUTF8());
    }
    deferred.Resolve(Napi::Number::New(env, slotId));
    return deferred.Promise();
   #else
    auto* worker = new LoadVSTWorker(env, deferred, std::move(pluginPath));
    worker->Queue();
    return deferred.Promise();
   #endif
}

class LoadNAMWorker : public Napi::AsyncWorker
{
public:
    LoadNAMWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string path)
        : Napi::AsyncWorker(env), deferred_(deferred), modelPath_(std::move(path)) {}

    void Execute() override
    {
        auto liveEngine = snapshotEngine();
        if (!liveEngine) { slotId_ = -1; return; }

        auto processor = std::make_unique<NAMProcessor>();
        if (processor->loadModel(juce::File(juce::String(modelPath_))))
        {
            auto name = processor->getModelName();
            slotId_ = liveEngine->getSignalChain().addProcessor(
                std::move(processor),
                ProcessorSlot::Type::NAM,
                "NAM: " + name,
                juce::String(modelPath_));
        }
    }

    void OnOK() override { deferred_.Resolve(Napi::Number::New(Env(), slotId_)); }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::string modelPath_;
    int slotId_ = -1;
};

static Napi::Value LoadNAMModel(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    if (!snapshotEngine() || info.Length() < 1) {
        deferred.Resolve(Napi::Number::New(env, -1));
        return deferred.Promise();
    }

    auto modelPath = info[0].As<Napi::String>().Utf8Value();
    auto worker = new LoadNAMWorker(env, deferred, modelPath);
    worker->Queue();
    return deferred.Promise();
}

class LoadIRWorker : public Napi::AsyncWorker
{
public:
    LoadIRWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string path)
        : Napi::AsyncWorker(env), deferred_(deferred), irPath_(std::move(path)) {}

    void Execute() override
    {
        auto liveEngine = snapshotEngine();
        if (!liveEngine) { slotId_ = -1; return; }

        const auto sr = liveEngine->getCurrentSampleRate();
        const auto bs = liveEngine->getCurrentBlockSize();
        auto processor = std::make_unique<IRLoader>();
        processor->setPlayConfigDetails(2, 2, sr, bs);
        processor->prepareToPlay(sr, bs);
        if (processor->loadIR(juce::File(juce::String(irPath_))))
        {
            auto name = processor->getIRName();
            slotId_ = liveEngine->getSignalChain().addProcessor(
                    std::move(processor),
                    ProcessorSlot::Type::IR,
                    "IR: " + name,
                    juce::String(irPath_));
        }
    }

    void OnOK() override { deferred_.Resolve(Napi::Number::New(Env(), slotId_)); }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::string irPath_;
    int slotId_ = -1;
};

static Napi::Value LoadIR(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    if (!snapshotEngine() || info.Length() < 1) {
        deferred.Resolve(Napi::Number::New(env, -1));
        return deferred.Promise();
    }

    auto irPath = info[0].As<Napi::String>().Utf8Value();
    auto worker = new LoadIRWorker(env, deferred, irPath);
    worker->Queue();
    return deferred.Promise();
}

static Napi::Value RemoveProcessor(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0)
    {
        int slotId = info[0].As<Napi::Number>().Int32Value();
        liveEngine->getSignalChain().removeProcessor(slotId);
    }
    return info.Env().Undefined();
}

static Napi::Value MoveProcessor(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2)
    {
        int from = info[0].As<Napi::Number>().Int32Value();
        int to = info[1].As<Napi::Number>().Int32Value();
        liveEngine->getSignalChain().moveProcessor(from, to);
    }
    return info.Env().Undefined();
}

static Napi::Value SetBypass(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2)
    {
        int slotId = info[0].As<Napi::Number>().Int32Value();
        bool bypassed = info[1].As<Napi::Boolean>().Value();
        liveEngine->getSignalChain().setBypass(slotId, bypassed);
    }
    return info.Env().Undefined();
}

static Napi::Value ClearChain(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine()) liveEngine->getSignalChain().clear();
    return info.Env().Undefined();
}

// ── Chain State ───────────────────────────────────────────────────────────────

static Napi::Value GetChainState(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto result = Napi::Array::New(env);
    auto liveEngine = snapshotEngine();

    if (liveEngine)
    {
        auto slots = liveEngine->getSignalChain().getAllSlots();
        for (int i = 0; i < slots.size(); ++i)
        {
            auto obj = Napi::Object::New(env);
            obj.Set("id", slots[i]->id);
            obj.Set("type", (int)slots[i]->type);
            obj.Set("name", slots[i]->name.toStdString());
            obj.Set("path", slots[i]->path.toStdString());
            obj.Set("bypassed", slots[i]->bypassed);
            obj.Set("hasEditor", slots[i]->processor && slots[i]->processor->hasEditor());
            result.Set((uint32_t)i, obj);
        }
    }

    return result;
}

// ── Plugin Editor Window ──────────────────────────────────────────────────────

class PluginEditorWindow;
static std::map<int, std::unique_ptr<PluginEditorWindow>> editorWindows;

class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow(juce::AudioProcessorEditor* ed, const juce::String& title)
        : DocumentWindow(title, juce::Colours::darkgrey, DocumentWindow::closeButton)
    {
        setContentOwned(ed, true);
        setResizable(true, false);
        setUsingNativeTitleBar(true);
        centreWithSize(ed->getWidth(), ed->getHeight());
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override
    {
        // Remove from map so editor can be reopened
        for (auto it = editorWindows.begin(); it != editorWindows.end(); ++it)
        {
            if (it->second.get() == this)
            {
                auto slotId = it->first;
                juce::MessageManager::callAsync([slotId]() {
                    editorWindows.erase(slotId);
                });
                break;
            }
        }
        setVisible(false);
    }
};

static Napi::Value OpenPluginEditor(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1)
        return Napi::Boolean::New(env, false);

    int slotId = info[0].As<Napi::Number>().Int32Value();

    // If already open, bring to front
    auto it = editorWindows.find(slotId);
    if (it != editorWindows.end() && it->second)
    {
        if (it->second->isVisible())
        {
            it->second->toFront(true);
            return Napi::Boolean::New(env, true);
        }
        // Window was hidden/closed, remove stale entry
        editorWindows.erase(it);
    }

    auto slot = liveEngine->getSignalChain().getSlot(slotId);
    if (!slot || !slot->processor || !slot->processor->hasEditor())
        return Napi::Boolean::New(env, false);

    // Create editor on the message thread
    auto* processor = slot->processor.get();
    auto name = slot->name;

    juce::MessageManager::callAsync([processor, name, slotId]()
    {
        juce::AudioProcessorEditor* editor = nullptr;
        try {
            editor = processor->createEditorAndMakeActive();
        } catch (const std::exception& e) {
            fprintf(stderr, "[AudioEngine] createEditorAndMakeActive crashed for '%s': %s\n", name.toRawUTF8(), e.what());
        } catch (...) {
            fprintf(stderr, "[AudioEngine] createEditorAndMakeActive crashed for '%s': unknown error\n", name.toRawUTF8());
        }
        if (editor)
        {
            editorWindows[slotId] = std::make_unique<PluginEditorWindow>(editor, name);
            fprintf(stderr, "[AudioEngine] Opened editor for slot %d: %s (%dx%d)\n",
                    slotId, name.toRawUTF8(), editor->getWidth(), editor->getHeight());
        }
    });

    return Napi::Boolean::New(env, true);
}

static Napi::Value ClosePluginEditor(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (info.Length() < 1) return Napi::Boolean::New(env, false);

    int slotId = info[0].As<Napi::Number>().Int32Value();
    auto it = editorWindows.find(slotId);
    if (it != editorWindows.end())
    {
        juce::MessageManager::callAsync([slotId]()
        {
            editorWindows.erase(slotId);
        });
        return Napi::Boolean::New(env, true);
    }
    return Napi::Boolean::New(env, false);
}

// ── Parameters ────────────────────────────────────────────────────────────────

static Napi::Value GetParameters(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1) return Napi::Array::New(env);

    int slotId = info[0].As<Napi::Number>().Int32Value();
    auto params = liveEngine->getSignalChain().getParameters(slotId);
    auto result = Napi::Array::New(env, params.size());

    for (int i = 0; i < params.size(); ++i)
    {
        auto obj = Napi::Object::New(env);
        obj.Set("index", params[i].index);
        obj.Set("name", params[i].name.toStdString());
        obj.Set("value", params[i].value);
        obj.Set("label", params[i].label.toStdString());
        obj.Set("text", params[i].text.toStdString());
        result.Set((uint32_t)i, obj);
    }

    return result;
}

static Napi::Value SetParameter(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 3)
    {
        int slotId = info[0].As<Napi::Number>().Int32Value();
        int paramIdx = info[1].As<Napi::Number>().Int32Value();
        float value = info[2].As<Napi::Number>().FloatValue();
        liveEngine->getSignalChain().setParameter(slotId, paramIdx, value);
    }
    return info.Env().Undefined();
}

// Restore a VST slot's full state from a base64 getStateInformation() blob.
static Napi::Value SetSlotState(const Napi::CallbackInfo& info)
{
    // Type-guard both args (NAPI_DISABLE_CPP_EXCEPTIONS): a malformed IPC
    // payload is a clean no-op rather than a hard addon failure.
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2 && info[0].IsNumber() && info[1].IsString())
    {
        int slotId = info[0].As<Napi::Number>().Int32Value();
        auto base64 = info[1].As<Napi::String>().Utf8Value();
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding(juce::String(base64)) && mb.getSize() > 0)
            liveEngine->getSignalChain().setSlotState(slotId, mb);
    }
    return info.Env().Undefined();
}

// ── MIDI ──────────────────────────────────────────────────────────────────────

static Napi::Value SendMidiToSlot(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 4)
        return Napi::Boolean::New(env, false);

    int slotId = info[0].As<Napi::Number>().Int32Value();
    int msgType = info[1].As<Napi::Number>().Int32Value();
    int channel = info[2].As<Napi::Number>().Int32Value();

    juce::MidiMessage midiMsg;
    if (msgType == 0) // Program Change
    {
        int program = info[3].As<Napi::Number>().Int32Value();
        midiMsg = juce::MidiMessage::programChange(channel, program);
    }
    else if (msgType == 1) // Control Change
    {
        int controller = info[3].As<Napi::Number>().Int32Value();
        int value = info.Length() > 4 ? info[4].As<Napi::Number>().Int32Value() : 0;
        midiMsg = juce::MidiMessage::controllerEvent(channel, controller, value);
    }
    else
        return Napi::Boolean::New(env, false);

    liveEngine->getSignalChain().queueMidiMessage(slotId, midiMsg);
    return Napi::Boolean::New(env, true);
}

// ── Backing Track ─────────────────────────────────────────────────────────────

static Napi::Value LoadBackingTrack(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1) return Napi::Boolean::New(env, false);

    auto path = info[0].As<Napi::String>().Utf8Value();
    bool result = liveEngine->loadBackingTrack(juce::File(juce::String(path)));
    return Napi::Boolean::New(env, result);
}

static Napi::Value StartBacking(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine()) liveEngine->startBacking();
    return info.Env().Undefined();
}

static Napi::Value StopBacking(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine()) liveEngine->stopBacking();
    return info.Env().Undefined();
}

static Napi::Value SeekBacking(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0)
        liveEngine->setBackingPosition(info[0].As<Napi::Number>().DoubleValue());
    return info.Env().Undefined();
}

static Napi::Value GetBackingPosition(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    double pos = liveEngine ? liveEngine->getBackingPosition() : 0.0;
    return Napi::Number::New(info.Env(), pos);
}

static Napi::Value GetBackingDuration(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    double dur = liveEngine ? liveEngine->getBackingDuration() : 0.0;
    return Napi::Number::New(info.Env(), dur);
}

static Napi::Value IsBackingPlaying(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    bool playing = liveEngine ? liveEngine->isBackingPlaying() : false;
    return Napi::Boolean::New(info.Env(), playing);
}

// ── Presets ───────────────────────────────────────────────────────────────────

static Napi::Value SavePreset(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();
    auto json = liveEngine->getSignalChain().savePreset();
    return Napi::String::New(env, json.toStdString());
}

class LoadPresetWorker : public Napi::AsyncWorker
{
public:
    LoadPresetWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string json)
        : Napi::AsyncWorker(env), deferred_(deferred), presetJson_(std::move(json)) {}

    void Execute() override
    {
        auto liveEngine = snapshotEngine();
        if (!liveEngine) { success_ = false; error_ = "No engine"; return; }

        auto parsed = juce::JSON::parse(juce::String(presetJson_));
        if (!parsed.isObject()) { success_ = false; error_ = "Invalid JSON"; return; }

        auto* root = parsed.getDynamicObject();
        if (!root) { success_ = false; error_ = "Invalid preset"; return; }

        auto chainVar = root->getProperty("chain");
        auto* chainArray = chainVar.getArray();
        if (!chainArray) { success_ = false; error_ = "No chain array"; return; }

        // Clear existing chain
        liveEngine->getSignalChain().clear();

        double sr = liveEngine->getCurrentSampleRate();
        int bs = liveEngine->getCurrentBlockSize();

        for (auto& slotVar : *chainArray)
        {
            auto* slotObj = slotVar.getDynamicObject();
            if (!slotObj) continue;

            int type = (int)slotObj->getProperty("type");
            auto name = slotObj->getProperty("name").toString();
            auto path = slotObj->getProperty("path").toString();
            bool bypassed = (bool)slotObj->getProperty("bypassed");
            auto stateB64 = slotObj->getProperty("state").toString();

            std::unique_ptr<juce::AudioProcessor> processor;

            if (type == (int)ProcessorSlot::Type::VST && snapshotVstHost())
            {
                // Sandbox-aware load: a crash-blocklisted plugin restored
                // from a preset must still go out-of-process, otherwise the
                // "one crash, then always sandbox" contract is defeated.
                juce::String err;
                bool sandboxRequired = false;
                processor = loadVstSandboxAware(path, sr, bs, err, sandboxRequired);
                if (!processor)
                {
                    fprintf(stderr, "[LoadPreset] VST load failed: %s (%s)\n",
                            name.toRawUTF8(), err.toRawUTF8());
                    continue;
                }
            }
            else if (type == (int)ProcessorSlot::Type::NAM)
            {
                auto nam = std::make_unique<NAMProcessor>();
                if (!nam->loadModel(juce::File(path)))
                {
                    fprintf(stderr, "[LoadPreset] NAM load failed: %s\n", path.toRawUTF8());
                    continue;
                }
                processor = std::move(nam);
            }
            else if (type == (int)ProcessorSlot::Type::IR)
            {
                auto ir = std::make_unique<IRLoader>();
                ir->setPlayConfigDetails(2, 2, sr, bs);
                ir->prepareToPlay(sr, bs);
                if (!ir->loadIR(juce::File(path)))
                {
                    fprintf(stderr, "[LoadPreset] IR load failed: %s\n", path.toRawUTF8());
                    continue;
                }
                processor = std::move(ir);
            }
            else continue;

            int slotId = liveEngine->getSignalChain().addProcessor(
                std::move(processor),
                (ProcessorSlot::Type)type,
                name, path);

            if (bypassed && slotId >= 0)
                liveEngine->getSignalChain().setBypass(slotId, true);

            // Restore processor state
            if (stateB64.isNotEmpty() && slotId >= 0)
            {
                juce::MemoryBlock state;
                if (state.fromBase64Encoding(stateB64))
                {
                    auto* slot = const_cast<ProcessorSlot*>(liveEngine->getSignalChain().getSlot(slotId));
                    if (slot) slot->setState(state);
                }
            }

            slotsLoaded_++;
        }

        success_ = true;
    }

    void OnOK() override
    {
        auto obj = Napi::Object::New(Env());
        obj.Set("success", success_);
        obj.Set("slotsLoaded", slotsLoaded_);
        if (!success_) obj.Set("error", error_);
        deferred_.Resolve(obj);
    }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::string presetJson_;
    bool success_ = false;
    std::string error_;
    int slotsLoaded_ = 0;
};

static Napi::Value LoadPreset(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    auto liveEngine = snapshotEngine();

    if (!liveEngine || info.Length() < 1) {
        auto obj = Napi::Object::New(env);
        obj.Set("success", false);
        obj.Set("error", "No engine or missing argument");
        deferred.Resolve(obj);
        return deferred.Promise();
    }

    auto json = info[0].As<Napi::String>().Utf8Value();
    auto worker = new LoadPresetWorker(env, deferred, json);
    worker->Queue();
    return deferred.Promise();
}

static Napi::Value SetMultiBypass(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsArray())
        return Napi::Boolean::New(env, false);

    auto arr = info[0].As<Napi::Array>();
    juce::Array<std::pair<int, bool>> changes;

    for (uint32_t i = 0; i < arr.Length(); i++)
    {
        auto item = arr.Get(i).As<Napi::Object>();
        int slotId = item.Get("slotId").As<Napi::Number>().Int32Value();
        bool bypassed = item.Get("bypassed").As<Napi::Boolean>().Value();
        changes.add({ slotId, bypassed });
    }

    liveEngine->getSignalChain().setMultiBypass(changes);
    return Napi::Boolean::New(env, true);
}

// ── Debug file logging ────────────────────────────────────────────────────────

// Redirect the C runtime's stderr stream to a file so the native
// [AudioEngine] / [audio-native] diagnostics are captured for a bug report on
// machines with no console (packaged Windows builds). Only invoked when
// SLOPSMITH_DEBUG is set. Returns "" on success, or an error description the
// JS layer logs as an [audio] line.
//
// freopen (not dup2): a packaged GUI-subsystem app has no console, so stderr
// has no valid fd — dup2 onto fileno(stderr) fails. freopen reassigns the
// stream itself and works with or without a console. freopen would close
// stderr before trying the path, so a bad path is ruled out FIRST with a
// throwaway fopen probe (which never touches stderr); only once the path is
// known-writable do we freopen. Append mode so the JS layer's header
// survives; unbuffered so a crash leaves a complete tail.
static Napi::Value EnableFileLogging(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsString())
    {
        Napi::TypeError::New(env, "enableFileLogging(path) requires a string")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

#if defined(_WIN32)
    // Widen UTF-16 → wchar_t by value-converting each code unit (not a
    // reinterpret_cast — char16_t and wchar_t are distinct types even though
    // both are 16-bit on Windows). Wide path so a profile dir with non-ASCII
    // characters isn't mangled by the ANSI codepage (cf. src/vst-host/main.cpp,
    // which uses the GetEnvironmentVariableW / _wfopen wide path for the same
    // reason).
    const std::u16string u16 = info[0].As<Napi::String>().Utf16Value();
    const std::wstring wpath(u16.begin(), u16.end());
    FILE* probe = _wfopen(wpath.c_str(), L"a");
#else
    const std::string path = info[0].As<Napi::String>().Utf8Value();
    FILE* probe = std::fopen(path.c_str(), "a");
#endif
    if (probe == nullptr)
    {
        // Capture errno before Napi::String::New / std::to_string, which may
        // call library code that clobbers it.
        const int e = errno;
        return Napi::String::New(env, std::string("fopen failed (errno=")
                                      + std::to_string(e) + ")");
    }
    std::fclose(probe);  // path is writable; stderr never touched on this path

#if defined(_WIN32)
    FILE* fp = _wfreopen(wpath.c_str(), L"a", stderr);
#else
    FILE* fp = std::freopen(path.c_str(), "a", stderr);
#endif
    if (fp == nullptr)
    {
        const int e = errno;
        // freopen closes stderr before trying the path; on failure it's left
        // closed. The probe just verified the path, so this is near-impossible
        // — but redirect stderr to the null device so it's a valid sink rather
        // than a closed stream that could trip later fprintf(stderr) calls.
#if defined(_WIN32)
        std::freopen("NUL", "w", stderr);
#else
        std::freopen("/dev/null", "w", stderr);
#endif
        return Napi::String::New(env, std::string("freopen failed (errno=")
                                      + std::to_string(e) + ")");
    }

    // Unbuffered: each [AudioEngine] fprintf hits disk immediately, so a
    // crash mid-reconfigure still leaves the diagnostic line that explains it.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "[audio-native] file logging enabled\n");
    return Napi::String::New(env, "");  // empty = success
}

// ── Module Registration ───────────────────────────────────────────────────────

static Napi::Object InitModule(Napi::Env env, Napi::Object exports)
{
    // Lifecycle
    exports.Set("init", Napi::Function::New(env, Init));
    exports.Set("shutdown", Napi::Function::New(env, Shutdown));
    exports.Set("enableFileLogging", Napi::Function::New(env, EnableFileLogging));

    // Devices
    exports.Set("getDeviceTypes", Napi::Function::New(env, GetDeviceTypes));
    exports.Set("getSampleRates", Napi::Function::New(env, GetSampleRates));
    exports.Set("getBufferSizes", Napi::Function::New(env, GetBufferSizes));
    exports.Set("probeDeviceOptions", Napi::Function::New(env, ProbeDeviceOptions));
    exports.Set("getCurrentDevice", Napi::Function::New(env, GetCurrentDevice));
    exports.Set("setDeviceType", Napi::Function::New(env, SetDeviceType));
    exports.Set("setDevice", Napi::Function::New(env, SetDevice));

    // Audio control
    exports.Set("startAudio", Napi::Function::New(env, StartAudio));
    exports.Set("stopAudio", Napi::Function::New(env, StopAudio));
    exports.Set("isAudioRunning", Napi::Function::New(env, IsAudioRunning));

    // Gain
    exports.Set("setGain", Napi::Function::New(env, SetGain));
    exports.Set("setInputChannel", Napi::Function::New(env, SetInputChannel));
    exports.Set("setMonitorMute", Napi::Function::New(env, SetMonitorMute));
    exports.Set("setMonitorMuteSuppressed", Napi::Function::New(env, SetMonitorMuteSuppressed));
    exports.Set("isMonitorMuted", Napi::Function::New(env, IsMonitorMuted));
    exports.Set("setNoiseGate", Napi::Function::New(env, SetNoiseGate));
    exports.Set("setTonePolish", Napi::Function::New(env, SetTonePolish));

    // Metering
    exports.Set("getLevels", Napi::Function::New(env, GetLevels));
    exports.Set("resetPeaks", Napi::Function::New(env, ResetPeaks));

    // Pitch detection
    exports.Set("getPitchDetection", Napi::Function::New(env, GetPitchDetection));
    exports.Set("scoreChord", Napi::Function::New(env, ScoreChord));
    exports.Set("setChart", Napi::Function::New(env, SetChart));
    exports.Set("getNoteVerdicts", Napi::Function::New(env, GetNoteVerdicts));
    exports.Set("getSampleRate", Napi::Function::New(env, GetSampleRate));
    exports.Set("loadNoteModel", Napi::Function::New(env, LoadNoteModel));
    exports.Set("isMlNoteDetection", Napi::Function::New(env, IsMlNoteDetection));
    exports.Set("detectNotes", Napi::Function::New(env, DetectNotes));

    // VST scanning
    exports.Set("scanPlugins", Napi::Function::New(env, ScanPlugins));
    exports.Set("getKnownPlugins", Napi::Function::New(env, GetKnownPlugins));
    exports.Set("savePluginList", Napi::Function::New(env, SavePluginList));
    exports.Set("loadPluginList", Napi::Function::New(env, LoadPluginList));
    exports.Set("setCrashedPlugins", Napi::Function::New(env, SetCrashedPlugins));

    // Signal chain
    exports.Set("loadVST", Napi::Function::New(env, LoadVST));
    exports.Set("loadNAMModel", Napi::Function::New(env, LoadNAMModel));
    exports.Set("loadIR", Napi::Function::New(env, LoadIR));
    exports.Set("removeProcessor", Napi::Function::New(env, RemoveProcessor));
    exports.Set("moveProcessor", Napi::Function::New(env, MoveProcessor));
    exports.Set("setBypass", Napi::Function::New(env, SetBypass));
    exports.Set("clearChain", Napi::Function::New(env, ClearChain));
    exports.Set("getChainState", Napi::Function::New(env, GetChainState));
    exports.Set("openPluginEditor", Napi::Function::New(env, OpenPluginEditor));
    exports.Set("closePluginEditor", Napi::Function::New(env, ClosePluginEditor));

    // Parameters
    exports.Set("getParameters", Napi::Function::New(env, GetParameters));
    exports.Set("setParameter", Napi::Function::New(env, SetParameter));
    exports.Set("setSlotState", Napi::Function::New(env, SetSlotState));

    // MIDI
    exports.Set("sendMidiToSlot", Napi::Function::New(env, SendMidiToSlot));

    // Backing track
    exports.Set("loadBackingTrack", Napi::Function::New(env, LoadBackingTrack));
    exports.Set("startBacking", Napi::Function::New(env, StartBacking));
    exports.Set("stopBacking", Napi::Function::New(env, StopBacking));
    exports.Set("seekBacking", Napi::Function::New(env, SeekBacking));
    exports.Set("getBackingPosition", Napi::Function::New(env, GetBackingPosition));
    exports.Set("getBackingDuration", Napi::Function::New(env, GetBackingDuration));
    exports.Set("isBackingPlaying", Napi::Function::New(env, IsBackingPlaying));

    // Presets
    exports.Set("savePreset", Napi::Function::New(env, SavePreset));
    exports.Set("loadPreset", Napi::Function::New(env, LoadPreset));
    exports.Set("setMultiBypass", Napi::Function::New(env, SetMultiBypass));

    // Drain JUCE message thread + sandbox subprocesses before DLL unload, so a
    // JS process exit without an explicit addon.shutdown() doesn't crash in
    // static destructors.
    napi_add_env_cleanup_hook(env, [](void*) { doShutdown(); }, nullptr);

    return exports;
}

NODE_API_MODULE(slopsmith_audio, InitModule)
