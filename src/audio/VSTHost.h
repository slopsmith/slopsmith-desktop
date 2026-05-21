#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

class VSTHost
{
public:
    VSTHost();
    ~VSTHost();

    // Plugin scanning — runs on background thread, reports progress
    using ScanProgressCallback = std::function<void(float progress, const juce::String& pluginName)>;
    void scanDirectories(const juce::StringArray& directories, ScanProgressCallback callback);
    void scanDefaultDirectories(ScanProgressCallback callback);
    bool isScanning() const { return scanning.load(); }
    void cancelScan() { scanCancelled.store(true); }

    // Access scan results
    struct PluginInfo
    {
        juce::String name;
        juce::String manufacturer;
        juce::String category;
        juce::String formatName; // VST3, AU, LV2
        juce::String fileOrIdentifier;
        juce::String uid;
        bool isInstrument = false;
    };
    juce::Array<PluginInfo> getKnownPlugins() const;

    // Scan a single plugin file in-process and return its PluginDescriptions
    // serialised as XML (root <PLUGINS>, one child element per description;
    // <PLUGINS/> when the file yields nothing). Used by slopsmith-vst-host's
    // --scan-plugin subprocess mode so a crashy plugin can't take the app down.
    juce::String scanPluginFileToXml(const juce::String& path);

    // Merge PluginDescriptions from XML produced by scanPluginFileToXml (in a
    // child process) into the known-plugins list. Returns false if the XML
    // could not be parsed as a <PLUGINS> document (a child that exited 0 but
    // emitted garbage) — the caller treats that as a failed probe rather than
    // silently counting the plugin as scanned. A valid but empty <PLUGINS/>
    // returns true (the file genuinely yielded no descriptors).
    bool addPluginsFromXml(const juce::String& xml);

    // Load a plugin instance
    std::unique_ptr<juce::AudioPluginInstance> loadPlugin(
        const juce::String& fileOrIdentifier,
        double sampleRate, int blockSize,
        juce::String& errorMessage);

    // Async variant: uses JUCE's createPluginInstanceAsync so the message
    // thread is free to pump during plugin initialisation. Required for
    // plugins that post WM_USER / WM_TIMER messages to themselves during
    // init (AmpliTube, and a class of other DAW-targeted VST3s) — the sync
    // createPluginInstance would block the pump and the plugin's init never
    // finishes wiring up internal state, producing a half-wired editor that
    // crashes on its first WindowProc dispatch.
    //
    // Must be called from the JUCE message thread; the callback fires there
    // too. Callers waiting on the result must do so from a *different*
    // thread (e.g. a libuv worker) so the message thread can keep pumping.
    void loadPluginAsync(
        const juce::String& fileOrIdentifier,
        double sampleRate, int blockSize,
        std::function<void(std::unique_ptr<juce::AudioPluginInstance>, juce::String)> callback);

    // Persistence
    void savePluginList(const juce::File& xmlFile);
    void loadPluginList(const juce::File& xmlFile);

    // Default scan paths per platform
    static juce::StringArray getDefaultScanDirectories();

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::CriticalSection listLock;

    std::atomic<bool> scanning{false};
    std::atomic<bool> scanCancelled{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VSTHost)
};
