#include "VSTHost.h"
#include "VSTTrace.h"

// The out-of-process scan path is compiled only into the audio addon
// (SLOPSMITH_AUDIO_ADDON, set in src/audio/CMakeLists.txt). slopsmith-vst-host
// also links VSTHost.cpp but must NOT pull in SandboxFactory — and it never
// calls scanDirectories anyway (it runs the --scan-plugin one-shot instead).
#if JUCE_WINDOWS && defined(SLOPSMITH_AUDIO_ADDON)
 #include "Sandbox/SandboxedProcessor.h"
#endif

#if JUCE_WINDOWS && defined(SLOPSMITH_AUDIO_ADDON)
namespace {

// Probe one plugin file in a child slopsmith-vst-host.exe so a plugin that
// crashes / aborts / hangs during init can't take down the host process.
// Returns the descriptor XML on success; sets `reason` and returns empty on
// failure (spawn failure, timeout, non-zero exit, or no output).
juce::String scanPluginOutOfProcess(const juce::File& hostExe,
                                    const juce::String& pluginPath,
                                    int timeoutMs,
                                    juce::String& reason)
{
    const juce::File outFile = juce::File::createTempFile(".scan.xml");

    juce::ChildProcess proc;
    const juce::StringArray args {
        hostExe.getFullPathName(),
        "--scan-plugin", pluginPath,
        "--scan-out",    outFile.getFullPathName(),
    };
    if (! proc.start(args, 0))
    {
        reason = "failed to spawn scan host";
        outFile.deleteFile();
        return {};
    }
    if (! proc.waitForProcessToFinish(timeoutMs))
    {
        // A plugin that hangs during init (license-wait deadlock, modal
        // dialog) never returns — kill the child and move on.
        proc.kill();
        reason = "scan timed out after " + juce::String(timeoutMs) + " ms";
        outFile.deleteFile();
        return {};
    }
    const auto exitCode = proc.getExitCode();
    if (exitCode != 0)
    {
        reason = "scan host exited with code " + juce::String((int) exitCode);
        outFile.deleteFile();
        return {};
    }
    const juce::String xml = outFile.loadFileAsString();
    outFile.deleteFile();
    if (xml.isEmpty())
    {
        reason = "scan host produced no output";
        return {};
    }
    return xml;
}

} // anonymous
#endif

VSTHost::VSTHost()
{
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());

#if JUCE_PLUGINHOST_AU
    formatManager.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
#endif

#if JUCE_PLUGINHOST_LV2
    formatManager.addFormat(std::make_unique<juce::LV2PluginFormat>());
#endif
}

VSTHost::~VSTHost()
{
    cancelScan();
}

// ── Scanning ──────────────────────────────────────────────────────────────────

juce::StringArray VSTHost::getDefaultScanDirectories()
{
    juce::StringArray dirs;

#if JUCE_LINUX
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
             .getChildFile(".vst3").getFullPathName());
    dirs.add("/usr/lib/vst3");
    dirs.add("/usr/local/lib/vst3");
    // LV2
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
             .getChildFile(".lv2").getFullPathName());
    dirs.add("/usr/lib/lv2");
    dirs.add("/usr/local/lib/lv2");
#elif JUCE_MAC
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
             .getChildFile("Library/Audio/Plug-Ins/VST3").getFullPathName());
    dirs.add("/Library/Audio/Plug-Ins/VST3");
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
             .getChildFile("Library/Audio/Plug-Ins/Components").getFullPathName());
    dirs.add("/Library/Audio/Plug-Ins/Components");
#elif JUCE_WINDOWS
    dirs.add("C:\\Program Files\\Common Files\\VST3");
    dirs.add("C:\\Program Files (x86)\\Common Files\\VST3");
    auto localAppData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
    dirs.add(localAppData.getChildFile("VST3").getFullPathName());
#endif

    return dirs;
}

void VSTHost::scanDefaultDirectories(ScanProgressCallback callback)
{
    scanDirectories(getDefaultScanDirectories(), std::move(callback));
}

void VSTHost::scanDirectories(const juce::StringArray& directories, ScanProgressCallback callback)
{
    if (scanning.load()) return;

    scanning.store(true);
    scanCancelled.store(false);

    // Collect all plugin files first
    juce::StringArray filesToScan;
    for (auto& dir : directories)
    {
        juce::File d(dir);
        if (!d.isDirectory()) continue;

        // VST3
        for (auto& f : d.findChildFiles(juce::File::findFilesAndDirectories, true, "*.vst3"))
            filesToScan.addIfNotAlreadyThere(f.getFullPathName());

        // AU (macOS bundles)
#if JUCE_MAC
        for (auto& f : d.findChildFiles(juce::File::findFilesAndDirectories, true, "*.component"))
            filesToScan.addIfNotAlreadyThere(f.getFullPathName());
#endif

        // LV2
#if JUCE_PLUGINHOST_LV2
        for (auto& f : d.findChildFiles(juce::File::findDirectories, true, "*.lv2"))
            filesToScan.addIfNotAlreadyThere(f.getFullPathName());
#endif
    }

    const int totalFiles = filesToScan.size();
    int scannedCount = 0;

#if JUCE_WINDOWS && defined(SLOPSMITH_AUDIO_ADDON)
    // Out-of-process scan: probe each plugin in a child slopsmith-vst-host.exe.
    // The addon is loaded into the Electron main process via N-API, so a
    // plugin that crashes / aborts during in-process scanAndAddFile would take
    // the whole app down (see issue: iZotope/Melodyne abort() during scan).
    // A child process boundary is the only thing that contains an abort() on
    // an arbitrary plugin-spawned thread.
    {
        const juce::File hostExe = slopsmith::sandbox::resolveSandboxExe();
        if (hostExe.existsAsFile())
        {
            constexpr int kScanTimeoutMs = 20000;
            for (auto& file : filesToScan)
            {
                if (scanCancelled.load()) break;

                juce::String reason;
                const juce::String xml = scanPluginOutOfProcess(
                    hostExe, file, kScanTimeoutMs, reason);
                if (xml.isNotEmpty())
                    addPluginsFromXml(xml);
                else
                    juce::Logger::writeToLog("VST scan: skipped " + file
                                             + " — " + reason);

                ++scannedCount;
                const float progress = totalFiles > 0
                    ? (float) scannedCount / (float) totalFiles : 1.0f;
                if (callback)
                    callback(progress,
                             juce::File(file).getFileNameWithoutExtension());
            }
            scanning.store(false);
            return;
        }
        juce::Logger::writeToLog("VST scan: slopsmith-vst-host.exe not found —"
                                 " falling back to in-process scan");
    }
#endif

    // In-process scan — used on macOS/Linux, and as a fallback on Windows when
    // the out-of-process host binary can't be located.
    for (auto& file : filesToScan)
    {
        if (scanCancelled.load()) break;

        juce::String pluginName = juce::File(file).getFileNameWithoutExtension();

        for (auto* format : formatManager.getFormats())
        {
            if (scanCancelled.load()) break;

            juce::OwnedArray<juce::PluginDescription> found;
            {
                const juce::ScopedLock sl(listLock);
                knownPlugins.scanAndAddFile(file, true, found, *format);
            }

            for (auto* desc : found)
                pluginName = desc->name;
        }

        scannedCount++;
        float progress = totalFiles > 0 ? (float)scannedCount / (float)totalFiles : 1.0f;
        if (callback) callback(progress, pluginName);
    }

    scanning.store(false);
}

juce::String VSTHost::scanPluginFileToXml(const juce::String& path)
{
    juce::XmlElement root("PLUGINS");
    for (auto* format : formatManager.getFormats())
    {
        juce::OwnedArray<juce::PluginDescription> found;
        {
            const juce::ScopedLock sl(listLock);
            knownPlugins.scanAndAddFile(path, true, found, *format);
        }
        for (auto* desc : found)
            root.addChildElement(desc->createXml().release());
    }
    // Always a parseable document — <PLUGINS/> when the file yields nothing,
    // so the parent treats "scanned, empty" as success rather than failure.
    return root.toString();
}

void VSTHost::addPluginsFromXml(const juce::String& xml)
{
    const auto parsed = juce::parseXML(xml);
    if (parsed == nullptr) return;

    const juce::ScopedLock sl(listLock);
    for (auto* child : parsed->getChildIterator())
    {
        juce::PluginDescription desc;
        if (desc.loadFromXml(*child))
            knownPlugins.addType(desc);
    }
}

// ── Plugin Access ─────────────────────────────────────────────────────────────

juce::Array<VSTHost::PluginInfo> VSTHost::getKnownPlugins() const
{
    juce::Array<PluginInfo> result;
    const juce::ScopedLock sl(listLock);

    for (auto& desc : knownPlugins.getTypes())
    {
        PluginInfo info;
        info.name = desc.name;
        info.manufacturer = desc.manufacturerName;
        info.category = desc.category;
        info.formatName = desc.pluginFormatName;
        info.fileOrIdentifier = desc.fileOrIdentifier;
        info.uid = desc.createIdentifierString();
        info.isInstrument = desc.isInstrument;
        result.add(info);
    }

    return result;
}

std::unique_ptr<juce::AudioPluginInstance> VSTHost::loadPlugin(
    const juce::String& fileOrIdentifier,
    double sampleRate, int blockSize,
    juce::String& errorMessage)
{
    // Find matching description
    juce::PluginDescription matchedDesc;
    bool found = false;

    {
        const juce::ScopedLock sl(listLock);
        for (auto& desc : knownPlugins.getTypes())
        {
            if (desc.fileOrIdentifier == fileOrIdentifier ||
                desc.createIdentifierString() == fileOrIdentifier)
            {
                matchedDesc = desc;
                found = true;
                break;
            }
        }
    }

    if (!found)
    {
        // Try scanning the file directly if not in known list
        juce::OwnedArray<juce::PluginDescription> descs;
        for (auto* format : formatManager.getFormats())
        {
            const juce::ScopedLock sl(listLock);
            knownPlugins.scanAndAddFile(fileOrIdentifier, true, descs, *format);
        }

        if (descs.isEmpty())
        {
            errorMessage = "Plugin not found: " + fileOrIdentifier;
            return nullptr;
        }

        matchedDesc = *descs[0];
    }

    // Create instance synchronously
    juce::String error;
    VST_TRACE("VSTHost.loadPlugin: createPluginInstance BEGIN  name='%s' format='%s' file='%s' sr=%.0f bs=%d",
              matchedDesc.name.toRawUTF8(),
              matchedDesc.pluginFormatName.toRawUTF8(),
              matchedDesc.fileOrIdentifier.toRawUTF8(),
              sampleRate, blockSize);
    auto instance = formatManager.createPluginInstance(
        matchedDesc, sampleRate, blockSize, error);
    VST_TRACE("VSTHost.loadPlugin: createPluginInstance END    instance=%s error='%s'",
              instance ? "OK" : "null",
              error.toRawUTF8());

    if (!instance)
    {
        errorMessage = error.isNotEmpty() ? error : "Failed to create plugin instance";
        return nullptr;
    }

    return instance;
}

// ── Persistence ───────────────────────────────────────────────────────────────

void VSTHost::savePluginList(const juce::File& xmlFile)
{
    const juce::ScopedLock sl(listLock);
    if (auto xml = knownPlugins.createXml())
        xml->writeTo(xmlFile);
}

void VSTHost::loadPluginList(const juce::File& xmlFile)
{
    if (!xmlFile.existsAsFile()) return;

    if (auto xml = juce::XmlDocument::parse(xmlFile))
    {
        const juce::ScopedLock sl(listLock);
        knownPlugins.recreateFromXml(*xml);
    }
}
