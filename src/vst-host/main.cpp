// slopsmith-vst-host.exe — out-of-process VST3 host.
//
// Spawned by slopsmith_audio.node (Slopsmith Desktop) for plugins that don't
// survive in-process loading (notably Qt5-using plugins from Native
// Instruments). Loads exactly one VST3, owns its editor window, and serves
// the host over the IPC protocol defined in ../audio/Sandbox/Protocol.h.
//
// Invariants this binary maintains:
//   * JUCE's MessageManager is created on the OS main thread (this WinMain).
//     This is the crucial property the diag PoC validated; in-process loading
//     in the Node addon cannot guarantee it because V8 owns the main thread.
//   * The plugin's editor is created and reparented on the same main thread.
//   * Audio processing runs on a dedicated worker thread that drains the
//     input ring and writes the output ring; the message thread is free to
//     pump GUI events.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

// `CommandLineToArgvW` / `LocalFree` from shellapi.h come in via JUCE's
// transitive Windows headers below. Including shellapi.h directly here
// triggers a syntax error in the Win11 SDK (10.0.26100.0) when the JUCE
// preamble hasn't run yet, so the transitive dep is intentional.

#if !JUCE_WINDOWS
 #error "slopsmith-vst-host is Windows-only for now."
#endif

#include <windows.h>

#include "../audio/Sandbox/Protocol.h"
#include "../audio/Sandbox/ControlChannel.h"
#include "../audio/Sandbox/AudioChannel.h"
#include "../audio/VSTHost.h"
#include "../audio/VSTTrace.h"

using namespace slopsmith::sandbox;

// Forward decl so the dispatchRequest anonymous-namespace block can log; the
// real definition lives at the bottom of the file with the FILE* it owns.
static void hostLogf(const char* fmt, ...);

namespace {

struct Args
{
    juce::String pluginPath;
    juce::String controlPipe;
    AudioChannel::Names audio;
    int sampleRate = 48000;
    int maxBlock   = 1024;
    int channels   = 2;
};

// Accepts only the space-separated `--key value` form (loop steps i+=2).
// The `--key=value` form is **not** supported: SandboxedProcessor builds the
// command line with separate args (see SandboxedProcessor::initialise's
// args.add() pairs), so adding `--key=value` parsing here would be dead code
// for our own call path; an external caller passing `--sample-rate=48000`
// gets a clear "unknown flag '--sample-rate=48000'" rather than silent
// misparsing.
// Strict positive-integer parse for numeric CLI flags. juce::String
// ::getIntValue() parses a numeric prefix and ignores trailing garbage
// ("512foo" → 512, "48k" → 48), which lets a malformed spawn silently
// mutate runtime audio settings. from_chars + ptr==end requires the
// whole string be a clean integer.
inline bool parseStrictPositiveInt(const juce::String& text, int& dst)
{
    const char* begin = text.toRawUTF8();
    const char* end   = begin + std::strlen(begin);
    if (begin == end) return false;
    int value = 0;
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value <= 0) return false;
    dst = value;
    return true;
}

bool parseArgs(int argc, wchar_t** argv, Args& out, juce::String& whyFailed)
{
    // Track which flags have been set so a duplicate (e.g. from a future
    // spawn-args refactor with a copy-paste bug) errors loudly instead of
    // silently using the last-wins value.
    //
    // Limitations for external callers (not in scope today — binary is
    // host-spawned with controlled args):
    //  - Flag matching is case-sensitive (`--SAMPLE-RATE` would route
    //    through the unknown-flag path).
    //  - On an unrecognized key, the i+=2 stepping consumes the next
    //    arg as `val` before erroring, so the diagnostic line number
    //    points at the value position rather than the flag itself.
    std::set<juce::String> seenKeys;
    for (int i = 1; i < argc; i += 2)
    {
        if (i + 1 >= argc)
        {
            whyFailed = "unpaired key at position " + juce::String(i);
            return false;
        }
        juce::String key(argv[i]);
        juce::String val(argv[i + 1]);
        // Catch the missing-value-followed-by-another-flag case
        // (e.g. `--plugin-path --control-pipe foo`) so the diagnostic
        // points at the actual mistake rather than misparsing the next
        // flag as the value and erroring out later on an unknown key.
        //
        // Limitation: this heuristic also rejects any legitimate value
        // whose first two chars are "--" (e.g. a VST3 path under a
        // directory named "--something"). The binary is only spawned by
        // SandboxedProcessor::initialise with host-built args, so the
        // value side is controlled — if an external caller needs the
        // restriction loosened, switch this to a strict allow-list of
        // known flag names (kFlagNames) rather than a prefix check.
        if (val.startsWith("--"))
        {
            whyFailed = "missing value for " + key + " (next token '"
                      + val + "' looks like a flag)";
            return false;
        }
        if (!seenKeys.insert(key).second)
        {
            whyFailed = "duplicate flag '" + key + "'";
            return false;
        }
        if      (key == "--plugin-path")     out.pluginPath = val;
        else if (key == "--control-pipe")    out.controlPipe = val;
        else if (key == "--audio-shm")       out.audio.shm = val;
        else if (key == "--audio-event-out") out.audio.evtToHost = val;
        else if (key == "--audio-event-in")  out.audio.evtToSandbox = val;
        else if (key == "--sample-rate")
        {
            if (!parseStrictPositiveInt(val, out.sampleRate))
            { whyFailed = "invalid --sample-rate='" + val + "'"; return false; }
        }
        else if (key == "--max-block")
        {
            if (!parseStrictPositiveInt(val, out.maxBlock))
            { whyFailed = "invalid --max-block='" + val + "'"; return false; }
        }
        else if (key == "--channels")
        {
            if (!parseStrictPositiveInt(val, out.channels))
            { whyFailed = "invalid --channels='" + val + "'"; return false; }
        }
        else
        {
            whyFailed = "unknown flag '" + key + "'";
            return false;
        }
    }
    // Per-field validation with specific error reporting so a typo on the
    // spawn command line surfaces immediately instead of as a generic
    // "bad args" log line.
    if (out.pluginPath.isEmpty())        { whyFailed = "missing --plugin-path"; return false; }
    if (out.controlPipe.isEmpty())       { whyFailed = "missing --control-pipe"; return false; }
    if (out.audio.shm.isEmpty())         { whyFailed = "missing --audio-shm"; return false; }
    if (out.audio.evtToHost.isEmpty())   { whyFailed = "missing --audio-event-out"; return false; }
    if (out.audio.evtToSandbox.isEmpty()){ whyFailed = "missing --audio-event-in"; return false; }
    // parseStrictPositiveInt above already rejects zero/negative values, so
    // the `<= 0` checks would never fire — strengthen to sane minimums
    // instead. A `--max-block 1` spawn would technically pass the >0
    // check and produce a 1-sample audio buffer; require at least 16
    // samples (the smallest block size any DAW realistically uses).
    if (out.sampleRate < 8000)
    {
        whyFailed = "invalid --sample-rate=" + juce::String(out.sampleRate)
                  + " (min=8000)";
        return false;
    }
    // Floor matches SandboxFactory_win::tryLoadSandboxed's jlimit(64, ...)
    // on the host side. Keeping the two in sync prevents an external caller
    // from spawning the binary with a smaller block size than the spawn
    // factory would clamp.
    if (out.maxBlock < 64 || out.maxBlock > (int)kAudioMaxBlockSamples)
    {
        whyFailed = "invalid --max-block=" + juce::String(out.maxBlock)
                  + " (min=64 cap=" + juce::String((int)kAudioMaxBlockSamples) + ")";
        return false;
    }
    // Match the host-side BusesProperties hardcode in
    // SandboxedProcessor::SandboxedProcessor (stereo). The factory
    // currently always spawns with --channels=2, so a value outside
    // [1, kAudioMaxChannels] means a future spawn-args refactor
    // drifted — surface it loudly instead of producing subtle
    // channel-count mismatches downstream.
    if (out.channels < 1 || out.channels > (int)kAudioMaxChannels)
    {
        whyFailed = "invalid --channels=" + juce::String(out.channels)
                  + " (range=[1, " + juce::String((int)kAudioMaxChannels) + "])";
        return false;
    }
    return true;
}

class EditorWindow : public juce::DocumentWindow
{
public:
    EditorWindow(const juce::String& name, juce::AudioProcessorEditor* ed)
        : DocumentWindow(name, juce::Colours::darkgrey,
                         /*buttonsNeeded*/ 0)
    {
        // No buttons: the window is reparented into the Electron renderer
        // and the host controls open/close via op::kOpenEditor /
        // op::kCloseEditor. JUCE-drawn title bar (NOT native) so
        // buttonsNeeded=0 in the DocumentWindow ctor above actually takes
        // effect — Windows' native title bar always renders min/max/close
        // regardless of the buttonsNeeded flag, and a user click on the
        // native close would fire closeButtonPressed() (default no-op
        // here), producing the "looks broken" UX this class is trying to
        // avoid.
        setUsingNativeTitleBar(false);
        setResizable(true, false);
        setContentNonOwned(ed, true);
        // Size first, then move offscreen. centreWithSize() would otherwise
        // reposition the window onto the active display before the host has
        // a chance to reparent it, producing a visible flash.
        setSize(ed->getWidth(), ed->getHeight());
        setTopLeftPosition(-32000, -32000);
    }
};

// Single-plugin host state, owned by the main thread; the worker thread reads
// pointers via std::atomic-like volatile reads (the plugin pointer is set
// before threads start and cleared on shutdown).
//
// Audio-thread sync (v2 / PR #2): non-realtime control ops that mutate plugin
// or buffer state (kPrepare, kSetBlockSize, kGetState, kSetState) acquire an
// AudioPauseGuard which sets `audioPauseRequested`, signals the audio worker
// out of its popInputBlock wait, waits on `audioPausedAck`, then performs the
// op exclusively. The guard's destructor signals `audioResume` and the worker
// re-syncs blockSize before resuming.
struct HostState
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VSTHost host;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<EditorWindow> editorWindow;
    ControlChannel control;
    AudioChannel audio;
    std::atomic<bool> running{true};
    // Set true while a kOpenEditor/kCloseEditor callAsync is in flight. The
    // I/O thread dispatches the next request immediately after replying, so
    // without this guard a host retrying kOpenEditor (timeout, double-click,
    // etc.) could race two async lambdas on st.editor / st.editorWindow.
    std::atomic<bool> editorRequestInFlight{false};
    std::thread audioThread;
    std::atomic<int> sampleRate{48000};
    std::atomic<int> blockSize{256};
    int channels = 2;                   // const after argv parse — main thread only

    std::atomic<bool>    audioPauseRequested{false};
    juce::WaitableEvent  audioPausedAck;    // audio thread → control thread
    juce::WaitableEvent  audioResume;       // control thread → audio thread
    // ^ Both default-constructed = auto-reset (manualReset=false), so a
    //   successful wait() clears the signaled state on the waiter side; no
    //   explicit reset() needed. The pause-loop in runAudioThread + the
    //   defensive resume signal in AudioPauseGuard's bail path both rely on
    //   this — manualReset=true would require explicit resets and break the
    //   self-recovery story.

    // Set by kPrepare on success. Gates two things:
    //   1. the audio worker's pop→processBlock loop — JUCE's contract is that
    //      processBlock must not be called before prepareToPlay, and the
    //      worker now starts BEFORE control.start so the spawn-to-first-
    //      kPrepare window is real.
    //   2. kSetBlockSize — the cached `sampleRate` defaults to 48000 at
    //      spawn; if a host calls kSetBlockSize before kPrepare, prepareToPlay
    //      would silently run at the wrong rate. Better to reject loudly.
    std::atomic<bool> prepared{false};
};

// RAII pause/drain/resume around non-realtime ops. Construct on the control
// (message) thread before touching plugin or blockSize state; destruct after.
struct AudioPauseGuard
{
    HostState& st;
    bool       active = false;
    explicit AudioPauseGuard(HostState& s) : st(s)
    {
        // Short-circuit if the worker is already shutting down so we don't
        // wait on an ack that won't come. Leaves active=false; callers MUST
        // check that before mutating plugin state.
        if (!st.running.load(std::memory_order_acquire))
            return;
        st.audioPauseRequested.store(true, std::memory_order_release);
        // Wake the audio thread out of popInputBlock so it notices the pause
        // flag without waiting the full 200 ms timeout.
        st.audio.signalSandboxWake();
        // Poll the ack on a 50 ms cadence rather than wait(-1), so that if
        // the worker exits between our `running` check and this wait (or if
        // a back-to-back pause guard already consumed the worker's
        // exit-time signal), we still notice and bail without deadlocking.
        // The `running` check happens BEFORE each wait, not after — that
        // way a back-to-back guard at shutdown observes running=false and
        // bails on the first iteration instead of paying the 50ms wait
        // before noticing. Worst-case latency is one 50ms slice (the
        // window where running flips between our check and wait return),
        // bounded by the acquire-load semantic.
        // No upper bound on total wait — a heavy plugin's processBlock can
        // legitimately run for tens of ms — but log at escalating thresholds
        // so a future "control op feels stuck" investigation has trail of
        // breadcrumbs (not just the first 250 ms data point).
        const auto waitStart = std::chrono::steady_clock::now();
        long long nextWarnMs = 250;        // 250ms → 1s → 5s → 30s → ...
        bool everWarned = false;
        while (true)
        {
            if (!st.running.load(std::memory_order_acquire))
            {
                // Bail-out path: the worker may already be in (or about to
                // enter) its pause branch. Clear the flag and signal resume
                // defensively so the worker doesn't block on
                // audioResume.wait(...) indefinitely after we leave; the
                // worker's bounded wait + running re-check is the
                // self-recovery belt, this is the suspenders.
                st.audioPauseRequested.store(false, std::memory_order_release);
                st.audioResume.signal();
                return;  // active=false → dtor is a no-op, callers skip mutation
            }
            if (st.audioPausedAck.wait(50)) break;
            using namespace std::chrono;
            const long long elapsedMs = duration_cast<milliseconds>(
                steady_clock::now() - waitStart).count();
            while (elapsedMs >= nextWarnMs)
            {
                hostLogf("AudioPauseGuard: ack still pending after %lld ms"
                         " — heavy processBlock or stuck worker?", elapsedMs);
                everWarned = true;
                // Geometric backoff so a multi-second stall doesn't spam
                // the log: 250ms, 1s, 5s, 30s, 5min, 30min, ...
                if      (nextWarnMs < 1000)    nextWarnMs = 1000;
                else if (nextWarnMs < 5000)    nextWarnMs = 5000;
                else if (nextWarnMs < 30000)   nextWarnMs = 30000;
                else                           nextWarnMs *= 10;
            }
        }
        if (everWarned)
        {
            // Final elapsed so the operator sees the actual stall length
            // instead of just the last "after Nms" line.
            using namespace std::chrono;
            const long long totalMs = duration_cast<milliseconds>(
                steady_clock::now() - waitStart).count();
            hostLogf("AudioPauseGuard: ack received after %lld ms total",
                     totalMs);
        }
        // Re-check running AFTER the ack succeeds. If `running` flipped to
        // false during the wait (worker exited, signaled audioPausedAck on
        // its way out per the runAudioThread bottom), the ack we just
        // consumed is a stale exit-time signal — the worker is gone, not
        // paused. Mutating plugin state would still be safe (no concurrent
        // processBlock) but `active=true` would misleadingly tell callers
        // the worker is alive. Treat as bail; signal resume defensively.
        if (!st.running.load(std::memory_order_acquire))
        {
            st.audioPauseRequested.store(false, std::memory_order_release);
            st.audioResume.signal();
            return;
        }
        active = true;
    }
    ~AudioPauseGuard()
    {
        if (!active) return;
        st.audioPauseRequested.store(false, std::memory_order_release);
        st.audioResume.signal();
    }
    AudioPauseGuard(const AudioPauseGuard&) = delete;
    AudioPauseGuard& operator=(const AudioPauseGuard&) = delete;
};

juce::var pluginMetadata(juce::AudioPluginInstance& p)
{
    // Caller contract: invoke from a thread that is NOT racing with
    // processBlock on the audio thread. Today only WinMain's startup
    // path calls this (before the audio worker is spawned), which is
    // safe. The getLatencySamples / getTotalNumIn/OutChannels reads
    // below are unsynchronized against in-plugin parameter mutation
    // that some plugins perform inside processBlock; if a future
    // caller adds a mid-session metadata refresh from dispatchRequest
    // (control I/O thread), route it through the planned audio-
    // thread-sync queue instead — same deferral that covers kPrepare /
    // kSetParameter / kGetState / kSetState.
    juce::DynamicObject::Ptr obj(new juce::DynamicObject());
    // Advertise the protocol version the sandbox was built against so the
    // host can detect version skew at handshake time with a clear error,
    // instead of waiting for the first mismatched frame's per-message `v`
    // check to tear the channel down.
    obj->setProperty("protocolVersion", (int)slopsmith::sandbox::kProtocolVersion);
    obj->setProperty("pluginName", p.getName());
    auto desc = p.getPluginDescription();
    obj->setProperty("manufacturer", desc.manufacturerName);
    // fileOrIdentifier + pluginFormatName from the plugin's actual
    // description, not the spawn-config hardcode. Lets the host cache
    // what the plugin reports (e.g. some VST3s normalise the path
    // differently than the caller passed in) instead of inferring.
    obj->setProperty("fileOrIdentifier", desc.fileOrIdentifier);
    obj->setProperty("pluginFormatName", desc.pluginFormatName);
    // uniqueId + deprecatedUid let SignalChain round-trip the plugin
    // identity across sessions. Without these, a persisted session
    // can't re-locate a sandboxed plugin by ID — only by file path.
    obj->setProperty("uniqueId", (int)desc.uniqueId);
    obj->setProperty("deprecatedUid", (int)desc.deprecatedUid);
    obj->setProperty("hasEditor", p.hasEditor());
    obj->setProperty("acceptsMidi", p.acceptsMidi());
    obj->setProperty("producesMidi", p.producesMidi());
    obj->setProperty("numParams", p.getParameters().size());
    obj->setProperty("latencySamples", p.getLatencySamples());
    // Total channel counts across all enabled buses. The host caches these
    // so a future SandboxedProcessor::BusesProperties pass (deferred until
    // the audio-thread-sync PR) can match the plugin's real topology
    // instead of hard-coding stereo↔stereo.
    obj->setProperty("numInputs",  p.getTotalNumInputChannels());
    obj->setProperty("numOutputs", p.getTotalNumOutputChannels());
    return juce::var(obj.get());
}

// Shared GUI-teardown path used by both kShutdown and the control-disconnect
// callback. The happy path runs editor/window destruction on the JUCE message
// thread (AsyncUpdater / MessageManagerLock during destruction require a live
// message loop). When the queue is gone (shutdown race), destruction is safe
// to do inline because the MessageManager is no longer pumping — there's
// nowhere for AsyncUpdater callbacks to dispatch to anyway.
//
// `postQuit` controls whether the lambda also calls PostQuitMessage to wake
// the WinMain dispatch loop (true for kShutdown which is replying to an
// explicit host request; false for the disconnect callback where the loop
// detects the running=false flip on the next iteration anyway). Plugin
// destruction stays in WinMain post-audioThread.join() (UAF protection).
inline void teardownGuiOnMessageThread(HostState& st, bool postQuit)
{
    if (!juce::MessageManager::callAsync([&st, postQuit]() {
        st.editorWindow.reset();
        st.editor.reset();
        st.running.store(false, std::memory_order_release);
        if (postQuit) PostQuitMessage(0);
    }))
    {
        // callAsync false does NOT guarantee the message thread has shut
        // down — JUCE returns false for several reasons (MessageManager
        // null, quitMessagePosted set, or postMessageToSystemQueue itself
        // failed for a transient reason). Destroying the editor/window
        // here would race the still-pumping message thread in the
        // transient-failure case (AsyncUpdater / MessageManagerLock would
        // be dispatching into freed objects).
        //
        // Safer: flip `running` so the dispatch loop exits at its next
        // tick, then let WinMain's post-loop cleanup destroy editor +
        // window. That path runs AFTER runDispatchLoopUntil returns, at
        // which point the message thread is no longer pumping and any
        // pending AsyncUpdater work has nowhere to dispatch to. Net
        // result: the destruction-while-loop-alive concern the
        // callAsync was added to avoid is preserved, AND the off-
        // thread-destruction concern is avoided too.
        st.running.store(false, std::memory_order_release);
    }
}

void runAudioThread(HostState& st)
{
    // Allocate at the spawn-time cap so the working buffer's storage is sized
    // for the largest blockSize the protocol allows. setSize(.., avoidRealloc)
    // on resume retargets to the current per-call size without a malloc.
    //
    // The realloc-free guarantee on resume relies on currentBlockSize <=
    // bufferCap so subsequent setSize(..) calls (kPrepare / kSetBlockSize
    // widening blockSize up to spawnCap) stay within the initial allocation.
    // This invariant holds because spawn-time blockSize is clamped to
    // kAudioMaxBlockSamples in SandboxFactory_win::tryLoadSandboxed and
    // bufferCap == st.audio.dims().maxBlockSamples == that same cap. The
    // jmax() below is a belt-and-braces guard for the case where some future
    // spawn path lets the initial blockSize exceed bufferCap; the jassert
    // makes the invariant explicit.
    const int bufferCap   = (int)st.audio.dims().maxBlockSamples;
    int currentBlockSize  = st.blockSize.load(std::memory_order_acquire);
    jassert(currentBlockSize <= bufferCap);
    juce::AudioBuffer<float> buffer(st.channels, juce::jmax(bufferCap,
                                                            currentBlockSize));
    buffer.setSize(st.channels, currentBlockSize,
                   /*keep*/false, /*clear*/true, /*avoidRealloc*/true);
    juce::MidiBuffer midi;
    while (st.running.load(std::memory_order_acquire))
    {
        if (st.audioPauseRequested.load(std::memory_order_acquire))
        {
            // Bounded wait + re-check loop, not wait(-1), so we self-recover
            // if the matching AudioPauseGuard never gets to its destructor:
            //   - shutdown bail (running flips false in the constructor's
            //     poll loop) — guard now defensively signals resume + clears
            //     the request, but this loop is the safety net in case any
            //     future caller construction path forgets to.
            //   - exception/early return between guard ctor and dtor.
            // Auto-reset WaitableEvent (default ctor) clears on successful
            // wait(), so we don't need an explicit reset() — that
            // assumption is documented at the audioResume field too.
            //
            // CRITICAL: re-ack on every pause cycle, not just once at entry.
            // If guard A's destructor clears pauseRequested + signals
            // resume, and guard B sets pauseRequested true *before* the
            // worker exits this branch, the worker stays in the loop
            // (pauseRequested still true) but never sends a fresh ack —
            // guard B would wait forever. `ackedThisPause` is reset
            // whenever resume fires so the next iteration re-acks.
            bool ackedThisPause = false;
            while (st.audioPauseRequested.load(std::memory_order_acquire)
                   && st.running.load(std::memory_order_acquire))
            {
                if (!ackedThisPause)
                {
                    st.audioPausedAck.signal();
                    ackedThisPause = true;
                }
                if (st.audioResume.wait(50))
                {
                    // Resume signaled. The next loop iteration re-checks
                    // pauseRequested; if a new guard already flipped it
                    // back to true, force a fresh ack on that pass.
                    ackedThisPause = false;
                }
            }
            // Re-sync block size in case the control op widened it.
            const int bs = juce::jlimit(1, bufferCap,
                                        st.blockSize.load(std::memory_order_acquire));
            if (bs != currentBlockSize)
            {
                buffer.setSize(st.channels, bs,
                               /*keep*/false, /*clear*/true, /*avoidRealloc*/true);
                currentBlockSize = bs;
            }
            continue;
        }
        midi.clear();
        if (!st.audio.popInputBlock(buffer, midi, currentBlockSize, /*timeoutMs=*/200))
            continue;
        // JUCE contract: processBlock must not be called before prepareToPlay.
        // The worker now starts BEFORE control.start (so pause-guarded ops
        // always have an acker), so the spawn-to-first-kPrepare window is
        // real. If a host pushes audio before kPrepare returns, push a
        // zero-output block (so the output ring stays in lockstep with the
        // input ring; otherwise the host's popBlock(true,…) times out and
        // bumps `dropouts` for every pre-prepare block, polluting the
        // metric). acquire-load pairs with kPrepare's release-store.
        if (!st.prepared.load(std::memory_order_acquire))
        {
            buffer.clear();
            st.audio.pushBlock(/*isOutputRing=*/true, buffer, currentBlockSize);
            continue;
        }
        if (auto* p = st.plugin.get())
            p->processBlock(buffer, midi);
        st.audio.pushBlock(/*isOutputRing=*/true, buffer, currentBlockSize);
    }
    // Defensive: any control-thread AudioPauseGuard waiting on us at the time
    // of shutdown must not deadlock. Signaling here is harmless if no one's
    // waiting — the event is auto-reset and absorbed by the next wait.
    st.audioPausedAck.signal();
}

void dispatchRequest(HostState& st, int requestId, const juce::String& op,
                     const juce::var& args)
{
    // Host uses requestId == -1 for fire-and-forget posts (postNoReply); the
    // sandbox would otherwise enqueue an unmatched reply frame each time.
    auto reply = [&st, requestId](bool ok, const juce::var& v,
                                  const juce::String& err = {}) {
        if (requestId >= 0)
            st.control.sendReply(requestId, ok, v, err);
    };

    if (op == op::kPrepare)
    {
        // Require a loaded plugin so a misordered host call (kPrepare before
        // anything is loaded — today loadPlugin happens at WinMain and is
        // mandatory before control.start, so this should never happen) is
        // loud rather than a silent ok with skipped prepareToPlay. Mirrors
        // the no-plugin guards in kSetBlockSize / kSetState / kSetParameter.
        if (!st.plugin)
        {
            reply(false, {}, "no plugin loaded");
            return;
        }
        // Both sr and bs come from JSON-deserialised juce::var — could be
        // double NaN, ±inf, or out-of-int-range. Read as double first and
        // validate finiteness + range BEFORE the narrowing int cast.
        // (int)NaN and (int)<INT_MIN-or->INT_MAX double are UB per C++.
        // Mirrors the validation in SandboxFactory_win::createSandboxed
        // at spawn time.
        //
        // Cap blockSize at the SPAWN-TIME ring size, not the protocol max:
        // the audio shm and the worker's pre-allocated buffer were sized to
        // dims().maxBlockSamples at spawn. Anything larger would silently
        // truncate inside push/popInputBlock and the sandbox would process
        // shorter blocks than the host pushed.
        double sr  = (double)args.getProperty("sampleRate", 48000);
        double bsd = (double)args.getProperty("blockSize",  256);
        const int spawnCap = (int)st.audio.dims().maxBlockSamples;
        if (! std::isfinite(sr)
            || sr <= 0.0
            || sr > (double)(std::numeric_limits<int>::max)()
            || std::floor(sr) != sr   // reject fractional sample rates: 44100.5 → 44100 silently mismatches plugin's prepareToPlay(sr,...)
            || ! std::isfinite(bsd)
            || bsd <= 0.0
            || std::floor(bsd) != bsd  // reject fractional: 256.5 → 256 silently changes effective size
            || bsd > (double)spawnCap)
        {
            reply(false, {}, "invalid prepare args: sr=" + juce::String(sr)
                             + " bs=" + juce::String(bsd)
                             + " (spawnCap=" + juce::String(spawnCap) + ")");
            return;
        }
        const int bs = (int)bsd;
        // Pause the audio worker before mutating shared blockSize / calling
        // prepareToPlay — otherwise processBlock can race the reconfigure.
        AudioPauseGuard pause(st);
        if (!pause.active)
        {
            // Worker is shutting down (or shut down). Don't mutate plugin
            // state without exclusive access — a final processBlock could
            // still be in flight on the audio thread between running.store
            // (false) and audioThread.join().
            reply(false, {}, "audio worker not paused (shutting down)");
            return;
        }
        // Make the "worker never sees `prepared=true` while plugin is
        // half-configured" invariant load-bearing rather than implicit:
        // explicitly clear `prepared` while we're holding the pause guard,
        // then republish after prepareToPlay returns. Today the worker is
        // paused throughout the reconfigure window so it never reads the
        // stale-true value, but a future code path that mutates plugin
        // state without holding the guard would otherwise be at risk.
        st.prepared.store(false, std::memory_order_release);
        st.sampleRate.store((int)sr, std::memory_order_release);
        st.blockSize.store(bs,       std::memory_order_release);
        st.plugin->setNonRealtime(false);
        // Caller contract: JUCE plugins must not throw from prepareToPlay
        // (the AudioProcessor base spec is noexcept-by-convention; a
        // throwing plugin is a bug). If a future plugin ever does throw,
        // this would propagate up the dispatch stack and `prepared` would
        // remain false, so the worker stays gated and no half-configured
        // processBlock fires. Acceptable failure mode; document but don't
        // wrap in try/catch (which would mask the bug).
        st.plugin->prepareToPlay(sr, bs);
        // Order matters: republish `prepared=true` AFTER prepareToPlay
        // returns so the audio worker (which gates on `prepared`) never
        // sees the flag before the plugin is actually ready. release-store
        // pairs with the worker's acquire-load.
        st.prepared.store(true, std::memory_order_release);
        // `ok` is already on the envelope via wire::makeReply — keeping it
        // off the result object so the schema stays uniform across ops
        // (kOpenEditor/kGetState/etc. don't double up either).
        juce::DynamicObject::Ptr res(new juce::DynamicObject());
        res->setProperty("latencySamples", st.plugin->getLatencySamples());
        reply(true, juce::var(res.get()));
    }
    else if (op == op::kSetBlockSize)
    {
        // Require a loaded AND prepared plugin: kSetBlockSize calls
        // prepareToPlay using the cached `sampleRate`, which defaults to
        // 48000 at spawn. A kSetBlockSize before kPrepare with a loaded
        // plugin would silently prepare at the wrong rate. Mirrors the
        // no-plugin guards on kSetState / kSetParameter and adds the
        // not-prepared guard for the cached-rate hazard.
        if (!st.plugin)
        {
            reply(false, {}, "no plugin loaded");
            return;
        }
        if (!st.prepared.load(std::memory_order_acquire))
        {
            // `prepared=false` here means either kPrepare was never called
            // OR we're inside the brief window between prepared.store(false)
            // and prepared.store(true) of an in-flight kPrepare /
            // kSetBlockSize. Today the control I/O thread serialises
            // dispatches so the in-flight window is unreachable from this
            // dispatch; the error message reflects the practically-reachable
            // case. If a future dispatch model parallelises this, revisit.
            reply(false, {}, "prepare not called — kSetBlockSize would use "
                             "stale cached sampleRate");
            return;
        }
        // Read as double first then narrow — JSON-deserialised juce::var could
        // be NaN/±inf/out-of-int-range; (int)NaN and out-of-range double cast
        // are UB. Same pattern as kPrepare / kSetParameter.
        // Same cap rationale as kPrepare: the spawn-time ring size is the
        // hard limit, not kAudioMaxBlockSamples.
        const double bsd = (double)args.getProperty("blockSize", 0);
        const int spawnCap = (int)st.audio.dims().maxBlockSamples;
        if (! std::isfinite(bsd)
            || bsd <= 0.0
            || std::floor(bsd) != bsd  // reject fractional: 256.5 → 256 silently changes effective size
            || bsd > (double)spawnCap)
        {
            reply(false, {}, "invalid setBlockSize: bs=" + juce::String(bsd)
                             + " (spawnCap=" + juce::String(spawnCap) + ")");
            return;
        }
        const int bs = (int)bsd;
        AudioPauseGuard pause(st);
        if (!pause.active)
        {
            reply(false, {}, "audio worker not paused (shutting down)");
            return;
        }
        // Same load-bearing-invariant pattern as kPrepare: clear `prepared`
        // under the pause guard, do the reconfigure, republish.
        st.prepared.store(false, std::memory_order_release);
        st.blockSize.store(bs, std::memory_order_release);
        // Mirror kPrepare's pre-amble so block-size changes don't subtly
        // differ from full prepares for plugins that key off
        // setNonRealtime (e.g. some sample-streamers gate background loads
        // behind it). prepareToPlay then rebuilds JUCE's processing
        // pipeline at the new block size — cheap for most plugins and the
        // only universally supported way to change buffer size in the
        // JUCE wrapper.
        st.plugin->setNonRealtime(false);
        st.plugin->prepareToPlay((double)st.sampleRate.load(std::memory_order_acquire),
                                 bs);
        st.prepared.store(true, std::memory_order_release);
        reply(true, {});
    }
    else if (op == op::kOpenEditor)
    {
        if (!st.plugin || !st.plugin->hasEditor())
        {
            reply(false, {}, "no editor");
            return;
        }
        // Reject overlapping open/close: the callAsync below mutates
        // st.editor/st.editorWindow, and the I/O thread can dispatch the
        // next request before that lambda runs. Two async lambdas racing
        // on the unique_ptr resets would corrupt the editor state.
        bool expected = false;
        if (!st.editorRequestInFlight.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
        {
            reply(false, {}, "editor open/close already in flight");
            return;
        }
        // Must run on the message thread.
        if (!juce::MessageManager::callAsync([&st, reply]
        {
            // Tear down any prior editor BEFORE replacing st.editor. The
            // existing EditorWindow holds st.editor.get() via
            // setContentNonOwned, so resetting st.editor first would leave
            // a dangling content pointer in the window. Resetting the
            // window first detaches its content before we touch the editor.
            if (st.editorWindow) st.editorWindow.reset();
            if (st.editor)       st.editor.reset();

            st.editor.reset(st.plugin->createEditor());
            if (!st.editor)
            {
                st.editorRequestInFlight.store(false, std::memory_order_release);
                reply(false, {}, "createEditor null");
                return;
            }
            if (st.editor->getWidth() < 16 || st.editor->getHeight() < 16)
                st.editor->setSize(slopsmith::sandbox::kDefaultEditorWidth,
                                   slopsmith::sandbox::kDefaultEditorHeight);
            st.editorWindow = std::make_unique<EditorWindow>(
                st.plugin->getName(), st.editor.get());
            st.editorWindow->setVisible(true);
            HWND hwnd = (HWND)st.editorWindow->getWindowHandle();
            if (hwnd == nullptr)
            {
                // Native peer never came up (rare — class-registration
                // failure or similar). Report the failure here so the host
                // surfaces a single "could not open editor" instead of
                // following up with a redundant kCloseEditor on hwnd==null.
                st.editorWindow.reset();
                st.editor.reset();
                st.editorRequestInFlight.store(false, std::memory_order_release);
                reply(false, {}, "failed to obtain native window handle");
                return;
            }
            juce::DynamicObject::Ptr res(new juce::DynamicObject());
            res->setProperty("hwnd", "0x" + juce::String::toHexString((juce::int64)(uintptr_t)hwnd));
            res->setProperty("w", st.editor->getWidth());
            res->setProperty("h", st.editor->getHeight());
            st.editorRequestInFlight.store(false, std::memory_order_release);
            reply(true, juce::var(res.get()));
        }))
        {
            // callAsync returns false when the message queue is gone (shutdown).
            // Lambda never runs, so undo the in-flight flag here and surface
            // the failure to the host — otherwise editorRequestInFlight would
            // stay true forever and block all subsequent open/close requests.
            st.editorRequestInFlight.store(false, std::memory_order_release);
            reply(false, {}, "message queue unavailable (shutdown)");
        }
    }
    else if (op == op::kCloseEditor)
    {
        bool expected = false;
        if (!st.editorRequestInFlight.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
        {
            reply(false, {}, "editor open/close already in flight");
            return;
        }
        if (!juce::MessageManager::callAsync([&st, reply]
        {
            st.editorWindow.reset();
            st.editor.reset();
            st.editorRequestInFlight.store(false, std::memory_order_release);
            reply(true, {});
        }))
        {
            st.editorRequestInFlight.store(false, std::memory_order_release);
            reply(false, {}, "message queue unavailable (shutdown)");
        }
    }
    else if (op == op::kGetState)
    {
        juce::MemoryBlock mb;
        // get/setStateInformation can mutate plugin internals (presets,
        // parameter trees, plugin-internal allocations). Pause the audio
        // worker to avoid racing processBlock against the plugin's state
        // serialisation.
        //
        // Shutdown semantics: if the worker has already exited, we reject
        // with "shutting down" rather than running getStateInformation
        // unguarded. That IS a behavior change from pre-v3 (where
        // kGetState was best-effort callable any time); a host needing
        // last-moment crash-recovery state must kGetState BEFORE
        // initiating shutdown. The trade-off is intentional — racing a
        // final processBlock at shutdown to grab state is exactly the
        // class of UB this guard was added to prevent.
        {
            AudioPauseGuard pause(st);
            if (!pause.active)
            {
                reply(false, {}, "audio worker not paused (shutting down)");
                return;
            }
            if (st.plugin) st.plugin->getStateInformation(mb);
        }
        juce::DynamicObject::Ptr res(new juce::DynamicObject());
        res->setProperty("stateBase64",
            juce::Base64::toBase64(mb.getData(), mb.getSize()));
        reply(true, juce::var(res.get()));
    }
    else if (op == op::kSetState)
    {
        auto b64 = args.getProperty("stateBase64", "").toString();
        juce::MemoryOutputStream mo;
        if (!juce::Base64::convertFromBase64(mo, b64))
        {
            reply(false, {}, "invalid base64 state payload");
            return;
        }
        if (!st.plugin)
        {
            reply(false, {}, "no plugin loaded");
            return;
        }
        AudioPauseGuard pause(st);
        if (!pause.active)
        {
            reply(false, {}, "audio worker not paused (shutting down)");
            return;
        }
        st.plugin->setStateInformation(mo.getData(), (int)mo.getDataSize());
        reply(true, {});
    }
    else if (op == op::kSetParameter)
    {
        if (!st.plugin)
        {
            reply(false, {}, "no plugin loaded");
            return;
        }
        // Read as double + validate finiteness/range BEFORE narrowing to
        // int/float. Same UB class as the kPrepare sampleRate/blockSize
        // fix — JSON-derived NaN or out-of-int-range double would invoke
        // UB at the `(int)` / `(float)` cast before any bounds check.
        const double idxd = (double)args.getProperty("index", -1);
        const double vald = (double)args.getProperty("value", 0.0);
        auto params = st.plugin->getParameters();
        if (! std::isfinite(idxd)
            || idxd < 0.0
            || idxd > (double)(std::numeric_limits<int>::max)()
            || std::floor(idxd) != idxd   // reject fractional indices: 1.9 → 1 would silently mutate the wrong parameter
            || ! std::isfinite(vald))
        {
            reply(false, {}, "non-finite or non-integer parameter index/value: "
                             "idx=" + juce::String(idxd)
                           + " val=" + juce::String(vald));
            return;
        }
        const int idx = (int)idxd;
        if (idx >= params.size())
        {
            reply(false, {}, "parameter index out of range: idx="
                           + juce::String(idx)
                           + " size=" + juce::String(params.size()));
            return;
        }
        // JUCE parameters expect [0, 1] — clamp rather than reject so a
        // slightly-out-of-range automation curve doesn't fail the request.
        params[idx]->setValue((float)juce::jlimit(0.0, 1.0, vald));
        reply(true, {});
    }
    else if (op == op::kShutdown)
    {
        reply(true, {});
        // Plugin destruction stays in WinMain post-audioThread.join() (audio
        // thread can be between its running.load() check and processBlock —
        // dropping plugin would dereference a freed AudioPluginInstance).
        // teardownGuiOnMessageThread sets running=false and posts WM_QUIT.
        teardownGuiOnMessageThread(st, /*postQuit=*/true);
        // Wake the audio worker so it observes running=false within one
        // popInputBlock turn instead of waiting up to 200 ms; the worker
        // signals audioPausedAck on the way out so a stale guard wait
        // (race window during shutdown) can't deadlock.
        st.audio.signalSandboxWake();
    }
    else if (op == op::kMidiEvent)
    {
        // Removed since protocol v2: MIDI is now bundled inline in the audio
        // shm's per-slot MidiQueue. The version handshake (now v3) rejects
        // any v1 host before it can reach this path, so this branch is a
        // paranoid fallback. Drop it when v1 host binaries are no longer
        // in circulation.
        static std::atomic<bool> warned{false};
        bool expected = false;
        if (warned.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel))
            hostLogf("warn: control-pipe op::kMidiEvent is removed since "
                     "protocol v2 — host is sending MIDI on the wrong channel");
        // Fire-and-forget; deliberately no reply.
    }
    else
    {
        reply(false, {}, "unknown op: " + op);
    }
}

} // anonymous

// Debug-log path: %TEMP%\slopsmith-vst-host-<pid>.log. Plain text, line-buffered
// so a fast-fail still leaves a useful tail. This is essential because the
// subprocess runs hidden (no console) so fprintf(stderr) goes nowhere; without
// the file we can't see why it died. The per-PID suffix keeps concurrent
// sandboxes from interleaving their log writes.
static FILE* g_hostLog = nullptr;
// Guards interleaved writes from any thread. Today's call sites are all on
// the main thread (WinMain's startup path), but `setRequestHandler` and
// `runAudioThread` are within reach of future edits, and a corrupted log
// file is exactly the diagnostic source we depend on when a sandbox dies
// silently. Mirrors VSTTrace.h's logMutex() pattern.
static std::mutex g_hostLogMutex;
static void hostLogf(const char* fmt, ...)
{
    // Null-check INSIDE the lock — HostLogCloser (or any future close path)
    // assigns g_hostLog = nullptr under the same mutex, so a lock-free fast
    // path would race the close-then-null with the read-then-use here.
    std::lock_guard<std::mutex> lock(g_hostLogMutex);
    if (!g_hostLog) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(g_hostLog, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_hostLog);
    std::fflush(g_hostLog);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    {
        // Use wide-char env + fopen so non-ASCII profile paths (e.g.
        // a user named with CJK or extended-Latin characters in
        // C:\Users\...) don't drop the diagnostic log silently —
        // GetEnvironmentVariableA loses bytes that don't map to the
        // current ANSI codepage, and fopen(narrow path) on Windows
        // applies the same lossy conversion.
        wchar_t path[1024]{};
        DWORD n = GetEnvironmentVariableW(L"TEMP", path, (DWORD)std::size(path));
        const unsigned long pid = (unsigned long)GetCurrentProcessId();
        wchar_t suffix[64]{};
        const int suffixLen = std::swprintf(
            suffix, std::size(suffix), L"\\slopsmith-vst-host-%lu.log", pid);
        // n < std::size(path) → got the actual length, not the truncation
        // sentinel (GetEnvironmentVariableW returns required-wchar-count
        // including NUL if the buffer was too small).
        if (n > 0 && n < std::size(path) && suffixLen > 0
                  && (size_t)n + (size_t)suffixLen < std::size(path))
        {
            std::wcscat(path, suffix);
        }
        else
        {
            // Fall back to %LOCALAPPDATA%\Temp — the canonical per-user
            // temp directory that %TEMP% normally resolves to. Writing
            // into USERPROFILE root would clutter the user's home with
            // per-PID files; LOCALAPPDATA\Temp is where these files
            // *belong* on the standard Windows layout. If LOCALAPPDATA
            // is also missing/oversized we leave `path` empty and the
            // diagnostic file just doesn't open (hostLogf no-ops). The
            // C:\ drive-root fallback is gone for the same reason as
            // the VSTTrace.h trace path: drive-root writes need admin
            // on default installs.
            wchar_t localAppData[1024]{};
            const DWORD la = GetEnvironmentVariableW(L"LOCALAPPDATA",
                                                     localAppData,
                                                     (DWORD)std::size(localAppData));
            constexpr const wchar_t* kTempSubdir = L"\\Temp";
            const size_t kTempSubdirLen = std::wcslen(kTempSubdir);
            if (la > 0 && la < std::size(localAppData)
                       && (size_t)la + kTempSubdirLen + (size_t)suffixLen < std::size(path))
            {
                std::swprintf(path, std::size(path), L"%ls%ls%ls",
                              localAppData, kTempSubdir, suffix);
            }
        }
        // Open the per-PID host log with "w" (truncate) rather than "a"
        // (append). The filename includes the PID and is single-writer,
        // so append-vs-truncate doesn't change correlation, but truncate
        // means a long-lived install isn't accumulating thousands of
        // stale slopsmith-vst-host-<pid>.log files indefinitely (the
        // file is recreated fresh on every spawn). Long-term cleanup of
        // *historical* per-PID logs from prior installs is a separate
        // janitor pass not in scope here.
        g_hostLog = path[0] ? _wfopen(path, L"w") : nullptr;
        if (g_hostLog)
            hostLogf("\n==== slopsmith-vst-host pid=%lu starting ====", pid);
    }
    // RAII close for g_hostLog: every early `return N` below would otherwise
    // leak the FILE* until process exit. The OS reaps it anyway, but making
    // the lifetime explicit means future early-returns get it for free.
    struct HostLogCloser {
        ~HostLogCloser() {
            // Same mutex hostLogf takes so a future concurrent caller can't
            // observe a half-torn-down g_hostLog (NULL-with-FILE-still-open
            // or post-fclose dangling pointer).
            std::lock_guard<std::mutex> lock(g_hostLogMutex);
            if (g_hostLog) { std::fclose(g_hostLog); g_hostLog = nullptr; }
        }
    } hostLogCloser;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr)
    {
        hostLogf("CommandLineToArgvW failed err=%lu",
                 (unsigned long)GetLastError());
        return 1;
    }
    Args parsed;
    juce::String parseFailReason;
    if (!parseArgs(argc, argv, parsed, parseFailReason))
    {
        hostLogf("bad args (argc=%d): %s", argc, parseFailReason.toRawUTF8());
        for (int i = 0; i < argc; ++i)
        {
            char buf[512]; std::snprintf(buf, sizeof(buf), "  argv[%d]=%ls", i, argv[i]);
            hostLogf("%s", buf);
        }
        LocalFree(argv);
        return 2;
    }
    LocalFree(argv);
    // Don't log the full pipe + shm + event names — they're the OS-level
    // kernel-object names an attacker reading our per-PID temp log
    // (`%TEMP%\slopsmith-vst-host-<pid>.log`, unconditional) could harvest
    // to OpenFileMappingW / OpenEventW the live sandbox audio shm before
    // DACL hardening lands (existing deferral). The plugin path is fine
    // (already known to anyone with read access to the install) and the
    // numeric args carry no exfiltration value. Logging just the *length*
    // of the kernel-object names keeps the diagnostic useful (truncation
    // / empty cases visible) without leaking the literal name.
    hostLogf("args ok: plugin=%s pipe=<%dchars> shm=<%dchars> sr=%d bs=%d ch=%d",
             parsed.pluginPath.toRawUTF8(),
             (int)parsed.controlPipe.length(),
             (int)parsed.audio.shm.length(),
             parsed.sampleRate, parsed.maxBlock, parsed.channels);

    HostState st;
    st.sampleRate = parsed.sampleRate;
    st.blockSize  = parsed.maxBlock;
    st.channels   = parsed.channels;

    juce::String err;
    if (!st.audio.openSandboxSide(parsed.audio, err))
    {
        hostLogf("audio shm open failed: %s", err.toRawUTF8());
        return 3;
    }
    hostLogf("audio shm opened");
    if (!st.control.connectClientSide(parsed.controlPipe, err))
    {
        hostLogf("control pipe connect failed: %s", err.toRawUTF8());
        // No control thread to stop here (connect failed), but no
        // explicit disconnect signal possible either — the host's pipe
        // server will detect the close when our HANDLE goes through the
        // process exit.
        return 4;
    }
    hostLogf("control pipe connected");

    // Load the plugin BEFORE starting the control loop so we can return a
    // populated `ready` event.
    hostLogf("calling host.loadPlugin");
    st.plugin = st.host.loadPlugin(parsed.pluginPath,
                                    (double)parsed.sampleRate,
                                    parsed.maxBlock, err);
    if (!st.plugin)
    {
        hostLogf("loadPlugin failed: %s", err.toRawUTF8());
        // Control channel is connected (no I/O thread yet — start() hasn't
        // been called), so a sendEvent(kGoodbye) is the cheapest way to
        // tell the host "fast-fail" instead of letting it wait out the
        // 30s handshake timeout. Best-effort; ignore failures.
        st.control.sendEvent(event::kGoodbye, {});
        return 5;
    }
    hostLogf("plugin loaded: %s", st.plugin->getName().toRawUTF8());
    // Clamp the audio worker's channel count to what the plugin actually
    // reports. spawn-time --channels is a hint (the host doesn't know the
    // real topology until the plugin loads); using the plugin's
    // getTotalNumIn/OutChannels prevents undersized buffers being passed
    // to a mono effect or extra silent channels for a >stereo synth.
    // BusesProperties-aware buffer sizing is the deferred follow-up;
    // until then take the max of in/out so the buffer is at least big
    // enough for either direction.
    {
        const int pIn  = st.plugin->getTotalNumInputChannels();
        const int pOut = st.plugin->getTotalNumOutputChannels();
        const int pMax = juce::jmax(pIn, pOut);
        const int shmCh = (int)st.audio.dims().maxChannels;
        if (pMax > (int)kAudioMaxChannels)
        {
            // Fail closed against the absolute protocol cap. A plugin
            // reporting >kAudioMaxChannels means our protocol literally
            // can't represent its topology.
            hostLogf("plugin reports %d channels, exceeding protocol cap %d",
                     pMax, (int)kAudioMaxChannels);
            st.control.sendEvent(event::kGoodbye, {});
            return 5;
        }
        // The SHM ring width is fixed at spawn time (SandboxFactory_win
        // currently hardcodes 2; cf. the deferred BusesProperties
        // refactor that would size SHM from the plugin's reported
        // topology). If the plugin reports more channels than the ring
        // can carry, the audio worker runs with a clamped buffer:
        // pushBlock/popBlock cap at header->maxChannels and the plugin
        // sees zero-padded input on its extra channels. Today's only
        // sandboxed plugin (Guitar Rig 6) reports 4-channel topology
        // (sidechain) but works fine on 2-channel buffers, so clamping
        // is the pragmatic choice — loudly logged so the limitation is
        // visible in the diagnostic trail.
        const int effective = juce::jmin(pMax, shmCh);
        if (pMax > shmCh)
        {
            hostLogf("WARNING: plugin reports %d channels but SHM ring width "
                     "is %d — clamping to %d. Plugin's extra channels will "
                     "see zero-padded input and their output will be dropped. "
                     "Deferred fix: BusesProperties refactor sizes SHM from "
                     "plugin topology.", pMax, shmCh, effective);
        }
        if (effective > 0 && effective != st.channels)
        {
            hostLogf("channel count: spawn-arg=%d effective=%d "
                     "(plugin=%d, shm=%d)",
                     st.channels, effective, pMax, shmCh);
            st.channels = effective;
        }
    }
    // Don't eagerly prepareToPlay here. The host always sends op::kPrepare
    // immediately after the ready handshake (SignalChain::addProcessor
    // calls prepare on every newly-added processor), which reruns
    // prepareToPlay with the actual session sample-rate/block-size.
    // Some plugins allocate or reset internal state on each prepare —
    // doing it twice (spawn-time + first kPrepare) is wasteful at best
    // and visibly disruptive at worst.

    st.control.setRequestHandler(
        [&st](int id, const juce::String& op, const juce::var& args)
        {
            dispatchRequest(st, id, op, args);
        });
    // Start the audio thread BEFORE control.start so that any pause-guarded
    // request (kPrepare, kSetBlockSize, kGetState, kSetState) dispatched on
    // the control I/O thread always has a worker to ack the pause flag —
    // otherwise an early kPrepare would deadlock in AudioPauseGuard waiting
    // forever on audioPausedAck.
    st.audioThread = std::thread([&st] { runAudioThread(st); });

    auto stopAudioWorker = [&st]
    {
        st.running.store(false, std::memory_order_release);
        // Wake the worker out of popInputBlock so it observes running=false
        // promptly; the worker also signals audioPausedAck on its way out.
        st.audio.signalSandboxWake();
        if (st.audioThread.joinable()) st.audioThread.join();
    };

    if (!st.control.start({}, [&st](const juce::String&)
    {
        // Pipe dropped → mirror the kShutdown GUI teardown. postQuit=true
        // because the WinMain dispatch loop is still pumping and needs to
        // wake from its WaitMessage; running=false alone wouldn't break the
        // 20ms runDispatchLoopUntil tick if the loop happens to be inside
        // it when the disconnect fires.
        teardownGuiOnMessageThread(st, /*postQuit=*/true);
        // Wake the audio worker so it observes running=false within one
        // popInputBlock turn instead of waiting up to 200 ms; the worker
        // signals audioPausedAck on the way out so a stale guard wait
        // (race window during shutdown) can't deadlock.
        st.audio.signalSandboxWake();
    }))
    {
        hostLogf("control channel start failed: %s",
                 st.control.getLastStartError().toRawUTF8());
        // Audio worker was started before control.start (so pause-guarded
        // ops always have an acker); on this failure path it's still
        // running and must be stopped before we exit.
        stopAudioWorker();
        // Same fast-fail signal as the loadPlugin failure path: best-effort
        // goodbye so the host doesn't burn its 30s handshake timeout.
        // Safe even after start() failure — connectClientSide opened the
        // pipe earlier in WinMain and start() only spins up the I/O
        // thread, so the underlying handle is valid; ControlChannel::
        // writeFrame is mutex-guarded and short-circuits on
        // INVALID_HANDLE_VALUE so the worst case is a false return.
        st.control.sendEvent(event::kGoodbye, {});
        return 6;
    }

    hostLogf("sending ready event");
    if (!st.control.sendEvent(event::kReady, pluginMetadata(*st.plugin)))
    {
        hostLogf("failed to send ready event — host won't see us as ready");
        st.control.stop();
        stopAudioWorker();
        return 7;
    }
    hostLogf("ready event sent");

    // Pump the main message loop. JUCE's MessageManager is bound to this
    // thread (the OS main thread), which is the key correctness property
    // for Qt-using plugins per the diag PoC. We also exit when JUCE sees a
    // stop message — SubprocessHandle::shutdown sends WM_QUIT to this
    // thread, and the JUCE handler treats it as a stop request; without
    // this check the loop would keep going until `st.running` is also
    // cleared by some other path.
    auto* mm = juce::MessageManager::getInstance();
    while (st.running.load(std::memory_order_acquire)
           && !mm->hasStopMessageBeenSent())
        mm->runDispatchLoopUntil(20);

    // The loop can exit because hasStopMessageBeenSent() is true while
    // st.running is still true (host sent WM_QUIT but never kShutdown).
    // The audio thread only watches st.running, so without this store the
    // join() below would hang forever.
    st.running.store(false, std::memory_order_release);
    st.audioThread.join();
    // Stop the control channel BEFORE destroying plugin/editor state.
    // Otherwise the ControlChannel I/O thread can dispatch a late
    // request (or the disconnect callback) into a half-torn-down state.
    // sendEvent(kGoodbye) is best-effort just before stop so a peer
    // sees a clean exit; if it fails (broken pipe) we don't care.
    st.control.sendEvent(event::kGoodbye, {});
    st.control.stop();
    st.editorWindow.reset();
    st.editor.reset();
    st.plugin.reset();
    // g_hostLog is closed by HostLogCloser as this function returns
    // (RAII near the top of WinMain). The explicit close that used to
    // live here was redundant after introducing the RAII guard.
    return 0;
}
