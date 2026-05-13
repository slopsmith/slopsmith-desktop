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
    namesOut.shm          = makeUniqueName("audio");
    namesOut.evtToHost    = makeUniqueName("evt-out");
    namesOut.evtToSandbox = makeUniqueName("evt-in");

    auto totalBytes = dims.totalShmBytes();
    impl->mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        (DWORD)(totalBytes >> 32), (DWORD)(totalBytes & 0xFFFFFFFFu),
        namesOut.shm.toWideCharPointer());
    if (impl->mapping == nullptr)
    {
        errorOut = "CreateFileMapping failed: " + juce::String((int)GetLastError());
        return false;
    }
    impl->view = MapViewOfFile(impl->mapping, FILE_MAP_ALL_ACCESS, 0, 0, totalBytes);
    if (!impl->view)
    {
        errorOut = "MapViewOfFile failed";
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
        return false;
    }
    impl->view = MapViewOfFile(impl->mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!impl->view)
    {
        errorOut = "MapViewOfFile (sandbox) failed";
        return false;
    }
    impl->header = reinterpret_cast<AudioShmHeader*>(impl->view);
    if (impl->header->magic != kAudioShmMagic)
    {
        errorOut = "audio shm magic mismatch";
        return false;
    }
    cachedDims.maxBlocks = impl->header->maxBlocks;
    cachedDims.maxBlockSamples = impl->header->maxBlockSamples;
    cachedDims.maxChannels = impl->header->maxChannels;
    cachedDims.sampleRate = impl->header->sampleRate;
    auto* base = reinterpret_cast<char*>(impl->view);
    impl->inputRing  = reinterpret_cast<float*>(base + impl->header->inputRingOffset);
    impl->outputRing = reinterpret_cast<float*>(base + impl->header->outputRingOffset);

    impl->evtToHost    = OpenEventW(EVENT_ALL_ACCESS, FALSE,
                                    names.evtToHost.toWideCharPointer());
    impl->evtToSandbox = OpenEventW(EVENT_ALL_ACCESS, FALSE,
                                    names.evtToSandbox.toWideCharPointer());
    if (!impl->evtToHost || !impl->evtToSandbox)
    {
        errorOut = "OpenEvent failed";
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

static std::atomic<uint64_t>& atomicAt(uint64_t& slot)
{
    return *reinterpret_cast<std::atomic<uint64_t>*>(&slot);
}

bool AudioChannel::pushBlock(bool isOutputRing, const juce::AudioBuffer<float>& src,
                             int numSamples)
{
    if (!impl->header) return false;
    auto& writeIdx = atomicAt(impl->header->writeIdx);
    auto& readIdx  = atomicAt(impl->header->readIdx);
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

    int channels = juce::jmin((int)impl->header->maxChannels,
                              src.getNumChannels());
    int samples  = juce::jmin((int)impl->header->maxBlockSamples, numSamples);
    for (int ch = 0; ch < channels; ++ch)
    {
        std::memcpy(dst + ch * impl->header->maxBlockSamples,
                    src.getReadPointer(ch),
                    sizeof(float) * (size_t)samples);
    }
    writeIdx.store(w + 1, std::memory_order_release);
    SetEvent(isOutputRing ? impl->evtToHost : impl->evtToSandbox);
    return true;
}

bool AudioChannel::popBlock(bool isOutputRing, juce::AudioBuffer<float>& dst,
                            int numSamples, int timeoutMs)
{
    if (!impl->header) return false;
    HANDLE evt = isOutputRing ? impl->evtToHost : impl->evtToSandbox;
    if (WaitForSingleObject(evt, timeoutMs) != WAIT_OBJECT_0)
    {
        atomicAt(impl->header->dropouts).fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto& writeIdx = atomicAt(impl->header->writeIdx);
    auto& readIdx  = atomicAt(impl->header->readIdx);
    uint64_t r = readIdx.load(std::memory_order_relaxed);
    uint64_t w = writeIdx.load(std::memory_order_acquire);
    if (w == r) return false; // spurious wakeup

    auto slot = r % impl->header->maxBlocks;
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* src = (isOutputRing ? impl->outputRing : impl->inputRing)
              + slot * (bytesPerSlot / sizeof(float));

    int channels = juce::jmin((int)impl->header->maxChannels,
                              dst.getNumChannels());
    int samples  = juce::jmin((int)impl->header->maxBlockSamples, numSamples);
    for (int ch = 0; ch < channels; ++ch)
    {
        std::memcpy(dst.getWritePointer(ch),
                    src + ch * impl->header->maxBlockSamples,
                    sizeof(float) * (size_t)samples);
    }
    readIdx.store(r + 1, std::memory_order_release);
    return true;
}

} // namespace slopsmith::sandbox
