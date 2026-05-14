#include "AudioChannel.h"
#include "../VSTTrace.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #error "AudioChannel.cpp is Windows-only for now."
#endif

namespace slopsmith::sandbox {

struct AudioChannel::Impl
{
    HANDLE mapping = nullptr;
    void*  view = nullptr;
    HANDLE evtToHost = nullptr;
    HANDLE evtToSandbox = nullptr;
    AudioShmHeader* header = nullptr;
    float* inputRing = nullptr;   // host writes, sandbox reads
    float* outputRing = nullptr;  // sandbox writes, host reads
    MidiQueue* midiQueues = nullptr; // [maxBlocks], one per input slot
};

AudioChannel::AudioChannel() : impl(std::make_unique<Impl>()) {}

AudioChannel::~AudioChannel() { close(); }

static juce::String makeUniqueName(const char* suffix)
{
    return "Local\\slopsmith-vst-" + juce::Uuid().toDashedString() + "-" + suffix;
}

bool AudioChannel::createHostSide(const AudioDimensions& dims, Names& namesOut,
                                  juce::String& errorOut)
{
    namesOut.shm          = makeUniqueName(kShmNameSuffix);
    namesOut.evtToHost    = makeUniqueName(kEvtToHostSuffix);
    namesOut.evtToSandbox = makeUniqueName(kEvtToSandboxSuffix);

    auto totalBytes = dims.totalShmBytes();
    impl->mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        (DWORD)(totalBytes >> 32), (DWORD)(totalBytes & 0xFFFFFFFFu),
        namesOut.shm.toWideCharPointer());
    if (impl->mapping == nullptr)
    {
        errorOut = "CreateFileMapping failed: " + juce::String((int)GetLastError());
        close();
        return false;
    }
    impl->view = MapViewOfFile(impl->mapping, FILE_MAP_ALL_ACCESS, 0, 0, totalBytes);
    if (!impl->view)
    {
        errorOut = "MapViewOfFile failed";
        close();
        return false;
    }
    impl->header = reinterpret_cast<AudioShmHeader*>(impl->view);

    // Initialise the header on the host side.
    impl->header->magic = kAudioShmMagic;
    impl->header->protocolVersion = kProtocolVersion;
    impl->header->maxBlocks = dims.maxBlocks;
    impl->header->maxBlockSamples = dims.maxBlockSamples;
    impl->header->maxChannels = dims.maxChannels;
    impl->header->sampleRate = dims.sampleRate;
    impl->header->inWriteIdx  = 0;
    impl->header->inReadIdx   = 0;
    impl->header->outWriteIdx = 0;
    impl->header->outReadIdx  = 0;
    impl->header->xruns = 0;
    impl->header->dropouts = 0;
    impl->header->midiOverflows = 0;
    impl->header->ringBytesPerSlot = dims.bytesPerSlot();
    impl->header->inputRingOffset  = sizeof(AudioShmHeader);
    impl->header->outputRingOffset = impl->header->inputRingOffset
                                   + uint64_t(dims.maxBlocks) * dims.bytesPerSlot();
    impl->header->midiQueueOffset  = impl->header->outputRingOffset
                                   + uint64_t(dims.maxBlocks) * dims.bytesPerSlot();

    // Release fence so all the header writes above are visible before the
    // sandbox observes the mapping. CreateProcessW (the publish point on
    // the host side) is a strong synchronisation primitive on Windows, so
    // in practice the writes are already flushed before the child starts —
    // but the fence makes the spawn-order invariant documented in
    // openSandboxSide explicit at the producer rather than relying on the
    // implicit semantics of the spawn call.
    std::atomic_thread_fence(std::memory_order_release);

    auto* base = reinterpret_cast<char*>(impl->view);
    impl->inputRing  = reinterpret_cast<float*>(base + impl->header->inputRingOffset);
    impl->outputRing = reinterpret_cast<float*>(base + impl->header->outputRingOffset);
    impl->midiQueues = reinterpret_cast<MidiQueue*>(base + impl->header->midiQueueOffset);
    // Zero-initialise the per-slot MidiQueues so a producer's first publish
    // doesn't have to clear count/overflow bookkeeping.
    std::memset(impl->midiQueues, 0,
                sizeof(MidiQueue) * (size_t)dims.maxBlocks);

    impl->evtToHost = CreateEventW(
        nullptr, /*manualReset*/FALSE, /*initial*/FALSE,
        namesOut.evtToHost.toWideCharPointer());
    impl->evtToSandbox = CreateEventW(
        nullptr, FALSE, FALSE,
        namesOut.evtToSandbox.toWideCharPointer());
    if (!impl->evtToHost || !impl->evtToSandbox)
    {
        errorOut = "CreateEvent failed";
        close();
        return false;
    }
    cachedDims = dims;
    return true;
}

bool AudioChannel::openSandboxSide(const Names& names, juce::String& errorOut)
{
    impl->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                                     names.shm.toWideCharPointer());
    if (!impl->mapping)
    {
        errorOut = "OpenFileMapping failed: " + juce::String((int)GetLastError());
        close();
        return false;
    }
    impl->view = MapViewOfFile(impl->mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!impl->view)
    {
        errorOut = "MapViewOfFile (sandbox) failed";
        close();
        return false;
    }
    // Before the magic check (which reads header->magic), verify the mapping
    // is at least sizeof(AudioShmHeader). `MapViewOfFile(...,0)` maps the
    // whole object, but if a corrupted/malicious named-mapping pointed at a
    // smaller object the magic check itself would be an OOB read. The
    // expectedTotal bounds check below uses header-derived fields and so
    // cannot detect an undersized real mapping — this is the only place we
    // can close that gap.
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(impl->view, &mbi, sizeof(mbi)) == 0
            || mbi.RegionSize < sizeof(AudioShmHeader))
        {
            errorOut = "audio shm mapping too small for header ("
                     + juce::String((int64_t)mbi.RegionSize) + " < "
                     + juce::String((int64_t)sizeof(AudioShmHeader)) + ")";
            close();
            return false;
        }
    }
    impl->header = reinterpret_cast<AudioShmHeader*>(impl->view);
    if (impl->header->magic != kAudioShmMagic)
    {
        errorOut = "audio shm magic mismatch";
        close();
        return false;
    }
    if (impl->header->protocolVersion != kProtocolVersion)
    {
        errorOut = "audio shm protocol mismatch: expected "
                 + juce::String((int)kProtocolVersion) + ", got "
                 + juce::String((int)impl->header->protocolVersion);
        close();
        return false;
    }
    // Validate dims against compile-time caps BEFORE computing bytesPerSlot()
    // and expectedTotal. Without this, pathological header values (corrupted
    // or malicious mapping that passed magic+protocolVersion) can overflow
    // uint64_t in `maxBlockSamples * maxChannels * 4` or
    // `2 * maxBlocks * ringBytesPerSlot`, defeating the inEnd/outEnd bounds
    // check below and pointing inputRing/outputRing past the actual mapping.
    // Host-side spawn validates these at SandboxedProcessor::spawn but the
    // sandbox side has been trusting whatever the mapping says.
    if (impl->header->maxBlocks == 0 || impl->header->maxBlocks > kAudioMaxBlocks
        || impl->header->maxBlockSamples == 0
        || impl->header->maxBlockSamples > kAudioMaxBlockSamples
        || impl->header->maxChannels == 0
        || impl->header->maxChannels > kAudioMaxChannels)
    {
        errorOut = "audio shm dims exceed protocol caps: blocks="
                 + juce::String((int64_t)impl->header->maxBlocks)
                 + " blockSamples=" + juce::String((int64_t)impl->header->maxBlockSamples)
                 + " channels=" + juce::String((int64_t)impl->header->maxChannels);
        close();
        return false;
    }
    cachedDims.maxBlocks = impl->header->maxBlocks;
    cachedDims.maxBlockSamples = impl->header->maxBlockSamples;
    cachedDims.maxChannels = impl->header->maxChannels;
    cachedDims.sampleRate = impl->header->sampleRate;

    // Cross-check ringBytesPerSlot against the dims the host published. A
    // mismatch here would silently produce misaligned ring access — the
    // protocol version check upstream already guarantees host/sandbox
    // agree on the layout, this just makes the contract local.
    const uint64_t expectedSlotBytes = cachedDims.bytesPerSlot();
    if (impl->header->ringBytesPerSlot != expectedSlotBytes)
    {
        errorOut = "audio shm ringBytesPerSlot mismatch: expected "
                 + juce::String((int64_t)expectedSlotBytes) + ", got "
                 + juce::String((int64_t)impl->header->ringBytesPerSlot);
        close();
        return false;
    }

    // Spawn-order invariant: the host fully initialises the shared header
    // (sets magic, protocolVersion, maxBlocks, etc.) BEFORE calling
    // CreateProcessW, so by the time the sandbox observes the mapping the
    // header is fully populated. If a future refactor lets the sandbox map
    // the view before host-side init completes, add explicit
    // synchronisation (atomic seq counter, or an event signalled after
    // init) — without it the sandbox could observe partial fields.

    // Sandbox side maps the view with size=0, which means "whole object".
    // We don't know the OS-reported view size, but we can reconstruct the
    // expected total from the validated dims and bounds-check the offsets
    // against that. Anything beyond would be a stale/incompatible host
    // header that survived magic+version validation (unlikely but cheap
    // to catch — and the alternative is an out-of-bounds memcpy on the
    // first pushBlock/popBlock).
    const uint64_t expectedTotal = sizeof(AudioShmHeader)
        + 2 * uint64_t(impl->header->maxBlocks) * impl->header->ringBytesPerSlot;
    const uint64_t inEnd  = impl->header->inputRingOffset
        + uint64_t(impl->header->maxBlocks) * impl->header->ringBytesPerSlot;
    const uint64_t outEnd = impl->header->outputRingOffset
        + uint64_t(impl->header->maxBlocks) * impl->header->ringBytesPerSlot;
    if (inEnd > expectedTotal || outEnd > expectedTotal)
    {
        errorOut = "audio shm ring offsets out of bounds";
        close();
        return false;
    }
    // Now that we know what the header *claims* the object should be, verify
    // the actual mapped region is at least that big. The earlier VirtualQuery
    // covered just sizeof(AudioShmHeader); a stale/foreign mapping that
    // reused the same name with a smaller backing object would pass magic +
    // protocolVersion + dim-cap checks and still let the first pushBlock /
    // popBlock memcpy outside the mapping.
    {
        MEMORY_BASIC_INFORMATION mbi2{};
        if (VirtualQuery(impl->view, &mbi2, sizeof(mbi2)) == 0
            || mbi2.RegionSize < expectedTotal)
        {
            errorOut = "audio shm mapping too small for ring layout: region="
                     + juce::String((int64_t)mbi2.RegionSize) + " expected>="
                     + juce::String((int64_t)expectedTotal);
            close();
            return false;
        }
    }

    auto* base = reinterpret_cast<char*>(impl->view);
    impl->inputRing  = reinterpret_cast<float*>(base + impl->header->inputRingOffset);
    impl->outputRing = reinterpret_cast<float*>(base + impl->header->outputRingOffset);
    impl->midiQueues = reinterpret_cast<MidiQueue*>(base + impl->header->midiQueueOffset);

    impl->evtToHost    = OpenEventW(EVENT_ALL_ACCESS, FALSE,
                                    names.evtToHost.toWideCharPointer());
    impl->evtToSandbox = OpenEventW(EVENT_ALL_ACCESS, FALSE,
                                    names.evtToSandbox.toWideCharPointer());
    if (!impl->evtToHost || !impl->evtToSandbox)
    {
        errorOut = "OpenEvent failed: " + juce::String((int)GetLastError());
        close();
        return false;
    }
    return true;
}

void AudioChannel::close()
{
    if (impl->evtToHost)    { CloseHandle(impl->evtToHost);    impl->evtToHost = nullptr; }
    if (impl->evtToSandbox) { CloseHandle(impl->evtToSandbox); impl->evtToSandbox = nullptr; }
    if (impl->view)         { UnmapViewOfFile(impl->view);     impl->view = nullptr; }
    if (impl->mapping)      { CloseHandle(impl->mapping);      impl->mapping = nullptr; }
    impl->header = nullptr;
    impl->inputRing = nullptr;
    impl->outputRing = nullptr;
    impl->midiQueues = nullptr;
}

// std::atomic_ref needs C++20 (P0019, finalised in libstdc++ 11+ and recent
// MSVC). The repo's CMAKE_CXX_STANDARD is set to 20, but a contributor
// building this TU under an older toolchain would get a confusing template
// lookup error rather than a clear "your compiler is too old" message —
// the feature-test macro converts that into a fast compile-time signal.
#ifndef __cpp_lib_atomic_ref
#  error "std::atomic_ref<uint64_t> requires C++20 + libstdc++ 11+ / recent MSVC; \
upgrade the toolchain or guard this file behind an alternate atomic strategy."
#endif

// Header indices are written by the host side and read by the sandbox side
// over shared memory. std::atomic_ref gives us atomic access without UB from
// reinterpret_casting the uint64_t storage to std::atomic<uint64_t>* (the
// latter relies on layout-compatibility C++ doesn't promise).
// `atomic_ref<T>::required_alignment` may exceed `alignof(T)` on some
// platforms; header fields are `alignas(8) uint64_t`. The static_assert
// below fails at compile time on a platform where required_alignment > 8
// rather than producing UB at runtime — bump the alignas to match if it
// ever trips.
static_assert(std::atomic_ref<uint64_t>::required_alignment <= 8,
              "AudioShmHeader uint64_t fields are alignas(8); bump the "
              "alignas to std::atomic_ref<uint64_t>::required_alignment "
              "or atomic_ref construction is undefined behavior");
static std::atomic_ref<uint64_t> atomicAt(uint64_t& slot)
{
    return std::atomic_ref<uint64_t>(slot);
}

static std::atomic_ref<uint32_t> atomicAt32(uint32_t& slot)
{
    return std::atomic_ref<uint32_t>(slot);
}

namespace
{
    // Pick the right (write, read) index pair for a direction. Input ring
    // (host → sandbox) is produced by host / consumed by sandbox; output
    // ring (sandbox → host) is the inverse.
    struct RingIndices { uint64_t& write; uint64_t& read; };

    RingIndices indicesFor(AudioShmHeader& h, bool isOutputRing)
    {
        return isOutputRing
             ? RingIndices{ h.outWriteIdx, h.outReadIdx }
             : RingIndices{ h.inWriteIdx,  h.inReadIdx  };
    }
}

bool AudioChannel::pushBlock(bool isOutputRing, const juce::AudioBuffer<float>& src,
                             int numSamples)
{
    // Input-direction pushes MUST go through pushInputBlock (which publishes
    // the slot's MidiQueue alongside the audio). Calling pushBlock(false,…)
    // directly would leave whatever MIDI count was in the slot from a prior
    // pushInputBlock and the next popInputBlock would replay those stale
    // events against fresh audio.
    //
    // jassert in debug + return false in release: a release-build regression
    // would otherwise silently corrupt MIDI delivery rather than failing
    // loudly. Today the only input producer is
    // SandboxedProcessor::processBlock and it always calls pushInputBlock.
    jassert(isOutputRing);
    if (!isOutputRing) return false;
    if (!impl->header) return false;
    auto idx = indicesFor(*impl->header, isOutputRing);
    auto writeIdx = atomicAt(idx.write);
    auto readIdx  = atomicAt(idx.read);
    uint64_t w = writeIdx.load(std::memory_order_relaxed);
    uint64_t r = readIdx.load(std::memory_order_acquire);
    if (w - r >= impl->header->maxBlocks)
    {
        // v1 shared writeIdx/readIdx pair means this counter is shared
        // across both rings — a host-side input-push xrun and a sandbox-
        // side output-push xrun both increment the same value. PR #2's
        // per-direction indices split will give us per-ring counters so
        // diagnosis can pin back-pressure to the upstream side.
        atomicAt(impl->header->xruns).fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto slot = w % impl->header->maxBlocks;
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* dst = (isOutputRing ? impl->outputRing : impl->inputRing)
              + slot * (bytesPerSlot / sizeof(float));

    const int maxCh      = (int)impl->header->maxChannels;
    const int maxSamples = (int)impl->header->maxBlockSamples;
    const int channels   = juce::jmin(maxCh, src.getNumChannels());
    const int samples    = juce::jmin(maxSamples, numSamples);
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* slotCh = dst + ch * maxSamples;
        std::memcpy(slotCh, src.getReadPointer(ch),
                    sizeof(float) * (size_t)samples);
        // Wipe tail samples so a shorter block doesn't leave audio from a
        // previous slot-overwrite hanging around for the consumer.
        if (samples < maxSamples)
            std::memset(slotCh + samples, 0,
                        sizeof(float) * (size_t)(maxSamples - samples));
    }
    // Wipe channels the producer didn't write at all — same rationale.
    for (int ch = channels; ch < maxCh; ++ch)
        std::memset(dst + ch * maxSamples, 0,
                    sizeof(float) * (size_t)maxSamples);

    // The release store on writeIdx pairs with the consumer's acquire load
    // in popBlock, so the slot memcpy/memset above are happens-before any
    // read of writeIdx >= w+1. On x86/x64 (today's only target) plain
    // memory ops are ordered enough that this pairing alone is sufficient.
    // On the deferred macOS/Linux ports — and ARM specifically — slot
    // writes happen via plain memcpy/memset (not atomic) and weakly
    // ordered architectures may need an explicit `atomic_thread_fence
    // (memory_order_release)` immediately before the store. Track with
    // the macOS/Linux sandbox follow-up PRs.
    writeIdx.store(w + 1, std::memory_order_release);
    SetEvent(isOutputRing ? impl->evtToHost : impl->evtToSandbox);
    return true;
}

bool AudioChannel::popBlock(bool isOutputRing, juce::AudioBuffer<float>& dst,
                            int numSamples, int timeoutMs)
{
    if (!impl->header) return false;
    HANDLE evt = isOutputRing ? impl->evtToHost : impl->evtToSandbox;
    auto idx = indicesFor(*impl->header, isOutputRing);
    auto writeIdx = atomicAt(idx.write);
    auto readIdx  = atomicAt(idx.read);

    // Check indices BEFORE waiting. SetEvent on an auto-reset event is not
    // counting: if the producer signals twice in a row (queue 2 blocks),
    // both signals collapse to one wake. Without this fast path, the
    // consumer would block on the second pop even though w > r already.
    uint64_t r = readIdx.load(std::memory_order_relaxed);
    uint64_t w = writeIdx.load(std::memory_order_acquire);
    if (w == r)
    {
        if (WaitForSingleObject(evt, timeoutMs) != WAIT_OBJECT_0)
        {
            atomicAt(impl->header->dropouts).fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Re-read; the wake might have been from teardown, an
        // AudioPauseGuard's signalSandboxWake (every kPrepare /
        // kSetBlockSize / kGetState / kSetState), or a kShutdown /
        // disconnect callback. NONE of those are dropouts — they're
        // intentional non-data wakes. Don't bump `dropouts` here or the
        // counter pollutes every pause-guarded control op. Real
        // dropouts are still counted on the WaitForSingleObject
        // timeout path above and at the SandboxedProcessor pop-timeout
        // call site.
        r = readIdx.load(std::memory_order_relaxed);
        w = writeIdx.load(std::memory_order_acquire);
        if (w == r) return false;
    }

    auto slot = r % impl->header->maxBlocks;
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* src = (isOutputRing ? impl->outputRing : impl->inputRing)
              + slot * (bytesPerSlot / sizeof(float));

    const int maxSamples = (int)impl->header->maxBlockSamples;
    const int dstCh      = dst.getNumChannels();
    const int channels   = juce::jmin((int)impl->header->maxChannels, dstCh);
    const int samples    = juce::jmin(maxSamples, numSamples);
    if (numSamples > maxSamples)
    {
        // One-shot warn: caller passed more samples than the spawn-time cap
        // allows, so we'll truncate to maxSamples and zero-fill the tail.
        // Producer-side push paths apply the same clamp, so this fires only
        // if a misconfigured consumer asks for too much (kPrepare /
        // kSetBlockSize spawn-cap validation should have prevented it).
        static std::atomic<bool> warned{false};
        bool expected = false;
        if (warned.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel))
        {
            VST_TRACE("[audio-shm] popBlock: caller numSamples=%d > spawn cap "
                      "maxBlockSamples=%d — truncating, tail zeroed",
                      numSamples, maxSamples);
        }
    }
    for (int ch = 0; ch < channels; ++ch)
    {
        std::memcpy(dst.getWritePointer(ch),
                    src + ch * maxSamples,
                    sizeof(float) * (size_t)samples);
        // Zero any portion of dst beyond what we copied (the caller's buffer
        // may be longer than the producer's payload).
        if (samples < numSamples)
            std::memset(dst.getWritePointer(ch) + samples, 0,
                        sizeof(float) * (size_t)(numSamples - samples));
    }
    // Zero channels the producer didn't fill so dst doesn't carry stale audio.
    for (int ch = channels; ch < dstCh; ++ch)
        dst.clear(ch, 0, numSamples);

    readIdx.store(r + 1, std::memory_order_release);
    return true;
}

bool AudioChannel::pushInputBlock(const juce::AudioBuffer<float>& src,
                                  const juce::MidiBuffer& midi,
                                  int numSamples)
{
    // Inlined audio + MIDI publish so the slot's MidiQueue is published
    // alongside the audio under the same inWriteIdx release. Earlier this
    // method delegated to pushBlock(false, ...) for the audio half, but
    // pushBlock used to clobber `count` to 0 between our MIDI publish and
    // the inWriteIdx bump — every MIDI event was being dropped.
    if (!impl->header || !impl->midiQueues) return false;
    // Reject up front when the caller exceeds the spawn-time cap rather
    // than silently truncating audio + dropping MIDI in [maxSamples,
    // numSamples) into midiOverflows. Spawn-cap validation in kPrepare /
    // kSetBlockSize should prevent this; if it ever fires the caller
    // gets a `false` return (xruns is the wrong counter — bump dropouts
    // so this misuse is visible without conflating with ring-full).
    if (numSamples > (int)impl->header->maxBlockSamples)
    {
        atomicAt(impl->header->dropouts).fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto writeIdx = atomicAt(impl->header->inWriteIdx);
    auto readIdx  = atomicAt(impl->header->inReadIdx);
    uint64_t w = writeIdx.load(std::memory_order_relaxed);
    uint64_t r = readIdx.load(std::memory_order_acquire);
    if (w - r >= impl->header->maxBlocks)
    {
        atomicAt(impl->header->xruns).fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto slot = w % impl->header->maxBlocks;
    auto& queue = impl->midiQueues[slot];

    // Compute the truncated sample count up front so the MIDI loop below
    // can clamp event frames against the SAME bound the audio copy uses.
    // If the caller passed numSamples > maxSamples, both halves truncate
    // to maxSamples consistently — otherwise the sandbox would receive
    // MIDI frames pointing past the end of the audio it actually got.
    const int maxCh      = (int)impl->header->maxChannels;
    const int maxSamples = (int)impl->header->maxBlockSamples;
    const int channels   = juce::jmin(maxCh, src.getNumChannels());
    const int samples    = juce::jmin(maxSamples, numSamples);

    // 1. Pack MIDI into the slot's queue. The slot is owned by the host
    //    until we publish the new inWriteIdx below, so writes here are
    //    private — no need for the relaxed-clear-then-release-store on
    //    `count`, the release on inWriteIdx publishes both `count` and
    //    `events[]` together. Using atomic_ref for the count store anyway
    //    so the layout stays consistent for the sandbox-side acquire load.
    uint32_t written = 0;
    auto bumpMidiOverflow = [&](uint64_t n = 1)
    {
        // Global cumulative counter — per-slot was confusing because slots
        // round-robin (the per-slot value would mix counts from many
        // different blocks rather than answering "did THIS block
        // overflow?"). Per-event accuracy past the cap is not a documented
        // contract — the bulk-bump on cap-overflow keeps the audio thread
        // from iterating arbitrarily many events on a real-time path.
        atomicAt(impl->header->midiOverflows).fetch_add(n, std::memory_order_relaxed);
    };
    int scanned = 0;
    const int totalEvents = midi.getNumEvents();
    // Hard cap on iterations regardless of accept/reject ratio. The
    // cap-overflow break below bounds the loop when events are valid-and-
    // fit (written climbs to kMidiEventsPerSlot quickly), but a flood of
    // pure SysEx would never increment `written` and could otherwise
    // iterate the entire buffer one event at a time on the RT thread.
    // 2× kMidiEventsPerSlot leaves headroom for normal mixed-in
    // SysEx-among-CCs blocks while still bounding the worst case.
    constexpr int kMaxScanIterations = 2 * (int)kMidiEventsPerSlot;
    for (const auto meta : midi)
    {
        if (scanned >= kMaxScanIterations)
        {
            // Hit the per-block scan cap. Bulk-bump remaining and break;
            // per-event accuracy past the cap is not a documented
            // contract, the bound matters more on the audio thread.
            bumpMidiOverflow((uint64_t)(totalEvents - scanned));
            break;
        }
        ++scanned;
        const auto& msg = meta.getMessage();
        const int rawSize = msg.getRawDataSize();
        if (rawSize <= 0 || rawSize > (int)kMidiEventMaxBytes)
        {
            // Doesn't fit (SysEx etc.). Audio thread never blocks; the
            // lossy policy is documented in PR #2.
            bumpMidiOverflow();
            continue;
        }
        if (written >= kMidiEventsPerSlot)
        {
            // Bulk-bump for THIS event + every remaining event the
            // iterator would visit, then break. Together with the
            // scan-cap above, total audio-thread MIDI work is bounded
            // at 2× kMidiEventsPerSlot iterations regardless of how
            // bloated or pathological the inbound buffer is.
            bumpMidiOverflow((uint64_t)(totalEvents - scanned + 1));
            break;
        }
        // Reject events whose frame is past the truncated audio (samples
        // ≤ numSamples — see the comment on the maxSamples computation
        // above). Clamping would silently re-time the event into the
        // audible portion, which is a worse failure mode than dropping it.
        // samplePosition < 0 is an invalid input; treat it as out-of-range
        // and drop too.
        if (meta.samplePosition < 0 || meta.samplePosition >= samples)
        {
            bumpMidiOverflow();
            continue;
        }
        auto& ev = queue.events[written];
        ev.frame = (uint32_t)meta.samplePosition;
        ev.size  = (uint32_t)rawSize;
        std::memcpy(ev.bytes, msg.getRawData(), (size_t)rawSize);
        ++written;
    }
    // Relaxed: the inWriteIdx release-store below synchronises this write
    // with the sandbox's acquire-load of inWriteIdx in popInputBlock, so
    // when the consumer observes the new write index it also observes
    // count + events[].
    atomicAt32(queue.count).store(written, std::memory_order_relaxed);

    // 2. Copy audio into the same slot of the input ring.
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* dst = impl->inputRing + slot * (bytesPerSlot / sizeof(float));
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* slotCh = dst + ch * maxSamples;
        std::memcpy(slotCh, src.getReadPointer(ch),
                    sizeof(float) * (size_t)samples);
        if (samples < maxSamples)
            std::memset(slotCh + samples, 0,
                        sizeof(float) * (size_t)(maxSamples - samples));
    }
    for (int ch = channels; ch < maxCh; ++ch)
        std::memset(dst + ch * maxSamples, 0,
                    sizeof(float) * (size_t)maxSamples);

    // 3. Publish the slot — release-synchronises with the consumer's acquire
    //    on inWriteIdx in popInputBlock, which makes both the audio bytes
    //    and the MIDI queue visible together.
    writeIdx.store(w + 1, std::memory_order_release);
    SetEvent(impl->evtToSandbox);
    return true;
}

bool AudioChannel::popInputBlock(juce::AudioBuffer<float>& dst,
                                 juce::MidiBuffer& midi,
                                 int numSamples, int timeoutMs)
{
    // Inlined audio + MIDI drain so we hold the slot until both are read.
    // Earlier this method delegated to popBlock(false, ...), which advanced
    // inReadIdx before the MIDI was drained — the host could then immediately
    // reuse the slot and overwrite the queue we were still reading.
    if (!impl->header || !impl->midiQueues) return false;
    // Symmetric with pushInputBlock: reject up front when the caller
    // exceeds the spawn-time cap, so a misconfigured consumer learns
    // about the misuse via the false return rather than getting silently
    // truncated audio. Don't advance inReadIdx — the producer's slot
    // stays full until the consumer corrects its numSamples (or the host
    // tears down). bumps dropouts so the misuse is visible.
    if (numSamples > (int)impl->header->maxBlockSamples)
    {
        atomicAt(impl->header->dropouts).fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    HANDLE evt = impl->evtToSandbox;
    auto writeIdx = atomicAt(impl->header->inWriteIdx);
    auto readIdx  = atomicAt(impl->header->inReadIdx);

    // Same fast-path / wait / recheck pattern as popBlock: SetEvent on an
    // auto-reset event collapses signals, so if pushInputBlock fires twice
    // in a row we'd otherwise block on the second pop even though w > r.
    uint64_t r = readIdx.load(std::memory_order_relaxed);
    uint64_t w = writeIdx.load(std::memory_order_acquire);
    if (w == r)
    {
        if (WaitForSingleObject(evt, timeoutMs) != WAIT_OBJECT_0)
        {
            atomicAt(impl->header->dropouts).fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        r = readIdx.load(std::memory_order_relaxed);
        w = writeIdx.load(std::memory_order_acquire);
        // Intentional non-data wake (AudioPauseGuard signalSandboxWake on
        // every pause-guarded control op, kShutdown, disconnect). Same
        // rationale as popBlock: don't count these as dropouts. Real
        // missed-deadline events are caught by the timeout branch above
        // and by SandboxedProcessor's pop-timeout call site.
        if (w == r) return false;
    }

    const auto slot = r % impl->header->maxBlocks;

    // 1. Drain MIDI from the slot. Relaxed-load on `count` is sufficient:
    //    the synchronisation that publishes count + events[] is the
    //    acquire-load on inWriteIdx above (paired with the producer's
    //    release-store on inWriteIdx in pushInputBlock), and the producer
    //    writes count itself with relaxed semantics. The acquire here
    //    would be redundant overhead and slightly misleading about the
    //    actual sync model.
    auto& queue = impl->midiQueues[slot];
    const uint32_t count = atomicAt32(queue.count)
                                .load(std::memory_order_relaxed);
    const uint32_t safeCount = juce::jmin(count, kMidiEventsPerSlot);
    for (uint32_t i = 0; i < safeCount; ++i)
    {
        const auto& ev = queue.events[i];
        const uint32_t size = juce::jmin(ev.size, kMidiEventMaxBytes);
        if (size == 0) continue;
        midi.addEvent(juce::MidiMessage(ev.bytes, (int)size),
                      (int)ev.frame);
    }

    // 2. Copy audio out of the slot.
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* src = impl->inputRing + slot * (bytesPerSlot / sizeof(float));
    // numSamples ≤ maxSamples here (cap enforced by the early-return guard
    // at the top of this function), so samples == numSamples and no
    // tail-zero / one-shot warn is needed — both belonged to the old
    // truncate-and-continue path.
    const int maxSamples = (int)impl->header->maxBlockSamples;
    const int dstCh      = dst.getNumChannels();
    const int channels   = juce::jmin((int)impl->header->maxChannels, dstCh);
    for (int ch = 0; ch < channels; ++ch)
        std::memcpy(dst.getWritePointer(ch),
                    src + ch * maxSamples,
                    sizeof(float) * (size_t)numSamples);
    for (int ch = channels; ch < dstCh; ++ch)
        dst.clear(ch, 0, numSamples);

    // 3. Release the slot — the host can now reuse it; we've finished both
    //    audio and MIDI reads.
    readIdx.store(r + 1, std::memory_order_release);
    return true;
}

void AudioChannel::signalSandboxWake()
{
    if (impl->evtToSandbox) SetEvent(impl->evtToSandbox);
}

} // namespace slopsmith::sandbox
