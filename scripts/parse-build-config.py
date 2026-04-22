#!/usr/bin/env python3
"""Parse `.build-config.json` and emit a value (or the whole doc).

Usage:
  parse-build-config.py <path>                # pretty-print the whole document
  parse-build-config.py <path> .versions.node # print a single value

Keys are dot-delimited, e.g. `.external.rs2014net.commit`. Plain JSON
only — if a future config needs comments, add a proper JSONC parser
(naive regex-based `//` stripping breaks on URLs like `https://...`).
"""

import json
import sys


def main() -> None:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path> [.json.path]", file=sys.stderr)
        sys.exit(1)

    config_file = sys.argv[1]
    json_path = sys.argv[2] if len(sys.argv) > 2 else None

    try:
        with open(config_file, 'r') as f:
            data = json.load(f)
    except FileNotFoundError:
        print(f"Error: file not found: {config_file}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: invalid JSON in {config_file}: {e}", file=sys.stderr)
        sys.exit(1)

    if json_path is None:
        print(json.dumps(data, indent=2))
        return

    value = data
    for key in json_path.lstrip('.').split('.'):
        try:
            value = value[key]
        except (KeyError, TypeError):
            print(f"Error: key {json_path!r} not found in {config_file}", file=sys.stderr)
            sys.exit(1)
    print(value)


if __name__ == '__main__':
    main()
