// Windows sandbox factory.
//
// Decides whether a plugin should be loaded through the sandbox and, if so,
// constructs a SandboxedProcessor (which spawns slopsmith-vst-host.exe).

#include "SandboxedProcessor.h"
#include "../VSTTrace.h"

#include <juce_core/juce_core.h>
#include <cmath>      // std::isfinite, std::lround
#include <limits>     // std::numeric_limits
#include <mutex>      // guards the runtime crash blocklist
#include <vector>     // dynamic buffer for SLOPSMITH_DEV_SANDBOX_PATH
#include <windows.h> // GetModuleHandleExW / GetModuleFileNameW

namespace slopsmith::sandbox {

namespace {

// Filename-only matching in v1. The manufacturer-based denylist was
// removed: at LoadVST time we synthesise a PluginDescription whose
// manufacturerName is empty (we haven't scanned the plugin yet), so a
// manufacturer match was unreachable in practice. The manufacturer +
// vst3 UID route lands with the proper plugin-scan / sandbox-list.json
// follow-up.
//
// v1 is effect-plugins only: the sandbox path doesn't yet deliver MIDI to
// the plugin (op::kMidiEvent is no-op'd in vst-host and gets sent over the
// control channel, which the audio-shm MIDI follow-up replaces). Forcing
// MIDI-driven instruments through here would silently make them mute.
//
// Once inline MIDI lands in PR #2, add the other NI offenders back:
//   "Massive", "Reaktor", "Kontakt", "Battery", "Komplete Kontrol",
//   "FM8", "Absynth", "Maschine", "Monark".
const juce::StringArray kDefaultNeedsSandboxFilenames = {
    "Guitar Rig",
    // PolyChrome DSP Graphene — its editor faults (access violation) when
    // created in-process; the sandbox owns the editor window out-of-process.
    // This is also the pre-seed for the runtime crash blocklist below: known
    // offenders never crash anyone, while unknown ones are caught after one
    // crash by the renderer's VST crash guard and registered via
    // setCrashedPlugins().
    "Graphene",
    // IK Multimedia TONEX — corrupts memory when hosted in-process and takes
    // the whole app down. Field minidumps show two manifestations of the same
    // disease: a 0xC0000374 heap double-free, and a 0xC0000005 DEP/execute
    // fault from a call through a recycled vtable. The fault is inside TONEX's
    // own code and only surfaces in-process, so route it out-of-process where
    // a crash is contained to the sandbox child instead of killing Slopsmith.
    // TONEX is an audio effect (no MIDI), so the sandbox MIDI caveat above
    // does not apply.
    "TONEX",
};

// Runtime crash blocklist: full plugin paths that crashed the app on a
// previous run, supplied by the renderer's VST crash guard via
// setCrashedPlugins(). Guarded by a mutex — set once at startup on the addon
// thread, read by shouldSandbox() on the JUCE message thread.
std::mutex g_crashedPluginsMutex;
juce::StringArray g_crashedPlugins;

} // anonymous

juce::File resolveSandboxExe()
{
    // Locate the directory of the .node DLL via GetModuleHandleEx with this
    // function's address — juce::File::currentExecutableFile returns the
    // host's exe (node.exe / electron.exe / slopsmith.exe) when we're loaded
    // as an addon, which points at the wrong directory entirely.
    juce::File addonDir;
    HMODULE selfModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                              | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&resolveSandboxExe),
                           &selfModule)
        && selfModule != nullptr)
    {
        // Grow the buffer until GetModuleFileNameW returns < capacity
        // (success). On truncation it returns == capacity with
        // ERROR_INSUFFICIENT_BUFFER. MAX_PATH is the floor for back-compat,
        // but long-paths-enabled installs / deep dev trees can blow past
        // it. Cap at 32K to mirror the Windows long-path ceiling.
        std::vector<wchar_t> buf(MAX_PATH + 1);
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(selfModule, buf.data(),
                                               (DWORD)buf.size());
            if (n > 0 && n < buf.size())
            {
                addonDir = juce::File(juce::String(buf.data())).getParentDirectory();
                break;
            }
            if (buf.size() >= 32768) break;
            buf.resize(buf.size() * 2);
        }
    }

    // Dev: build/Release/slopsmith-vst-host.exe next to the addon .node.
    if (addonDir.exists())
    {
        auto candidate = addonDir.getChildFile("slopsmith-vst-host.exe");
        if (candidate.existsAsFile()) return candidate;

        // Electron-packaged: resources/app.asar.unpacked/build/Release/...
        // currently the build/Release is the addon dir, so a sibling layout
        // resolves here. The full asar-unpacked lookup is on the packaging
        // follow-up (see PR-body checklist).
    }

    // Explicit dev override — opt-in via SLOPSMITH_DEV_SANDBOX_PATH.
    // The previous implicit CWD probe would have spawned any
    // `build/Release/slopsmith-vst-host.exe` happening to be in the
    // launch directory, which is a search-path attack vector. Fail
    // closed in production; require the env var for dev workflows.
    // Use the W variant + dynamic buffer so long-paths-enabled installs
    // and deep dev trees beyond MAX_PATH still work. First call probes
    // for the required size; second call reads into the right-sized
    // buffer. The first-call return semantics are: required-size-incl-NUL
    // if buffer too small, actual-length-excl-NUL otherwise, 0 if missing.
    const wchar_t* kVar = L"SLOPSMITH_DEV_SANDBOX_PATH";
    const DWORD probe = GetEnvironmentVariableW(kVar, nullptr, 0);
    if (probe > 0)
    {
        std::vector<wchar_t> buf(probe);
        const DWORD got = GetEnvironmentVariableW(kVar, buf.data(), probe);
        if (got > 0 && got < probe)
        {
            // Brace-init avoids the most-vexing-parse on
            // `File explicitPath(juce::String(buf.data()));`.
            const juce::File explicitPath{ juce::String(buf.data()) };
            if (explicitPath.existsAsFile()) return explicitPath;
        }
        else
        {
            VST_TRACE("SandboxFactory: SLOPSMITH_DEV_SANDBOX_PATH read race "
                      "(probe=%lu, got=%lu)",
                      (unsigned long)probe, (unsigned long)got);
        }
    }

    return {};
}

bool shouldSandbox(const juce::PluginDescription& desc)
{
    // Filename match: useful at LoadVST time before we have the manufacturer.
    // Anchor to prefix on a .vst3 file rather than a loose substring so a
    // non-NI plugin whose name happens to contain "Guitar Rig" doesn't get
    // forced into the sandbox.
    const auto path = juce::File(desc.fileOrIdentifier);
    if (!path.getFileName().endsWithIgnoreCase(".vst3"))
        return false;

    // Runtime crash blocklist: a plugin that took the app down in-process on
    // a previous run is routed through the sandbox even if it doesn't match
    // the filename heuristic below. Compared in canonical full-path form so a
    // slash-direction or relative/absolute difference between the persisted
    // path and the one handed to LoadVST can't cause a silent miss.
    {
        const std::lock_guard<std::mutex> lock(g_crashedPluginsMutex);
        const auto canonical = path.getFullPathName();
        if (g_crashedPlugins.contains(canonical, /*ignoreCase*/ true))
        {
            VST_TRACE("shouldSandbox: %s — on the runtime crash blocklist",
                      desc.fileOrIdentifier.toRawUTF8());
            return true;
        }
    }

    const auto basename = path.getFileNameWithoutExtension();
    for (auto& needle : kDefaultNeedsSandboxFilenames)
    {
        if (basename.startsWithIgnoreCase(needle))
        {
            VST_TRACE("shouldSandbox: %s — filename starts with '%s'",
                      desc.fileOrIdentifier.toRawUTF8(),
                      needle.toRawUTF8());
            return true;
        }
    }
    return false;
}

std::unique_ptr<juce::AudioProcessor> tryLoadSandboxed(
    const juce::PluginDescription& desc,
    double sampleRate, int blockSize,
    juce::String& errorOut)
{
    if (!shouldSandbox(desc))
        return nullptr;

    auto exe = resolveSandboxExe();
    if (!exe.existsAsFile())
    {
        errorOut = "slopsmith-vst-host.exe not found";
        return nullptr;
    }

    // Validate sampleRate before narrowing to uint32_t — `(uint32_t)NaN` is UB
    // and silently accepting 0 / negative / overflow makes a bad caller surface
    // as a late sandbox-spawn failure instead of a clear errorOut here.
    if (! std::isfinite(sampleRate) || sampleRate <= 0.0
        || sampleRate > (double)(std::numeric_limits<uint32_t>::max)())
    {
        errorOut = "invalid sampleRate: " + juce::String(sampleRate);
        return nullptr;
    }

    SandboxedProcessor::SpawnConfig cfg;
    cfg.pluginPath = desc.fileOrIdentifier;
    cfg.pluginName = desc.name.isNotEmpty() ? desc.name : "plugin";
    cfg.sandboxExePath = exe.getFullPathName();
    cfg.audio.sampleRate = (uint32_t)std::lround(sampleRate);
    // Clamp to the protocol cap: vst-host's kPrepare rejects blockSize
    // > kAudioMaxBlockSamples, so spawning a larger shm layout would later
    // fail the prepare round-trip rather than silently misbehave.
    cfg.audio.maxBlockSamples = (uint32_t)juce::jlimit(
        64, (int)kAudioMaxBlockSamples, blockSize);
    cfg.audio.maxChannels = 2;
    cfg.audio.maxBlocks = kAudioMaxBlocks;

    return SandboxedProcessor::spawn(cfg, errorOut);
}

void setCrashedPlugins(const juce::StringArray& pluginPaths)
{
    const std::lock_guard<std::mutex> lock(g_crashedPluginsMutex);
    // Store in canonical full-path form so the shouldSandbox() lookup matches
    // regardless of slash direction or relative/absolute differences between
    // the persisted path and the one LoadVST is given.
    g_crashedPlugins.clearQuick();
    for (const auto& p : pluginPaths)
        g_crashedPlugins.add(p.isNotEmpty() ? juce::File(p).getFullPathName() : p);
    VST_TRACE("setCrashedPlugins: %d plugin(s) on the runtime crash blocklist",
              g_crashedPlugins.size());
}

} // namespace slopsmith::sandbox
