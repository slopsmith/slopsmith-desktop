// Slopsmith plugin-sandbox IPC protocol.
//
// One pair of files defines the wire format used by both the host (Slopsmith
// Desktop, inside the slopsmith_audio.node addon) and the sandbox subprocess
// (slopsmith-vst-host.exe).
//
// Two channels:
//   * Control: a bidirectional named pipe carrying length-prefixed JSON
//     messages. Used for everything that isn't per-block audio.
//   * Audio: shared-memory ring buffers plus a pair of auto-reset OS events.
//     Used for the audio fast-path; sized for one block at the configured
//     sample rate.
//
// See SANDBOX-DESIGN.md (in the PR description) for the rationale.

#pragma once

#include <cstdint>
#include <juce_core/juce_core.h>

namespace slopsmith::sandbox {

// Bumped whenever the wire format changes incompatibly. Host and sandbox MUST
// agree on this number during the `ready` handshake; mismatched versions abort
// the spawn and fall back to in-process loading.
inline constexpr uint32_t kProtocolVersion = 1;

// Magic number stamped at the head of the audio shared memory so a stale
// mapping from a crashed sandbox can be detected.
inline constexpr uint32_t kAudioShmMagic = 0x534C5341u; // 'SLSA'

// Frame format on the control pipe:
//   [u32 length-LE][utf8 json body of `length` bytes]
inline constexpr uint32_t kMaxControlMessageBytes = 8 * 1024 * 1024; // 8 MiB
inline constexpr uint32_t kControlPipeBufferBytes = 64 * 1024;

// Tightest reasonable budget: 4 blocks at 1024 samples / 8 channels / float32.
inline constexpr uint32_t kAudioMaxBlocks = 4;
inline constexpr uint32_t kAudioMaxBlockSamples = 1024;
inline constexpr uint32_t kAudioMaxChannels = 8;

// Wall-clock cap on host-side waits for sandbox replies. Plugins doing slow
// state-restore can legitimately take a while, so this is generous.
inline constexpr int kDefaultReplyTimeoutMs = 10000;

// Named-object suffixes used by the audio shm + event pair. Names are
// generated host-side and passed to the sandbox via command-line args, so
// only the host's AudioChannel::createHostSide actually uses these — but
// centralising them prevents future drift if the sandbox ever needs to
// reconstruct a name (e.g. for diagnostic logging that surfaces the names
// in stable form rather than via argv echo).
inline constexpr const char* kShmNameSuffix       = "audio";
inline constexpr const char* kEvtToHostSuffix     = "evt-out";
inline constexpr const char* kEvtToSandboxSuffix  = "evt-in";

// Editor size default applied when the plugin reports an invalid (< 16 in
// either axis) editor size. Used by both the sandbox host (kOpenEditor
// reply) and the host-side SandboxedEditor fallback so the two sides
// don't drift.
inline constexpr int kDefaultEditorWidth  = 1000;
inline constexpr int kDefaultEditorHeight = 600;

// Watchdog: a sandbox that doesn't send `ready` this fast is presumed broken.
// Generous because some plugins (NI Guitar Rig 6 in particular) spin up an
// embedded Qt5/QML engine on first load, which can take 8-12 seconds on a
// cold cache.
inline constexpr int kReadyTimeoutMs = 30000;

// Control channel — operation names.
//
// Kept as string constants (rather than an enum) because they appear verbatim
// in JSON on the wire and in log output; matching tools downstream don't have
// to know the enum.
namespace op {
    // Host → sandbox requests
    inline constexpr const char* kPrepare        = "prepare";
    inline constexpr const char* kSetBlockSize   = "setBlockSize";
    inline constexpr const char* kSetParameter   = "setParameter";
    inline constexpr const char* kListParameters = "listParameters";
    inline constexpr const char* kGetState       = "getState";
    inline constexpr const char* kSetState       = "setState";
    inline constexpr const char* kMidiEvent      = "midiEvent";
    inline constexpr const char* kOpenEditor     = "openEditor";
    inline constexpr const char* kResizeEditor   = "resizeEditor";
    inline constexpr const char* kCloseEditor    = "closeEditor";
    inline constexpr const char* kShutdown       = "shutdown";
}

// Control channel — sandbox-originated event names (requestId is null).
namespace event {
    inline constexpr const char* kReady             = "ready";
    inline constexpr const char* kParameterChanged  = "parameterChanged";
    inline constexpr const char* kEditorClosed      = "editorClosed";
    inline constexpr const char* kLog               = "log";
    inline constexpr const char* kError             = "error";
    inline constexpr const char* kGoodbye           = "goodbye";
}

// Shared-memory layout for the audio fast path.
//
// All offsets are bytes from the start of the mapping. Sized at spawn time
// from the prepared sample rate / block size / channel count; the values
// below are hard caps a sandbox will refuse to exceed.
//
// Atomic indices use std::atomic<uint64_t>. The host writes `writeIdx` and
// reads `readIdx`; the sandbox is the inverse. Modulo-`maxBlocks` gives the
// block slot. The producer drops blocks (incrementing `xruns`) rather than
// blocking if the consumer is more than `maxBlocks` behind.
struct AudioShmHeader
{
    uint32_t magic = 0;             // kAudioShmMagic
    uint32_t protocolVersion = 0;   // kProtocolVersion
    uint32_t maxBlocks = 0;
    uint32_t maxBlockSamples = 0;
    uint32_t maxChannels = 0;
    uint32_t sampleRate = 0;

    // Plain uint64_t for shm layout portability across the host/sandbox
    // boundary. The .cpp side accesses each via std::atomic_ref<uint64_t>
    // at the point of use (C++20). Don't reintroduce reinterpret_cast to
    // std::atomic<uint64_t>* — that's not layout-guaranteed.
    alignas(8) uint64_t writeIdx = 0;   // host increments
    alignas(8) uint64_t readIdx  = 0;   // sandbox increments
    // Both counters are direction-agnostic: either side bumps the same field.
    // xruns covers any pushBlock where the destination ring was full;
    // dropouts covers any popBlock that timed out waiting for the partner.
    // Splitting per direction is on the audio-shm-MIDI follow-up checklist.
    alignas(8) uint64_t xruns    = 0;
    alignas(8) uint64_t dropouts = 0;

    // Byte offsets of the two rings inside the mapping. Convenient for tools
    // and asserts; computed at spawn time.
    uint64_t inputRingOffset = 0;
    uint64_t outputRingOffset = 0;
    uint64_t ringBytesPerSlot = 0;
};

// Convenience: a fully-validated set of audio dimensions, agreed at handshake.
struct AudioDimensions
{
    uint32_t maxBlocks = kAudioMaxBlocks;
    uint32_t maxBlockSamples = kAudioMaxBlockSamples;
    uint32_t maxChannels = 2;
    uint32_t sampleRate = 48000;

    constexpr bool operator==(const AudioDimensions& o) const
    {
        return maxBlocks == o.maxBlocks && maxBlockSamples == o.maxBlockSamples
            && maxChannels == o.maxChannels && sampleRate == o.sampleRate;
    }

    constexpr uint64_t bytesPerSlot() const
    {
        // float32, planar across channels.
        return uint64_t(maxBlockSamples) * uint64_t(maxChannels) * sizeof(float);
    }

    constexpr uint64_t totalShmBytes() const
    {
        // Header + two rings.
        return sizeof(AudioShmHeader) + 2 * uint64_t(maxBlocks) * bytesPerSlot();
    }
};

// JSON helpers — thin wrappers around juce::JSON to keep call sites tidy.
// All sandbox messages flow through these so the framing/encoding is
// consistent (and any future migration to a binary codec is one-spot).
namespace wire {

// Build the standard envelope:
//   { "v": 1, "id": <int|null>, "op": "<name>", "args": { ... } }
juce::var makeRequest(int requestId, const char* op, const juce::var& args);

// Sandbox-originated:
//   { "v": 1, "event": "<name>", "data": { ... } }
juce::var makeEvent(const char* eventName, const juce::var& data);

// Reply:
//   { "v": 1, "id": <int>, "ok": true/false, "result": ..., "error": ... }
juce::var makeReply(int requestId, bool ok, const juce::var& result,
                    const juce::String& errorMessage = {});

// Serialise to a UTF-8 buffer (no length prefix — that's the channel's job).
juce::MemoryBlock encode(const juce::var& v);

// Parse a UTF-8 buffer back into a var. Returns juce::var::undefined() on parse
// error (and stashes the reason in `errorOut` if provided).
juce::var decode(const void* data, size_t bytes, juce::String* errorOut = nullptr);

} // namespace wire

} // namespace slopsmith::sandbox
