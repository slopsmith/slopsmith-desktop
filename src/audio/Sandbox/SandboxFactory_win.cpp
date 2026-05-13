// Windows sandbox factory.
//
// Decides whether a plugin should be loaded through the sandbox and, if so,
// constructs a SandboxedProcessor (which spawns slopsmith-vst-host.exe).

#include "SandboxedProcessor.h"
#include "../VSTTrace.h"

#include <juce_core/juce_core.h>
#include <cmath>      // std::isfinite, std::lround
#include <limits>     // std::numeric_limits
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
};

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
        wchar_t buf[MAX_PATH + 1] = {};
        const DWORD n = GetModuleFileNameW(selfModule, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            addonDir = juce::File(juce::String(buf)).getParentDirectory();
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
    char devOverride[1024]{};
    const DWORD dn = GetEnvironmentVariableA("SLOPSMITH_DEV_SANDBOX_PATH",
                                             devOverride, sizeof(devOverride));
    if (dn >= sizeof(devOverride))
    {
        // GetEnvironmentVariableA returns required-size-including-NUL when
        // the buffer is too small. The user opted into this path explicitly
        // via env var, so a silent fall-through to "no override" makes
        // misconfigurations look like missing-env-var bugs. Trace it.
        VST_TRACE("SandboxFactory: SLOPSMITH_DEV_SANDBOX_PATH truncated "
                  "(required=%lu, buffer=%lu) — ignoring override",
                  (unsigned long)dn, (unsigned long)sizeof(devOverride));
    }
    else if (dn > 0)
    {
        // Use brace-init to avoid the most-vexing-parse on
        // `File explicitPath(juce::String(devOverride));`, which MSVC
        // reads as a function declaration.
        const juce::File explicitPath{ juce::String(devOverride) };
        if (explicitPath.existsAsFile()) return explicitPath;
    }

    return {};
}

} // anonymous

bool shouldSandbox(const juce::PluginDescription& desc)
{
    // Filename match: useful at LoadVST time before we have the manufacturer.
    // Anchor to prefix on a .vst3 file rather than a loose substring so a
    // non-NI plugin whose name happens to contain "Guitar Rig" doesn't get
    // forced into the sandbox.
    const auto path = juce::File(desc.fileOrIdentifier);
    if (!path.getFileName().endsWithIgnoreCase(".vst3"))
        return false;
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

} // namespace slopsmith::sandbox
