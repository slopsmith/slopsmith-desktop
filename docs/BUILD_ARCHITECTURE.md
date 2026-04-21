# Build System Architecture

This document describes how the Slopsmith Desktop build system works, focusing on how we maintain a single source of truth for build processes across different environments.

## Philosophy

The build system follows these principles:

1. **npm scripts are the API**: `package.json` scripts are the single source of truth for build steps
2. **Shell scripts implement**: Complex operations are implemented in shell scripts called by npm
3. **CI and local use the same paths**: GitHub Actions and local builds both invoke npm scripts
4. **Configuration is centralized**: Tool versions and package lists are defined in shared configuration files

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         Build Environments                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   GitHub Actions      Local Dev Container       Developer CLI           │
│        │                      │                      │                  │
│        │                      │                      │                  │
│        └──────────────────────┼──────────────────────┘                  │
│                               │                                         │
│                               ▼                                         │
│                    ┌─────────────────────┐                              │
│                    │   npm scripts       │  ← Single source of truth    │
│                    │   (package.json)    │    for build steps           │
│                    └─────────────────────┘                              │
│                               │                                         │
│           ┌───────────────────┼───────────────────┐                     │
│           │                   │                   │                     │
│           ▼                   ▼                   ▼                     │
│    ┌──────────────┐   ┌──────────────┐   ┌──────────────┐               │
│    │ build:native │   │    bundle    │   │   build:ts   │               │
│    │              │   │              │   │              │               │
│    │ • build:audio│   │ • bundle:    │   │   (tsc)      │               │
│    │ • build:rscli│   │   slopsmith  │   │              │               │
│    └──────────────┘   │ • bundle:    │   └──────────────┘               │
│                       │   python     │                                  │
│                       │ • bundle:    │                                  │
│                       │   binaries   │                                  │
│                       └──────────────┘                                  │
│                               │                                         │
│                               ▼                                         │
│                    ┌─────────────────────┐                              │
│                    │  electron-builder   │                              │
│                    │  (dist:linux, etc.) │                              │
│                    └─────────────────────┘                              │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                         Configuration Files                             │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   .build-config.json        .packages/                                  │
│   ├── versions              ├── apt.txt      (Ubuntu/Debian)            │
│   │   ├── node              ├── brew.txt     (macOS)                    │
│   │   ├── python            └── choco.txt    (Windows)                  │
│   │   ├── dotnet                                                        │
│   │   ├── electron                                                      │
│   │   ├── cmake                                                         │
│   │   └── ubuntu                                                        │
│   └── (future: other configs)                                           │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

## Build Steps Explained

### High-Level Flow (`npm run dist:linux`)

```bash
npm run dist:linux
  └── npm run dist -- --linux
       └── npm run build:native && npm run bundle && npm run build:ts && electron-builder --linux
```

### Detailed Breakdown

1. **`npm run build:native`**
   - Builds the native components required at runtime
   - Runs: `npm run build:audio && npm run build:rscli`
   - **Audio engine**: Compiles JUCE C++ code into Node.js native addon
   - **RsCli**: Builds .NET tool for Rocksmith 2014 file operations

2. **`npm run bundle`**
   - Bundles resources into `resources/` directory
   - Runs: `npm run bundle:slopsmith && npm run bundle:python && npm run bundle:binaries`
   - **Slopsmith**: Copies server code and plugins
   - **Python**: Creates portable Python environment with dependencies
   - **Binaries**: Copies system tools (ffmpeg, vgmstream-cli)

3. **`npm run build:ts`**
   - Compiles TypeScript to JavaScript
   - Runs: `tsc`

4. **`electron-builder --linux`**
   - Packages everything into distributable format
   - Creates `.AppImage` and `.deb` files

## Script Organization

### Scripts Directory

```
scripts/
├── build-audio.sh          # Build JUCE audio engine native addon
├── build-rscli.sh          # Build .NET Rocksmith2014.NET CLI tool
├── build-linux-release.sh  # Docker wrapper for reproducible builds
├── bundle-slopsmith.sh     # Bundle Slopsmith server and plugins
├── bundle-python.sh        # Create portable Python environment
├── bundle-binaries.sh      # Copy system binaries
├── bundle.sh               # Legacy monolithic bundle (deprecated)
└── setup-dev.sh            # Development environment setup
```

### Why This Structure?

- **Modularity**: Each script does one thing well
- **Composability**: Can run individual steps for debugging
- **Testability**: Easier to test smaller scripts
- **Documentation**: Script names describe what they do

## Configuration Management

### Centralized Configuration

All build tools reference these files:

**`.build-config.json`**
- Language versions (Node, Python, .NET, Electron)
- Build tool versions (CMake)
- Base OS version (Ubuntu LTS)

**`.packages/`**
- System packages per platform
- One file per package manager (apt, brew, choco)

### Benefits

1. **Single source of truth**: Change one file, affects all environments
2. **Version consistency**: CI and local use identical versions
3. **Easy updates**: Bump versions in one place
4. **Self-documenting**: Files describe what's installed

## Comparison: CI vs Local

| Aspect | GitHub Actions | Local Dev Container |
|--------|----------------|---------------------|
| **Entry point** | `npm run dist:linux` | `npm run dist:linux` |
| **Node version** | From `.build-config.json` | From `.build-config.json` |
| **Python version** | From `.build-config.json` | From `.build-config.json` |
| **System packages** | From `.packages/apt.txt` | From `.packages/apt.txt` |
| **Build steps** | npm scripts | npm scripts |
| **Slopsmith source** | Cloned fresh | Mounted from `../slopsmith` |

## Adding New Build Steps

To add a new step to the build process:

1. **Create a script** in `scripts/` (e.g., `scripts/build-foo.sh`)
2. **Add npm script** in `package.json` (e.g., `"build:foo": "bash scripts/build-foo.sh"`)
3. **Integrate into dependency chain** (e.g., add to `build:native` or `bundle`)
4. **Test locally** with `npm run <script>`
5. **CI automatically picks it up** (uses same npm scripts)

## Troubleshooting

### Debug Individual Steps

```bash
# Run just the audio build
npm run build:audio

# Run just the bundle
npm run bundle

# Run just one bundle component
npm run bundle:python
```

### Check Configuration

```bash
# View build config
cat .build-config.json | jq .

# View package list
cat .packages/apt.txt
```

### Reproducible Build Issues

If local and CI builds differ:

1. Check `.build-config.json` is the same
2. Check `.packages/apt.txt` is the same
3. Compare Docker base image version
4. Compare `npm run build:native` output
5. Compare `npm run bundle` output

## See Also

- `../.build-config.json` - Build version configuration
- `../.packages/` - System package lists
- `../.devcontainer/` - Local reproducible build environment
- `../.github/workflows/build.yml` - CI workflow
