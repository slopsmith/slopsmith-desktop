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
#include <cstdarg>
#include <cstring>
#include <thread>

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

bool parseArgs(int argc, wchar_t** argv, Args& out)
{
    for (int i = 1; i < argc; i += 2)
    {
        if (i + 1 >= argc) return false;
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
    }
    return out.pluginPath.isNotEmpty() && out.controlPipe.isNotEmpty()
        && out.audio.shm.isNotEmpty();
}

class EditorWindow : public juce::DocumentWindow
{
public:
    EditorWindow(const juce::String& name, juce::AudioProcessorEditor* ed)
        : DocumentWindow(name, juce::Colours::darkgrey, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setContentNonOwned(ed, true);
        // Position offscreen until the host reparents us.
        setTopLeftPosition(-32000, -32000);
        centreWithSize(ed->getWidth(), ed->getHeight());
    }
    void closeButtonPressed() override {}
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
    if (op == op::kPrepare)
    {
        double sr = (double)args.getProperty("sampleRate", 48000);
        int bs    = (int)args.getProperty("blockSize", 256);
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
        st.control.sendReply(requestId, true, juce::var(res.get()));
    }
    else if (op == op::kOpenEditor)
    {
        if (!st.plugin || !st.plugin->hasEditor())
        {
            st.control.sendReply(requestId, false, {}, "no editor");
            return;
        }
        // Must run on the message thread.
        juce::MessageManager::callAsync([&st, requestId]
        {
            st.editor.reset(st.plugin->createEditor());
            if (!st.editor)
            {
                st.control.sendReply(requestId, false, {}, "createEditor null");
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
            st.control.sendReply(requestId, true, juce::var(res.get()));
        });
    }
    else if (op == op::kCloseEditor)
    {
        juce::MessageManager::callAsync([&st, requestId]
        {
            st.editorWindow.reset();
            st.editor.reset();
            st.control.sendReply(requestId, true, {});
        });
    }
    else if (op == op::kGetState)
    {
        juce::MemoryBlock mb;
        if (st.plugin) st.plugin->getStateInformation(mb);
        juce::DynamicObject::Ptr res(new juce::DynamicObject());
        res->setProperty("stateBase64",
            juce::Base64::toBase64(mb.getData(), mb.getSize()));
        st.control.sendReply(requestId, true, juce::var(res.get()));
    }
    else if (op == op::kSetState)
    {
        auto b64 = args.getProperty("stateBase64", "").toString();
        juce::MemoryOutputStream mo;
        juce::Base64::convertFromBase64(mo, b64);
        if (st.plugin)
            st.plugin->setStateInformation(mo.getData(), (int)mo.getDataSize());
        st.control.sendReply(requestId, true, {});
    }
    else if (op == op::kSetParameter)
    {
        int idx = (int)args.getProperty("index", -1);
        float val = (float)(double)args.getProperty("value", 0.0);
        if (st.plugin && idx >= 0 && idx < st.plugin->getParameters().size())
            st.plugin->getParameters()[idx]->setValue(val);
        st.control.sendReply(requestId, true, {});
    }
    else if (op == op::kShutdown)
    {
        st.control.sendReply(requestId, true, {});
        st.running.store(false, std::memory_order_release);
        juce::MessageManager::callAsync([] { juce::JUCEApplicationBase::quit(); });
    }
    else if (op == op::kMidiEvent)
    {
        // Fire-and-forget — but no-op for now. Bundled MIDI is a follow-up.
    }
    else
    {
        st.control.sendReply(requestId, false, {}, "unknown op: " + op);
    }
}

} // anonymous

// Debug-log path: %TEMP%\slopsmith-vst-host-<pid>.log. Plain text, line-buffered
// so a fast-fail still leaves a useful tail. This is essential because the
// subprocess runs hidden (no console) so fprintf(stderr) goes nowhere; without
// the file we can't see why it died.
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
        if (n > 0 && n < sizeof(path))
            std::strncat(path, "\\slopsmith-vst-host.log", sizeof(path) - n - 1);
        else
            std::strncpy(path, "C:\\slopsmith-vst-host.log", sizeof(path) - 1);
        g_hostLog = std::fopen(path, "a");
        if (g_hostLog)
            hostLogf("\n==== slopsmith-vst-host pid=%lu starting ====",
                     (unsigned long)GetCurrentProcessId());
    }

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Args parsed;
    if (!parseArgs(argc, argv, parsed))
    {
        hostLogf("bad args (argc=%d)", argc);
        for (int i = 0; i < argc; ++i)
        {
            char buf[512]; std::snprintf(buf, sizeof(buf), "  argv[%d]=%ls", i, argv[i]);
            hostLogf("%s", buf);
        }
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
    st.control.start({}, [&st](const juce::String&)
    {
        st.running.store(false, std::memory_order_release);
    });

    hostLogf("sending ready event");
    st.control.sendEvent(event::kReady, pluginMetadata(*st.plugin));
    hostLogf("ready event sent");

    st.audioThread = std::thread([&st] { runAudioThread(st); });

    // Pump the main message loop. JUCE's MessageManager is bound to this
    // thread (the OS main thread), which is the key correctness property
    // for Qt-using plugins per the diag PoC.
    while (st.running.load(std::memory_order_acquire))
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);

    st.audioThread.join();
    st.editorWindow.reset();
    st.editor.reset();
    st.plugin.reset();
    st.control.sendEvent(event::kGoodbye, {});
    st.control.stop();
    return 0;
}
