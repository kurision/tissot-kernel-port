#!/usr/bin/env python3
"""Gate the in-tree BORE scheduler on the upstream patch it was checked against.

Upstream BORE (https://github.com/firelzrd/bore-scheduler) is released as one
patch per kernel version under patches/stable/. Our BORE code is that patch
carried by hand on top of ACK, so an LTS merge that touches the scheduler can
leave it out of step with what upstream ships for the new base.

  record <patch>  Check that every line <patch> adds is present in the tree,
                  then write porting/tools/bore.sync: the patch name and the
                  sha256 of each file the patch touches.
  check           Re-hash those files and fail if any changed since the
                  last record.

A failed check means the scheduler changed under BORE. Compare the tree with
the upstream patch for this kernel series, port whatever it changed, and
record again.
"""

import hashlib
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SYNC = os.path.join(REPO, "porting", "tools", "bore.sync")
UPSTREAM = "https://github.com/firelzrd/bore-scheduler/tree/main/patches/stable"


def sha256(path):
    with open(os.path.join(REPO, path), "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def parse_patch(patch):
    """Return {file: [non-empty added lines, stripped]} for a git patch."""
    added = {}
    cur = None
    with open(patch, errors="replace") as f:
        for line in f:
            if line.startswith("diff --git "):
                cur = line.split()[3][2:]
                added.setdefault(cur, [])
            elif cur and line.startswith("+") and not line.startswith("+++"):
                text = line[1:].strip()
                if text:
                    added[cur].append(text)
    return added


def record(patch):
    added = parse_patch(patch)
    if not added:
        sys.exit(f"bore-sync: {patch}: no file diffs found")

    bad = False
    for path, lines in sorted(added.items()):
        try:
            with open(os.path.join(REPO, path), errors="replace") as f:
                tree = {l.strip() for l in f}
        except FileNotFoundError:
            print(f"bore-sync: {path}: missing from the tree", file=sys.stderr)
            bad = True
            continue
        missing = [l for l in lines if l not in tree]
        for l in missing:
            print(f"bore-sync: {path}: missing: {l}", file=sys.stderr)
        bad |= bool(missing)
    if bad:
        sys.exit("bore-sync: the tree does not carry this patch; not recorded")

    with open(SYNC, "w") as f:
        f.write("# Written by porting/tools/bore-sync.py record; do not edit.\n")
        f.write(f"patch {os.path.basename(patch)}\n")
        for path in sorted(added):
            f.write(f"{sha256(path)}  {path}\n")
    print(f"bore-sync: recorded {os.path.basename(patch)}, {len(added)} files")


def check():
    try:
        with open(SYNC) as f:
            entries = [l.split() for l in f if l.strip() and not l.startswith("#")]
    except FileNotFoundError:
        sys.exit(f"bore-sync: {SYNC} missing; run: bore-sync.py record <patch>")

    patch = next((e[1] for e in entries if e[0] == "patch"), "?")
    changed = []
    for digest, path in (e for e in entries if e[0] != "patch"):
        try:
            if sha256(path) != digest:
                changed.append(path)
        except FileNotFoundError:
            changed.append(path + " (removed)")

    if not changed:
        print(f"bore-sync: scheduler files unchanged since {patch}")
        return

    print(f"bore-sync: files BORE patches changed since it was checked against {patch}:",
          file=sys.stderr)
    for path in changed:
        print(f"  {path}", file=sys.stderr)
    print(f"bore-sync: compare with the upstream patch for this kernel ({UPSTREAM}),\n"
          "bore-sync: port its changes, then run: porting/tools/bore-sync.py record <patch>",
          file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "record":
        record(sys.argv[2])
    elif len(sys.argv) == 2 and sys.argv[1] == "check":
        check()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
