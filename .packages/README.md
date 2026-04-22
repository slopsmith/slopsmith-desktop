# Platform package lists

System packages required to build Slopsmith Desktop on each OS.

## Purpose

Single source of truth for system dependencies across:
- **Local development** (via `.devcontainer/`)
- **GitHub Actions CI** (`.github/workflows/build.yml`)
- **Manual installation** by contributors

## Files

| File | Platform | Package manager |
|---|---|---|
| `apt.txt` | Ubuntu / Debian | apt |
| `brew.txt` | macOS | Homebrew |
| `choco.txt` | Windows | Chocolatey |

## Format

One package per line. Lines starting with `#` and blank lines are
ignored. Consumers must strip those before piping to the package
manager.

## Consuming the lists

### Shared filter snippet

These lists contain comments and blank lines; a raw `xargs -a` will
pass those to the package manager verbatim and fail (or worse, try to
install `#` as a package). Always filter first:

```bash
grep -v '^[[:space:]]*#' .packages/apt.txt | grep -v '^[[:space:]]*$'
```

### GitHub Actions (Linux)

```yaml
- name: Install Linux dependencies
  run: |
    sudo apt-get update
    PACKAGES=$(grep -v '^[[:space:]]*#' .packages/apt.txt | grep -v '^[[:space:]]*$' | tr '\n' ' ')
    sudo apt-get install -y $PACKAGES
```

### GitHub Actions (macOS)

```yaml
- name: Install macOS dependencies
  run: |
    PACKAGES=$(grep -v '^[[:space:]]*#' .packages/brew.txt | grep -v '^[[:space:]]*$' | tr '\n' ' ')
    brew install $PACKAGES
```

### Manual install (Ubuntu / Debian)

```bash
grep -v '^[[:space:]]*#' .packages/apt.txt | grep -v '^[[:space:]]*$' | xargs sudo apt-get install -y
```

### Manual install (macOS)

```bash
grep -v '^[[:space:]]*#' .packages/brew.txt | grep -v '^[[:space:]]*$' | xargs brew install
```

### Manual install (Windows, PowerShell)

```powershell
Get-Content .packages/choco.txt | Where-Object { $_ -notmatch '^\s*#' -and $_ -notmatch '^\s*$' } | ForEach-Object { choco install $_ -y }
```

## Updating

When adding a system dependency:

1. Add the package to the appropriate `.packages/*.txt`
2. Update every OS's file if the package exists for all three
3. Test locally via the DevContainer where possible
4. Open a PR and let CI validate

Changes here affect both local builds and CI, so verify end-to-end
before merging.
