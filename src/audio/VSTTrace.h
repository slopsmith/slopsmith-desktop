// Diagnostic logger for VST3 host ↔ plugin handshake.
//
// Defined in a header so it's reachable from both the addon
// (NodeAddon.cpp, VSTHost.cpp) and the vendored JUCE VST3 host context
// (juce_VST3PluginFormatImpl.h). Writes every call to a file + stderr
// with immediate flush so a process abort (e.g. `__fastfail` from a
// crashy plugin) doesn't lose the last few lines.
//
// Compiled into all builds, but no-op at runtime unless
// SLOPSMITH_SANDBOX_DEBUG is set to any non-empty value other than "0"
// when the addon loads. The first call caches the env var, so flipping
// it mid-process has no effect.

#pragma once

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace slopsmith_vst_trace {

// Gate on SLOPSMITH_SANDBOX_DEBUG=1 so release builds don't litter %TEMP%
// with the trace file or burn stderr cycles on every host callback.
inline bool isEnabled()
{
    static const bool v = [] {
        const char* s = std::getenv("SLOPSMITH_SANDBOX_DEBUG");
        return s && *s && std::strcmp(s, "0") != 0;
    }();
    return v;
}

inline std::FILE* logFile()
{
    static std::FILE* f = []() -> std::FILE* {
        if (!isEnabled()) return nullptr;
        char path[1024] = {0};
#if defined(_WIN32)
        // Prefer %TEMP%; fall back to %USERPROFILE% (per-PID log path in
        // src/vst-host/main.cpp does the same — non-elevated users can't
        // write to C:\ root, and "trace gated on env var but silently
        // creates no file" is the worst-of-both-worlds failure mode).
        DWORD n = GetEnvironmentVariableA("TEMP", path, sizeof(path));
        if (n == 0 || n >= sizeof(path)) {
            n = GetEnvironmentVariableA("USERPROFILE", path, sizeof(path));
        }
        if (n > 0 && n < sizeof(path)) {
            // Per-PID suffix: this header is compiled into both the addon
            // and the sandbox host, so concurrent runs would otherwise
            // interleave traces in one file and make sandbox debugging
            // (host A spawning host B) hard to correlate. Mirrors the
            // per-PID naming the sandbox-host log already uses.
            char suffix[64]{};
            const int suffixLen = std::snprintf(
                suffix, sizeof(suffix), "\\slopsmith-vst-trace-%lu.log",
                (unsigned long)GetCurrentProcessId());
            if (suffixLen > 0
                && (size_t)n + (size_t)suffixLen < sizeof(path))
            {
                std::strncat(path, suffix, sizeof(path) - n - 1);
            }
        }
        // If both env vars failed, leave `path` empty — writing to the
        // drive root requires admin on a default Windows install and the
        // sandbox host's per-PID log made the same decision. fopen(nullptr-
        // equivalent path) returns NULL, which the rest of the code handles
        // cleanly.
#else
        std::snprintf(path, sizeof(path), "/tmp/slopsmith-vst-trace.log");
#endif
        std::FILE* fp = path[0] ? std::fopen(path, "a") : nullptr;
        if (fp) {
            std::fprintf(fp, "\n========== slopsmith-vst-trace opened (pid=%lu) ==========\n",
                         (unsigned long)
#if defined(_WIN32)
                         GetCurrentProcessId()
#else
                         (unsigned long) getpid()
#endif
                        );
            std::fflush(fp);
        }
        return fp;
    }();
    return f;
}

inline std::mutex& logMutex()
{
    static std::mutex m;
    return m;
}

inline void writef(const char* fmt, ...)
{
    if (!isEnabled()) return;
    std::lock_guard<std::mutex> lock(logMutex());

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    auto* fp = logFile();
    if (fp) {
        std::fputs(buf, fp);
        std::fputc('\n', fp);
        std::fflush(fp);
    }
    std::fputs("[vst-trace] ", stderr);
    std::fputs(buf, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// Format 16 hex bytes of a TUID. The Steinberg TUID type is `char[16]`.
// Rotates through a 4-slot ring of thread-local buffers so multiple tuidHex
// arguments in a single VST_TRACE call don't clobber each other before the
// formatter consumes them.
inline const char* tuidHex(const void* tuid)
{
    static thread_local char ring[4][40];
    static thread_local unsigned idx = 0;
    char* out = ring[idx++ & 3u];
    const unsigned char* b = static_cast<const unsigned char*>(tuid);
    std::snprintf(out, sizeof(ring[0]),
                  "%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3],   b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

} // namespace slopsmith_vst_trace

// Check the enabled flag at the call site so disabled builds don't pay for
// formatting arg evaluation (e.g. `tuidHex(...)`, `cmd.toRawUTF8()`).
#define VST_TRACE(...)                                                       \
    do {                                                                     \
        if (::slopsmith_vst_trace::isEnabled())                              \
            ::slopsmith_vst_trace::writef(__VA_ARGS__);                      \
    } while (0)
