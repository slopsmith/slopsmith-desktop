// Windows sandbox factory.
//
// Decides whether a plugin should be loaded through the sandbox and, if so,
// constructs a SandboxedProcessor (which spawns slopsmith-vst-host.exe).

#include "SandboxedProcessor.h"

#include <juce_core/juce_core.h>

namespace slopsmith::sandbox {

namespace {

// Static rules: the manufacturer names below are presumed Qt5-using and
// therefore go through the sandbox by default. Users can extend this list
// via %APPDATA%/Slopsmith/sandbox-list.json — that work lands in a follow-up
// commit.
const juce::StringArray kDefaultNeedsSandboxManufacturers = {
    "Native Instruments",
    "Native Instruments GmbH",
};

// Filename substrings of NI plugins we know need the sandbox. At LoadVST
// time we don't have the manufacturer yet (that would require scanning the
// plugin first), so we match the file name. Case-insensitive.
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
    // Packaged: resources/bin/slopsmith-vst-host.exe relative to the addon DLL.
    auto self = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile);
    auto candidate = self.getParentDirectory()
                          .getChildFile("resources/bin/slopsmith-vst-host.exe");
    if (candidate.existsAsFile()) return candidate;

    // Dev: build/Release/slopsmith-vst-host.exe relative to the addon .node.
    candidate = self.getParentDirectory()
                     .getChildFile("slopsmith-vst-host.exe");
    if (candidate.existsAsFile()) return candidate;

    // Repo layout while iterating: build\Release\ next to the .node
    auto buildDir = juce::File::getCurrentWorkingDirectory()
                         .getChildFile("build/Release/slopsmith-vst-host.exe");
    if (buildDir.existsAsFile()) return buildDir;

    return {};
}

} // anonymous

bool shouldSandbox(const juce::PluginDescription& desc)
{
    if (kDefaultNeedsSandboxManufacturers.contains(desc.manufacturerName))
    {
        VST_TRACE("shouldSandbox: %s — manufacturer '%s' on denylist",
                  desc.fileOrIdentifier.toRawUTF8(),
                  desc.manufacturerName.toRawUTF8());
        return true;
    }

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

    SandboxedProcessor::SpawnConfig cfg;
    cfg.pluginPath = desc.fileOrIdentifier;
    cfg.pluginName = desc.name.isNotEmpty() ? desc.name : "plugin";
    cfg.sandboxExePath = exe.getFullPathName();
    cfg.audio.sampleRate = (uint32_t)sampleRate;
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
