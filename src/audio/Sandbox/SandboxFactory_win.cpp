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
    return kDefaultNeedsSandboxManufacturers.contains(desc.manufacturerName);
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
    cfg.audio.maxBlockSamples = (uint32_t)juce::jmax(blockSize, 64);
    cfg.audio.maxChannels = 2;
    cfg.audio.maxBlocks = kAudioMaxBlocks;

    return SandboxedProcessor::spawn(cfg, errorOut);
}

} // namespace slopsmith::sandbox
