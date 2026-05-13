#include "SandboxedProcessor.h"
#include "ControlChannel.h"
#include "AudioChannel.h"
#include "SubprocessHandle.h"
#include "../VSTTrace.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace slopsmith::sandbox {

namespace {
    // Editor wrapper: an AudioProcessorEditor that hosts the plugin's native
    // HWND (returned from the sandbox over the openEditor request). v1 only
    // reparents and resizes the embedded HWND locally; forwarding size
    // changes back to the sandbox via op::kResizeEditor is on the follow-up
    // checklist (see SANDBOX-DESIGN.md §3 "planned" ops).
    class SandboxedEditor : public juce::AudioProcessorEditor
    {
    public:
        SandboxedEditor(SandboxedProcessor& p, void* hwnd, int w, int h)
            : juce::AudioProcessorEditor(&p), proc(p), nativeHwnd(hwnd)
        {
            setSize(w > 0 ? w : kDefaultEditorWidth,
                    h > 0 ? h : kDefaultEditorHeight);
            setOpaque(true);
            // Reparenting the plugin HWND happens in parentHierarchyChanged()
            // — we need a valid peer parent which doesn't exist until we're
            // added to a Component hierarchy.
        }

        ~SandboxedEditor() override
        {
            // Cache the processor by reference at construction rather than
            // re-fetching via getAudioProcessor() at destruction — if the
            // host ever inverts the lifetime (editor outlives the processor,
            // legal under JUCE's contract during atypical teardown), the
            // base-class pointer dangles and dynamic_cast is UB. The
            // reference points at the same processor that owns *this*
            // editor, and JUCE guarantees the processor outlives any editor
            // it created via createEditor().
            proc.notifyEditorClosing();
        }

        void parentHierarchyChanged() override
        {
           #if JUCE_WINDOWS
            if (!nativeHwnd) return;
            auto* peer = getPeer();
            HWND parent = peer ? (HWND)peer->getNativeHandle() : nullptr;
            if (!parent)
            {
                // No top-level JUCE peer — the common case inside the
                // Electron addon today (Electron-side HWND reparenting is
                // the deferred follow-up). Trace once per editor so
                // "plugin loaded but editor never appears" is at least
                // diagnosable from logs rather than completely silent.
                if (! peerlessLogged)
                {
                    VST_TRACE("[sandbox] SandboxedEditor: no JUCE peer for "
                              "parent reparent (editor stays at -32000,-32000 "
                              "until Electron-side HWND embedding lands)");
                    peerlessLogged = true;
                }
                return;
            }
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
            // Painted area is fully covered by the embedded HWND once the
            // reparenting follow-up wires up the Electron-side embedding.
            // Until then this fill is what users see if a host hasn't yet
            // implemented the embedding — keep it a dim neutral grey so
            // it reads as "loading placeholder" rather than the alarming
            // opaque black rectangle a v1 host would otherwise show.
            g.fillAll(juce::Colour::fromRGB(32, 32, 32));
        }

    private:
        SandboxedProcessor& proc;
        void* nativeHwnd = nullptr;
        bool peerlessLogged = false;
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
    // Destruction is a deliberate teardown, not a crash. Drop the onCrash
    // callback before teardown so it doesn't fire — consumers reasonably
    // assume onCrash means "the sandbox died unexpectedly".
    setOnCrash(nullptr);
    teardown("destructor");
}

void SandboxedProcessor::setOnCrash(CrashCallback cb)
{
    std::lock_guard<std::mutex> lock(onCrashMutex);
    onCrash = std::move(cb);
}

std::unique_ptr<SandboxedProcessor> SandboxedProcessor::spawn(const SpawnConfig& cfg,
                                                              juce::String& errorOut)
{
    // Validate caller-supplied dims against the protocol caps before any
    // shm/pipe allocation — both sides assume slots fit inside the cap-
    // derived layout; an oversize maxBlocks/maxChannels would let
    // createHostSide allocate beyond what openSandboxSide validates
    // (which today only checks magic + protocolVersion).
    if (cfg.audio.maxBlocks == 0 || cfg.audio.maxBlocks > kAudioMaxBlocks)
    {
        errorOut = "invalid audio.maxBlocks: " + juce::String((int)cfg.audio.maxBlocks)
                 + " (cap=" + juce::String((int)kAudioMaxBlocks) + ")";
        return nullptr;
    }
    if (cfg.audio.maxChannels == 0 || cfg.audio.maxChannels > kAudioMaxChannels)
    {
        errorOut = "invalid audio.maxChannels: " + juce::String((int)cfg.audio.maxChannels)
                 + " (cap=" + juce::String((int)kAudioMaxChannels) + ")";
        return nullptr;
    }
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

    // ControlChannel keeps the event callback past initialise()'s return, so
    // the ready-handshake state has to outlive this stack frame. Wrap it in a
    // shared_ptr the lambda copies — the future's shared state lives via the
    // promise inside.
    struct ReadyState
    {
        std::promise<bool> readyP;
        std::atomic<bool>  readySet{false};
    };
    auto readyState = std::make_shared<ReadyState>();
    auto readyF     = readyState->readyP.get_future();

    // Ordering invariant for the cached fields below:
    //   1. The event callback (control I/O thread) writes the cached fields.
    //   2. The same callback then publishes via `alive.store(release)`.
    // Reads MUST observe `alive` via `isAlive()` (which does `load(acquire)`)
    // before touching the cached fields. The getters in SandboxedProcessor.h
    // already gate this way; new getters must follow the same pattern, or
    // the cached field has to become std::atomic.
    auto eventCb = [this, readyState](const juce::String& evname,
                                      const juce::var& data)
    {
        if (evname == event::kReady)
        {
            bool expected = false;
            if (readyState->readySet.compare_exchange_strong(expected, true,
                                                             std::memory_order_acq_rel))
            {
                descriptionCached.name = data.getProperty("pluginName", "").toString();
                descriptionCached.manufacturerName =
                    data.getProperty("manufacturer", "").toString();
                // Populate identifiers known at spawn time so the
                // description survives a SignalChain round-trip — every
                // other code path uses fileOrIdentifier to re-locate the
                // plugin, and pluginFormatName == "VST3" is fixed for
                // this sandbox binary.
                descriptionCached.fileOrIdentifier = spawnConfig.pluginPath;
                descriptionCached.pluginFormatName = "VST3";
                hasEditorCached    = (bool)data.getProperty("hasEditor", false);
                acceptsMidiCached  = (bool)data.getProperty("acceptsMidi", false);
                producesMidiCached = (bool)data.getProperty("producesMidi", false);
                numInputsCached    = (int)data.getProperty("numInputs",
                                                            (int)spawnConfig.audio.maxChannels);
                numOutputsCached   = (int)data.getProperty("numOutputs",
                                                            (int)spawnConfig.audio.maxChannels);
                alive.store(true, std::memory_order_release);
                try { readyState->readyP.set_value(true); } catch (...) {}
            }
        }
        onControlEvent(evname, data);
    };

    auto failHandshake = [readyState]()
    {
        bool expected = false;
        if (readyState->readySet.compare_exchange_strong(expected, true,
                                                         std::memory_order_acq_rel))
        {
            try { readyState->readyP.set_value(false); } catch (...) {}
        }
    };

    // Lifetime invariant for the callbacks below: both capture `this` and
    // call teardown(). teardown() calls control->stop() and subprocess->
    // shutdown() which JOIN the threads invoking these callbacks. So the
    // callbacks always complete before destruction proceeds — but only
    // because stop()/shutdown() happen at the top of teardown(), before
    // member-destruction. Keep that ordering when editing teardown(); if
    // a future refactor moves member destruction before the joins, the
    // watcher thread could re-enter teardown on a partially-destroyed
    // `this`. A weak_ptr-based state block would make this self-evident.
    auto disconnectCb = [this, failHandshake](const juce::String& reason)
    {
        failHandshake();
        teardown(reason);
    };

    if (!control->start(eventCb, disconnectCb))
    {
        errorOut = "control->start failed";
        return false;
    }
    if (!subprocess->start(spawnConfig.sandboxExePath, args,
                           [this, failHandshake](int code)
                           {
                               failHandshake();
                               teardown("sandbox exit code " + juce::String(code));
                           }, err))
    {
        errorOut = "subprocess: " + err;
        // Symmetry with the success path: control->start() already armed the
        // I/O thread inside ConnectNamedPipe; stopping it explicitly here
        // shortens the time we hold the pipe + the watchdog grace period
        // (otherwise it'd sit blocked until the destructor's teardown picks
        // it up at unique_ptr drop).
        //
        // Lifetime invariant for the disconnect callback fired in this
        // window: control->start() succeeded before subprocess->start(), so
        // the disconnectCb (captures `this`, calls teardown) is reachable
        // for the brief gap between the two starts. teardown() calls
        // subprocess->shutdown() on a SubprocessHandle whose start() never
        // ran — safe because `running` defaults false and shutdown() bails
        // immediately, leaving the (empty) PROCESS_INFORMATION handles
        // as nullptr for CloseHandle to no-op.
        control->stop();
        return false;
    }

    if (readyF.wait_for(std::chrono::milliseconds(spawnConfig.spawnTimeoutMs))
        != std::future_status::ready)
    {
        errorOut = "sandbox did not become ready within timeout";
        // Explicit teardown so all the resource-release wiring lives in
        // one place. The destructor would otherwise pick this up when the
        // outer unique_ptr drops, but it's clearer to tear down on the
        // failure edge and not rely on destruction order.
        teardown("ready timeout");
        return false;
    }
    if (!readyF.get())
    {
        // The promise was resolved with false by failHandshake (subprocess
        // exit, control disconnect, etc.). errorOut was likely empty until
        // now — surface a concrete reason so callers don't see "unknown".
        if (errorOut.isEmpty())
            errorOut = "sandbox handshake failed before ready (subprocess "
                       "exit or control-pipe disconnect)";
        return false;
    }
    return true;
}

void SandboxedProcessor::teardown(const juce::String& reason)
{
    const bool wasAlive = alive.exchange(false, std::memory_order_acq_rel);
    // Copy under the mutex so a concurrent setOnCrash() can't race with
    // std::function's internals. Invoking happens later (outside the lock)
    // so a callback that re-enters setOnCrash doesn't deadlock.
    CrashCallback cb;
    if (wasAlive)
    {
        std::lock_guard<std::mutex> lock(onCrashMutex);
        cb = onCrash;
    }

    // The closers themselves are individually idempotent, but running them
    // concurrently from the destructor and the subprocess-exit watcher races
    // on CloseHandle. Gate the whole block on a single-fire latch.
    bool expected = false;
    if (resourcesReleased.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel))
    {
        if (control)    control->stop();
        if (subprocess) subprocess->shutdown(1500);
        if (audio)      audio->close();
    }

    if (cb) cb(reason);
}

void SandboxedProcessor::notifyEditorClosing()
{
    if (!control) return;
    if (!editorOpen.exchange(false, std::memory_order_acq_rel)) return;
    if (!isAlive()) return;
    control->postNoReply(op::kCloseEditor, {});
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
    if (err.isNotEmpty())
        VST_TRACE("[sandbox] prepareToPlay sr=%.0f bs=%d failed: %s",
                  sampleRate, samplesPerBlock, err.toRawUTF8());
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

    // Drop MIDI entirely until the audio-shm inline-MIDI follow-up lands.
    // vst-host's op::kMidiEvent dispatcher is a no-op stub today, so
    // serializing every event from the realtime audio thread through
    // base64 + JSON + the control pipe was pure heap pressure with no
    // observable effect at the sandbox. The inline-per-block shm queue
    // covered by the audio-shm follow-up is the right shape (lock-free,
    // bounded ring); this branch is a deliberate v1 silence.
    midiMessages.clear();

    if (!audio->pushBlock(/*isOutputRing=*/false, buffer, n))
    {
        // Input ring full — sandbox isn't keeping up. Don't wait the full
        // pop timeout (which would extend the dropout); zero output and
        // exit. xruns was incremented inside pushBlock. WARNING: with the
        // v1 shared writeIdx/readIdx pair (see SANDBOX-DESIGN §4 v2 split),
        // skipping the corresponding pop leaves the two rings phase-shifted
        // for subsequent blocks. The per-direction-indices refactor in
        // PR #2 fixes this cleanly; a v1 sandbox hitting back-pressure
        // here will produce one block of stale output before recovering.
        VST_TRACE("[sandbox] processBlock: input ring full, dropping (xruns++)");
        buffer.clear();
        return;
    }
    // Pop timeout = 4× the block period, floored at 2 ms so very high
    // sample rates / small blocks don't end up with sub-millisecond budgets.
    constexpr int kPopTimeoutBlockMultiplier = 4;
    const int popTimeoutMs = (int)juce::jmax(2.0,
        1000.0 * n / juce::jmax(1, (int)spawnConfig.audio.sampleRate)
            * kPopTimeoutBlockMultiplier);
    if (!audio->popBlock(/*isOutputRing=*/true, buffer, n, popTimeoutMs))
    {
        // Missed deadline — sandbox is too slow or hung. AudioChannel
        // already bumped `dropouts`; trace so the missed-deadline path is
        // diagnosable without a debugger.
        VST_TRACE("[sandbox] processBlock: pop timeout (%d ms), inserting silence",
                  popTimeoutMs);
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
    // A blank or unparseable HWND means the sandbox claimed openEditor but
    // didn't actually create a window. Don't hand back an editor that has
    // nothing to reparent — the host would render a blank placeholder. But
    // the sandbox HAS already created its editor on its side (openEditor
    // succeeded), so best-effort close it to avoid leaking a sandbox-side
    // window.
    if (hwnd == nullptr)
    {
        if (control)
            control->postNoReply(op::kCloseEditor, {});
        return nullptr;
    }
   #else
    juce::ignoreUnused(hwndStr);
   #endif
    int w = (int)result.getProperty("w", 800);
    int h = (int)result.getProperty("h", 600);
    // Publish editorOpen BEFORE constructing the editor so that
    // ~SandboxedEditor (which calls notifyEditorClosing → editorOpen
    // .exchange(false) and posts kCloseEditor on the true→false edge)
    // works correctly even if JUCE destroys the editor before this
    // function returns. On throw, roll back AND post the best-effort
    // kCloseEditor (mirroring the null-HWND branch above) so host and
    // sandbox state stay aligned.
    editorOpen.store(true, std::memory_order_release);
    SandboxedEditor* ed = nullptr;
    try
    {
        ed = new SandboxedEditor(*this, hwnd, w, h);
    }
    catch (...)
    {
        editorOpen.store(false, std::memory_order_release);
        if (control)
            control->postNoReply(op::kCloseEditor, {});
        throw;
    }
    return ed;
}

void SandboxedProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (!isAlive()) return;
    juce::String err;
    auto reply = control->request(op::kGetState, {}, kDefaultReplyTimeoutMs, &err);
    if (err.isNotEmpty())
    {
        // Otherwise a failed round-trip would silently emit an empty blob —
        // JUCE writes that to the host's preset, which then round-trips to
        // setStateInformation later and resets the plugin to defaults.
        // Until the state-cache work lands (PR-body checklist), at least
        // make the failure visible.
        VST_TRACE("[sandbox] getStateInformation request failed: %s",
                  err.toRawUTF8());
        return;
    }
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
    if (err.isNotEmpty())
        VST_TRACE("[sandbox] setStateInformation request failed (%d bytes): %s",
                  sizeInBytes, err.toRawUTF8());
}

} // namespace slopsmith::sandbox
