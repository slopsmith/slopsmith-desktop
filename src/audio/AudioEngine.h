#pragma once
#include "SignalChain.h"
#include "PitchDetector.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    SignalChain& getSignalChain() { return signalChain; }
    PitchDetector& getPitchDetector() { return pitchDetector; }

    // Device enumeration
    struct DeviceTypeInfo
    {
        juce::String name;
        juce::StringArray inputDevices;
        juce::StringArray outputDevices;
    };
    juce::Array<DeviceTypeInfo> getDeviceTypes();
    juce::Array<double> getSampleRates();
    juce::Array<int> getBufferSizes();
    juce::String getCurrentDeviceType();
    juce::String getCurrentInputDevice();
    juce::String getCurrentOutputDevice();
    double getCurrentSampleRate() const { return currentSampleRate; }
    int getCurrentBlockSize() const { return currentBlockSize; }

    // Device selection
    bool setDeviceType(const juce::String& typeName);
    bool setAudioDevice(const juce::String& inputName, const juce::String& outputName,
                        double sampleRate = 48000.0, int bufferSize = 256);

    // Audio start/stop
    void startAudio();
    void stopAudio();
    bool isAudioRunning() const { return audioRunning; }

    // Gain controls
    void setInputGain(float gain) { inputGain.store(gain); }
    void setOutputGain(float gain) { outputGain.store(gain); }
    float getInputGain() const { return inputGain.load(); }
    float getOutputGain() const { return outputGain.load(); }

    // Input channel selection (for multi-channel interfaces like Valeton GP-5)
    // 0=left (dry), 1=right (wet), -1=both (mono mix)
    void setInputChannel(int channel) { selectedInputChannel.store(channel); }
    int getInputChannel() const { return selectedInputChannel.load(); }

    // Monitor mute — when true, input is still processed (pitch detection, metering)
    // but output is silenced unless there are processors in the signal chain
    void setMonitorMute(bool mute) { monitorMuted.store(mute); }
    bool isMonitorMuted() const { return monitorMuted.load(); }

    // Backing track
    void setBackingVolume(float vol) { backingVolume.store(vol); }
    bool loadBackingTrack(const juce::File& file);
    void setBackingPosition(double seconds);
    void startBacking();
    void stopBacking();
    // Non-blocking reads — do not acquire backingLock and never block the audio callback
    bool isBackingPlaying() const { return backingPlaying.load(); }
    double getBackingPosition() const { return cachedBackingPosition.load(); }
    double getBackingDuration() const { return cachedBackingDuration.load(); }

    // Metering (read from any thread — atomic)
    float getInputLevel() const { return currentInputLevel.load(); }
    float getOutputLevel() const { return currentOutputLevel.load(); }
    float getInputPeak() const { return inputPeak.load(); }
    float getOutputPeak() const { return outputPeak.load(); }
    void resetPeaks();

    // Latency
    double getLatencyMs() const;

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputData,
                                          int numInputChannels,
                                          float* const* outputData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void stopBackingNoLock(); // stop transport without acquiring backingLock (caller holds it)

    juce::AudioDeviceManager deviceManager;
    SignalChain signalChain;
    PitchDetector pitchDetector;
    juce::AudioFormatManager formatManager;

    std::atomic<float> inputGain{1.0f};
    std::atomic<float> outputGain{1.0f};
    std::atomic<float> backingVolume{0.7f};
    std::atomic<float> currentInputLevel{0.0f};
    std::atomic<float> currentOutputLevel{0.0f};
    std::atomic<float> inputPeak{0.0f};
    std::atomic<float> outputPeak{0.0f};
    std::atomic<int> selectedInputChannel{-1}; // -1 = mono mix
    std::atomic<bool> monitorMuted{true}; // mute pass-through by default

    // Backing track
    std::unique_ptr<juce::AudioFormatReaderSource> backingSource;
    std::unique_ptr<juce::AudioTransportSource> backingTransport;
    juce::AudioBuffer<float> backingBuffer;
    std::atomic<bool> backingPlaying{false};
    std::atomic<double> cachedBackingPosition{0.0};
    std::atomic<double> cachedBackingDuration{0.0};
    juce::CriticalSection backingLock;

    bool audioRunning = false;
    double currentSampleRate = 48000.0;
    int currentBlockSize = 256;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
