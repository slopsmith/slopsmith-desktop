#include "SubprocessHandle.h"
#include "../VSTTrace.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #error "SubprocessHandle.cpp is Windows-only for now."
#endif

namespace slopsmith::sandbox {

struct SubprocessHandle::Impl
{
    PROCESS_INFORMATION pi{};
};

SubprocessHandle::SubprocessHandle() : impl(std::make_unique<Impl>()) {}

SubprocessHandle::~SubprocessHandle()
{
    shutdown(2000);
}

bool SubprocessHandle::start(const juce::String& exePath,
                              const juce::StringArray& args,
                              std::function<void(int)> onExit,
                              juce::String& errorOut)
{
    // Refuse to re-spawn over a still-running process — overwriting impl->pi
    // would leak the existing process/thread handles, and reassigning a
    // joinable std::thread calls std::terminate.
    if (running.load(std::memory_order_acquire) || watcher.joinable())
    {
        errorOut = "subprocess already running — call shutdown() first";
        return false;
    }

    // Build a properly-quoted command line: each arg quoted; the exe path also
    // quoted so spaces in install paths work.
    juce::String cmd;
    cmd << "\"" << exePath << "\"";
    for (auto& a : args)
        cmd << " \"" << a.replace("\"", "\\\"") << "\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // detach console; the sandbox is GUI-only

    std::wstring wcmd = cmd.toWideCharPointer();
    VST_TRACE("SubprocessHandle.start: CreateProcessW cmd='%s'", cmd.toRawUTF8());
    if (!CreateProcessW(
            nullptr, wcmd.data(),
            nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            nullptr, nullptr,
            &si, &impl->pi))
    {
        DWORD err = GetLastError();
        errorOut = "CreateProcessW failed: " + juce::String((int)err);
        VST_TRACE("SubprocessHandle.start: CreateProcessW FAILED err=%lu", (unsigned long)err);
        return false;
    }
    VST_TRACE("SubprocessHandle.start: spawned pid=%lu",
              (unsigned long)impl->pi.dwProcessId);
    running.store(true, std::memory_order_release);
    cachedPid = (uint32_t)impl->pi.dwProcessId;
    onExitCb = std::move(onExit);

    HANDLE procHandle = impl->pi.hProcess;
    watcher = std::thread([this, procHandle]
    {
        WaitForSingleObject(procHandle, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(procHandle, &code);
        running.store(false, std::memory_order_release);
        if (onExitCb) onExitCb((int)code);
    });
    return true;
}

void SubprocessHandle::shutdown(int timeoutMs)
{
    if (running.load(std::memory_order_acquire))
    {
        // Try a clean shutdown: post WM_QUIT to the subprocess's initial
        // thread (`pi.dwThreadId` from PROCESS_INFORMATION). PostThreadMessageW
        // is per-TID, not per-process — this works because vst-host's WinMain
        // runs the JUCE message loop on the initial thread (the audio worker
        // is a child thread that doesn't pump messages), so WM_QUIT lands on
        // the right pump. If a future refactor moves the message loop off the
        // initial thread, this needs the new TID or to switch to a
        // process-wide signalling mechanism (named event, etc.). If
        // dwThreadId is zero (start() failed mid-way) we skip and let the
        // wait+TerminateProcess below clean up.
        if (impl->pi.dwThreadId != 0)
            PostThreadMessageW(impl->pi.dwThreadId, WM_QUIT, 0, 0);

        DWORD wait = WaitForSingleObject(impl->pi.hProcess, (DWORD)timeoutMs);
        if (wait != WAIT_OBJECT_0)
            TerminateProcess(impl->pi.hProcess, 1);
    }

    if (watcher.joinable())
    {
        if (std::this_thread::get_id() == watcher.get_id())
            watcher.detach();
        else
            watcher.join();
    }

    // Always close handles — when the watcher detected a crash, `running` is
    // already false here, but the kernel handles are still ours to release.
    if (impl->pi.hThread)  { CloseHandle(impl->pi.hThread);  impl->pi.hThread = nullptr; }
    if (impl->pi.hProcess) { CloseHandle(impl->pi.hProcess); impl->pi.hProcess = nullptr; }
}

} // namespace slopsmith::sandbox
