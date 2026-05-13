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

    HANDLE h = CreateNamedPipeW(
        pipeName.toWideCharPointer(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
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
    // Reassigning a joinable std::thread aborts via std::terminate, so refuse
    // a second start. Callers should stop() then re-create the channel.
    if (ioThread.joinable() || alive.load(std::memory_order_acquire))
        return false;
    if (!impl || impl->pipe == INVALID_HANDLE_VALUE)
        return false;

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
    alive.store(false, std::memory_order_release);

    // CancelIoEx unblocks the I/O thread's pending read so it can exit. The
    // handle must stay valid until the thread has returned — closing it
    // first is a TOCTOU on the in-flight read.
    if (impl && impl->pipe != INVALID_HANDLE_VALUE)
        CancelIoEx(impl->pipe, nullptr);

    if (ioThread.joinable())
    {
        if (std::this_thread::get_id() == ioThread.get_id())
            ioThread.detach();
        else
            ioThread.join();
    }

    if (impl && impl->pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(impl->pipe);
        impl->pipe = INVALID_HANDLE_VALUE;
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

// Helper for synchronous overlapped I/O — issues the operation, waits for it
// up to `timeoutMs`, and on timeout cancels the pending I/O so the caller
// reliably returns. INFINITE is the default for reads, which must wait for
// the peer; writes pass a finite timeout so a stalled reader can't pin the
// caller indefinitely.
static bool overlappedTransfer(HANDLE pipe, bool isWrite, void* buf,
                               DWORD bytesPerOp, DWORD timeoutMs = INFINITE)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr)
    {
        // Out of handle quota / kernel resources. GetLastError() is preserved
        // for the caller — no synthetic code to mask the real reason.
        return false;
    }
    BOOL ok = isWrite
        ? WriteFile(pipe, buf, bytesPerOp, nullptr, &ov)
        : ReadFile (pipe, buf, bytesPerOp, nullptr, &ov);
    DWORD lastErr = ok ? 0 : GetLastError();
    if (!ok && lastErr != ERROR_IO_PENDING)
    {
        CloseHandle(ov.hEvent);
        SetLastError(lastErr);
        return false;
    }
    if (timeoutMs != INFINITE)
    {
        // WaitForSingleObject + bounded timeout, then either complete or
        // cancel. CancelIoEx + a final GetOverlappedResult(TRUE) is the
        // documented pattern that lets us safely CloseHandle the event
        // without leaving the kernel mid-operation.
        const DWORD wait = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (wait != WAIT_OBJECT_0)
        {
            CancelIoEx(pipe, &ov);
            DWORD drained = 0;
            GetOverlappedResult(pipe, &ov, &drained, TRUE);
            CloseHandle(ov.hEvent);
            SetLastError(WAIT_TIMEOUT);
            return false;
        }
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe, &ov, &transferred, TRUE))
    {
        DWORD err = GetLastError();
        CloseHandle(ov.hEvent);
        SetLastError(err);
        return false;
    }
    CloseHandle(ov.hEvent);
    return transferred == bytesPerOp;
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
        CTL_TRACE("readFrame: oversized frame len=%lu", (unsigned long)lenLE);
        return false;
    }
    out.setSize(lenLE, false);
    if (lenLE == 0) return true;
    if (!overlappedTransfer(impl->pipe, false, out.getData(), (DWORD)lenLE))
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
            CTL_TRACE("decode failed: %s; first bytes: %.32s",
                      parseError.toRawUTF8(), (const char*)frame.getData());
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
            if (requestHandler)
            {
                int id = (int)msg.getProperty("id", -1);
                requestHandler(id, msg["op"].toString(), msg["args"]);
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
    auto cb = onDisconnect;
    if (cb) cb(reason);
    // Fail any in-flight requests.
    std::lock_guard<std::mutex> lk(pendingMutex);
    for (auto& [id, p] : pending)
    {
        try { p->promise.set_value({}); } catch (...) {}
    }
    pending.clear();
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
    requestHandler = std::move(handler);
}

} // namespace slopsmith::sandbox
