// Slopsmith Audio Engine — Node.js Native Addon (N-API)
// Bridges the JUCE-based C++ audio engine to Electron via node-addon-api.
// All audio processing happens in C++; JS communicates via IPC.

#include <napi.h>
#include <thread>
#include <atomic>

#include "AudioEngine.h"
#include "VSTHost.h"
#include "NAMProcessor.h"
#include "IRLoader.h"

#include <juce_events/juce_events.h>

static std::unique_ptr<AudioEngine> engine;
static std::unique_ptr<VSTHost> vstHost;
static std::thread juceMessageThread;
static std::atomic<bool> juceRunning{false};

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
    juce::WaitableEvent done;
    juce::MessageManager::callAsync([&]() {
        func();
        done.signal();
    });
    done.wait(15000);
#endif
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

static Napi::Value Init(const Napi::CallbackInfo& info)
{
    auto env = info.Env();

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

static Napi::Value Shutdown(const Napi::CallbackInfo& info)
{
    dispatchOnMessageThread([]() {
        if (engine) { engine->stopAudio(); engine.reset(); }
        vstHost.reset();
    });

    stopJuceMessageThread();
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

// Returns the most recent `numSamples` (default 4096) of mono input audio
// from the engine's lock-free ring buffer as a Float32Array. Renderer uses
// this to feed notedetect's polyphonic chord scorer (_ndScoreChord), which
// runs on raw audio frames rather than the monophonic pitch detector output.
// The buffer is owned by the JS realm post-return, so no lifetime gymnastics.
static Napi::Value GetInputFrame(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    int numSamples = 4096;
    if (info.Length() > 0 && info[0].IsNumber())
        numSamples = info[0].As<Napi::Number>().Int32Value();
    if (!engine)
        return Napi::Float32Array::New(env, 0);

    auto frame = engine->getInputFrame(numSamples);
    const size_t byteLength = frame.size() * sizeof(float);
    auto ab = Napi::ArrayBuffer::New(env, byteLength);
    if (byteLength > 0)
        std::memcpy(ab.Data(), frame.data(), byteLength);
    return Napi::Float32Array::New(env, frame.size(), ab, 0);
}

// Sample rate the audio device is running at. Notedetect's chord scorer
// needs this to map FFT bins to Hz; on the bridge path there's no
// AudioContext to read it from. Falls back to 48000 if the engine isn't
// ready (matches the historical fallback in screen.js).
static Napi::Value GetSampleRate(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (!engine)
        return Napi::Number::New(env, 48000.0);
    return Napi::Number::New(env, engine->getCurrentSampleRate());
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
    auto instance = vstHost->loadPlugin(
        juce::String(pluginPath),
        engine->getCurrentSampleRate(),
        engine->getCurrentBlockSize(),
        error);

    if (instance)
    {
        auto name = instance->getName();
        slotId = engine->getSignalChain().addProcessor(
            std::move(instance),
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
                processor = vstHost->loadPlugin(path, sr, bs, err);
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
    exports.Set("getInputFrame", Napi::Function::New(env, GetInputFrame));
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

    return exports;
}

NODE_API_MODULE(slopsmith_audio, InitModule)
