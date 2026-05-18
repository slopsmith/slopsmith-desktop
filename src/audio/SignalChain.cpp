#include "SignalChain.h"

// ── ProcessorSlot ─────────────────────────────────────────────────────────────

juce::MemoryBlock ProcessorSlot::getState() const
{
    juce::MemoryBlock state;
    if (processor)
        processor->getStateInformation(state);
    return state;
}

void ProcessorSlot::setState(const juce::MemoryBlock& state)
{
    if (processor && state.getSize() > 0)
        processor->setStateInformation(state.getData(), (int)state.getSize());
}

// ── SignalChain ───────────────────────────────────────────────────────────────

SignalChain::SignalChain() {}

SignalChain::~SignalChain()
{
    const juce::ScopedLock sl(lock);
    slots.clear();
}

void SignalChain::prepare(double sampleRate, int blockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize = blockSize;

    const juce::ScopedLock sl(lock);
    for (auto* slot : slots)
    {
        if (slot->processor)
        {
            slot->processor->releaseResources();
            slot->processor->setPlayConfigDetails(2, 2, sampleRate, blockSize);
            slot->processor->prepareToPlay(sampleRate, blockSize);
        }
    }
}

void SignalChain::releaseResources()
{
    const juce::ScopedLock sl(lock);
    for (auto* slot : slots)
    {
        if (slot->processor)
            slot->processor->releaseResources();
    }
}

void SignalChain::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const juce::ScopedTryLock sl(lock);
    if (!sl.isLocked()) return;

    // Drain pending MIDI messages from the lock-free queue
    struct DrainedMsg { int slotId; juce::MidiMessage msg; };
    DrainedMsg drained[kMidiQueueSize];
    int numDrained = 0;

    const auto scope = midiQueueFifo.read(midiQueueFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1 && numDrained < kMidiQueueSize; ++i)
        drained[numDrained++] = { midiRingBuffer[(size_t)scope.startIndex1 + i].targetSlotId,
                                   midiRingBuffer[(size_t)scope.startIndex1 + i].msg };
    for (int i = 0; i < scope.blockSize2 && numDrained < kMidiQueueSize; ++i)
        drained[numDrained++] = { midiRingBuffer[(size_t)scope.startIndex2 + i].targetSlotId,
                                   midiRingBuffer[(size_t)scope.startIndex2 + i].msg };

    for (auto* slot : slots)
    {
        if (slot->processor && !slot->bypassed)
        {
            // Build per-slot MIDI buffer from drained messages
            juce::MidiBuffer slotMidi(midi); // start with pass-through MIDI
            for (int i = 0; i < numDrained; ++i)
            {
                if (drained[i].slotId == slot->id || drained[i].slotId == -1)
                    slotMidi.addEvent(drained[i].msg, 0);
            }
            slot->processor->processBlock(buffer, slotMidi);
        }
    }
}

void SignalChain::queueMidiMessage(int targetSlotId, const juce::MidiMessage& msg)
{
    const auto scope = midiQueueFifo.write(1);
    if (scope.blockSize1 > 0)
        midiRingBuffer[(size_t)scope.startIndex1] = { targetSlotId, msg };
    else if (scope.blockSize2 > 0)
        midiRingBuffer[(size_t)scope.startIndex2] = { targetSlotId, msg };
    // If queue full, message silently dropped (acceptable for PC messages)
}

int SignalChain::addProcessor(std::unique_ptr<juce::AudioProcessor> processor,
                               ProcessorSlot::Type type,
                               const juce::String& name,
                               const juce::String& path)
{
    if (!processor) return -1;

    processor->setPlayConfigDetails(2, 2, currentSampleRate, currentBlockSize);
    processor->prepareToPlay(currentSampleRate, currentBlockSize);

    auto slot = std::make_unique<ProcessorSlot>();
    slot->type = type;
    slot->processor = std::move(processor);
    slot->name = name;
    slot->path = path;
    slot->id = nextSlotId++;

    int id = slot->id;

    const juce::ScopedLock sl(lock);
    slots.add(slot.release());
    return id;
}

void SignalChain::removeProcessor(int slotId)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx >= 0) slots.remove(idx);
}

void SignalChain::moveProcessor(int fromIndex, int toIndex)
{
    const juce::ScopedLock sl(lock);
    if (fromIndex >= 0 && fromIndex < slots.size() &&
        toIndex >= 0 && toIndex < slots.size() && fromIndex != toIndex)
    {
        slots.move(fromIndex, toIndex);
    }
}

void SignalChain::setBypass(int slotId, bool bypassed)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx >= 0) slots[idx]->bypassed = bypassed;
}

void SignalChain::setMultiBypass(const juce::Array<std::pair<int, bool>>& changes)
{
    const juce::ScopedLock sl(lock);
    for (auto& [slotId, bypassed] : changes)
    {
        int idx = findSlotIndex(slotId);
        if (idx >= 0) slots[idx]->bypassed = bypassed;
    }
}

void SignalChain::clear()
{
    const juce::ScopedLock sl(lock);
    slots.clear();
}

int SignalChain::getNumSlots() const
{
    const juce::ScopedLock sl(lock);
    return slots.size();
}

const ProcessorSlot* SignalChain::getSlot(int slotId) const
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    return idx >= 0 ? slots[idx] : nullptr;
}

juce::Array<const ProcessorSlot*> SignalChain::getAllSlots() const
{
    juce::Array<const ProcessorSlot*> result;
    const juce::ScopedLock sl(lock);
    for (auto* slot : slots)
        result.add(slot);
    return result;
}

juce::Array<SignalChain::ParamInfo> SignalChain::getParameters(int slotId) const
{
    juce::Array<ParamInfo> result;
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx < 0) return result;

    auto* proc = slots[idx]->processor.get();
    if (!proc) return result;

    auto& params = proc->getParameters();
    for (int i = 0; i < params.size(); ++i)
    {
        ParamInfo info;
        info.index = i;
        info.name = params[i]->getName(128);
        info.value = params[i]->getValue();
        info.label = params[i]->getLabel();
        info.text = params[i]->getCurrentValueAsText();
        result.add(info);
    }
    return result;
}

void SignalChain::setParameter(int slotId, int paramIndex, float value)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx < 0) return;

    auto* proc = slots[idx]->processor.get();
    if (!proc) return;

    auto& params = proc->getParameters();
    if (paramIndex >= 0 && paramIndex < params.size())
        params[paramIndex]->setValue(value);
}

void SignalChain::setSlotState(int slotId, const juce::MemoryBlock& state)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx >= 0)
        slots[idx]->setState(state); // ProcessorSlot::setState() is null/empty-safe
}

// ── Presets ───────────────────────────────────────────────────────────────────

juce::String SignalChain::savePreset() const
{
    auto root = new juce::DynamicObject();
    root->setProperty("version", 1);

    juce::Array<juce::var> chainArray;
    const juce::ScopedLock sl(lock);

    for (auto* slot : slots)
    {
        auto slotObj = new juce::DynamicObject();
        slotObj->setProperty("id", slot->id);
        slotObj->setProperty("type", (int)slot->type);
        slotObj->setProperty("name", slot->name);
        slotObj->setProperty("path", slot->path);
        slotObj->setProperty("bypassed", slot->bypassed);

        // Save processor state as base64
        auto state = slot->getState();
        if (state.getSize() > 0)
            slotObj->setProperty("state", state.toBase64Encoding());

        chainArray.add(juce::var(slotObj));
    }

    root->setProperty("chain", juce::var(chainArray));
    return juce::JSON::toString(juce::var(root));
}

void SignalChain::loadPreset(const juce::String& json)
{
    // Preset loading is handled at a higher level (NodeAddon) because
    // it needs to re-instantiate processors (VSTs, NAMs, IRs) which
    // requires the VSTHost and other components. The chain just needs
    // to be rebuilt via addProcessor() calls followed by setState().
}

// ── Private ───────────────────────────────────────────────────────────────────

int SignalChain::findSlotIndex(int slotId) const
{
    for (int i = 0; i < slots.size(); ++i)
        if (slots[i]->id == slotId) return i;
    return -1;
}
