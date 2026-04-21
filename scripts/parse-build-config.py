#!/usr/bin/env python3
"""
Parse .build-config.jsonc (JSON with Comments) and output clean JSON.
Used by build scripts that need to read the central build configuration.

Usage:
  parse-build-config.py /path/to/.build-config.jsonc
  parse-build-config.py /path/to/.build-config.jsonc .versions.node
  parse-build-config.py /path/to/.build-config.jsonc .versions.python
"""

import json
import re
import sys


def strip_jsonc_comments(text):
    """Remove C-style comments from JSONC text."""
    # Remove single-line comments (// to end of line)
    text = re.sub(r'//.*$', '', text, flags=re.MULTILINE)
    # Remove multi-line block comments (/* ... */)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    return text


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path-to-jsonc> [json-path]", file=sys.stderr)
        print(f"Example: {sys.argv[0]} .build-config.jsonc .versions.node", file=sys.stderr)
        sys.exit(1)

    config_file = sys.argv[1]
    json_path = sys.argv[2] if len(sys.argv) > 2 else None

    try:
        with open(config_file, 'r') as f:
            content = f.read()

        # Strip comments
        clean_content = strip_jsonc_comments(content)

        # Parse JSON
        data = json.loads(clean_content)

        # Extract value if path provided, else output full JSON
        if json_path:
            # Path like ".versions.node"
            keys = json_path.lstrip('.').split('.')
            value = data
            for key in keys:
                value = value[key]
            print(value)
        else:
            # Output clean JSON
            print(json.dumps(data, indent=2))

    except FileNotFoundError:
        print(f"Error: File not found: {config_file}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: Invalid JSON in {config_file}: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyError as e:
        print(f"Error: Key not found: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
