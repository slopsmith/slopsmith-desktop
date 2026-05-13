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
#include <cstdarg>
#include <cstdio>
#include <cstring>
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
    if (out.maxBlock   <= 0) { whyFailed = "invalid --max-block="   + juce::String(out.maxBlock);   return false; }
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
    std::thread audioThread;
    int sampleRate = 48000;
    int blockSize  = 256;
    int channels   = 2;
};

juce::var pluginMetadata(juce::AudioPluginInstance& p)
{
    juce::DynamicObject::Ptr obj(new juce::DynamicObject());
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

void runAudioThread(HostState& st)
{
    juce::AudioBuffer<float> buffer(st.channels, st.blockSize);
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
        if (sr <= 0.0 || bs <= 0 || bs > (int)kAudioMaxBlockSamples)
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
        juce::DynamicObject::Ptr res(new juce::DynamicObject());
        res->setProperty("ok", true);
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
        // Must run on the message thread.
        juce::MessageManager::callAsync([&st, reply]
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
                reply(false, {}, "createEditor null");
                return;
            }
            if (st.editor->getWidth() < 16 || st.editor->getHeight() < 16)
                st.editor->setSize(1000, 600);
            st.editorWindow = std::make_unique<EditorWindow>(
                st.plugin->getName(), st.editor.get());
            st.editorWindow->setVisible(true);
            HWND hwnd = (HWND)st.editorWindow->getWindowHandle();
            juce::DynamicObject::Ptr res(new juce::DynamicObject());
            res->setProperty("hwnd", "0x" + juce::String::toHexString((juce::int64)(uintptr_t)hwnd));
            res->setProperty("w", st.editor->getWidth());
            res->setProperty("h", st.editor->getHeight());
            reply(true, juce::var(res.get()));
        });
    }
    else if (op == op::kCloseEditor)
    {
        juce::MessageManager::callAsync([&st, reply]
        {
            st.editorWindow.reset();
            st.editor.reset();
            reply(true, {});
        });
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
        st.running.store(false, std::memory_order_release);
        juce::MessageManager::callAsync([] { juce::JUCEApplicationBase::quit(); });
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
static void hostLogf(const char* fmt, ...)
{
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
        if (n > 0 && suffixLen > 0
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
            if (up > 0 && up < sizeof(userprofile) - sizeof(suffix))
                std::snprintf(path, sizeof(path), "%s%s", userprofile, suffix);
            else
                std::snprintf(path, sizeof(path),
                              "C:\\slopsmith-vst-host-%lu.log", pid);
        }
        g_hostLog = std::fopen(path, "a");
        if (g_hostLog)
            hostLogf("\n==== slopsmith-vst-host pid=%lu starting ====", pid);
    }

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
        st.running.store(false, std::memory_order_release);
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
    if (g_hostLog) { std::fclose(g_hostLog); g_hostLog = nullptr; }
    return 0;
}
