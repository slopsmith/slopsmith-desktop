// Slopsmith Audio Engine — Node.js Native Addon (N-API)
// Bridges the JUCE-based C++ audio engine to Electron via node-addon-api.
// All audio processing happens in C++; JS communicates via IPC.

#include <napi.h>
#include <thread>
#include <atomic>
#include <cstring>
#include <cmath>

#include "AudioEngine.h"
#include "VSTHost.h"
#include "VSTTrace.h"
#include "NAMProcessor.h"
#include "IRLoader.h"
#include "Sandbox/SandboxedProcessor.h"

#include <juce_events/juce_events.h>

static std::unique_ptr<AudioEngine> engine;
static std::unique_ptr<VSTHost> vstHost;
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
        engine = std::make_unique<AudioEngine>();
        vstHost = std::make_unique<VSTHost>();

        auto types = engine->getDeviceTypes();
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

    if (juceRunning.load() || engine || vstHost)
    {
        dispatchOnMessageThread([]() {
            if (engine) { engine->stopAudio(); engine.reset(); }
            vstHost.reset();
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
    if (!engine) return env.Null();

    // Device types are already scanned during init — safe to read from any thread
    auto types = engine->getDeviceTypes();

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
    if (!engine) return Napi::Array::New(env);

    auto rates = engine->getSampleRates();
    auto result = Napi::Array::New(env, rates.size());
    for (int i = 0; i < rates.size(); ++i)
        result.Set((uint32_t)i, rates[i]);
    return result;
}

static Napi::Value GetBufferSizes(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine) return Napi::Array::New(env);

    auto sizes = engine->getBufferSizes();
    auto result = Napi::Array::New(env, sizes.size());
    for (int i = 0; i < sizes.size(); ++i)
        result.Set((uint32_t)i, sizes[i]);
    return result;
}

static Napi::Value ProbeDeviceOptions(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
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
    if (!engine)
    {
        obj.Set("error", "Audio engine not initialized");
        return obj;
    }

    auto options = engine->probeDeviceOptions(juce::String(type), juce::String(input), juce::String(output));
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
    if (!engine) return env.Null();

    auto obj = Napi::Object::New(env);
    obj.Set("type", engine->getCurrentDeviceType().toStdString());
    obj.Set("input", engine->getCurrentInputDevice().toStdString());
    obj.Set("output", engine->getCurrentOutputDevice().toStdString());
    obj.Set("sampleRate", engine->getCurrentSampleRate());
    obj.Set("blockSize", engine->getCurrentBlockSize());
    obj.Set("latencyMs", engine->getLatencyMs());
    return obj;
}

// ── Device Selection ──────────────────────────────────────────────────────────

static Napi::Value SetDeviceType(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine || info.Length() < 1) return Napi::Boolean::New(env, false);

    auto typeName = info[0].As<Napi::String>().Utf8Value();
    bool result = engine->setDeviceType(juce::String(typeName));
    return Napi::Boolean::New(env, result);
}

static Napi::Value SetDevice(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine) return Napi::Boolean::New(env, false);

    auto input = info.Length() > 0 && !info[0].IsNull() ? info[0].As<Napi::String>().Utf8Value() : "";
    auto output = info.Length() > 1 && !info[1].IsNull() ? info[1].As<Napi::String>().Utf8Value() : "";
    double sr = info.Length() > 2 && !info[2].IsUndefined() ? info[2].As<Napi::Number>().DoubleValue() : 48000.0;
    int bs = info.Length() > 3 && !info[3].IsUndefined() ? info[3].As<Napi::Number>().Int32Value() : 256;

    // Must run on the main thread — JUCE's ALSA backend deadlocks if called from a worker
    bool result = engine->setAudioDevice(juce::String(input), juce::String(output), sr, bs);
    return Napi::Boolean::New(env, result);
}

// ── Audio Control ─────────────────────────────────────────────────────────────

static Napi::Value StartAudio(const Napi::CallbackInfo& info)
{
    if (engine) engine->startAudio();
    return info.Env().Undefined();
}

static Napi::Value StopAudio(const Napi::CallbackInfo& info)
{
    if (engine) engine->stopAudio();
    return info.Env().Undefined();
}

static Napi::Value IsAudioRunning(const Napi::CallbackInfo& info)
{
    return Napi::Boolean::New(info.Env(), engine ? engine->isAudioRunning() : false);
}

// ── Gain ──────────────────────────────────────────────────────────────────────

static Napi::Value SetGain(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine || info.Length() < 2) return env.Undefined();

    auto which = info[0].As<Napi::String>().Utf8Value();
    float value = info[1].As<Napi::Number>().FloatValue();

    if (which == "input") engine->setInputGain(value);
    else if (which == "output") engine->setOutputGain(value);
    else if (which == "backing") engine->setBackingVolume(value);

    return env.Undefined();
}

static Napi::Value SetInputChannel(const Napi::CallbackInfo& info)
{
    if (engine && info.Length() > 0)
        engine->setInputChannel(info[0].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

static Napi::Value SetMonitorMute(const Napi::CallbackInfo& info)
{
    if (engine && info.Length() > 0)
        engine->setMonitorMute(info[0].As<Napi::Boolean>().Value());
    return info.Env().Undefined();
}

static Napi::Value SetNoiseGate(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine || info.Length() < 1 || !info[0].IsObject())
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

    engine->setNoiseGate(enabled, thresholdDb, releaseMs, depthDb);
    return env.Undefined();
}

static Napi::Value IsMonitorMuted(const Napi::CallbackInfo& info)
{
    return Napi::Boolean::New(info.Env(), engine ? engine->isMonitorMuted() : true);
}

// ── Metering (polled — read atomics) ──────────────────────────────────────────

static Napi::Value GetLevels(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);

    if (engine)
    {
        obj.Set("inputLevel", engine->getInputLevel());
        obj.Set("outputLevel", engine->getOutputLevel());
        obj.Set("inputPeak", engine->getInputPeak());
        obj.Set("outputPeak", engine->getOutputPeak());
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
    if (engine) engine->resetPeaks();
    return info.Env().Undefined();
}

// ── Pitch Detection (polled) ──────────────────────────────────────────────────

static Napi::Value GetPitchDetection(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);

    if (engine)
    {
        auto det = engine->getPitchDetector().getLatestDetection();
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
// }
static Napi::Value ScoreChord(const Napi::CallbackInfo& info)
{
    auto env = info.Env();

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

    if (!engine || info.Length() < 1 || !info[0].IsObject())
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

    auto result = engine->scoreChord(req);

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
    if (!engine)
        return Napi::Number::New(env, kFallbackSampleRate);
    const double sr = engine->getCurrentSampleRate();
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
        if (!vstHost) return;
        vstHost->scanDirectories(directories, [](float, const juce::String&) {});
    }

    void OnOK() override
    {
        auto env = Env();
        auto result = Napi::Array::New(env);

        if (vstHost)
        {
            auto plugins = vstHost->getKnownPlugins();
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

    if (vstHost)
    {
        auto plugins = vstHost->getKnownPlugins();
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
    if (vstHost && info.Length() > 0)
        vstHost->savePluginList(juce::File(juce::String(info[0].As<Napi::String>().Utf8Value())));
    return info.Env().Undefined();
}

static Napi::Value LoadPluginList(const Napi::CallbackInfo& info)
{
    if (vstHost && info.Length() > 0)
        vstHost->loadPluginList(juce::File(juce::String(info[0].As<Napi::String>().Utf8Value())));
    return info.Env().Undefined();
}

// ── Signal Chain Management ──────────────────────────────────────────────────

static Napi::Value LoadVST(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine || !vstHost || info.Length() < 1)
        return Napi::Number::New(env, -1);

    auto pluginPath = info[0].As<Napi::String>().Utf8Value();
    int slotId = -1;

    juce::String error;
    std::unique_ptr<juce::AudioProcessor> processor;
    auto sr = engine->getCurrentSampleRate();
    auto bs = engine->getCurrentBlockSize();
    VST_TRACE("LoadVST: path='%s' sr=%.0f bs=%d", pluginPath.c_str(), sr, bs);

    // 1) Try the out-of-process sandbox path for plugins on the denylist.
    //    This is a no-op on macOS/Linux until those sandbox PRs land — the
    //    stub factory returns nullptr and we fall through.
    bool sandboxRequired = false;
    juce::String sandboxErr;
    {
        juce::PluginDescription probeDesc;
        probeDesc.fileOrIdentifier = juce::String(pluginPath);
        // We haven't scanned the plugin yet, so manufacturer/UID matching
        // isn't available. shouldSandbox() falls back to a filename heuristic
        // for the NI denylist (Guitar Rig / Massive / Kontakt / …).
        probeDesc.name = juce::File(juce::String(pluginPath)).getFileNameWithoutExtension();
        if (slopsmith::sandbox::shouldSandbox(probeDesc))
        {
            sandboxRequired = true;
            processor = slopsmith::sandbox::tryLoadSandboxed(
                probeDesc, sr, bs, sandboxErr);
            if (!processor)
                VST_TRACE("LoadVST: sandbox path declined/failed: %s",
                          sandboxErr.toRawUTF8());
        }
    }

    // 2) If the plugin is on the denylist, sandboxing is *required* — falling
    //    back to in-process is what crashed the addon to begin with (the
    //    motivation for the denylist). Surface the failure to the caller.
    if (sandboxRequired && !processor)
    {
        error = "sandbox load failed: "
              + (sandboxErr.isEmpty() ? juce::String("unknown error") : sandboxErr);
        // Mirror the in-process load's stderr format so a JS test harness or
        // Electron renderer sees the same diagnostics regardless of path.
        fprintf(stderr, "[LoadVST] Failed: %s\n", error.toRawUTF8());
        // Throw a Napi::Error so the JS caller gets the actual sandbox-spawn
        // diagnostic instead of an opaque -1. Renderers can `try/catch
        // addon.loadVST(...)` and surface `err.message` directly to the
        // user; legacy callers that ignore exceptions still see the
        // historical -1 return because Napi::Error.ThrowAsJavaScriptException
        // unwinds back to JS where the function call evaluates to undefined,
        // which they were already expected to handle.
        Napi::Error::New(env, error.toStdString())
            .ThrowAsJavaScriptException();
        return Napi::Number::New(env, -1);
    }

    // 3) Otherwise: in-process JUCE load (today's path). Instantiate on the
    //    JUCE message thread for the COM-apartment reason documented above.
    if (!processor)
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        dispatchOnMessageThread([&]() {
            instance = vstHost->loadPlugin(juce::String(pluginPath), sr, bs, error);
        });
        processor = std::move(instance);
    }

    if (processor)
    {
        auto name = processor->getName();
        slotId = engine->getSignalChain().addProcessor(
            std::move(processor),
            ProcessorSlot::Type::VST,
            name,
            juce::String(pluginPath));
    }
    else
        fprintf(stderr, "[LoadVST] Failed: %s\n", error.toRawUTF8());

    return Napi::Number::New(env, slotId);
}

class LoadNAMWorker : public Napi::AsyncWorker
{
public:
    LoadNAMWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string path)
        : Napi::AsyncWorker(env), deferred_(deferred), modelPath_(std::move(path)) {}

    void Execute() override
    {
        if (!engine) { slotId_ = -1; return; }

        auto processor = std::make_unique<NAMProcessor>();
        if (processor->loadModel(juce::File(juce::String(modelPath_))))
        {
            auto name = processor->getModelName();
            slotId_ = engine->getSignalChain().addProcessor(
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

    if (!engine || info.Length() < 1) {
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
        if (!engine) { slotId_ = -1; return; }

        auto processor = std::make_unique<IRLoader>();
        processor->setPlayConfigDetails(2, 2, engine->getCurrentSampleRate(), engine->getCurrentBlockSize());
        processor->prepareToPlay(engine->getCurrentSampleRate(), engine->getCurrentBlockSize());
        if (processor->loadIR(juce::File(juce::String(irPath_))))
        {
            auto name = processor->getIRName();
            slotId_ = engine->getSignalChain().addProcessor(
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

    if (!engine || info.Length() < 1) {
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
    if (engine && info.Length() > 0)
    {
        int slotId = info[0].As<Napi::Number>().Int32Value();
        engine->getSignalChain().removeProcessor(slotId);
    }
    return info.Env().Undefined();
}

static Napi::Value MoveProcessor(const Napi::CallbackInfo& info)
{
    if (engine && info.Length() >= 2)
    {
        int from = info[0].As<Napi::Number>().Int32Value();
        int to = info[1].As<Napi::Number>().Int32Value();
        engine->getSignalChain().moveProcessor(from, to);
    }
    return info.Env().Undefined();
}

static Napi::Value SetBypass(const Napi::CallbackInfo& info)
{
    if (engine && info.Length() >= 2)
    {
        int slotId = info[0].As<Napi::Number>().Int32Value();
        bool bypassed = info[1].As<Napi::Boolean>().Value();
        engine->getSignalChain().setBypass(slotId, bypassed);
    }
    return info.Env().Undefined();
}

static Napi::Value ClearChain(const Napi::CallbackInfo& info)
{
    if (engine) engine->getSignalChain().clear();
    return info.Env().Undefined();
}

// ── Chain State ───────────────────────────────────────────────────────────────

static Napi::Value GetChainState(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto result = Napi::Array::New(env);

    if (engine)
    {
        auto slots = engine->getSignalChain().getAllSlots();
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
    if (!engine || info.Length() < 1)
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

    auto slot = engine->getSignalChain().getSlot(slotId);
    if (!slot || !slot->processor || !slot->processor->hasEditor())
        return Napi::Boolean::New(env, false);

    // Create editor on the message thread
    auto* processor = slot->processor.get();
    auto name = slot->name;

    juce::MessageManager::callAsync([processor, name, slotId]()
    {
        juce::AudioProcessorEditor* editor = nullptr;
        try {
            editor = processor->createEditor();
        } catch (const std::exception& e) {
            fprintf(stderr, "[AudioEngine] createEditor crashed for '%s': %s\n", name.toRawUTF8(), e.what());
        } catch (...) {
            fprintf(stderr, "[AudioEngine] createEditor crashed for '%s': unknown error\n", name.toRawUTF8());
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
    if (!engine || info.Length() < 1) return Napi::Array::New(env);

    int slotId = info[0].As<Napi::Number>().Int32Value();
    auto params = engine->getSignalChain().getParameters(slotId);
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
    if (engine && info.Length() >= 3)
    {
        int slotId = info[0].As<Napi::Number>().Int32Value();
        int paramIdx = info[1].As<Napi::Number>().Int32Value();
        float value = info[2].As<Napi::Number>().FloatValue();
        engine->getSignalChain().setParameter(slotId, paramIdx, value);
    }
    return info.Env().Undefined();
}

// ── MIDI ──────────────────────────────────────────────────────────────────────

static Napi::Value SendMidiToSlot(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine || info.Length() < 4)
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

    engine->getSignalChain().queueMidiMessage(slotId, midiMsg);
    return Napi::Boolean::New(env, true);
}

// ── Backing Track ─────────────────────────────────────────────────────────────

static Napi::Value LoadBackingTrack(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine || info.Length() < 1) return Napi::Boolean::New(env, false);

    auto path = info[0].As<Napi::String>().Utf8Value();
    bool result = engine->loadBackingTrack(juce::File(juce::String(path)));
    return Napi::Boolean::New(env, result);
}

static Napi::Value StartBacking(const Napi::CallbackInfo& info)
{
    if (engine) engine->startBacking();
    return info.Env().Undefined();
}

static Napi::Value StopBacking(const Napi::CallbackInfo& info)
{
    if (engine) engine->stopBacking();
    return info.Env().Undefined();
}

static Napi::Value SeekBacking(const Napi::CallbackInfo& info)
{
    if (engine && info.Length() > 0)
        engine->setBackingPosition(info[0].As<Napi::Number>().DoubleValue());
    return info.Env().Undefined();
}

static Napi::Value GetBackingPosition(const Napi::CallbackInfo& info)
{
    double pos = engine ? engine->getBackingPosition() : 0.0;
    return Napi::Number::New(info.Env(), pos);
}

static Napi::Value GetBackingDuration(const Napi::CallbackInfo& info)
{
    double dur = engine ? engine->getBackingDuration() : 0.0;
    return Napi::Number::New(info.Env(), dur);
}

static Napi::Value IsBackingPlaying(const Napi::CallbackInfo& info)
{
    bool playing = engine ? engine->isBackingPlaying() : false;
    return Napi::Boolean::New(info.Env(), playing);
}

// ── Presets ───────────────────────────────────────────────────────────────────

static Napi::Value SavePreset(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine) return env.Null();
    auto json = engine->getSignalChain().savePreset();
    return Napi::String::New(env, json.toStdString());
}

class LoadPresetWorker : public Napi::AsyncWorker
{
public:
    LoadPresetWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string json)
        : Napi::AsyncWorker(env), deferred_(deferred), presetJson_(std::move(json)) {}

    void Execute() override
    {
        if (!engine) { success_ = false; error_ = "No engine"; return; }

        auto parsed = juce::JSON::parse(juce::String(presetJson_));
        if (!parsed.isObject()) { success_ = false; error_ = "Invalid JSON"; return; }

        auto* root = parsed.getDynamicObject();
        if (!root) { success_ = false; error_ = "Invalid preset"; return; }

        auto chainVar = root->getProperty("chain");
        auto* chainArray = chainVar.getArray();
        if (!chainArray) { success_ = false; error_ = "No chain array"; return; }

        // Clear existing chain
        engine->getSignalChain().clear();

        double sr = engine->getCurrentSampleRate();
        int bs = engine->getCurrentBlockSize();

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

            if (type == (int)ProcessorSlot::Type::VST && vstHost)
            {
                juce::String err;
                std::unique_ptr<juce::AudioPluginInstance> instance;
                // See LoadVST for why this must run on the JUCE message thread.
                dispatchOnMessageThread([&]() {
                    instance = vstHost->loadPlugin(path, sr, bs, err);
                });
                if (!instance)
                {
                    fprintf(stderr, "[LoadPreset] VST load failed: %s (%s)\n",
                            name.toRawUTF8(), err.toRawUTF8());
                    continue;
                }
                processor = std::move(instance);
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

            int slotId = engine->getSignalChain().addProcessor(
                std::move(processor),
                (ProcessorSlot::Type)type,
                name, path);

            if (bypassed && slotId >= 0)
                engine->getSignalChain().setBypass(slotId, true);

            // Restore processor state
            if (stateB64.isNotEmpty() && slotId >= 0)
            {
                juce::MemoryBlock state;
                if (state.fromBase64Encoding(stateB64))
                {
                    auto* slot = const_cast<ProcessorSlot*>(engine->getSignalChain().getSlot(slotId));
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

    if (!engine || info.Length() < 1) {
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
    if (!engine || info.Length() < 1 || !info[0].IsArray())
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

    engine->getSignalChain().setMultiBypass(changes);
    return Napi::Boolean::New(env, true);
}

// ── Module Registration ───────────────────────────────────────────────────────

static Napi::Object InitModule(Napi::Env env, Napi::Object exports)
{
    // Lifecycle
    exports.Set("init", Napi::Function::New(env, Init));
    exports.Set("shutdown", Napi::Function::New(env, Shutdown));

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
    exports.Set("isMonitorMuted", Napi::Function::New(env, IsMonitorMuted));
    exports.Set("setNoiseGate", Napi::Function::New(env, SetNoiseGate));

    // Metering
    exports.Set("getLevels", Napi::Function::New(env, GetLevels));
    exports.Set("resetPeaks", Napi::Function::New(env, ResetPeaks));

    // Pitch detection
    exports.Set("getPitchDetection", Napi::Function::New(env, GetPitchDetection));
    exports.Set("scoreChord", Napi::Function::New(env, ScoreChord));
    exports.Set("getSampleRate", Napi::Function::New(env, GetSampleRate));

    // VST scanning
    exports.Set("scanPlugins", Napi::Function::New(env, ScanPlugins));
    exports.Set("getKnownPlugins", Napi::Function::New(env, GetKnownPlugins));
    exports.Set("savePluginList", Napi::Function::New(env, SavePluginList));
    exports.Set("loadPluginList", Napi::Function::New(env, LoadPluginList));

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
