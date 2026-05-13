// SandboxedProcessor — a juce::AudioProcessor that forwards every call to a
// separate slopsmith-vst-host.exe subprocess via the IPC protocol defined in
// Protocol.h.
//
// SignalChain stores plugins as `std::unique_ptr<juce::AudioProcessor>`. This
// class makes a sandboxed plugin *mostly* indistinguishable from an in-process
// one from SignalChain's point of view: SignalChain calls processBlock() and
// state methods normally; we marshal everything across the IPC boundary.
//
// Known v1 gaps (tracked as follow-up PRs, see PR-body checklist):
//   * getParameters() returns no juce::AudioProcessorParameter proxies, so
//     parameter automation / UI / preset save round-trip via JUCE's parameter
//     API doesn't reach the sandboxed plugin. The control protocol carries
//     kSetParameter/kListParameters; the proxy layer that maps them onto
//     juce::AudioProcessorParameter is a dedicated follow-up PR.
//   * BusesProperties is hard-coded stereo↔stereo at construction (the
//     numInputs/numOutputs from the ready event are cached but not yet
//     applied — JUCE wants the bus layout at construction time, so dynamic
//     reconfiguration lands with the audio-thread-sync follow-up).
//
// One SandboxedProcessor owns exactly one sandbox subprocess. The subprocess
// dies when the SandboxedProcessor is destroyed.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>

#include "Protocol.h"

namespace slopsmith::sandbox {

class ControlChannel;
class AudioChannel;
class SubprocessHandle;

class SandboxedProcessor final : public juce::AudioProcessor
{
public:
    // Parameters captured at spawn time. The factory fills these from the
    // PluginDescription before construction so we can pass them to the
    // subprocess on its command line.
    struct SpawnConfig
    {
        juce::String pluginPath;       // VST3 file path
        juce::String pluginName;       // for logging
        juce::String sandboxExePath;   // resolved slopsmith-vst-host.exe path
        AudioDimensions audio;         // initial dimensions; can grow via setBlockSize
        int spawnTimeoutMs = kReadyTimeoutMs;
    };

    // Construct + spawn. Returns nullptr on any failure (subprocess start
    // error, control-pipe disconnect, no `ready` within timeout) and writes
    // a descriptive reason into `errorOut`. No exceptions are thrown.
    static std::unique_ptr<SandboxedProcessor> spawn(const SpawnConfig& cfg,
                                                     juce::String& errorOut);

    ~SandboxedProcessor() override;

    // True after `ready` was received and the subprocess accepted the protocol
    // version. False once the subprocess crashes — the audio thread observes
    // this and inserts silence rather than blocking.
    bool isAlive() const noexcept;

    // Callback fired when the subprocess unexpectedly exits or its control
    // pipe breaks. Always invoked from a background thread; mutex-guarded
    // so concurrent assignment from the owner thread + read from the I/O
    // thread don't race on std::function's internal state.
    using CrashCallback = std::function<void(const juce::String& reason)>;
    void setOnCrash(CrashCallback cb);

    // juce::AudioProcessor overrides ────────────────────────────────────────
    const juce::String getName() const override { return spawnName; }
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer& midiMessages) override;

    double getTailLengthSeconds() const override { return 0.0; }
    // Cached-field getters: gate on isAlive() with acquire-semantics so a
    // caller that opportunistically queries before the `ready` handshake
    // completes can't observe uninitialised state. The event-callback
    // populates the cache *before* alive.store(release), so the matching
    // alive.load(acquire) in isAlive() pairs as a synchronizes-with edge.
    bool   acceptsMidi() const override { return isAlive() && acceptsMidiCached; }
    bool   producesMidi() const override { return isAlive() && producesMidiCached; }
    bool   isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return isAlive() && hasEditorCached; }

    // Idempotent — callers don't need to track editor state themselves.
    void notifyEditorClosing();

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Plugin description we synthesised from the `ready` event. Returned by
    // getPluginDescription() so SignalChain can present it like any other
    // plugin.
    juce::PluginDescription getDescription() const
    {
        // Empty description before the ready handshake — better than a
        // torn read of the cached fields.
        return isAlive() ? descriptionCached : juce::PluginDescription{};
    }

private:
    SandboxedProcessor(SpawnConfig cfg);

    bool initialise(juce::String& errorOut);
    void onControlEvent(const juce::String& event, const juce::var& data);
    void teardown(const juce::String& reason);

    SpawnConfig spawnConfig;
    juce::String spawnName;
    juce::PluginDescription descriptionCached;
    bool hasEditorCached = false;
    bool acceptsMidiCached = false;
    bool producesMidiCached = false;
    // Cached from the `ready` event so the deferred BusesProperties refactor
    // can use them without an extra round-trip. Currently informational only
    // (the constructor hard-codes stereo I/O — see PR-body follow-up list).
    int  numInputsCached  = 2;
    int  numOutputsCached = 2;

    std::atomic<bool> alive{false};
    std::atomic<bool> editorOpen{false};
    // Single-fire latch so resource closers only run on the first teardown
    // path that reaches them, even if destructor + watcher onExit fire
    // concurrently from different threads.
    std::atomic<bool> resourcesReleased{false};

    std::unique_ptr<SubprocessHandle> subprocess;
    std::unique_ptr<ControlChannel> control;
    std::unique_ptr<AudioChannel> audio;

    std::mutex   onCrashMutex;
    CrashCallback onCrash;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SandboxedProcessor)
};

// Factory entry point used by NodeAddon / VSTHost::loadPlugin.
//
// Returns the wrapped sandboxed processor when sandboxing is appropriate for
// the given plugin, or nullptr otherwise. The caller should fall back to the
// existing in-process loader on nullptr.
//
// On non-Windows builds this is a no-op that always returns nullptr; the
// existing in-process loader handles all plugins.
std::unique_ptr<juce::AudioProcessor> tryLoadSandboxed(
    const juce::PluginDescription& desc,
    double sampleRate, int blockSize,
    juce::String& errorOut);

// Decision predicate: should this plugin be loaded through the sandbox?
// Currently uses a hard-coded filename heuristic (NI Guitar Rig / Massive /
// Kontakt / ...). The %APPDATA%/Slopsmith/sandbox-list.json override path
// is on the PR-body follow-up checklist; see SandboxFactory_win.cpp.
// Exposed for tests and for the UI to surface "this plugin needs the sandbox"
// status.
bool shouldSandbox(const juce::PluginDescription& desc);

} // namespace slopsmith::sandbox
