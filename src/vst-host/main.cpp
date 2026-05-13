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
#include <cmath>
#include <limits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
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
bool parseArgs(int argc, wchar_t** argv, Args& out, juce::String& whyFailed)
{
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
        if      (key == "--plugin-path")     out.pluginPath = val;
        else if (key == "--control-pipe")    out.controlPipe = val;
        else if (key == "--audio-shm")       out.audio.shm = val;
        else if (key == "--audio-event-out") out.audio.evtToHost = val;
        else if (key == "--audio-event-in")  out.audio.evtToSandbox = val;
        else if (key == "--sample-rate")     out.sampleRate = val.getIntValue();
        else if (key == "--max-block")       out.maxBlock = val.getIntValue();
        else if (key == "--channels")        out.channels = val.getIntValue();
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
    if (out.sampleRate <= 0) { whyFailed = "invalid --sample-rate=" + juce::String(out.sampleRate); return false; }
    if (out.maxBlock <= 0 || out.maxBlock > (int)kAudioMaxBlockSamples)
    {
        whyFailed = "invalid --max-block=" + juce::String(out.maxBlock)
                  + " (cap=" + juce::String((int)kAudioMaxBlockSamples) + ")";
        return false;
    }
    if (out.channels   <= 0) { whyFailed = "invalid --channels="    + juce::String(out.channels);   return false; }
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
        // op::kCloseEditor. A user-visible close button with a no-op
        // handler would just look broken.
        setUsingNativeTitleBar(true);
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
    int sampleRate = 48000;
    int blockSize  = 256;
    int channels   = 2;
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
        st.editorWindow.reset();
        st.editor.reset();
        st.running.store(false, std::memory_order_release);
    }
}

void runAudioThread(HostState& st)
{
    juce::AudioBuffer<float> buffer(st.channels, st.blockSize);
    // `midi` stays empty in v1 — MIDI is moving to an inline per-block shm
    // queue in the audio-shm follow-up PR. The control-channel op::kMidiEvent
    // path is a no-op deprecation stub; nothing populates this buffer today.
    juce::MidiBuffer midi;
    while (st.running.load(std::memory_order_acquire))
    {
        if (!st.audio.popBlock(/*isOutputRing=*/false, buffer, st.blockSize,
                               /*timeoutMs=*/200))
            continue;
        if (auto* p = st.plugin.get())
            p->processBlock(buffer, midi);
        st.audio.pushBlock(/*isOutputRing=*/true, buffer, st.blockSize);
        midi.clear();
    }
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
        double sr = (double)args.getProperty("sampleRate", 48000);
        int bs    = (int)args.getProperty("blockSize", 256);
        // (int)NaN and (int)<INT_MIN-or->INT_MAX double are UB — guard
        // finiteness + the int range before the narrowing on the
        // st.sampleRate = (int)sr line below. Mirrors the validation in
        // SandboxFactory_win::createSandboxed at spawn time.
        if (! std::isfinite(sr)
            || sr <= 0.0
            || sr > (double)(std::numeric_limits<int>::max)()
            || bs <= 0
            || bs > (int)kAudioMaxBlockSamples)
        {
            reply(false, {}, "invalid prepare args: sr=" + juce::String(sr)
                             + " bs=" + juce::String(bs));
            return;
        }
        st.sampleRate = (int)sr;
        st.blockSize  = bs;
        if (st.plugin)
        {
            st.plugin->setNonRealtime(false);
            st.plugin->prepareToPlay(sr, bs);
        }
        // `ok` is already on the envelope via wire::makeReply — keeping it
        // off the result object so the schema stays uniform across ops
        // (kOpenEditor/kGetState/etc. don't double up either).
        juce::DynamicObject::Ptr res(new juce::DynamicObject());
        res->setProperty("latencySamples",
            st.plugin ? st.plugin->getLatencySamples() : 0);
        reply(true, juce::var(res.get()));
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
        if (st.plugin) st.plugin->getStateInformation(mb);
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
        const int idx = (int)args.getProperty("index", -1);
        const float val = (float)(double)args.getProperty("value", 0.0);
        auto params = st.plugin->getParameters();
        if (idx < 0 || idx >= params.size() || !std::isfinite(val))
        {
            reply(false, {}, "invalid parameter index/value");
            return;
        }
        // JUCE parameters expect [0, 1] — clamp rather than reject so a
        // slightly-out-of-range automation curve doesn't fail the request.
        params[idx]->setValue(juce::jlimit(0.0f, 1.0f, val));
        reply(true, {});
    }
    else if (op == op::kShutdown)
    {
        reply(true, {});
        // Plugin destruction stays in WinMain post-audioThread.join() (audio
        // thread can be between its running.load() check and processBlock —
        // dropping plugin would dereference a freed AudioPluginInstance).
        teardownGuiOnMessageThread(st, /*postQuit=*/true);
    }
    else if (op == op::kMidiEvent)
    {
        // Fire-and-forget — but no-op for now. Bundled MIDI is a follow-up.
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
        char path[1024]{};
        DWORD n = GetEnvironmentVariableA("TEMP", path, sizeof(path));
        const unsigned long pid = (unsigned long)GetCurrentProcessId();
        char suffix[64];
        const int suffixLen = std::snprintf(
            suffix, sizeof(suffix), "\\slopsmith-vst-host-%lu.log", pid);
        // strlen of the formatted suffix, NOT sizeof(suffix) — the array is
        // 64 bytes but the actual content is typically ~30 bytes, and using
        // sizeof would over-reserve room and falsely fall back for moderately
        // long TEMP paths.
        //
        // n < sizeof(path) → got the actual value, not the truncation
        // sentinel (GetEnvironmentVariableA returns required-buf-size if
        // the buffer was too small). Mirrors the USERPROFILE branch below.
        if (n > 0 && n < sizeof(path) && suffixLen > 0
                  && (size_t)n + (size_t)suffixLen < sizeof(path))
        {
            std::strncat(path, suffix, sizeof(path) - n - 1);
        }
        else
        {
            // Fall back to USERPROFILE — writing into C:\ requires admin
            // and a non-admin install just loses the diagnostic file. The
            // user profile path is always writable by the calling user.
            char userprofile[1024]{};
            const DWORD up = GetEnvironmentVariableA("USERPROFILE",
                                                     userprofile,
                                                     sizeof(userprofile));
            // up < sizeof(userprofile) → got the value (not truncated);
            // up + suffixLen < sizeof(path) → concatenation fits. Two
            // separate predicates so the intent is obvious.
            if (up > 0 && up < sizeof(userprofile)
                       && (size_t)up + (size_t)suffixLen < sizeof(path))
                std::snprintf(path, sizeof(path), "%s%s", userprofile, suffix);
            // No C:\ fallback — writing to the drive root requires admin on
            // a default Windows install, so the previous fallback would
            // silently lose the diagnostic anyway. Leaving `path` empty
            // makes fopen return NULL, which the call below handles cleanly
            // (g_hostLog stays null, hostLogf becomes a no-op).
        }
        // Open the per-PID host log with "w" (truncate) rather than "a"
        // (append). The filename includes the PID and is single-writer,
        // so append-vs-truncate doesn't change correlation, but truncate
        // means a long-lived install isn't accumulating thousands of
        // stale slopsmith-vst-host-<pid>.log files indefinitely (the
        // file is recreated fresh on every spawn). Long-term cleanup of
        // *historical* per-PID logs from prior installs is a separate
        // janitor pass not in scope here.
        g_hostLog = path[0] ? std::fopen(path, "w") : nullptr;
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
    hostLogf("args ok: plugin=%s pipe=%s shm=%s sr=%d bs=%d ch=%d",
             parsed.pluginPath.toRawUTF8(),
             parsed.controlPipe.toRawUTF8(),
             parsed.audio.shm.toRawUTF8(),
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
        return 5;
    }
    hostLogf("plugin loaded: %s", st.plugin->getName().toRawUTF8());
    st.plugin->prepareToPlay((double)parsed.sampleRate, parsed.maxBlock);

    st.control.setRequestHandler(
        [&st](int id, const juce::String& op, const juce::var& args)
        {
            dispatchRequest(st, id, op, args);
        });
    if (!st.control.start({}, [&st](const juce::String&)
    {
        // Pipe dropped → mirror the kShutdown GUI teardown. postQuit=true
        // because the WinMain dispatch loop is still pumping and needs to
        // wake from its WaitMessage; running=false alone wouldn't break the
        // 20ms runDispatchLoopUntil tick if the loop happens to be inside
        // it when the disconnect fires.
        teardownGuiOnMessageThread(st, /*postQuit=*/true);
    }))
    {
        hostLogf("control channel start failed");
        return 6;
    }

    hostLogf("sending ready event");
    if (!st.control.sendEvent(event::kReady, pluginMetadata(*st.plugin)))
    {
        hostLogf("failed to send ready event — host won't see us as ready");
        st.control.stop();
        return 7;
    }
    hostLogf("ready event sent");

    st.audioThread = std::thread([&st] { runAudioThread(st); });

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
