#include "SandboxedProcessor.h"
#include "ControlChannel.h"
#include "AudioChannel.h"
#include "SubprocessHandle.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace slopsmith::sandbox {

namespace {
    // Editor wrapper: an AudioProcessorEditor that hosts the plugin's native
    // HWND (returned from the sandbox over the openEditor request). The
    // editor's componentMovedOrResized() forwards size changes to the sandbox.
    class SandboxedEditor : public juce::AudioProcessorEditor
    {
    public:
        SandboxedEditor(SandboxedProcessor& p, void* hwnd, int w, int h)
            : juce::AudioProcessorEditor(&p), nativeHwnd(hwnd)
        {
            setSize(w > 0 ? w : 800, h > 0 ? h : 600);
            setOpaque(true);
           #if JUCE_WINDOWS
            if (nativeHwnd)
            {
                // Reparent the plugin's window into this Component's peer
                // when the editor is added to a parent. Done in
                // parentHierarchyChanged() so we have a valid HWND to use.
            }
           #endif
        }

        void parentHierarchyChanged() override
        {
           #if JUCE_WINDOWS
            if (!nativeHwnd) return;
            auto* peer = getPeer();
            HWND parent = peer ? (HWND)peer->getNativeHandle() : nullptr;
            if (parent)
            {
                auto style = GetWindowLongPtrW((HWND)nativeHwnd, GWL_STYLE);
                style = (style | WS_CHILD) & ~WS_POPUP;
                SetWindowLongPtrW((HWND)nativeHwnd, GWL_STYLE, style);
                SetParent((HWND)nativeHwnd, parent);
                SetWindowPos((HWND)nativeHwnd, nullptr, 0, 0,
                             getWidth(), getHeight(),
                             SWP_NOZORDER | SWP_FRAMECHANGED);
            }
           #endif
        }

        void resized() override
        {
           #if JUCE_WINDOWS
            if (nativeHwnd)
                SetWindowPos((HWND)nativeHwnd, nullptr, 0, 0,
                             getWidth(), getHeight(),
                             SWP_NOZORDER | SWP_NOACTIVATE);
           #endif
        }

        void paint(juce::Graphics& g) override
        {
            // Painted area is fully covered by the embedded HWND; the
            // fill is just a fallback if the plugin window isn't attached yet.
            g.fillAll(juce::Colours::black);
        }

    private:
        void* nativeHwnd = nullptr;
    };
} // anonymous

SandboxedProcessor::SandboxedProcessor(SpawnConfig cfg)
    : juce::AudioProcessor(BusesProperties()
        .withInput("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , spawnConfig(std::move(cfg))
    , spawnName(spawnConfig.pluginName)
{
}

SandboxedProcessor::~SandboxedProcessor()
{
    teardown("destructor");
}

std::unique_ptr<SandboxedProcessor> SandboxedProcessor::spawn(const SpawnConfig& cfg,
                                                              juce::String& errorOut)
{
    std::unique_ptr<SandboxedProcessor> p(new SandboxedProcessor(cfg));
    if (!p->initialise(errorOut))
        return nullptr;
    return p;
}

bool SandboxedProcessor::initialise(juce::String& errorOut)
{
    control = std::make_unique<ControlChannel>();
    audio   = std::make_unique<AudioChannel>();

    juce::String pipeName, err;
    if (!control->createServerSide(pipeName, err))
    {
        errorOut = "control pipe: " + err;
        return false;
    }
    AudioChannel::Names audioNames;
    if (!audio->createHostSide(spawnConfig.audio, audioNames, err))
    {
        errorOut = "audio shm: " + err;
        return false;
    }

    subprocess = std::make_unique<SubprocessHandle>();
    juce::StringArray args;
    args.add("--plugin-path");      args.add(spawnConfig.pluginPath);
    args.add("--control-pipe");     args.add(pipeName);
    args.add("--audio-shm");        args.add(audioNames.shm);
    args.add("--audio-event-out");  args.add(audioNames.evtToHost);
    args.add("--audio-event-in");   args.add(audioNames.evtToSandbox);
    args.add("--sample-rate");      args.add(juce::String((int)spawnConfig.audio.sampleRate));
    args.add("--max-block");        args.add(juce::String((int)spawnConfig.audio.maxBlockSamples));
    args.add("--channels");         args.add(juce::String((int)spawnConfig.audio.maxChannels));

    std::promise<bool> readyP;
    auto readyF = readyP.get_future();
    bool readySet = false;

    auto eventCb = [this, &readyP, &readySet](const juce::String& evname,
                                              const juce::var& data)
    {
        if (evname == event::kReady && !readySet)
        {
            descriptionCached.name = data.getProperty("pluginName", "").toString();
            descriptionCached.manufacturerName =
                data.getProperty("manufacturer", "").toString();
            hasEditorCached    = (bool)data.getProperty("hasEditor", false);
            acceptsMidiCached  = (bool)data.getProperty("acceptsMidi", false);
            producesMidiCached = (bool)data.getProperty("producesMidi", false);
            alive.store(true, std::memory_order_release);
            readySet = true;
            try { readyP.set_value(true); } catch (...) {}
        }
        onControlEvent(evname, data);
    };

    auto disconnectCb = [this](const juce::String& reason)
    {
        teardown(reason);
    };

    if (!control->start(eventCb, disconnectCb))
    {
        errorOut = "control->start failed";
        return false;
    }
    if (!subprocess->start(spawnConfig.sandboxExePath, args,
                           [this](int code)
                           {
                               teardown("sandbox exit code " + juce::String(code));
                           }, err))
    {
        errorOut = "subprocess: " + err;
        return false;
    }

    if (readyF.wait_for(std::chrono::milliseconds(spawnConfig.spawnTimeoutMs))
        != std::future_status::ready)
    {
        errorOut = "sandbox did not become ready within timeout";
        return false;
    }
    return readyF.get();
}

void SandboxedProcessor::teardown(const juce::String& reason)
{
    if (!alive.exchange(false, std::memory_order_acq_rel)) return;
    auto cb = onCrash;
    if (control) control->stop();
    if (subprocess) subprocess->shutdown(1500);
    if (audio) audio->close();
    if (cb) cb(reason);
}

bool SandboxedProcessor::isAlive() const noexcept
{
    return alive.load(std::memory_order_acquire);
}

void SandboxedProcessor::onControlEvent(const juce::String& evname, const juce::var& data)
{
    if (evname == event::kEditorClosed)
    {
        editorOpen.store(false, std::memory_order_release);
    }
    // Other events (parameterChanged, log, error) are handled by upper layers
    // in follow-up sessions.
    (void)data;
}

void SandboxedProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    if (!control || !isAlive()) return;
    juce::DynamicObject::Ptr args(new juce::DynamicObject());
    args->setProperty("sampleRate", sampleRate);
    args->setProperty("blockSize", samplesPerBlock);
    juce::String err;
    control->request(op::kPrepare, juce::var(args.get()),
                     kDefaultReplyTimeoutMs, &err);
}

void SandboxedProcessor::releaseResources()
{
    // Nothing to release here — block-size changes are signalled via prepareToPlay.
}

void SandboxedProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midiMessages)
{
    if (!isAlive() || !audio)
    {
        // Sandbox is gone — pass silence through so the chain keeps flowing.
        buffer.clear();
        midiMessages.clear();
        return;
    }
    const int n = buffer.getNumSamples();

    // Forward MIDI via the control channel for now. Sample-accurate timing
    // requires bundling MIDI inline with the audio block (see Protocol.h
    // notes); follow-up PR.
    for (const auto meta : midiMessages)
    {
        const auto& msg = meta.getMessage();
        juce::DynamicObject::Ptr midiObj(new juce::DynamicObject());
        midiObj->setProperty("frame", meta.samplePosition);
        // Encode raw bytes as base64 so JSON survives binary content.
        juce::MemoryBlock bytes(msg.getRawData(), (size_t)msg.getRawDataSize());
        midiObj->setProperty("bytes", juce::Base64::toBase64(bytes.getData(),
                                                              bytes.getSize()));
        control->postNoReply(op::kMidiEvent, juce::var(midiObj.get()));
    }
    midiMessages.clear();

    audio->pushBlock(/*isOutputRing=*/false, buffer, n);
    if (!audio->popBlock(/*isOutputRing=*/true, buffer, n,
                         /*timeoutMs=*/ (int)juce::jmax(2.0,
                             1000.0 * n / juce::jmax(1, (int)spawnConfig.audio.sampleRate) * 4.0)))
    {
        // Missed deadline — sandbox is too slow or hung.
        buffer.clear();
    }
}

juce::AudioProcessorEditor* SandboxedProcessor::createEditor()
{
    if (!isAlive() || !hasEditor()) return nullptr;
    juce::String err;
    auto result = control->request(op::kOpenEditor, {}, kDefaultReplyTimeoutMs, &err);
    if (!result.isObject()) return nullptr;

    // The sandbox returns the HWND as a string ("0x...") because juce::var
    // doesn't carry 64-bit ints portably.
    auto hwndStr = result.getProperty("hwnd", "").toString();
    void* hwnd = nullptr;
   #if JUCE_WINDOWS
    hwnd = reinterpret_cast<void*>(hwndStr.getHexValue64());
   #else
    juce::ignoreUnused(hwndStr);
   #endif
    int w = (int)result.getProperty("w", 800);
    int h = (int)result.getProperty("h", 600);
    editorOpen.store(true, std::memory_order_release);
    return new SandboxedEditor(*this, hwnd, w, h);
}

void SandboxedProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (!isAlive()) return;
    juce::String err;
    auto reply = control->request(op::kGetState, {}, kDefaultReplyTimeoutMs, &err);
    auto b64 = reply.getProperty("stateBase64", "").toString();
    juce::MemoryOutputStream mo(destData, false);
    juce::Base64::convertFromBase64(mo, b64);
}

void SandboxedProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (!isAlive() || data == nullptr || sizeInBytes <= 0) return;
    juce::DynamicObject::Ptr args(new juce::DynamicObject());
    args->setProperty("stateBase64", juce::Base64::toBase64(data, (size_t)sizeInBytes));
    juce::String err;
    control->request(op::kSetState, juce::var(args.get()),
                     kDefaultReplyTimeoutMs, &err);
}

} // namespace slopsmith::sandbox
