# Third-Party Notices

Slopsmith Desktop bundles, links to, or fetches at build time the
components listed below. Each entry names the upstream project, the
license it is distributed under, and where the corresponding source or
license text lives — either in this repository, in an installed
artefact, or upstream.

The desktop binary as a whole is distributed under the GNU Affero
General Public License v3.0 (see [`LICENSE`](LICENSE)) because the JUCE
framework — statically linked into the audio engine — is licensed under
AGPL-3.0 when not used under the separate commercial JUCE licence.
AGPL's copyleft propagates through that linkage, which is why the
combined work is AGPL-3.0-only.

Permissive components (MIT, BSD, Apache-2.0, etc.) listed below retain
their own terms; their notices must accompany any redistribution of
this software.

---

## JUCE 8

- **Project:** https://juce.com / https://github.com/juce-framework/JUCE
- **License:** AGPL-3.0-only **OR** the commercial JUCE 8 End User
  Licence Agreement (the upstream `JUCE/LICENSE.md` does not grant the
  "or any later version" option)
- **Use:** Statically linked into the JUCE C++ audio engine
  (`src/audio/`, native addon `slopsmith_audio.node`).
- **Notes:** Slopsmith Desktop is distributed under the AGPL-3.0 arm of
  this dual licence; the commercial JUCE licence is not used.
- **Notice text:** [`JUCE/LICENSE.md`](JUCE/LICENSE.md) (git submodule).

## Electron

- **Project:** https://www.electronjs.org/
- **License:** MIT
- **Use:** Desktop shell. Pinned via `package.json` (`electron`
  devDependency).
- **Notice text:** Distributed with the Electron runtime; redistributed
  inside the installer / app bundle under `LICENSE.electron.txt` (and
  equivalent paths) emitted by `electron-builder`.

## NeuralAmpModelerCore (NAM)

- **Project:** https://github.com/sdatkinson/NeuralAmpModelerCore
- **License:** MIT (Copyright © 2023–2025 Steven Atkinson)
- **Use:** Bundled as a git submodule at
  `src/audio/third_party/NAM/`; compiled into the audio engine to
  power the built-in Neural Amp Modeler.
- **Notice text:**
  [`src/audio/third_party/NAM/LICENSE`](src/audio/third_party/NAM/LICENSE).

## RTNeural

- **Project:** https://github.com/jatinchowdhury18/RTNeural
- **License:** BSD 3-Clause (Copyright © 2020 jatinchowdhury18)
- **Use:** Bundled as a git submodule at
  `src/audio/third_party/RTNeural/`; compiled into the audio engine
  for the real-time NN inference paths used by NAM.
- **Notice text:**
  [`src/audio/third_party/RTNeural/LICENSE`](src/audio/third_party/RTNeural/LICENSE).

## Rocksmith2014.NET (rscli)

- **Project:** https://github.com/byrongamatos/Rocksmith2014.NET (fork
  of https://github.com/iminashi/Rocksmith2014.NET)
- **License:** MIT
- **Use:** Built from a pinned upstream commit at packaging time (see
  the `external.rs2014net` block in
  [`.build-config.json`](.build-config.json) and
  [`scripts/build-rscli.sh`](scripts/build-rscli.sh)); the resulting
  `rscli` binary ships under `resources/bin/`.
- **Notice text:** Carried in the upstream repository's `LICENSE`
  file at the pinned commit; redistributed alongside the built binary
  in release packages.

## FluidSynth

- **Project:** https://www.fluidsynth.org / https://github.com/FluidSynth/fluidsynth
- **License:** LGPL-2.1-or-later
- **Use:** Bundled into `resources/bin/` for use by the embedded
  Slopsmith server (Guitar Pro → audio rendering path). On Windows,
  prebuilt binaries are downloaded at packaging time per the
  `external.fluidsynth_windows` block in
  [`.build-config.json`](.build-config.json). On macOS and Linux, the
  build host's `fluidsynth` binary is copied from `$PATH` by
  [`scripts/bundle-binaries.sh`](scripts/bundle-binaries.sh).
  Dynamically linked — users may relink against a compatible
  FluidSynth build per LGPL terms.
- **Notice text:** Included in the upstream FluidSynth release archive
  redistributed inside the installer (Windows) and travelling with the
  host's `fluidsynth` package (macOS/Linux).

## FFmpeg / ffprobe

- **Project:** https://ffmpeg.org / https://github.com/FFmpeg/FFmpeg
- **License:** LGPL-2.1-or-later at minimum; the specific bundled
  builds may be GPL-licensed (typically GPL-2.0-or-later or
  GPL-3.0-or-later) if they were compiled with `--enable-gpl` and
  linked against GPL components such as libx264 / libx265. The
  third-party macOS builds we fetch (osxexperts.net, evermeet.cx) ship
  with GPL components enabled, so on macOS the bundled `ffmpeg`/`ffprobe`
  binaries are effectively under GPL terms. AGPL-3.0-only is compatible
  with redistribution of GPL-2.0-or-later and GPL-3.0-or-later binaries
  (FSF compatibility matrix), and FFmpeg is invoked as a separate
  process — so its license terms do not propagate to the rest of the
  desktop binary.
- **Use:** Required by the embedded Slopsmith server for WAV → OGG
  transcoding on Guitar Pro 5 imports (`ffmpeg`) and for stream
  metadata reads by demucs during stem splitting (`ffprobe`). Bundled
  into `resources/bin/` on all platforms by
  [`scripts/bundle-binaries.sh`](scripts/bundle-binaries.sh):
  - macOS: prebuilt binaries downloaded per the
    `external.ffmpeg_macos_*` / `external.ffprobe_macos_*` blocks in
    [`.build-config.json`](.build-config.json).
  - Linux: copied from the build host's `$PATH` (e.g. apt `ffmpeg`).
  - Windows: copied from the build host's `$PATH` (e.g. Chocolatey /
    Scoop `ffmpeg`).
- **Notice text:** Distributed inside the upstream FFmpeg source /
  release archives (`COPYING.LGPLv2.1`, `COPYING.GPLv2`, etc.); the
  source code is publicly available at the upstream URL above.

## vgmstream-cli

- **Project:** https://github.com/vgmstream/vgmstream
- **License:** ISC-style permissive (see upstream
  [`COPYING`](https://github.com/vgmstream/vgmstream/blob/master/COPYING)).
- **Use:** Required by the embedded Slopsmith server for Rocksmith
  `.wem` → `.wav` decoding. Bundled into `resources/bin/` on all
  platforms by
  [`scripts/bundle-binaries.sh`](scripts/bundle-binaries.sh): copied
  from the build host's `$PATH` when available, or downloaded from the
  upstream
  [GitHub releases](https://github.com/vgmstream/vgmstream/releases/latest)
  as a fallback.
- **Notice text:** Distributed in the upstream release archive and
  source repository.

## GeneralUser GS SoundFont

- **Author:** S. Christian Collins —
  https://www.schristiancollins.com/generaluser
- **License:** Custom permissive licence (free redistribution with
  attribution; no stand-alone resale).
- **Use:** Bundled unmodified as `GeneralUser-GS.sf2` for the
  Guitar Pro → audio rendering path; pinned by SHA-256 per the
  `external.soundfont_general_user` block in
  [`.build-config.json`](.build-config.json).
- **Notice text:**
  [`resources/soundfonts/LICENSE`](resources/soundfonts/LICENSE).

## CPython (embedded Python runtime)

- **Project:** https://www.python.org/
- **License:** Python Software Foundation License (PSF)
- **Use:** A relocatable CPython build from
  [astral-sh/python-build-standalone](https://github.com/astral-sh/python-build-standalone)
  is fetched at packaging time (macOS / Linux) per the
  `python_standalone_*` blocks in
  [`.build-config.json`](.build-config.json) and bundled under
  `resources/python/` so the embedded Slopsmith server can run
  without a system Python install.
- **Notice text:** Distributed with the standalone Python build.

## .NET Runtime (rscli host)

- **Project:** https://github.com/dotnet/runtime
- **License:** MIT
- **Use:** The `rscli` tool is published as a self-contained .NET
  application; the .NET runtime files travel with it under
  `resources/bin/`.
- **Notice text:** Bundled by `dotnet publish` alongside the binary
  (`THIRD-PARTY-NOTICES.TXT`, `LICENSE.TXT`).

## Node.js modules (runtime dependencies)

The Electron main process pulls in two runtime npm dependencies; both
are MIT-licensed and travel inside the packaged app under
`resources/app.asar` (or unpacked, per
[`package.json`](package.json) `build.asarUnpack`):

- **lottie-web** — https://github.com/airbnb/lottie-web — MIT
- **node-addon-api** — https://github.com/nodejs/node-addon-api — MIT

Their full licence texts are present in the corresponding
`node_modules/<package>/LICENSE` files at install time and travel
with the packaged app.

---

## Adding a new dependency

When adding a new dependency, append an entry here describing the
project, its licence, how it's bundled, and where the notice text
lives. Permissive (MIT / BSD / Apache-2.0) and AGPL-compatible
copyleft licences are acceptable; anything more restrictive than
AGPL-3.0 — or any licence that forbids combination with AGPL — must
not be added without first discussing it on the issue tracker.
