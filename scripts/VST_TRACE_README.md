# VST3 host trace — diagnostic build instructions

This branch (`diag/vst-trace`) instruments the JUCE VST3 host context so we
can see exactly what a misbehaving plugin (currently: Guitar Rig 6 v6.x on
Windows 11 24H2) asks the host for right before it `__fastfail`s. Every
host callback the plugin invokes is appended to:

```text
%TEMP%\slopsmith-vst-trace.log
```

with immediate flush, so even a process-abort that bypasses SEH still
leaves the last line on disk.

**Do not merge this branch.** It bloats stderr/log output and patches the
vendored JUCE submodule.

## Windows build (one-shot)

From a *Developer Command Prompt for VS 2022* in the repo root:

```cmd
git checkout diag/vst-trace
git submodule update --init --recursive

REM Apply the JUCE patch (the change can't live in the submodule itself
REM because we don't own its remote)
cd JUCE
git apply ..\scripts\vst-trace.patch
cd ..

npm install
npm run build:audio
```

The build produces `build\Release\slopsmith_audio.node`. Copy that file
over `C:\Program Files\Slopsmith\resources\app.asar.unpacked\build\Release\slopsmith_audio.node`
(closing Slopsmith first), then launch Slopsmith and reproduce the GR6
crash.

Easier alternative if you don't want to touch the installed app: run
the dev build instead.

```cmd
npm run dev
```

That starts Electron pointing at this working tree directly.

## Collecting the log

After Slopsmith crashes:

```cmd
type %TEMP%\slopsmith-vst-trace.log
```

The interesting region is the last ~50 lines before the file ends. Paste
those back to the agent.

## Reverting

```cmd
cd JUCE
git checkout -- modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h
cd ..
git checkout main
```
