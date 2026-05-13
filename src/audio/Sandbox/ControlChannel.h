// ControlChannel — request/response + event messaging between the host
// (Slopsmith Desktop) and a sandbox subprocess.
//
// Transport: Windows named pipe in byte mode (PIPE_TYPE_BYTE |
// PIPE_READMODE_BYTE) with an explicit `[u32 length-LE][body]` framing
// layer. PIPE_TYPE_MESSAGE was tried first but dropped because the
// sandbox's `ready` frame wasn't being delivered to the host I/O thread
// reliably — see commit 2cb9ae9. Posix transport TBD when the macOS /
// Linux sandbox PRs land.
//
// Threading model: one I/O thread inside the channel reads frames and dispatches
// them to the event callback or to the matching pending request future. Callers
// invoke `request()` from arbitrary threads.

#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "Protocol.h"

namespace slopsmith::sandbox {

class ControlChannel
{
public:
    // Async callback fired for each sandbox-originated event. Invoked from the
    // channel's internal I/O thread; the callback must not block.
    using EventCallback = std::function<void(const juce::String& event,
                                             const juce::var& data)>;

    // Sentinel reason strings passed to the disconnect callback.
    static const juce::String kReasonPeerClosed;
    static const juce::String kReasonReadError;
    static const juce::String kReasonProtocolError;

    ControlChannel();
    ~ControlChannel();

    // Host-side: create a uniquely-named pipe in CONNECT (server) mode and
    // return the pipe name. The sandbox subprocess will connect to it shortly
    // after spawn.
    bool createServerSide(juce::String& pipeNameOut, juce::String& errorOut);

    // Sandbox-side: connect to a pipe created by the host. Used by the
    // slopsmith-vst-host subprocess. The host build also compiles this code
    // (it lives in the same translation unit), but the addon doesn't call it.
    bool connectClientSide(const juce::String& pipeName, juce::String& errorOut);

    // Start the background I/O thread. Must be called after either
    // createServerSide() or connectClientSide().
    bool start(EventCallback onEvent,
               std::function<void(const juce::String& reason)> onDisconnect);

    void stop();
    bool isAlive() const noexcept { return alive.load(std::memory_order_acquire); }

    // Synchronous request/response. Returns the parsed result `juce::var`, or
    // an undefined `var` on timeout/error (with the reason in `errorOut`).
    juce::var request(const char* op, const juce::var& args,
                      int timeoutMs, juce::String* errorOut = nullptr);

    // Fire-and-forget: no reply expected. Used for high-frequency messages
    // like MIDI events and parameter automation.
    bool postNoReply(const char* op, const juce::var& args);

    // Sandbox-side helpers: send a reply to the host's request, or originate
    // an event.
    bool sendReply(int requestId, bool ok, const juce::var& result,
                   const juce::String& errorMessage = {});
    bool sendEvent(const char* eventName, const juce::var& data);

    // Sandbox-side: when the channel parses an inbound request, the consumer
    // installs a request handler. MUST be called BEFORE start() — the I/O
    // thread reads `requestHandler` on every inbound request, and the
    // member is intentionally not synchronised. Installing after start()
    // races the read.
    using RequestHandler =
        std::function<void(int requestId, const juce::String& op,
                           const juce::var& args)>;
    void setRequestHandler(RequestHandler handler);

private:
    struct Pending
    {
        std::promise<juce::var> promise;
    };

    bool writeFrame(const juce::MemoryBlock& body);
    bool readFrame(juce::MemoryBlock& out);
    void ioLoop();
    void failWith(const juce::String& reason);

    // Set by readFrame() before it returns false so ioLoop can classify the
    // disconnect (ERROR_BROKEN_PIPE / ERROR_PIPE_NOT_CONNECTED / ERROR_NO_DATA
    // → kReasonPeerClosed, anything else → kReasonReadError). Only the I/O
    // thread writes or reads this field.
    unsigned long lastReadError = 0;

    struct Impl;
    std::unique_ptr<Impl> impl; // OS-specific handle wrapper

    std::atomic<bool> alive{false};
    std::atomic<int> nextRequestId{1};

    EventCallback onEvent;
    std::function<void(const juce::String& reason)> onDisconnect;
    RequestHandler requestHandler;

    std::mutex pendingMutex;
    std::unordered_map<int, std::shared_ptr<Pending>> pending;

    std::mutex writeMutex;     // serialises outbound writes
    std::thread ioThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ControlChannel)
};

} // namespace slopsmith::sandbox
