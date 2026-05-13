// AudioChannel — lock-free audio shared-memory ring between host and sandbox.
//
// Layout described in Protocol.h (AudioShmHeader). One mapping per sandbox;
// the host creates it before spawning the subprocess and passes the mapping
// name on the command line.
//
// Threading: the host's audio thread calls `pushInput()` / `popOutput()`. The
// sandbox's audio thread runs the mirrored loop in slopsmith-vst-host. Both
// sides are blocking on the partner OS event with a short timeout, so dropouts
// are detectable.

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
    bool pushBlock(bool isOutputRing, const juce::AudioBuffer<float>& src,
                   int numSamples);

    // Mirror of pushBlock: drain one block out. Returns false on timeout.
    bool popBlock(bool isOutputRing, juce::AudioBuffer<float>& dst,
                  int numSamples, int timeoutMs);

    const AudioDimensions& dims() const noexcept { return cachedDims; }

    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    AudioDimensions cachedDims;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioChannel)
};

} // namespace slopsmith::sandbox
