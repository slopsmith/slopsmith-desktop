# Windows Build Requirements

This document describes the dependencies and setup required to build Slopsmith Desktop on Windows.

## Required Software

### .NET SDK 10.0
- **Version**: 10.0.203 or later
- **Purpose**: Build RsCli (Rocksmith 2014 CLI tool for PSARC operations)
- **Install**: `winget install Microsoft.DotNet.SDK.10`
- **Note**: GitHub Actions Windows runners have .NET 10.0 Preview SDK pre-installed

### Visual Studio Build Tools 2022
- **Version**: 17.14 or later
- **Purpose**: Compile native C++ audio engine and Node.js native addons
- **Install**: `winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`
- **Required Components**: Desktop development with C++ workload

### CMake
- **Version**: 3.22 or later
- **Purpose**: Build system for native audio engine
- **Install**: `winget install Kitware.CMake`
- **Note**: Add to PATH: `C:\Program Files\CMake\bin`

### Git (for plugin cloning)
- **Purpose**: `clone_slopsmith` in `scripts/build-common.sh` clones the Slopsmith repo and every plugin via plain `git clone`. Git for Windows ships with Git Bash, which is required anyway to run the build scripts.
- **Install**: `winget install Git.Git` (you almost certainly already have this).
- **Optional (private plugins only)**: If any clones target private repos you have access to, configure git credentials (e.g. `git config --global credential.helper manager` plus a one-time push/clone to cache a PAT). The build does NOT use the GitHub CLI.

### Chocolatey (optional)
- **Purpose**: Package manager for cmake and ffmpeg (can also be downloaded directly)
- **Install**: See https://chocolatey.org/install

## Windows-Specific Build Notes

### Python Embeddable Distribution
The Windows build uses Python's embeddable distribution, which has a special configuration:

1. **`.pth` file isolation**: The Python embeddable distribution uses a `python312._pth` file that enables "isolated mode". In this mode, the `PYTHONPATH` environment variable is **completely ignored**.

2. **Solution**: Slopsmith paths must be added directly to the `._pth` file. This is handled automatically by `scripts/build-windows.sh`, but if you need to add custom paths:
   ```
   # In python312._pth (relative to resources/python):
   ../slopsmith
   ../slopsmith/lib
   ```

### Git authentication for private plugins
If your build pulls private plugin repos (e.g. `byrongamatos/slopsmith-plugin-cf`), make sure git can authenticate non-interactively before running the build — Git Credential Manager + a PAT or SSH key both work. The build uses `git clone --depth 1` for every plugin and does NOT call the GitHub CLI.

## Quick Start

```bash
# Install dependencies (Git for Windows brings Git Bash which the scripts run under)
winget install Microsoft.DotNet.SDK.10 Microsoft.VisualStudio.2022.BuildTools Kitware.CMake Git.Git

# Add CMake to PATH (if not automatic)
export PATH="$PATH:/c/Program Files/CMake/bin"

# Run the build
bash scripts/build-windows.sh
```

## Common Issues

### "No .NET SDK found"
- Install .NET SDK 10.0: `winget install Microsoft.DotNet.SDK.10`
- Verify: `dotnet --list-sdks` should show 10.0.203 or later

### "CMake is not installed"
- Install via winget: `winget install Kitware.CMake`
- Or add existing installation to PATH: `export PATH="$PATH:/c/Program Files/CMake/bin"`

### "ModuleNotFoundError: No module named 'psarc'"
- This indicates the Python .pth file is misconfigured
- The build script should add the paths automatically
- Check that `resources/python/python312._pth` contains:
  ```
  ../slopsmith
  ../slopsmith/lib
  ```

### "Repository not found" during plugin cloning
- Configure git itself for authentication (the build uses plain `git clone`, not the GitHub CLI):
  - Git Credential Manager + a Personal Access Token: `git config --global credential.helper manager`
  - Or an SSH key registered on your account
- Verify you can manually `git clone` the failing plugin URL from Git Bash before retrying the build
- Confirm the plugin repository exists on GitHub and your account has access to any private repos referenced in `scripts/build-common.sh`'s clone list