#include "AudioChannel.h"

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
    size_t viewBytes = 0;
    HANDLE evtToHost = nullptr;
    HANDLE evtToSandbox = nullptr;
    AudioShmHeader* header = nullptr;
    float* inputRing = nullptr;   // host writes, sandbox reads
    float* outputRing = nullptr;  // sandbox writes, host reads
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
    impl->viewBytes = (size_t)totalBytes;
    impl->header = reinterpret_cast<AudioShmHeader*>(impl->view);

    // Initialise the header on the host side.
    impl->header->magic = kAudioShmMagic;
    impl->header->protocolVersion = kProtocolVersion;
    impl->header->maxBlocks = dims.maxBlocks;
    impl->header->maxBlockSamples = dims.maxBlockSamples;
    impl->header->maxChannels = dims.maxChannels;
    impl->header->sampleRate = dims.sampleRate;
    impl->header->writeIdx = 0;
    impl->header->readIdx = 0;
    impl->header->xruns = 0;
    impl->header->dropouts = 0;
    impl->header->ringBytesPerSlot = dims.bytesPerSlot();
    impl->header->inputRingOffset  = sizeof(AudioShmHeader);
    impl->header->outputRingOffset = impl->header->inputRingOffset
                                   + uint64_t(dims.maxBlocks) * dims.bytesPerSlot();

    auto* base = reinterpret_cast<char*>(impl->view);
    impl->inputRing  = reinterpret_cast<float*>(base + impl->header->inputRingOffset);
    impl->outputRing = reinterpret_cast<float*>(base + impl->header->outputRingOffset);

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
}

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

bool AudioChannel::pushBlock(bool isOutputRing, const juce::AudioBuffer<float>& src,
                             int numSamples)
{
    if (!impl->header) return false;
    auto writeIdx = atomicAt(impl->header->writeIdx);
    auto readIdx  = atomicAt(impl->header->readIdx);
    uint64_t w = writeIdx.load(std::memory_order_relaxed);
    uint64_t r = readIdx.load(std::memory_order_acquire);
    if (w - r >= impl->header->maxBlocks)
    {
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

    writeIdx.store(w + 1, std::memory_order_release);
    SetEvent(isOutputRing ? impl->evtToHost : impl->evtToSandbox);
    return true;
}

bool AudioChannel::popBlock(bool isOutputRing, juce::AudioBuffer<float>& dst,
                            int numSamples, int timeoutMs)
{
    if (!impl->header) return false;
    HANDLE evt = isOutputRing ? impl->evtToHost : impl->evtToSandbox;
    auto writeIdx = atomicAt(impl->header->writeIdx);
    auto readIdx  = atomicAt(impl->header->readIdx);

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
        // Re-read; the wake might have been from teardown or another
        // spurious source.
        r = readIdx.load(std::memory_order_relaxed);
        w = writeIdx.load(std::memory_order_acquire);
        if (w == r)
        {
            // Bump dropouts so the spurious-wake class is visible in the
            // counters; otherwise it looks like a clean pop in operator
            // logs but the caller still sees a failed return.
            atomicAt(impl->header->dropouts).fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    auto slot = r % impl->header->maxBlocks;
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* src = (isOutputRing ? impl->outputRing : impl->inputRing)
              + slot * (bytesPerSlot / sizeof(float));

    const int maxSamples = (int)impl->header->maxBlockSamples;
    const int dstCh      = dst.getNumChannels();
    const int channels   = juce::jmin((int)impl->header->maxChannels, dstCh);
    const int samples    = juce::jmin(maxSamples, numSamples);
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

} // namespace slopsmith::sandbox
