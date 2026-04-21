# Platform Package Lists

This directory contains the system packages required to build Slopsmith Desktop on each platform.

## Purpose

These files are the single source of truth for system dependencies across:
- **Local development** (via `.devcontainer/`)
- **GitHub Actions CI** (via `.github/workflows/build.yml`)
- **Manual installation** (for contributors)

## Files

| File | Platform | Package Manager |
|------|----------|-----------------|
| `apt.txt` | Ubuntu/Debian | apt |
| `brew.txt` | macOS | Homebrew |
| `choco.txt` | Windows | Chocolatey |

## Format

Each file contains one package per line. Lines starting with `#` are comments.

Example:
```
# Audio libraries
libasound2-dev
libjack-jackd2-dev

# Build tools
cmake
```

## Usage

### GitHub Actions
The workflow reads these files and installs packages automatically:
```bash
PACKAGES=$(grep -v '^[[:space:]]*#' .packages/apt.txt | grep -v '^[[:space:]]*$' | tr '\n' ' ')
sudo apt-get install -y $PACKAGES
```

### DevContainer
The Dockerfile copies `.packages/` and installs from `apt.txt`.

### Manual Installation
Install packages appropriate for your platform:

**Ubuntu/Debian:**
```bash
xargs -a .packages/apt.txt sudo apt-get install -y
```

**macOS:**
```bash
xargs -a .packages/brew.txt brew install
```

**Windows (with Chocolatey):**
```powershell
Get-Content .packages/choco.txt | Where-Object { $_ -notmatch '^#' -and $_ -ne '' } | ForEach-Object { choco install $_ }
```

## Notes

- **Linux (apt)**: The DevContainer and GitHub Actions both use Ubuntu 22.04 LTS
- **macOS (brew)**: Package versions may vary based on Homebrew's current state
- **Windows (choco)**: Some packages (like `cmake`) require special install arguments in CI

## Updating Packages

When adding new system dependencies:

1. Add the package to the appropriate `.packages/*.txt` file
2. Update **all** platform files if the package exists across platforms
3. Test locally with the DevContainer
4. Submit a PR - CI will validate the changes

Remember: Changes here affect both local builds and CI, so test thoroughly!
