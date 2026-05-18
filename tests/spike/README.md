# Phase 0 de-risk spike — polyphonic ML note detection

**Throwaway.** Not part of the addon build. Proves Spotify Basic Pitch runs
under ONNX Runtime's C++ API before the real engine integration begins.

## Provenance

- **Model:** `nmp.onnx` from the `basic-pitch` PyPI package, v0.4.0
  (`basic_pitch/saved_models/icassp_2022/nmp.onnx`, 230 KB).
  Spotify Basic Pitch, **Apache-2.0**. The package ships a clean ONNX export —
  no `tf2onnx` conversion needed.
- **ONNX Runtime:** v1.20.1, official prebuilt CPU release.
  - Linux x64: `onnxruntime-linux-x64-1.20.1.tgz`
    SHA-256 `67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105`
  - URL pattern: `https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-<os-arch>-1.20.1.<ext>`

## Build & run

```sh
cmake -B build -DONNXRUNTIME_ROOT=/path/to/onnxruntime-linux-x64-1.20.1
cmake --build build
./build/spike /path/to/nmp.onnx test_guitar.wav
```

`test_guitar.wav` is a 48 kHz synthetic Karplus-Strong guitar clip: single
notes A2 / D3 / G3 at t≈0.3/1.3/2.3 s, then a C-major triad (C3+E3+G3) at
t≈3.3 s.

## Model I/O contract (verified)

- **Input** `serving_default_input_2:0` — `[batch, 43844, 1]` float32.
  43844 = 22050·2 − 256, a ~2 s mono window at **22050 Hz**.
- **Outputs** (3 posteriorgrams, ~86 frames/s, 172 frames/window):
  - `StatefulPartitionedCall:1` — **note/frame** `[batch, 172, 88]`
  - `StatefulPartitionedCall:2` — **onset** `[batch, 172, 88]`
  - `StatefulPartitionedCall:0` — **contour** `[batch, 172, 264]` (unused)
- 88 pitches = MIDI 21..108 (pitch index `p` → MIDI `21 + p`).

## Post-processing (minimal slice ported to C++)

A note onset = a rising edge of the onset posteriorgram past 0.5, gated by the
frame posteriorgram past 0.3:
`onset[f,p] ≥ 0.5 && onset[f-1,p] < 0.5 && note[f,p] ≥ 0.3`.
This is all the live hit/miss path needs — no full offline note-event
reconstruction.

## Findings

- **Accuracy:** all 4 events detected at the correct MIDI and time; the C-major
  triad resolved polyphonically (C3+E3+G3). Zero false positives in the C++ run.
- **Latency:** 33 ms/window inference, single-threaded
  (`IntraOpNumThreads=1`), ONNX Runtime 1.20.1, CPU EP. With a 64 ms hop,
  end-to-end detection latency (hop + inference + model onset lag) lands
  ≈100–150 ms — the ≤150 ms target is reachable; a 128 ms hop trades latency
  (~180–200 ms) for lower CPU.
- **Window boundaries:** non-overlapping ~2 s windows can re-onset a sustained
  note at a window edge. The production `MlNoteDetector` avoids this with a
  rolling 22050 Hz buffer, reading only the freshest frames each hop.
- **Resampling:** the spike uses Catmull-Rom cubic interpolation for
  48000→22050; the production detector will use `juce::LagrangeInterpolator`.

**Conclusion: de-risked. Model + ONNX Runtime C++ work; output is
interpretable. Proceed to Phase 1.**
