#include "ControlChannel.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #error "ControlChannel.cpp is Windows-only for now. Linux/macOS impls land with PRs #2 and #3."
#endif

#include <thread>

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
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
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
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        errorOut = "CreateFileW (client) failed: " + juce::String((int)GetLastError());
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(h, &mode, nullptr, nullptr))
    {
        errorOut = "SetNamedPipeHandleState failed: "
                 + juce::String((int)GetLastError());
        CloseHandle(h);
        return false;
    }
    impl->pipe = h;
    impl->isServer = false;
    return true;
}

bool ControlChannel::start(EventCallback evCb,
                            std::function<void(const juce::String&)> disconnectCb)
{
    onEvent = std::move(evCb);
    onDisconnect = std::move(disconnectCb);
    alive.store(true, std::memory_order_release);

    // On the server side, block waiting for the client to connect *before*
    // starting the read loop. ConnectNamedPipe returns immediately if the
    // client is already connected (ERROR_PIPE_CONNECTED), so both cases work.
    if (impl->isServer)
    {
        if (!ConnectNamedPipe(impl->pipe, nullptr))
        {
            DWORD err = GetLastError();
            if (err != ERROR_PIPE_CONNECTED)
            {
                alive.store(false, std::memory_order_release);
                return false;
            }
        }
    }

    ioThread = std::thread([this] { ioLoop(); });
    return true;
}

void ControlChannel::stop()
{
    bool wasAlive = alive.exchange(false, std::memory_order_acq_rel);
    if (impl && impl->pipe != INVALID_HANDLE_VALUE)
    {
        // CancelIoEx unblocks the read on the I/O thread.
        CancelIoEx(impl->pipe, nullptr);
        CloseHandle(impl->pipe);
        impl->pipe = INVALID_HANDLE_VALUE;
    }
    if (ioThread.joinable())
    {
        if (std::this_thread::get_id() == ioThread.get_id())
            ioThread.detach();
        else
            ioThread.join();
    }

    // Fail any in-flight requests so callers don't hang.
    std::lock_guard<std::mutex> lk(pendingMutex);
    for (auto& [id, p] : pending)
    {
        try { p->promise.set_value({}); }
        catch (const std::future_error&) {}
    }
    pending.clear();
    (void)wasAlive;
}

bool ControlChannel::writeFrame(const juce::MemoryBlock& body)
{
    std::lock_guard<std::mutex> lk(writeMutex);
    if (impl->pipe == INVALID_HANDLE_VALUE) return false;
    if (body.getSize() > kMaxControlMessageBytes) return false;

    uint32_t lenLE = (uint32_t)body.getSize();
    DWORD written = 0;
    if (!WriteFile(impl->pipe, &lenLE, sizeof(lenLE), &written, nullptr)
        || written != sizeof(lenLE))
        return false;
    if (body.getSize() > 0)
    {
        if (!WriteFile(impl->pipe, body.getData(), (DWORD)body.getSize(),
                       &written, nullptr) || written != body.getSize())
            return false;
    }
    return true;
}

bool ControlChannel::readFrame(juce::MemoryBlock& out)
{
    if (impl->pipe == INVALID_HANDLE_VALUE) return false;
    uint32_t lenLE = 0;
    DWORD got = 0;
    if (!ReadFile(impl->pipe, &lenLE, sizeof(lenLE), &got, nullptr)
        || got != sizeof(lenLE))
        return false;
    if (lenLE > kMaxControlMessageBytes) return false;
    out.setSize(lenLE, false);
    if (lenLE == 0) return true;
    if (!ReadFile(impl->pipe, out.getData(), lenLE, &got, nullptr) || got != lenLE)
        return false;
    return true;
}

void ControlChannel::ioLoop()
{
    juce::MemoryBlock frame;
    while (alive.load(std::memory_order_acquire))
    {
        if (!readFrame(frame))
        {
            failWith(kReasonReadError);
            return;
        }

        juce::String parseError;
        auto msg = wire::decode(frame.getData(), frame.getSize(), &parseError);
        if (!msg.isObject())
        {
            failWith(kReasonProtocolError + ": " + parseError);
            return;
        }

        // Reply ({id, ok, result/error}) vs event ({event, data}) vs request
        // ({id, op, args}). Dispatch by structure.
        if (msg.hasProperty("event"))
        {
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
