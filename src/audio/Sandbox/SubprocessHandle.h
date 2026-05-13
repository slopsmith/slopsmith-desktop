// SubprocessHandle — owns a slopsmith-vst-host.exe subprocess and observes its
// lifetime. Spawn via `start()`; the destructor performs a graceful close
// (terminate-after-timeout) if the process is still running.

#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace slopsmith::sandbox {

class SubprocessHandle
{
public:
    SubprocessHandle();
    ~SubprocessHandle();

    // Spawn the subprocess. `args` are the command-line arguments (the exe
    // path is implicit; callers pass it in `exePath`). The exit watcher
    // thread reports unexpected exits via `onExit`.
    bool start(const juce::String& exePath,
               const juce::StringArray& args,
               std::function<void(int exitCode)> onExit,
               juce::String& errorOut);

    // Send a graceful close: post WM_QUIT to the subprocess; if it doesn't
    // exit within `timeoutMs`, terminate.
    void shutdown(int timeoutMs);

    bool isRunning() const noexcept { return running.load(std::memory_order_acquire); }
    // Windows DWORD is unsigned 32-bit; storing as int would silently
    // narrow a high PID into a negative value when surfaced via pid().
    uint32_t pid() const noexcept { return cachedPid; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<bool> running{false};
    uint32_t cachedPid = 0;
    std::function<void(int)> onExitCb;
    std::thread watcher;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubprocessHandle)
};

} // namespace slopsmith::sandbox
