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
            tryEmbedNativeHwnd();
           #endif
        }

       #if JUCE_WINDOWS
        // Reparent the sandbox plugin's HWND under this editor's top-level
        // OS window so it appears inside the editor frame.
        //
        // parentHierarchyChanged() fires while the host DocumentWindow is
        // still being constructed (during setContentOwned), before the window
        // has been added to the desktop — so getPeer() is null on that first
        // call. Rather than give up, re-post the attempt: by the next message
        // loop cycle the window's setVisible(true) has run and the OS peer
        // exists. Bounded so a genuinely peerless host can't spin forever.
        void tryEmbedNativeHwnd()
        {
            if (!nativeHwnd || embedded)
                return;

            auto* peer = getPeer();
            HWND parent = peer ? (HWND)peer->getNativeHandle() : nullptr;
            if (!parent)
            {
                if (++peerRetries <= kMaxPeerRetries)
                {
                    juce::Component::SafePointer<SandboxedEditor> self(this);
                    juce::MessageManager::callAsync([self]()
                    {
                        if (self != nullptr)
                            self->tryEmbedNativeHwnd();
                    });
                }
                else if (! peerlessLogged)
                {
                    VST_TRACE("[sandbox] SandboxedEditor: host window never "
                              "got an OS peer after %d retries — editor HWND "
                              "left unparented", kMaxPeerRetries);
                    peerlessLogged = true;
                }
                return;
            }

            wchar_t winsta[128] = L"?";
            DWORD winstaLen = 0;
            GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME,
                                      winsta, sizeof(winsta), &winstaLen);
            VST_TRACE("[sandbox] SandboxedEditor: embed attempt — "
                      "nativeHwnd=%p IsWindow=%d, parentHwnd=%p IsWindow=%d, "
                      "winsta=%ls",
                      nativeHwnd, IsWindow((HWND)nativeHwnd) ? 1 : 0,
                      (void*)parent, IsWindow(parent) ? 1 : 0, winsta);

            auto style = GetWindowLongPtrW((HWND)nativeHwnd, GWL_STYLE);
            style = (style | WS_CHILD) & ~WS_POPUP;
            SetWindowLongPtrW((HWND)nativeHwnd, GWL_STYLE, style);

            // SetParent returns the *previous* parent, which is NULL both on
            // failure and when the old parent was the desktop — ambiguous.
            // Verify the reparent took by reading the parent back. Don't set
            // `embedded` on failure: that flag gates the retry guard, so a
            // false success would strand the editor offscreen forever.
            SetLastError(0);
            SetParent((HWND)nativeHwnd, parent);
            if (GetParent((HWND)nativeHwnd) != parent)
            {
                VST_TRACE("[sandbox] SandboxedEditor: SetParent failed "
                          "(GetLastError=%lu) — HWND not embedded",
                          (unsigned long)GetLastError());
                return;
            }
            if (! SetWindowPos((HWND)nativeHwnd, nullptr, 0, 0,
                               getWidth(), getHeight(),
                               SWP_NOZORDER | SWP_FRAMECHANGED))
            {
                VST_TRACE("[sandbox] SandboxedEditor: SetWindowPos failed "
                          "after reparent");
                return;
            }
            // The sandbox parks the editor window offscreen until it has a
            // parent; make it visible now that it's embedded.
            ShowWindow((HWND)nativeHwnd, SW_SHOW);
            embedded = true;
            VST_TRACE("[sandbox] SandboxedEditor: embedded plugin HWND under "
                      "host window (%dx%d)", getWidth(), getHeight());

            // The host window was shown and brought to front while it was
            // being constructed — before this async embed completed — so it
            // can end up behind the main app window. Raise it now that the
            // plugin HWND is actually in place.
            if (auto* top = getTopLevelComponent())
                top->toFront(true);
        }
       #endif

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
        static constexpr int kMaxPeerRetries = 50;

        SandboxedProcessor& proc;
        void* nativeHwnd = nullptr;
        bool embedded = false;
        bool peerlessLogged = false;
        int peerRetries = 0;
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
        // Millisecond-counter timestamp of the last `loading` heartbeat from
        // the sandbox. The ready-wait below measures its timeout against this
        // rather than against spawn time, so a slow-but-alive plugin that
        // keeps heart-beating is never fast-failed.
        std::atomic<juce::uint32> lastProgressMs{0};
    };
    auto readyState = std::make_shared<ReadyState>();
    readyState->lastProgressMs.store(juce::Time::getMillisecondCounter(),
                                     std::memory_order_relaxed);
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
        if (evname == event::kLoading)
        {
            // Slow-load heartbeat: advance the ready-wait deadline, nothing
            // else. Not forwarded to onControlEvent — it carries no payload.
            readyState->lastProgressMs.store(juce::Time::getMillisecondCounter(),
                                             std::memory_order_relaxed);
            return;
        }
        if (evname == event::kReady)
        {
            bool expected = false;
            if (readyState->readySet.compare_exchange_strong(expected, true,
                                                             std::memory_order_acq_rel))
            {
                // Validate the sandbox-advertised protocol version against
                // the host build. The per-frame `v` check in
                // ControlChannel::ioLoop catches mismatched messages too,
                // but doing it here makes the failure a clean handshake
                // error instead of "first message in/out the channel
                // suddenly tears down". 0 means the sandbox didn't emit
                // the field (pre-protocolVersion-in-ready build); treat
                // as legacy = current protocol so old test stubs still
                // work.
                const int wireVer = (int)data.getProperty("protocolVersion", 0);
                if (wireVer != 0 && wireVer != (int)kProtocolVersion)
                {
                    VST_TRACE("[sandbox] protocol version mismatch at handshake: "
                              "host=%d sandbox=%d",
                              (int)kProtocolVersion, wireVer);
                    try { readyState->readyP.set_value(false); } catch (...) {}
                    return;
                }
                descriptionCached.name = data.getProperty("pluginName", "").toString();
                descriptionCached.manufacturerName =
                    data.getProperty("manufacturer", "").toString();
                // Prefer the plugin's own reported fileOrIdentifier /
                // pluginFormatName (from desc.* on the sandbox side) when
                // the ready event carries them — some VST3s normalise the
                // path differently than the caller passed in. Fall back
                // to the spawn-time hardcodes when the wire field is
                // empty so the description always has _something_ usable
                // for a SignalChain round-trip.
                {
                    juce::String wireFOI = data.getProperty("fileOrIdentifier", "").toString();
                    juce::String wireFmt = data.getProperty("pluginFormatName", "").toString();
                    descriptionCached.fileOrIdentifier =
                        wireFOI.isNotEmpty() ? wireFOI : spawnConfig.pluginPath;
                    descriptionCached.pluginFormatName =
                        wireFmt.isNotEmpty() ? wireFmt : juce::String("VST3");
                    // uniqueId + deprecatedUid are critical for SignalChain
                    // persistence: a saved session re-locates plugins by
                    // identity, not by file path. Without these, sandboxed
                    // plugins wouldn't survive a session save/load round-
                    // trip across host machines (where file paths differ).
                    descriptionCached.uniqueId =
                        (int)data.getProperty("uniqueId", 0);
                    descriptionCached.deprecatedUid =
                        (int)data.getProperty("deprecatedUid", 0);
                }
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
        errorOut = "control->start failed: " + control->getLastStartError();
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

    // Wait for the `ready` handshake against two independent bounds:
    //   * per-heartbeat — fail if no `loading` heartbeat (or `ready`) arrives
    //     within spawnTimeoutMs. Catches a dead or frozen sandbox.
    //   * absolute — fail after kReadyAbsoluteTimeoutMs regardless of
    //     heartbeats. The heartbeat is a fixed timer, not a real progress
    //     signal, so a sandbox that stays alive and keeps heart-beating but
    //     whose plugin load never completes would defeat the per-heartbeat
    //     bound forever; the absolute cap is the hard backstop.
    const juce::uint32 waitStartMs = juce::Time::getMillisecondCounter();
    for (;;)
    {
        if (readyF.wait_for(std::chrono::milliseconds(250))
            == std::future_status::ready)
            break;
        const juce::uint32 nowMs = juce::Time::getMillisecondCounter();
        const juce::uint32 sinceProgress =
            nowMs - readyState->lastProgressMs.load(std::memory_order_relaxed);
        const juce::uint32 sinceStart = nowMs - waitStartMs;
        const bool absoluteExceeded =
            sinceStart > (juce::uint32) kReadyAbsoluteTimeoutMs;
        if (sinceProgress > (juce::uint32) spawnConfig.spawnTimeoutMs
            || absoluteExceeded)
        {
            errorOut = absoluteExceeded
                         ? "sandbox did not become ready within absolute timeout"
                         : "sandbox did not become ready within timeout";
            // Explicit teardown so all the resource-release wiring lives in
            // one place. The destructor would otherwise pick this up when the
            // outer unique_ptr drops, but it's clearer to tear down on the
            // failure edge and not rely on destruction order. Distinct reason
            // per bound so crash/teardown reporting can tell a silent sandbox
            // apart from a plugin that kept heart-beating but never loaded.
            teardown(absoluteExceeded ? "ready absolute timeout" : "ready timeout");
            return false;
        }
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
    alive.exchange(false, std::memory_order_acq_rel);
    // Copy under the mutex so a concurrent setOnCrash() can't race with
    // std::function's internals. Invoking happens later (outside the lock)
    // so a callback that re-enters setOnCrash doesn't deadlock.
    //
    // Invoke regardless of `wasAlive`: pre-ready failures (subprocess
    // exits before handshake, plugin DLL fails to load → exit code 5)
    // are exactly the case where an async caller most needs to know
    // the sandbox died. Today initialise() also surfaces such failures
    // via errorOut, but a future async-spawn caller that registers
    // setOnCrash and then waits asynchronously needs the callback.
    CrashCallback cb;
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
        // Sandbox is gone — pass silence through so the chain keeps
        // flowing. Leave midiMessages untouched: downstream processors
        // in a SignalChain expect to see MIDI we can't deliver to our
        // own sandbox, but they may still consume it themselves.
        buffer.clear();
        return;
    }
    const int n = buffer.getNumSamples();

    // v2: MIDI rides inline in the input slot's MidiQueue. Zero control-pipe
    // I/O on the audio thread (was the deferred Copilot finding from PR #63).
    //
    // We deliberately do NOT clear `midiMessages` after pushInputBlock — the
    // host has only published a *snapshot* into the slot's queue, the
    // original buffer still belongs to the SignalChain. Downstream
    // processors (e.g. a synth after a sandboxed effect) need to see the
    // same MIDI events the chain delivered to us. Mirrors the early-return
    // branch above which leaves midiMessages untouched for the same reason.
    if (!audio->pushInputBlock(buffer, midiMessages, n))
    {
        // Input ring full — sandbox isn't keeping up. Don't wait the full
        // pop timeout (which would extend the dropout); zero output and
        // exit. xruns was incremented inside pushInputBlock.
        VST_TRACE("[sandbox] processBlock: input ring full, dropping (xruns++)");
        buffer.clear();
        return;
    }

    // Pop timeout = 4× the block period, floored at 2 ms so very high
    // sample rates / small blocks don't end up with sub-millisecond budgets.
    constexpr int kPopTimeoutBlockMultiplier = 4;
    // Upstream invariant: SandboxFactory_win::tryLoadSandboxed caps
    // sampleRate at uint32_t::max via `(uint32_t)std::lround(sr)`, so
    // the `(int)spawnConfig.audio.sampleRate` cast below cannot wrap
    // negative today. If that cap ever loosens, this divisor could
    // become a wrapped-negative int and `jmax(1, negative)` would
    // yield 1 → enormous timeout. The jmax(1, ...) is the in-place
    // guard; keep it even though the upstream cap currently makes
    // it unreachable.
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
    // If a previous editor is still alive (JUCE didn't destroy it before
    // this createEditor call — atypical but legal), short-circuit with
    // nullptr rather than firing a second kOpenEditor that would queue
    // behind the first inside vst-host's dispatchRequest editor-request
    // serializer (10s wait + redundant editor creation in the sandbox).
    //
    // Caller contract: JUCE's AudioProcessor::createEditor may return
    // nullptr at any time per its docs; SignalChain treats that as
    // "this slot has no UI right now" and disables the slot's editor
    // control. A nullptr return AFTER hasEditor() returned true is
    // unusual but legal — callers must not crash on it. If a future
    // host treats `createEditor() == nullptr` after `hasEditor() ==
    // true` as a hard error, the right fix is to close the existing
    // editor first via notifyEditorClosing(); revisit then.
    if (editorOpen.load(std::memory_order_acquire))
        return nullptr;
    juce::String err;
    auto result = control->request(op::kOpenEditor, {}, kDefaultReplyTimeoutMs, &err);
    if (!result.isObject())
    {
        // The request could have failed for a benign reason (timeout, channel
        // disconnect, sandbox replied ok=false), but the sandbox may have
        // completed the open on its side after we gave up. Mirror the
        // null-HWND branch's best-effort cleanup so a sandbox-side editor
        // isn't orphaned until the next successful open / kShutdown.
        if (control)
            control->postNoReply(op::kCloseEditor, {});
        return nullptr;
    }

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
    if (! juce::Base64::convertFromBase64(mo, b64))
    {
        // Malformed base64 in the wire payload — leave destData empty
        // (mo writes nothing on failure) so the host sees a "no state"
        // outcome rather than a partial blob. Surface the failure so
        // IPC corruption is diagnosable instead of silently masquerading
        // as a plugin with no state.
        VST_TRACE("[sandbox] getStateInformation: invalid base64 in reply "
                  "(len=%d)", (int)b64.length());
    }
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
