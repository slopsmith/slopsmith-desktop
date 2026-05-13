# VST trace — runtime-gated diagnostic logging

`src/audio/VSTTrace.h` defines a `VST_TRACE(...)` macro the addon, the VST
host code, and the sandbox subprocess all use to emit lines into
`%TEMP%\slopsmith-vst-trace.log` (Linux/macOS: `/tmp/slopsmith-vst-trace.log`)
and stderr. It's compiled into every build but no-ops at runtime unless the
`SLOPSMITH_SANDBOX_DEBUG` environment variable is set to a non-empty value
other than `"0"`.

The first call caches the env var, so flipping the variable mid-process has
no effect — set it before launching the host process (`node ...`,
`electron ...`, or the sandbox subprocess via the parent's environment).

## What you'll see when it's enabled

* `[ctrl] ...` — control-channel framing from `ControlChannel.cpp`
  (`ConnectNamedPipe`, `readFrame got N bytes`, `event: ready`, error codes).
* Sandbox subprocess startup steps from `slopsmith-vst-host.exe`:
  `args ok`, `audio shm opened`, `control pipe connected`,
  `plugin loaded: <name>`, `sending ready event`.
* `LoadVST: path='...'`, `SubprocessHandle.start: spawned pid=N`, the full
  CreateProcess command line.
* `VSTHost.loadPlugin / VST3ComponentHolder.initialise` host-callback traces
  from the JUCE VST3 host context (in-process load path only).

The sandbox host also opens a per-PID file at
`%TEMP%\slopsmith-vst-host-<pid>.log` **unconditionally** — this is by
design and intentionally not gated on `SLOPSMITH_SANDBOX_DEBUG`. The
sandbox subprocess runs hidden (no console window) and can die before
the env var has propagated, so an always-on per-PID file is the only
reliable way to diagnose "the subprocess died and I have no console"
crashes in the field. The file is small (a handful of lines per session),
written from a single process, and rotates per PID so they cap naturally.

## Turning it on

```cmd
:: Windows — set before launching node / electron / the desktop app:
set SLOPSMITH_SANDBOX_DEBUG=1
node load-gr6.js
```

```bash
# macOS / Linux:
SLOPSMITH_SANDBOX_DEBUG=1 node load-gr6.js
```

## Reading the log

```cmd
:: Windows
type %TEMP%\slopsmith-vst-trace.log
```

```bash
# macOS / Linux
cat /tmp/slopsmith-vst-trace.log
```

The file is appended to across runs; truncate it (or `del`) between sessions
when bisecting.
