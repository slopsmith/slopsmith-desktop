#!/bin/bash
# Download + verify the default General-MIDI soundfont into
# resources/soundfonts/. Used by both CI and local `npm run bundle` on
# all three platforms.
#
# URL + SHA256 are read from .build-config.json (external.soundfont_general_user).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$PROJECT_DIR/.build-config.json"

SF_URL=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" .external.soundfont_general_user.url)
SF_SHA256=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" .external.soundfont_general_user.sha256)
SF_DIR="$PROJECT_DIR/resources/soundfonts"
SF_FILE="$SF_DIR/GeneralUser-GS.sf2"

mkdir -p "$SF_DIR"

if [ -f "$SF_FILE" ]; then
    echo "  Existing soundfont found — verifying checksum"
else
    echo "  Downloading GeneralUser-GS.sf2 (~32 MB) from $SF_URL"
    curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors "$SF_URL" -o "$SF_FILE"
fi

# macOS ships `shasum -a 256`; Linux / Windows-git-bash ship `sha256sum`.
if command -v sha256sum >/dev/null 2>&1; then
    echo "${SF_SHA256}  ${SF_FILE}" | sha256sum -c - >/dev/null
else
    echo "${SF_SHA256}  ${SF_FILE}" | shasum -a 256 -c - >/dev/null
fi

echo "  Soundfont: $(ls -lh "$SF_FILE" | awk '{print $5}') (SHA256 verified)"
