# GR6 fast-fail — PoC results + revised diagnosis

Date: 2026-05-13
Branch: `diag/vst-trace` (unchanged) + new scratch dir `C:\Users\byron\vst-poc\` on Win11 VM
PoC binary: `C:\Users\byron\vst-poc\build\Release\vst_poc.exe`

## TL;DR — the original diagnosis was half right

The original `DIAG-REPORT.md` proposed Fix Path 1 ("sandboxed plugin-host process with
QApplication"). The PoC confirms **the sandboxed-process direction is correct**, but
**QApplication is not needed at all** — the entire root cause is JUCE's message thread
not being the OS main thread.

| Mode | Process | JUCE MessageManager thread | QApplication? | GR6 editor opens? |
|---|---|---|---|---|
| Baseline | `node load-gr6.js` | JUCE-internal (TID ≠ main) | no | **crashes** in 1–2 s with `0xC0000409` |
| PoC #1 | `vst_poc.exe` | OS main thread (TID == main) | yes (Qt 5.15.2) | **clean**, editor 1110×780, runs 10 s, exit 0 |
| PoC #2 | `vst_poc.exe POC_NO_QAPP=1` | OS main thread (TID == main) | **no** | **clean**, editor 1110×780, runs 10 s, exit 0 |

PoC #2 is the load-bearing experiment: dropping QApplication entirely still works as long
as JUCE owns the OS main thread. The "no QCoreApplication instance" Qt warnings the
original report flagged don't even fire in PoC #2 — GR6's static-linked Qt5 is happy as
long as the calling thread has the OS-main-thread properties it expects.

## How the PoC reproduces / refutes the bug

```cpp
// main.cpp (essentials)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // OPTIONAL — POC_NO_QAPP env disables this leg. Same outcome either way.
    QApplication qapp(qArgc, qArgv);

    juce::ScopedJuceInitialiser_GUI juceInit;        // (a) MessageManager bound to TID=WinMain
    VSTHost host;
    auto plugin = host.loadPlugin(GR6_PATH, 48000.0, 256, err);
    auto editor = plugin->createEditor();             // (b) runs on the same TID
    editor->setSize(1000, 600);
    EditorWindow window(...); window.setVisible(true);

    while (alive) {                                   // (c) message pump on main thread
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    }
}
```

The three pieces above are necessary and sufficient. Compare to today's Slopsmith addon:

```cpp
// src/audio/NodeAddon.cpp::OpenPluginEditor
juce::MessageManager::callAsync([processor, ...]() {
    auto* editor = processor->createEditor();         // runs on JUCE's worker thread,
    ...                                                 // which is NOT the V8/main thread
});
```

Inside a Node native addon, V8 owns the OS main thread; JUCE silently spins up its own
"message thread" via `MessageManager::startup()` to give itself a place to run UI work.
That thread has a Win32 message queue but it is not a `QThread` and is not the OS main
thread. GR6's embedded Qt5 starts timers and connects QML signals on whatever thread
created its editor, hits Qt's thread-affinity guards (`Timers can only be used with
threads started with QThread`, `Illegal attempt to connect ... different thread than the
QML engine`), then `__fastfail`s.

## Revised recommended fix

`slopsmith-vst-host.exe` — a sandbox-process plugin host. **Same shape as the PoC,
without the Qt link.** Concretely:

1. **Subprocess binary**: standalone Win32 exe with `WinMain`. Loads exactly one VST3 via
   the existing JUCE `VSTHost`. Runs JUCE's MessageManager on its main thread, opens the
   editor on its main thread, owns the editor HWND.
2. **No QApplication, no Qt link**: PoC #2 proves it isn't needed for GR6. Drop the
   ~50 MB of Qt deps the original report assumed.
3. **IPC for audio + control**: shared-memory ring buffer for audio, named pipe for
   control messages (load, setParameter, openEditor, etc.). Same surface as the current
   in-process `VSTHost`, so `SignalChain` doesn't have to know.
4. **Detection**: only route through the sandbox for plugins that fail in-process.
   The PoC plus the Qt warnings in the JUCE-thread case strongly suggest any Qt-using
   plugin (Reaktor 6, Massive X, Komplete Kontrol, Battery 4, etc.) has the same issue;
   non-Qt plugins (most JUCE-based ones) are fine in-process.

## What is dropped vs. the original plan

- **Qt dependency entirely.** ~2 weeks of "make Qt cohabit with JUCE" work doesn't happen
  because we don't need Qt.
- The "Qt main loop must own the message pump" anxiety in the original §"Why fixing this
  in-process is hard" doesn't apply — Win32 message pump (drained by JUCE's
  `runDispatchLoopUntil`) is enough.
- The original cost estimate of ~2 weeks for single-plugin sandbox / ~1 month for pooled
  is still roughly right, dominated by the IPC layer, not Qt.

## Followups

- **Editor window reparenting**: the production sandbox needs to reparent the plugin's
  HWND into the Electron renderer (or pop a separate top-level window owned by Electron).
  Standard pattern — `SetParent` from the sandbox after sending the HWND over IPC.
- **Audio latency budget**: one extra IPC hop adds ~1–2 ms at 48 kHz/256 frames. Fine
  for guitar processing; reconsider for low-latency monitoring. Use a shared-memory ring
  + futex-style signalling (Windows events) to avoid syscall round-trips per buffer.
- **Crash isolation**: a plugin in the sandbox crashing only takes down its own process;
  Slopsmith Desktop can detect, log, restart. Bigger win than just fixing GR6.

## Artifacts

- PoC source: `/tmp/win11-srv/vst-poc/` on host, `C:\Users\byron\vst-poc\` on VM
  - `main.cpp` — single-file PoC (~150 lines)
  - `CMakeLists.txt` — pulls JUCE from slopsmith-desktop, optional Qt
  - `build.bat`, `run.bat` — convenience wrappers
- Build artefact: `C:\Users\byron\vst-poc\build\Release\vst_poc.exe`
- Run logs: `C:\Users\byron\vst-poc\poc-run.log` (last run's stderr)
- VST trace: `%TEMP%\slopsmith-vst-trace.log` on the VM
- Qt 5.15.2 install (for the Qt-on leg): `C:\Qt\5.15.2\msvc2019_64\` — can be deleted
  once the sandbox is implemented without Qt.

## Reproduce

```
# Baseline (still crashes today, confirmed 2026-05-13):
cd C:\Users\byron\slopsmith-desktop
node load-gr6.js

# PoC, with QApplication (clean):
cd C:\Users\byron\vst-poc
run.bat 1

# PoC, no QApplication (also clean — load-bearing experiment):
run.bat 0
```
