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
    // child process) into the known-plugins list.
    void addPluginsFromXml(const juce::String& xml);

    // Load a plugin instance
    std::unique_ptr<juce::AudioPluginInstance> loadPlugin(
        const juce::String& fileOrIdentifier,
        double sampleRate, int blockSize,
        juce::String& errorMessage);

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
