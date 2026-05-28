#!/bin/bash

# Pre-install bundled plugins' Python dependencies into the embedded Python
# at build time and emit resources/bundled_plugin_reqs.json so the slopsmith
# server skips its first-run pip install (which has historically taken
# 20-30 minutes for sloppak_converter — torch + demucs + whisperx, ~1 GB).
#
# Slopsmith contract (plugins/__init__.py::_install_requirements):
#   If SLOPSMITH_BUNDLED_PLUGIN_MANIFEST points at a JSON file mapping
#   {plugin_id: sha256(requirements.txt)} and the entry matches the plugin's
#   on-disk requirements.txt, _install_requirements returns True immediately
#   — no pip, no sys.path insert. Imports resolve from the embedded
#   site-packages this script populated.
#
# Must run AFTER clone_slopsmith (plugin sources present at $SLOPSMITH_DIR)
# and AFTER the platform's bundle_python_impl (pip available in the embedded
# runtime).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

if [[ -z "${SLOPSMITH_DIR:-}" ]] || [[ ! -d "$SLOPSMITH_DIR/plugins" ]]; then
    echo "ERROR: SLOPSMITH_DIR not set or missing plugins/ — clone_slopsmith must run first." >&2
    exit 1
fi

# Resolve the embedded Python the platform just bundled. Layout:
#   Linux / macOS (python-build-standalone): resources/python/runtime/bin/python3
#   Windows (embeddable zip):                 resources/python/python.exe
PYTHON_BUNDLE_ROOT="$PROJECT_DIR/resources/python"
PYTHON_EXTRA_ENV=()
if [[ -x "$PYTHON_BUNDLE_ROOT/runtime/bin/python3" ]]; then
    PYTHON_BIN="$PYTHON_BUNDLE_ROOT/runtime/bin/python3"
    # PBS bundles its libpython next to the interpreter; the runtime needs
    # this on the dynamic loader path for any module that links against it.
    PYTHON_EXTRA_ENV=(LD_LIBRARY_PATH="$PYTHON_BUNDLE_ROOT/runtime/lib")
elif [[ -x "$PYTHON_BUNDLE_ROOT/python.exe" ]]; then
    PYTHON_BIN="$PYTHON_BUNDLE_ROOT/python.exe"
else
    echo "ERROR: embedded Python not found under $PYTHON_BUNDLE_ROOT — bundle_python must run first." >&2
    exit 1
fi

run_python() {
    if [[ ${#PYTHON_EXTRA_ENV[@]} -gt 0 ]]; then
        env "${PYTHON_EXTRA_ENV[@]}" "$PYTHON_BIN" "$@"
    else
        "$PYTHON_BIN" "$@"
    fi
}

echo "=== Pre-installing bundled plugin requirements ==="
echo "  Python:   $PYTHON_BIN"
echo "  Plugins:  $SLOPSMITH_DIR/plugins"

MANIFEST_PATH="$PROJECT_DIR/resources/bundled_plugin_reqs.json"
mkdir -p "$(dirname "$MANIFEST_PATH")"

# Export the two paths the heredoc reads. clone_slopsmith already exports
# SLOPSMITH_DIR; re-exporting is a no-op but defends against this script
# being invoked standalone for local debugging.
export SLOPSMITH_DIR MANIFEST_PATH

# Build manifest + run pip installs. Done in Python because:
#   - jq isn't available on every build runner (Windows in particular).
#   - shelling out per plugin would re-resolve transitive deps repeatedly;
#     a single batched install lets pip's resolver see the full graph.
# We still record per-plugin sha256 so each plugin's marker check is keyed
# to its own requirements.txt (one plugin's reqs changing doesn't
# invalidate another plugin's manifest hit).
PYTHONPATH="" run_python - <<PYEOF
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

slopsmith_dir = Path(os.environ["SLOPSMITH_DIR"])
plugins_root = slopsmith_dir / "plugins"
manifest_path = Path(os.environ["MANIFEST_PATH"])

manifest: dict[str, str] = {}
combined_reqs: list[str] = []
seen_lines: set[str] = set()
plugin_summaries: list[tuple[str, str, int]] = []

for plugin_dir in sorted(plugins_root.iterdir()):
    if not plugin_dir.is_dir():
        continue
    if plugin_dir.name in ("__pycache__",):
        continue
    req_file = plugin_dir / "requirements.txt"
    if not req_file.exists():
        continue
    manifest_json = plugin_dir / "plugin.json"
    if not manifest_json.exists():
        # No plugin.json — skip. Slopsmith's loader won't load it either.
        continue
    try:
        meta = json.loads(manifest_json.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        print(f"  skip {plugin_dir.name}: malformed plugin.json ({e})", file=sys.stderr)
        continue
    plugin_id = meta.get("id")
    if not isinstance(plugin_id, str) or not plugin_id:
        print(f"  skip {plugin_dir.name}: missing/invalid 'id' in plugin.json", file=sys.stderr)
        continue

    req_bytes = req_file.read_bytes()
    sha = hashlib.sha256(req_bytes).hexdigest()
    manifest[plugin_id] = sha

    # Collapse identical requirement lines across plugins so pip resolves
    # the union once. Lines are preserved verbatim, including pip directives
    # like '--extra-index-url https://download.pytorch.org/whl/cpu' that
    # sloppak_converter needs for the CPU torch wheel.
    for raw in req_bytes.decode("utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line not in seen_lines:
            seen_lines.add(line)
            combined_reqs.append(line)
    plugin_summaries.append((plugin_id, sha[:8], len(req_bytes)))

manifest_path.parent.mkdir(parents=True, exist_ok=True)
manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

print(f"  Manifested {len(manifest)} plugin(s) with requirements.txt:")
for plugin_id, short_sha, size in plugin_summaries:
    print(f"    - {plugin_id}  sha256={short_sha}…  reqs={size}B")

if not combined_reqs:
    print("  No plugin requirements to install — skipping pip.")
    sys.exit(0)

import tempfile
with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as tf:
    for line in combined_reqs:
        tf.write(line + "\n")
    combined_path = tf.name

print(f"  Installing {len(combined_reqs)} unique requirement line(s) into embedded Python...")
# --no-cache-dir matches how bundle-python.sh installs slopsmith's base
# requirements — keeps the build cache off the runner's disk and prevents
# stale wheel-cache reuse across CI re-runs.
cmd = [sys.executable, "-m", "pip", "install", "--no-cache-dir", "-r", combined_path]
result = subprocess.run(cmd, check=False)
os.unlink(combined_path)
if result.returncode != 0:
    print(f"ERROR: pip install failed (exit {result.returncode})", file=sys.stderr)
    sys.exit(result.returncode)
print("  Bundled plugin requirements installed.")
PYEOF

echo "  Manifest written: $MANIFEST_PATH"
echo "=== Plugin dependency bundle complete ==="
