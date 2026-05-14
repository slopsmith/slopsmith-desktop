// audio_channel_midi_test — exercise pushInputBlock / popInputBlock + the
// global midiOverflows counter without spawning a subprocess.
//
// Closes the v2/v3 review-thread concern that the inline-MIDI path had no
// automated coverage (the existing GR6 smoke driver only pushes empty
// MidiBuffers). Both ends of an AudioChannel are opened in the same
// process — createHostSide on one instance, openSandboxSide on a second
// instance using the same Names — so we don't need a real spawn.
//
// Win32-only for the same reason AudioChannel.cpp is.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "../../src/audio/Sandbox/Protocol.h"
#include "../../src/audio/Sandbox/AudioChannel.h"

#include <atomic>
#include <cstdio>

#include <windows.h>  // OpenFileMappingW / MapViewOfFile for HeaderPeek

using namespace slopsmith::sandbox;

namespace {

int g_failed = 0;
int g_passed = 0;

void check(bool cond, const char* what, const char* file, int line)
{
    if (cond) { ++g_passed; return; }
    ++g_failed;
    std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", what, file, line);
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

// Reach into the shm header via the host's createHostSide-time mapping name
// to read midiOverflows. Both ends of the AudioChannel point at the same
// shared mapping, so any read off either side gives the same value — we use
// the host instance for convenience.
uint64_t readMidiOverflows(const AudioShmHeader* hdr)
{
    return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(hdr->midiOverflows))
        .load(std::memory_order_relaxed);
}

// Helper: open a fresh host+sandbox AudioChannel pair with a given dims, run
// a callback against both ends, then tear down. The pair is unique per call
// (suffix-randomised mapping name) so concurrent test runs don't collide.
struct ChannelPair
{
    AudioChannel host;
    AudioChannel sandbox;
    AudioChannel::Names names;
    AudioDimensions dims;
    juce::String err;
    bool ok = false;

    explicit ChannelPair(const AudioDimensions& d) : dims(d)
    {
        ok = host.createHostSide(dims, names, err);
        if (!ok) return;
        ok = sandbox.openSandboxSide(names, err);
    }
};

// Get a non-mutating pointer to the shared header (host side).
// AudioChannel doesn't expose this; we cheat via the dims accessor + a
// synthetic peek via a one-block round-trip-counter of midiOverflows. Since
// we don't have a public accessor and don't want to bloat the public API
// just for tests, we read midiOverflows BEFORE and AFTER each push and
// assert the delta. ChannelPair carries dims for the math.
//
// Actually, simpler: the host instance has `dims()` accessor and the
// underlying header is mapped at the top of the SHM. We can re-open the
// mapping by name to peek the header without going through AudioChannel.
struct HeaderPeek
{
    HANDLE mapping = nullptr;
    void*  view = nullptr;
    const AudioShmHeader* hdr = nullptr;

    explicit HeaderPeek(const juce::String& shmName)
    {
        mapping = OpenFileMappingW(FILE_MAP_READ, FALSE,
                                   shmName.toWideCharPointer());
        if (!mapping) return;
        view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0,
                             sizeof(AudioShmHeader));
        if (view) hdr = reinterpret_cast<const AudioShmHeader*>(view);
    }
    ~HeaderPeek()
    {
        if (view)    UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
    }
};

void testRoundtripSmallBuffer()
{
    std::printf("test: roundtrip small MidiBuffer (count, frames, bytes)\n");
    AudioDimensions dims;            // defaults: 4 blocks × 1024 samples × 2 ch
    ChannelPair pair{dims};
    CHECK(pair.ok);

    juce::AudioBuffer<float> srcAudio((int)dims.maxChannels, 256);
    srcAudio.clear();
    juce::MidiBuffer midi;
    // 3 events at distinct frames — Note On, CC, Note Off.
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 7, 64), 64);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 200);

    HeaderPeek peek{pair.names.shm};
    CHECK(peek.hdr != nullptr);
    const uint64_t overflowsBefore = peek.hdr ? readMidiOverflows(peek.hdr) : 0;

    CHECK(pair.host.pushInputBlock(srcAudio, midi, 256));

    juce::AudioBuffer<float> dstAudio((int)dims.maxChannels, 256);
    juce::MidiBuffer drained;
    CHECK(pair.sandbox.popInputBlock(dstAudio, drained, 256, /*timeoutMs*/ 1000));

    int n = 0;
    int frames[3] = {-1, -1, -1};
    juce::uint8 firstByte[3] = {0, 0, 0};
    for (const auto meta : drained)
    {
        if (n < 3) { frames[n] = meta.samplePosition;
                     firstByte[n] = meta.getMessage().getRawData()[0]; }
        ++n;
    }
    CHECK(n == 3);
    CHECK(frames[0] == 0);
    CHECK(frames[1] == 64);
    CHECK(frames[2] == 200);
    // Note On status nibble = 0x90, CC = 0xB0, Note Off = 0x80.
    CHECK((firstByte[0] & 0xF0) == 0x90);
    CHECK((firstByte[1] & 0xF0) == 0xB0);
    CHECK((firstByte[2] & 0xF0) == 0x80);

    // No overflows expected on the happy path.
    const uint64_t overflowsAfter = peek.hdr ? readMidiOverflows(peek.hdr) : 0;
    CHECK(overflowsAfter == overflowsBefore);
}

void testSysExBumpsOverflow()
{
    std::printf("test: SysEx-sized event drops + bumps midiOverflows\n");
    AudioDimensions dims;
    ChannelPair pair{dims};
    CHECK(pair.ok);

    juce::AudioBuffer<float> srcAudio((int)dims.maxChannels, 256);
    srcAudio.clear();
    juce::MidiBuffer midi;
    // Real SysEx — 5 bytes (F0 ... F7). > kMidiEventMaxBytes (4) so dropped.
    const juce::uint8 sysex[] = { 0xF0, 0x7E, 0x7F, 0x06, 0xF7 };
    midi.addEvent(juce::MidiMessage::createSysExMessage(sysex + 1, 3), 32);
    // Plus a normal CC event at frame 100 — should round-trip.
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 7, 64), 100);

    HeaderPeek peek{pair.names.shm};
    const uint64_t overflowsBefore = readMidiOverflows(peek.hdr);

    CHECK(pair.host.pushInputBlock(srcAudio, midi, 256));

    juce::AudioBuffer<float> dstAudio((int)dims.maxChannels, 256);
    juce::MidiBuffer drained;
    CHECK(pair.sandbox.popInputBlock(dstAudio, drained, 256, 1000));

    int n = 0;
    for ([[maybe_unused]] const auto meta : drained) ++n;
    CHECK(n == 1);  // SysEx dropped, CC survives.

    const uint64_t overflowsAfter = readMidiOverflows(peek.hdr);
    CHECK(overflowsAfter == overflowsBefore + 1);
}

void testOverCapBumpsOverflow()
{
    std::printf("test: events past kMidiEventsPerSlot drop + bump overflows\n");
    AudioDimensions dims;
    ChannelPair pair{dims};
    CHECK(pair.ok);

    juce::AudioBuffer<float> srcAudio((int)dims.maxChannels, 256);
    srcAudio.clear();
    juce::MidiBuffer midi;
    // Push kMidiEventsPerSlot + 8 events — the trailing 8 should be dropped.
    constexpr int kExtra = 8;
    const int total = (int)kMidiEventsPerSlot + kExtra;
    for (int i = 0; i < total; ++i)
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 7, i & 0x7F), i % 256);

    HeaderPeek peek{pair.names.shm};
    const uint64_t overflowsBefore = readMidiOverflows(peek.hdr);

    CHECK(pair.host.pushInputBlock(srcAudio, midi, 256));

    juce::AudioBuffer<float> dstAudio((int)dims.maxChannels, 256);
    juce::MidiBuffer drained;
    CHECK(pair.sandbox.popInputBlock(dstAudio, drained, 256, 1000));

    int n = 0;
    for ([[maybe_unused]] const auto meta : drained) ++n;
    CHECK(n == (int)kMidiEventsPerSlot);

    const uint64_t overflowsAfter = readMidiOverflows(peek.hdr);
    CHECK(overflowsAfter == overflowsBefore + (uint64_t)kExtra);
}

void testFramePastSamplesDropped()
{
    std::printf("test: events past truncated samples drop + bump overflows\n");
    AudioDimensions dims;
    dims.maxBlockSamples = 128;       // tighter cap to make the truncation real
    ChannelPair pair{dims};
    CHECK(pair.ok);

    juce::AudioBuffer<float> srcAudio((int)dims.maxChannels, 256);
    srcAudio.clear();
    juce::MidiBuffer midi;
    // Caller passes numSamples=256 but spawn cap is 128 — pushInputBlock
    // truncates audio to samples=128 and should DROP MIDI events at
    // frames >= 128 rather than clamping them into the audible portion.
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 50);   // in-range
    midi.addEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8)100), 127);  // last in-range frame
    midi.addEvent(juce::MidiMessage::noteOn(1, 62, (juce::uint8)100), 128);  // out-of-range (= samples)
    midi.addEvent(juce::MidiMessage::noteOn(1, 63, (juce::uint8)100), 200);  // out-of-range

    HeaderPeek peek{pair.names.shm};
    const uint64_t overflowsBefore = readMidiOverflows(peek.hdr);

    CHECK(pair.host.pushInputBlock(srcAudio, midi, 256));

    juce::AudioBuffer<float> dstAudio((int)dims.maxChannels, 256);
    juce::MidiBuffer drained;
    CHECK(pair.sandbox.popInputBlock(dstAudio, drained, 256, 1000));

    int n = 0;
    int lastFrame = -1;
    for (const auto meta : drained) { ++n; lastFrame = meta.samplePosition; }
    CHECK(n == 2);                    // events at 50 and 127
    CHECK(lastFrame == 127);          // 128 and 200 dropped, NOT clamped to 127

    const uint64_t overflowsAfter = readMidiOverflows(peek.hdr);
    CHECK(overflowsAfter == overflowsBefore + 2);
}

} // namespace

int main()
{
    std::printf("=== audio_channel_midi_test ===\n");
    testRoundtripSmallBuffer();
    testSysExBumpsOverflow();
    testOverCapBumpsOverflow();
    testFramePastSamplesDropped();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
