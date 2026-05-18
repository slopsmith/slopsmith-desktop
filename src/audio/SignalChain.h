#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>

// Represents a single processor slot in the signal chain.
// Can hold a VST3/AU/LV2 plugin, NAM model, or IR loader.
struct ProcessorSlot
{
    enum class Type { VST, NAM, IR, Empty };

    Type type = Type::Empty;
    std::unique_ptr<juce::AudioProcessor> processor;
    juce::String name;
    juce::String path; // plugin file path, NAM model path, or IR file path
    bool bypassed = false;
    int id = 0;

    // For VST plugins — their state as base64 for preset save/load
    juce::MemoryBlock getState() const;
    void setState(const juce::MemoryBlock& state);
};

class SignalChain
{
public:
    SignalChain();
    ~SignalChain();

    void prepare(double sampleRate, int blockSize);
    void releaseResources();
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // Chain management
    int addProcessor(std::unique_ptr<juce::AudioProcessor> processor,
                     ProcessorSlot::Type type,
                     const juce::String& name,
                     const juce::String& path);
    void removeProcessor(int slotId);
    void moveProcessor(int fromIndex, int toIndex);
    void setBypass(int slotId, bool bypassed);
    void setMultiBypass(const juce::Array<std::pair<int, bool>>& changes);
    void clear();

    // Info
    int getNumSlots() const;
    const ProcessorSlot* getSlot(int slotId) const;
    juce::Array<const ProcessorSlot*> getAllSlots() const;

    // Parameters for a specific slot
    struct ParamInfo
    {
        int index;
        juce::String name;
        float value;
        juce::String label;
        juce::String text;
    };
    juce::Array<ParamInfo> getParameters(int slotId) const;
    void setParameter(int slotId, int paramIndex, float value);

    // Restore a processor's full state (a getStateInformation() blob) by slot
    // id. Used to re-apply per-slot VST state when the tone-switcher rebuilds
    // a chain processor-by-processor rather than via a whole-chain loadPreset.
    void setSlotState(int slotId, const juce::MemoryBlock& state);

    // Preset serialization
    juce::String savePreset() const;
    void loadPreset(const juce::String& json);

    // MIDI message injection (lock-free, called from N-API thread)
    void queueMidiMessage(int targetSlotId, const juce::MidiMessage& msg);

private:
    int findSlotIndex(int slotId) const;

    juce::OwnedArray<ProcessorSlot> slots;
    juce::CriticalSection lock;
    int nextSlotId = 1;

    double currentSampleRate = 48000.0;
    int currentBlockSize = 256;

    // Lock-free SPSC MIDI queue (N-API thread writes, audio thread reads)
    struct PendingMidiMessage { int targetSlotId = -1; juce::MidiMessage msg; };
    static constexpr int kMidiQueueSize = 64;
    std::array<PendingMidiMessage, kMidiQueueSize> midiRingBuffer;
    juce::AbstractFifo midiQueueFifo { kMidiQueueSize };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalChain)
};
