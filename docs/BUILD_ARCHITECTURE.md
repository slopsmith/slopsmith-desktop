# Build system architecture

How Slopsmith Desktop is built — the flow of scripts, config, and
packaging across local dev and CI.

## Philosophy

1. **npm scripts are the API.** Every build step is callable via
   `npm run <name>`. CI, local dev, and the DevContainer all use the
   same entry points.
2. **Shell scripts implement.** Complex bundling lives in
   `scripts/bundle-*.sh`; npm just invokes them.
3. **Configuration is centralized.** Tool versions + external pins
   live in `.build-config.json`; per-OS system deps in `.packages/`.
4. **Reproducibility is pinned, not floating.** External dependencies
   (Rocksmith2014.NET, FluidR3/GeneralUser soundfonts, fluidsynth
   Windows zip) are tied to specific commits or SHA256s; CI and local
   see the same bytes.

## High-level flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Build environments                          │
│                                                                     │
│   GitHub Actions      DevContainer (Docker)     Developer CLI       │
│        │                     │                       │              │
│        └─────────────────────┼───────────────────────┘              │
│                              │                                      │
│                              ▼                                      │
│                    ┌─────────────────────┐                          │
│                    │     npm scripts     │                          │
│                    │    (package.json)   │  ← single-source entry   │
│                    └─────────────────────┘                          │
│                              │                                      │
│           ┌──────────────────┼──────────────────┐                   │
│           ▼                  ▼                  ▼                   │
│    ┌────────────┐    ┌──────────────┐    ┌────────────┐             │
│    │build:native│    │    bundle    │    │  build:ts  │             │
│    │            │    │              │    │            │             │
│    │• build:audio│   │• bundle:     │    │   (tsc)    │             │
│    │• build:rscli│   │  slopsmith   │    │            │             │
│    └────────────┘    │• bundle:     │    └────────────┘             │
│                      │  python      │                               │
│                      │• bundle:     │                               │
│                      │  binaries    │                               │
│                      │• bundle:     │                               │
│                      │  soundfont   │                               │
│                      └──────────────┘                               │
│                              │                                      │
│                              ▼                                      │
│                    ┌─────────────────────┐                          │
│                    │  electron-builder   │                          │
│                    │  (dist:linux etc.)  │                          │
│                    └─────────────────────┘                          │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                       Configuration files                           │
│                                                                     │
│  .build-config.json                 .packages/                      │
│  ├── versions                       ├── apt.txt    (Ubuntu/Debian)  │
│  │   ├── node                       ├── brew.txt   (macOS)          │
│  │   ├── python                     ├── choco.txt  (Windows)        │
│  │   ├── dotnet                     └── README.md                   │
│  │   ├── electron                                                   │
│  │   ├── cmake                                                      │
│  │   └── ubuntu                                                     │
│  └── external                                                       │
│      ├── rs2014net           (commit pin)                           │
│      ├── fluidsynth_windows  (version + URL)                        │
│      └── soundfont_general_user (URL + SHA256)                      │
└─────────────────────────────────────────────────────────────────────┘
```

## Detailed flow for `npm run dist:linux`

```
npm run dist:linux
  └── npm run dist -- --linux
       └── npm run bundle && npm run build:ts && electron-builder --linux
            └── bundle
                 ├── bundle-slopsmith.sh  — server.py + lib + static + plugins
                 ├── bundle-python.sh     — portable Python 3.12 runtime + pip + app deps
                 ├── bundle-binaries.sh   — ffmpeg + vgmstream-cli + fluidsynth + its .so chain
                 └── bundle-soundfont.sh  — GeneralUser-GS.sf2 (SHA256-verified)
            └── build:ts
                 └── tsc
            └── electron-builder
                 └── AppImage + .deb
```

For `build:native` (run separately, not part of `dist`):

```
npm run build:native
  ├── build:audio    — cmake-js builds the JUCE C++ native addon
  └── build:rscli    — dotnet publish of Rocksmith2014.NET's RsCli at pinned commit
```

## Scripts directory

```
scripts/
├── bundle.sh                 — top-level Linux delegator (→ bundle-*.sh)
├── bundle-slopsmith.sh       — copy server + plugins
├── bundle-python.sh          — portable Python runtime (Linux)
├── bundle-binaries.sh        — ffmpeg + vgmstream + fluidsynth + libs (Linux)
├── bundle-soundfont.sh       — GeneralUser-GS.sf2 download + verify (cross-platform)
├── build-rscli.sh            — .NET RsCli build with pinned commit
├── build-linux-release.sh    — Docker wrapper for reproducible Linux AppImage
├── parse-build-config.py     — JSON value extractor
└── setup-dev.sh              — local prerequisite checker
```

### Why modular

- **Composability**: debug or re-run a single step (`npm run bundle:python`)
- **Readability**: each script does one thing and has a single reason to change
- **CI/local parity**: CI calls the same scripts where platform-independent; platform-specific inline bash stays in `build.yml` for macOS/Windows (dylibbundler, zip downloads) only because those tools don't fit the Linux shell-script model

### Platform scope per script

| Script | Linux | macOS | Windows |
|---|---|---|---|
| `bundle.sh` | yes | n/a | n/a |
| `bundle-slopsmith.sh` | yes | yes (untested) | yes (untested) |
| `bundle-python.sh` | yes | no (CI inline) | no (CI inline) |
| `bundle-binaries.sh` | yes | no (CI uses dylibbundler inline) | no (CI downloads zips) |
| `bundle-soundfont.sh` | yes | yes | yes |
| `build-rscli.sh` | yes | yes | yes |

## Configuration

### `.build-config.json`

Tool versions pinned once, read by the CI workflow (via `node -p`) and
by the DevContainer (via `parse-build-config.py`). External pins
(Rocksmith2014.NET commit, soundfont SHA256, fluidsynth Windows zip
URL) live under `external.*`.

Update tool versions in one place; every consumer picks them up.

### `.packages/`

Platform system-dep lists. One package per line; `#` comments and
blank lines are filtered by consumers. See `.packages/README.md` for
manual install snippets.

### Electron version

One source of truth: `devDependencies.electron` in `package.json`.
`build:audio` derives `--runtime-version` via
`node -p "require('electron/package.json').version"` so cmake-js
rebuilds the native addon against the currently-installed Electron.

## CI vs local parity

| Aspect | GitHub Actions | DevContainer |
|---|---|---|
| Entry | `npm run dist:linux` (via checkout + setup-* + install) | `npm run dist:linux` (inside container) |
| Node / Python / .NET | From `.build-config.json` | From `.build-config.json` |
| System deps | `.packages/apt.txt` | `.packages/apt.txt` |
| Slopsmith source | Cloned fresh to `$RUNNER_TEMP` | Bind-mounted from `../slopsmith` |
| Python bundle | `scripts/bundle-python.sh` | `scripts/bundle-python.sh` |
| Binary bundle | Inline bash (ffmpeg / vgmstream / fluidsynth lib chain) | `scripts/bundle-binaries.sh` (invoked by `npm run bundle`) |
| Soundfont bundle | `scripts/bundle-soundfont.sh` | `scripts/bundle-soundfont.sh` |
| Smoke test | `fluidsynth --version` + `ffmpeg -version` + `vgmstream-cli --help` | (run manually) |

## Adding a new build step

1. Create a script in `scripts/` (e.g. `bundle-foo.sh`)
2. Register an npm script in `package.json` (e.g. `"bundle:foo": "bash scripts/bundle-foo.sh"`)
3. Wire into the appropriate chain (`bundle`, `build:native`, etc.)
4. Test locally: `npm run bundle:foo`
5. Open a PR — CI consumes the same npm scripts

## Troubleshooting

### Run individual steps

```bash
npm run bundle:python        # rebuild just the Python runtime
npm run bundle:binaries      # rebuild just the binary chain
npm run bundle:soundfont     # re-verify the soundfont
```

### Read the current config

```bash
python3 scripts/parse-build-config.py .build-config.json
python3 scripts/parse-build-config.py .build-config.json .versions.electron
```

### Local vs CI divergence

If local and CI builds differ:

1. `.build-config.json` — pinned versions identical?
2. `.packages/apt.txt` — are you running in the DevContainer?
3. The CI workflow inline-bundles some things differently on
   macOS/Windows (dylibbundler, zip downloads). Local linux dev won't
   reproduce those.

## See also

- [`../.build-config.json`](../.build-config.json)
- [`../.packages/`](../.packages/)
- [`../.devcontainer/`](../.devcontainer/)
- [`../.github/workflows/build.yml`](../.github/workflows/build.yml)
