# Bundled ML models

## basic_pitch.onnx — Spotify Basic Pitch (polyphonic note detection)

The note model used by `MlNoteDetector` (`src/audio/MlNoteDetector.cpp`) for
TST-style polyphonic note detection.

- **Source:** the `basic-pitch` PyPI package, v0.4.0 —
  `basic_pitch/saved_models/icassp_2022/nmp.onnx`, copied here verbatim.
- **Upstream:** Spotify Basic Pitch — https://github.com/spotify/basic-pitch
- **License:** Apache-2.0 (see the upstream `LICENSE`).
- **SHA-256:** `2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec`
- **I/O contract:** input `[batch, 43844, 1]` float32 (~2 s mono @ 22050 Hz);
  outputs onset / note / contour posteriorgrams. See
  `tests/spike/README.md` for the verified contract.

Bundled into the packaged app via `electron-builder`'s `extraResources`
(Constitution IV — offline first-run). When the file is absent the engine
falls back to the YIN `PitchDetector` / `ChordScorer` (Constitution VII).
