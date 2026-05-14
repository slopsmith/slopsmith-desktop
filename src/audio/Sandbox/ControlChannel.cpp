#include "ControlChannel.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #error "ControlChannel.cpp is Windows-only for now. Linux/macOS impls land with PRs #2 and #3."
#endif

#include <thread>

// Reuse the existing VST trace logger for diagnostics. Cheap and survives
// crashes thanks to its synchronous flush.
#include "../VSTTrace.h"
#define CTL_TRACE(...) VST_TRACE("[ctrl] " __VA_ARGS__)

namespace slopsmith::sandbox {

const juce::String ControlChannel::kReasonPeerClosed     = "peer-closed";
const juce::String ControlChannel::kReasonReadError      = "read-error";
const juce::String ControlChannel::kReasonProtocolError  = "protocol-error";

struct ControlChannel::Impl
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool   isServer = false;
    // Set by stop() before the join. The I/O thread's ConnectNamedPipe wait
    // observes it via WaitForMultipleObjects so a stop() that races the
    // start of ioLoop still tears down promptly — CancelIoEx alone is a
    // no-op against I/O that hasn't been issued yet, which is exactly the
    // race in the wait-after-create-but-before-connect window.
    HANDLE stopEvent = nullptr;
};

ControlChannel::ControlChannel() : impl(std::make_unique<Impl>()) {}

ControlChannel::~ControlChannel()
{
    stop();
}

bool ControlChannel::createServerSide(juce::String& pipeNameOut,
                                       juce::String& errorOut)
{
    juce::Uuid uuid;
    juce::String pipeName = "\\\\.\\pipe\\slopsmith-vst-" + uuid.toDashedString();

    // PIPE_REJECT_REMOTE_CLIENTS (Vista+) refuses connections from machines
    // other than the local one. The pipe name is random per-spawn, but
    // rejecting remote clients narrows the attack surface regardless.
    HANDLE h = CreateNamedPipeW(
        pipeName.toWideCharPointer(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
            | PIPE_REJECT_REMOTE_CLIENTS,
        /*maxInstances*/ 1,
        kControlPipeBufferBytes,
        kControlPipeBufferBytes,
        /*default timeout*/ 0,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        errorOut = "CreateNamedPipeW failed: " + juce::String((int)GetLastError());
        return false;
    }
    impl->pipe = h;
    impl->isServer = true;
    pipeNameOut = pipeName;
    return true;
}

bool ControlChannel::connectClientSide(const juce::String& pipeName,
                                        juce::String& errorOut)
{
    HANDLE h = CreateFileW(
        pipeName.toWideCharPointer(),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        errorOut = "CreateFileW (client) failed: " + juce::String((int)GetLastError());
        return false;
    }
    // Pipe was opened in BYTE mode (default) which matches our framing —
    // no SetNamedPipeHandleState call needed since CreateNamedPipeW on the
    // server side is now also PIPE_TYPE_BYTE | PIPE_READMODE_BYTE.
    impl->pipe = h;
    impl->isServer = false;
    return true;
}

bool ControlChannel::start(EventCallback evCb,
                            std::function<void(const juce::String&)> disconnectCb)
{
    lastStartError.clear();
    // Reassigning a joinable std::thread aborts via std::terminate, so refuse
    // a second start. Callers should stop() then re-create the channel.
    if (ioThread.joinable() || alive.load(std::memory_order_acquire))
    {
        lastStartError = "channel already started";
        return false;
    }
    if (!impl || impl->pipe == INVALID_HANDLE_VALUE)
    {
        lastStartError = "no pipe handle (createServerSide/connectClientSide not called or failed)";
        return false;
    }

    // Manual-reset so once stop() signals it, every subsequent wait inside
    // ioLoop returns immediately.
    impl->stopEvent = CreateEventW(nullptr, /*manualReset*/TRUE, FALSE, nullptr);
    if (impl->stopEvent == nullptr)
    {
        lastStartError = "CreateEventW(stopEvent) failed: GetLastError="
                       + juce::String((int)GetLastError());
        return false;
    }

    onEvent = std::move(evCb);
    onDisconnect = std::move(disconnectCb);
    alive.store(true, std::memory_order_release);
    // ConnectNamedPipe is performed inside the I/O thread so the caller never
    // blocks. If the sandbox subprocess dies before connecting, the caller's
    // watchdog can call stop(), which CancelIoEx's the pending connect and
    // unwinds cleanly.
    ioThread = std::thread([this] { ioLoop(); });
    return true;
}

void ControlChannel::stop()
{
    // Callback lifetime invariant: by the time stop() returns, both
    // `onEvent` and `onDisconnect` have been observed for the last time —
    // the I/O thread is either joined (non-self path) or has already
    // returned from its last dispatch (self-detach path; see below).
    // Owners therefore MUST call stop() before destroying any state
    // captured by-reference into onEvent/onDisconnect.
    //
    // We deliberately do NOT clear `onEvent` here: clearing under a lock
    // before joining doesn't help (the I/O thread can be mid-invocation
    // and read the std::function while we're clearing it), and clearing
    // after joining is unnecessary (no more callers). Owners that need
    // tighter callback teardown should use a shared_ptr indirection or
    // weak_ptr capture in the lambda — same pattern the dispatchOnMessageThread
    // WaitableEvent fix uses for the same UAF class.
    alive.store(false, std::memory_order_release);

    // Signal stop BEFORE CancelIoEx. The race we're guarding against is
    // stop() running between ioThread spawn and the I/O thread issuing
    // ConnectNamedPipe — CancelIoEx would be a no-op there. With the
    // stop event signalled, the I/O thread's WaitForMultipleObjects exits
    // promptly regardless of whether the connect was ever started.
    if (impl && impl->stopEvent != nullptr)
        SetEvent(impl->stopEvent);

    // CancelIoEx unblocks the I/O thread's pending read so it can exit. The
    // handle must stay valid until the thread has returned — closing it
    // first is a TOCTOU on the in-flight read.
    if (impl && impl->pipe != INVALID_HANDLE_VALUE)
        CancelIoEx(impl->pipe, nullptr);

    if (ioThread.joinable())
    {
        if (std::this_thread::get_id() == ioThread.get_id())
        {
            // Self-stop: the I/O thread is unwinding through ioLoop /
            // failWith / disconnect-callback / our caller into here.
            // Detaching is the only choice (self-join deadlocks), but
            // closing impl->pipe / impl->stopEvent below races the
            // detached thread's last few instructions inside failWith /
            // its caller stack. Two things make it safe today:
            //   1. failWith captures everything it needs by value /
            //      shared_ptr and does NOT re-touch impl->pipe after
            //      setting `alive = false`.
            //   2. CancelIoEx + SetEvent above already shoved the I/O
            //      thread past any blocking syscall on the handles, so
            //      the window between detach and CloseHandle is just
            //      stack unwinding — no pipe-handle dereference.
            // If a future refactor moves pipe access into the unwind
            // path (e.g. a flush-on-stop in failWith), revisit: a
            // shared_ptr-guarded impl + resourcesReleased latch is the
            // standard fix.
            ioThread.detach();
        }
        else
            ioThread.join();
    }

    if (impl && impl->pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(impl->pipe);
        impl->pipe = INVALID_HANDLE_VALUE;
    }
    if (impl && impl->stopEvent != nullptr)
    {
        CloseHandle(impl->stopEvent);
        impl->stopEvent = nullptr;
    }

    // Fail any in-flight requests so callers don't hang.
    std::lock_guard<std::mutex> lk(pendingMutex);
    for (auto& [id, p] : pending)
    {
        try { p->promise.set_value({}); }
        catch (const std::future_error&) {}
    }
    pending.clear();
}

// One-shot overlapped issue/wait/result, returning the actual bytes
// transferred (or 0 on failure with GetLastError set). Used by
// overlappedTransfer below in a loop, because byte-mode pipes can satisfy
// a single ReadFile/WriteFile with fewer bytes than requested.
static DWORD overlappedChunk(HANDLE pipe, bool isWrite, void* buf,
                             DWORD bytes, DWORD timeoutMs)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr)
        return 0;
    const BOOL started = isWrite
        ? WriteFile(pipe, buf, bytes, nullptr, &ov)
        : ReadFile (pipe, buf, bytes, nullptr, &ov);
    const DWORD startErr = started ? 0 : GetLastError();
    if (!started && startErr != ERROR_IO_PENDING)
    {
        CloseHandle(ov.hEvent);
        SetLastError(startErr);
        return 0;
    }
    if (timeoutMs != INFINITE)
    {
        if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0)
        {
            CancelIoEx(pipe, &ov);
            DWORD drained = 0;
            GetOverlappedResult(pipe, &ov, &drained, TRUE);
            CloseHandle(ov.hEvent);
            // ERROR_TIMEOUT (1460) is the Win32 last-error sentinel; the
            // WaitForSingleObject return value WAIT_TIMEOUT (258) is in a
            // different number space and would be misleading in diagnostics.
            SetLastError(ERROR_TIMEOUT);
            return 0;
        }
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe, &ov, &transferred, TRUE))
    {
        const DWORD err = GetLastError();
        CloseHandle(ov.hEvent);
        SetLastError(err);
        return 0;
    }
    CloseHandle(ov.hEvent);
    return transferred;
}

// Loop over overlappedChunk until `bytesPerOp` have been transferred. Needed
// because the pipe is in byte mode (PIPE_TYPE_BYTE), so a single ReadFile or
// WriteFile can return fewer bytes than requested even when the rest is
// still on the wire. INFINITE is the default for reads (the I/O thread is
// the server-side wait); writes pass a finite timeout so a stalled reader
// can't pin the caller indefinitely.
static bool overlappedTransfer(HANDLE pipe, bool isWrite, void* buf,
                               DWORD bytesPerOp, DWORD timeoutMs = INFINITE)
{
    auto* p = static_cast<char*>(buf);
    DWORD remaining = bytesPerOp;
    while (remaining > 0)
    {
        const DWORD got = overlappedChunk(pipe, isWrite, p, remaining,
                                          timeoutMs);
        if (got == 0)
            return false; // GetLastError() preserved from overlappedChunk
        p         += got;
        remaining -= got;
    }
    return true;
}

bool ControlChannel::writeFrame(const juce::MemoryBlock& body)
{
    std::lock_guard<std::mutex> lk(writeMutex);
    if (impl->pipe == INVALID_HANDLE_VALUE) return false;
    if (body.getSize() > kMaxControlMessageBytes) return false;

    // 5 s is comfortably longer than any normal protocol turn (frames are
    // small JSON over a local pipe) but short enough that a stalled reader
    // doesn't pin the calling thread until the higher-level request timeout.
    constexpr DWORD kWriteTimeoutMs = 5000;
    uint32_t lenLE = (uint32_t)body.getSize();
    if (!overlappedTransfer(impl->pipe, true, &lenLE, sizeof(lenLE),
                            kWriteTimeoutMs))
        return false;
    if (body.getSize() > 0)
    {
        if (!overlappedTransfer(impl->pipe, true,
                                const_cast<void*>(body.getData()),
                                (DWORD)body.getSize(), kWriteTimeoutMs))
            return false;
    }
    return true;
}

bool ControlChannel::readFrame(juce::MemoryBlock& out)
{
    lastReadError = 0;
    if (impl->pipe == INVALID_HANDLE_VALUE)
    {
        CTL_TRACE("readFrame: pipe is INVALID_HANDLE_VALUE");
        return false;
    }
    uint32_t lenLE = 0;
    if (!overlappedTransfer(impl->pipe, false, &lenLE, sizeof(lenLE)))
    {
        // Capture before CTL_TRACE — formatting/IO can clobber GetLastError.
        lastReadError = GetLastError();
        CTL_TRACE("readFrame: ReadFile(len) failed err=%lu", lastReadError);
        return false;
    }
    if (lenLE > kMaxControlMessageBytes)
    {
        // ERROR_INVALID_DATA is the closest Win32 sentinel; ioLoop maps
        // anything that isn't a peer-closed code to kReasonReadError, but
        // setting it explicitly avoids the default 0 (NO_ERROR), which
        // would be confusing in logs.
        lastReadError = ERROR_INVALID_DATA;
        CTL_TRACE("readFrame: oversized frame len=%lu", (unsigned long)lenLE);
        return false;
    }
    out.setSize(lenLE, false);
    if (lenLE == 0) return true;
    // Finite body timeout. Once the 4-byte length prefix has arrived, the
    // peer is committed to sending lenLE more bytes; INFINITE on the body
    // read would wedge the I/O thread indefinitely if a buggy/malicious
    // peer wrote the prefix and stalled. 30s is generous for the largest
    // legitimate frame (state-restore can be a few MB through a slow link)
    // while bounding the DoS window. On timeout, GetLastError surfaces as
    // ERROR_OPERATION_ABORTED via the helper's CancelIoEx path → ioLoop
    // maps that to kReasonReadError and tears down the channel cleanly.
    constexpr DWORD kBodyReadTimeoutMs = 30000;
    if (!overlappedTransfer(impl->pipe, false, out.getData(),
                            (DWORD)lenLE, kBodyReadTimeoutMs))
    {
        lastReadError = GetLastError();
        CTL_TRACE("readFrame: ReadFile(body len=%lu) failed err=%lu",
                  (unsigned long)lenLE, lastReadError);
        return false;
    }
    return true;
}

void ControlChannel::ioLoop()
{
    CTL_TRACE("ioLoop entered (isServer=%d)", (int)impl->isServer);
    // Server side: wait for the sandbox to connect. Now overlapped because
    // the pipe was opened with FILE_FLAG_OVERLAPPED — synchronous wait via
    // GetOverlappedResult.
    if (impl->isServer && impl->pipe != INVALID_HANDLE_VALUE)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr)
        {
            CTL_TRACE("ConnectNamedPipe: CreateEventW failed err=%lu",
                      (unsigned long)GetLastError());
            failWith(kReasonReadError + " (event)");
            return;
        }
        BOOL ok = ConnectNamedPipe(impl->pipe, &ov);
        DWORD err = ok ? 0 : GetLastError();
        if (!ok && err == ERROR_IO_PENDING)
        {
            // Wait on the connect event AND the stop event. The stop event
            // is signalled by ControlChannel::stop() even if it ran during
            // the brief window between ioThread spawn and this call —
            // CancelIoEx alone is a no-op against not-yet-issued I/O.
            HANDLE waits[2] = { ov.hEvent, impl->stopEvent };
            const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (which == WAIT_OBJECT_0 + 1)
            {
                // Stop requested — cancel the pending connect so the kernel
                // releases the OVERLAPPED before we CloseHandle it.
                CancelIoEx(impl->pipe, &ov);
                DWORD drained = 0;
                GetOverlappedResult(impl->pipe, &ov, &drained, TRUE);
                CloseHandle(ov.hEvent);
                CTL_TRACE("ConnectNamedPipe cancelled by stop()");
                failWith(kReasonReadError + " (stopped)");
                return;
            }
            DWORD t = 0;
            ok = GetOverlappedResult(impl->pipe, &ov, &t, TRUE);
            if (!ok) err = GetLastError();
        }
        else if (!ok && err == ERROR_PIPE_CONNECTED)
        {
            ok = TRUE;
        }
        CloseHandle(ov.hEvent);
        if (!ok)
        {
            CTL_TRACE("ConnectNamedPipe failed err=%lu", (unsigned long)err);
            failWith(kReasonReadError + " (connect)");
            return;
        }
        CTL_TRACE("ConnectNamedPipe returned (client connected)");
    }

    juce::MemoryBlock frame;
    while (alive.load(std::memory_order_acquire))
    {
        if (!readFrame(frame))
        {
            CTL_TRACE("readFrame failed; exiting loop");
            // Distinguish a clean peer-side shutdown from an actual I/O fault
            // so the disconnect callback's caller can decide between
            // "expected" and "should restart".
            const bool peerClosed = (lastReadError == ERROR_BROKEN_PIPE
                                  || lastReadError == ERROR_PIPE_NOT_CONNECTED
                                  || lastReadError == ERROR_NO_DATA);
            failWith(peerClosed ? kReasonPeerClosed : kReasonReadError);
            return;
        }
        CTL_TRACE("readFrame got %d bytes", (int)frame.getSize());

        juce::String parseError;
        auto msg = wire::decode(frame.getData(), frame.getSize(), &parseError);
        if (!msg.isObject())
        {
            // %.*s with an explicit length — the frame buffer is not
            // NUL-terminated and can be 0 bytes (no body), so %.32s would
            // read past the end (or dereference null).
            const int previewLen = juce::jmin<int>(32, (int)frame.getSize());
            CTL_TRACE("decode failed: %s; first %d bytes: %.*s",
                      parseError.toRawUTF8(), previewLen,
                      previewLen, (const char*)frame.getData());
            failWith(kReasonProtocolError + ": " + parseError);
            return;
        }

        // Reject frames missing or mismatching the protocol version — better
        // to fail fast on host/sandbox skew than to keep going and misparse a
        // payload that doesn't match the schema we expect.
        const int incomingVersion = (int)msg.getProperty("v", -1);
        if (incomingVersion != (int)kProtocolVersion)
        {
            CTL_TRACE("protocol version mismatch: got=%d expected=%d",
                      incomingVersion, (int)kProtocolVersion);
            failWith(kReasonProtocolError + ": version mismatch (got "
                     + juce::String(incomingVersion) + ", expected "
                     + juce::String((int)kProtocolVersion) + ")");
            return;
        }

        // Reply ({id, ok, result/error}) vs event ({event, data}) vs request
        // ({id, op, args}). Dispatch by structure.
        if (msg.hasProperty("event"))
        {
            CTL_TRACE("event: %s", msg["event"].toString().toRawUTF8());
            if (onEvent)
                onEvent(msg["event"].toString(), msg["data"]);
            continue;
        }
        if (msg.hasProperty("op"))
        {
            const int id = (int)msg.getProperty("id", -1);
            if (requestHandler)
            {
                requestHandler(id, msg["op"].toString(), msg["args"]);
            }
            else if (id >= 0)
            {
                // No handler installed (host side never accepts inbound
                // requests). Reply with an explicit error so a misbehaving
                // or forged peer can't pin our request() with a 10 s wait.
                sendReply(id, false, {}, "no request handler installed");
            }
            continue;
        }
        // Reply path
        int id = (int)msg.getProperty("id", -1);
        std::shared_ptr<Pending> pendingEntry;
        {
            std::lock_guard<std::mutex> lk(pendingMutex);
            auto it = pending.find(id);
            if (it != pending.end())
            {
                pendingEntry = it->second;
                pending.erase(it);
            }
        }
        if (pendingEntry)
        {
            bool ok = (bool)msg.getProperty("ok", false);
            juce::DynamicObject::Ptr replyObj(new juce::DynamicObject());
            replyObj->setProperty("ok", ok);
            replyObj->setProperty("result", msg["result"]);
            replyObj->setProperty("error", msg["error"]);
            try { pendingEntry->promise.set_value(juce::var(replyObj.get())); }
            catch (const std::future_error&) {}
        }
    }
}

void ControlChannel::failWith(const juce::String& reason)
{
    if (!alive.exchange(false, std::memory_order_acq_rel)) return;

    // Drain internal state BEFORE invoking the callback. The disconnect
    // handler is allowed to tear down higher-level owners that destroy this
    // ControlChannel (typical teardown chain: SandboxedProcessor::teardown
    // → ControlChannel::stop → ~ControlChannel), so any member access after
    // the callback returns would be use-after-free.
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        for (auto& [id, p] : pending)
        {
            try { p->promise.set_value({}); } catch (...) {}
        }
        pending.clear();
    }

    auto cb = std::move(onDisconnect);
    if (cb) cb(reason);
}

juce::var ControlChannel::request(const char* op, const juce::var& args,
                                   int timeoutMs, juce::String* errorOut)
{
    if (!alive.load(std::memory_order_acquire))
    {
        if (errorOut) *errorOut = "channel not alive";
        return {};
    }
    int id = nextRequestId.fetch_add(1, std::memory_order_relaxed);

    auto entry = std::make_shared<Pending>();
    auto fut = entry->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        pending[id] = entry;
    }

    auto frame = wire::encode(wire::makeRequest(id, op, args));
    if (!writeFrame(frame))
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        pending.erase(id);
        if (errorOut) *errorOut = "write failed";
        return {};
    }

    if (fut.wait_for(std::chrono::milliseconds(timeoutMs))
        == std::future_status::timeout)
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        pending.erase(id);
        if (errorOut) *errorOut = "timeout";
        return {};
    }

    auto reply = fut.get();
    // stop() / failWith() resolve in-flight requests with an undefined `var`
    // so callers don't hang. Detect that case explicitly — otherwise
    // getProperty(...) returns defaults and the caller sees an empty
    // errorOut even though the real cause was disconnect/cancellation.
    if (!reply.isObject())
    {
        if (errorOut) *errorOut = "control channel disconnected";
        return {};
    }
    if (!(bool)reply.getProperty("ok", false))
    {
        if (errorOut) *errorOut = reply.getProperty("error", "").toString();
        return {};
    }
    return reply["result"];
}

bool ControlChannel::postNoReply(const char* op, const juce::var& args)
{
    if (!alive.load(std::memory_order_acquire)) return false;
    auto frame = wire::encode(wire::makeRequest(-1, op, args));
    return writeFrame(frame);
}

bool ControlChannel::sendReply(int requestId, bool ok, const juce::var& result,
                                const juce::String& errorMessage)
{
    auto frame = wire::encode(wire::makeReply(requestId, ok, result, errorMessage));
    return writeFrame(frame);
}

bool ControlChannel::sendEvent(const char* eventName, const juce::var& data)
{
    auto frame = wire::encode(wire::makeEvent(eventName, data));
    return writeFrame(frame);
}

void ControlChannel::setRequestHandler(RequestHandler handler)
{
    // The I/O thread reads requestHandler unsynchronized — assignments after
    // start() would race. Header documents "MUST be called BEFORE start()";
    // assert it so a future regression (e.g. wiring a handler from a ready
    // callback) fails loudly in debug builds rather than silently racing.
    jassert(!ioThread.joinable() && !alive.load(std::memory_order_acquire));
    requestHandler = std::move(handler);
}

} // namespace slopsmith::sandbox
