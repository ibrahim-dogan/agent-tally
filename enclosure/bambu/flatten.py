#!/usr/bin/env python3
"""Resolve Bambu Studio profile inheritance into flat JSON files for the CLI.

    python3 flatten.py <profiles/BBL dir> <out dir> <profile.json>...

A profile may be a system preset name ("Bambu PLA Basic @BBL A1M") or a local
JSON file that "inherits" from one and overrides a few keys.
"""

import json
import os
import sys

KINDS = ("machine", "process", "filament")


def index(root):
    idx = {}
    for kind in KINDS:
        d = os.path.join(root, kind)
        for f in os.listdir(d):
            if f.endswith(".json"):
                with open(os.path.join(d, f)) as fh:
                    try:
                        data = json.load(fh)
                    except ValueError:
                        continue
                idx[data.get("name", f[:-5])] = data
    return idx


def resolve(data, idx, depth=0):
    parent = data.get("inherits")
    if not parent:
        return dict(data)
    if parent not in idx or depth > 20:
        sys.exit(f"cannot resolve parent preset: {parent}")
    flat = resolve(idx[parent], idx, depth + 1)
    flat.update({k: v for k, v in data.items() if k != "inherits"})
    flat.pop("inherits", None)
    return flat


def main():
    root, out = sys.argv[1], sys.argv[2]
    idx = index(root)
    os.makedirs(out, exist_ok=True)
    for spec in sys.argv[3:]:
        if os.path.exists(spec):
            with open(spec) as fh:
                data = json.load(fh)
        elif spec in idx:
            data = idx[spec]
        else:
            sys.exit(f"unknown preset: {spec}")
        flat = resolve(data, idx)
        flat["from"] = "system" if spec in idx else "User"
        name = flat["name"]
        with open(os.path.join(out, name + ".json"), "w") as fh:
            json.dump(flat, fh, indent=1)
        print(os.path.join(out, name + ".json"))


if __name__ == "__main__":
    main()
