// AudioChannel — lock-free audio shared-memory ring between host and sandbox.
//
// Layout described in Protocol.h (AudioShmHeader). One mapping per sandbox;
// the host creates it before spawning the subprocess and passes the mapping
// name on the command line.
//
// Threading: the host's audio thread calls `pushInputBlock()` (publishes a
// block of audio + the per-block MIDI queue together) and `popBlock(true,…)`
// (drains the matching processed-output block). The sandbox's audio thread
// runs the mirror: `popInputBlock()` → plugin->processBlock → `pushBlock(true,
// …)`. Both sides block on the partner's OS event with a short timeout, so
// dropouts are detectable. `signalSandboxWake()` lets the host break the
// sandbox out of its popInputBlock wait without publishing a real block —
// used by the audio-thread pause/drain protocol around non-realtime control
// ops (kPrepare / kSetBlockSize / kGetState / kSetState).

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>

#include "Protocol.h"

namespace slopsmith::sandbox {

class AudioChannel
{
public:
    // Sentinel names returned/consumed by the OS API.
    struct Names
    {
        juce::String shm;       // file-mapping object name
        juce::String evtToHost; // sandbox→host (output ready)
        juce::String evtToSandbox; // host→sandbox (input ready)
    };

    AudioChannel();
    ~AudioChannel();

    // Host side: create the shm + both events, return the names for passing to
    // the subprocess.
    bool createHostSide(const AudioDimensions& dims, Names& namesOut,
                        juce::String& errorOut);

    // Sandbox side: open existing shm + events by name.
    bool openSandboxSide(const Names& names, juce::String& errorOut);

    // Whichever side we are: copy a block of audio in (host: input → sandbox;
    // sandbox: processed output → host). Returns false if the ring is full.
    //
    // For the INPUT direction, callers MUST use pushInputBlock() — pushBlock
    // does not touch the slot's MidiQueue, so a direct pushBlock(false, ...)
    // would leave whatever MIDI count was in the slot from a prior
    // pushInputBlock and the next popInputBlock would replay those stale
    // events against fresh audio. Today the only input producer is
    // SandboxedProcessor::processBlock and it always goes through
    // pushInputBlock; this overload exists for the OUTPUT direction
    // (sandbox → host audio, no MIDI carried back).
    bool pushBlock(bool isOutputRing, const juce::AudioBuffer<float>& src,
                   int numSamples);

    // Mirror of pushBlock: drain one block out. Returns false on timeout.
    bool popBlock(bool isOutputRing, juce::AudioBuffer<float>& dst,
                  int numSamples, int timeoutMs);

    // Host-side input push that bundles per-block MIDI into the upcoming
    // slot's MidiQueue. Events past kMidiEventsPerSlot (or larger than
    // kMidiEventMaxBytes, e.g. SysEx) bump the queue's overflow counter and
    // are dropped. The audio thread never blocks; lossy MIDI is the
    // documented v2 policy.
    bool pushInputBlock(const juce::AudioBuffer<float>& src,
                        const juce::MidiBuffer& midi,
                        int numSamples);

    // Sandbox-side input pop that drains the matching MidiQueue into `midi`.
    // The MIDI queue is read before the read-index is advanced so the slot
    // stays owned by the sandbox until both audio and MIDI are consumed.
    bool popInputBlock(juce::AudioBuffer<float>& dst,
                       juce::MidiBuffer& midi,
                       int numSamples, int timeoutMs);

    // Wake the sandbox audio thread out of its popInputBlock wait without
    // pushing a real block. Used by the host-side audio-thread pause/drain
    // protocol so non-realtime control ops don't have to wait the full
    // popInputBlock timeout for the audio worker to notice the pause flag.
    // Sandbox-side: also called on shutdown to break the loop's WaitFor.
    void signalSandboxWake();

    const AudioDimensions& dims() const noexcept { return cachedDims; }

    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    AudioDimensions cachedDims;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioChannel)
};

} // namespace slopsmith::sandbox
