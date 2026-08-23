#!/usr/bin/env python3
"""Find persisted .uasset files that are older than the DreamShader sources they are built from.

DreamShader generates in memory inside the editor, but a commandlet run PERSISTS the asset, and a
persisted asset then SHADOWS the in-memory generation. So a `.uasset` on disk is a cache, and like
any cache it can go stale -- except this one goes stale silently and destructively:

    change a .dsf's Outputs -> every already-persisted asset that CALLS it reloads, rebuilds that
    function-call node's pins, and drops the connections it can no longer match. The asset is now
    damaged, and DreamShader's divergence gate then refuses to rebuild it, because from the digest's
    point of view somebody edited it by hand.

That is not hypothetical -- it is what changing MF_MoonToonBaseInput's outputs did to M_MoonToon and
then, unnoticed, to M_MoonToonOutline (2026-08-20).

    python Tools/check_asset_staleness.py

Exit 0 if every persisted asset is at least as new as its whole source closure.

Staleness here is transitive on purpose: M_MoonToonOutline's own .dsm did not change, only the
function it calls did, and that is exactly the case a per-file check misses.
"""
import io
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SRC_DIR = os.path.join(ROOT, "DShader")
CONTENT = os.path.join(ROOT, "Content")


def sources():
    """base name -> source path, for every .dsf / .dsm."""
    out = {}
    for d, _sub, files in os.walk(SRC_DIR):
        for f in files:
            if f.endswith((".dsf", ".dsm")):
                out[os.path.splitext(f)[0]] = os.path.join(d, f)
    return out


def callees(path, known):
    """Names this source calls. A DreamShader call is `Name(` or a VirtualFunction Asset path."""
    body = re.sub(r"//[^\n]*", "", io.open(path, encoding="utf-8").read())
    hits = set(re.findall(r"\b(MF_\w+)\s*\(", body))
    hits |= set(re.findall(r'Asset\s*=\s*Path\([^,]+,\s*"[^"]*/([\w]+)"', body))
    self_name = os.path.splitext(os.path.basename(path))[0]
    return {h for h in hits if h in known and h != self_name}


def closure(name, known, seen=None):
    """Every source that feeds `name`, including itself."""
    seen = seen if seen is not None else set()
    if name in seen:
        return seen
    seen.add(name)
    for c in callees(known[name], known):
        closure(c, known, seen)
    return seen


def main():
    known = sources()
    stale = []
    checked = 0
    for d, _sub, files in os.walk(CONTENT):
        for f in files:
            if not f.endswith(".uasset"):
                continue
            name = os.path.splitext(f)[0]
            if name not in known:
                continue          # not a DreamShader-generated asset
            checked += 1
            asset = os.path.join(d, f)
            asset_time = os.path.getmtime(asset)
            newest, culprit = 0.0, None
            for src in closure(name, known):
                t = os.path.getmtime(known[src])
                if t > newest:
                    newest, culprit = t, src
            if newest > asset_time:
                stale.append((name, culprit, asset))

    for name, culprit, asset in sorted(stale):
        why = "its own source" if culprit == name else "%s, which it calls" % culprit
        print("STALE  %-30s older than %s" % (name, why))
        print("       rebuild: dsc.ps1 compile %s -Force"
              % os.path.relpath(known[name], os.path.dirname(ROOT)).replace("\\", "/"))
    print("%d persisted DreamShader assets checked: %s"
          % (checked, "OK" if not stale else "%d STALE" % len(stale)))
    return 0 if not stale else 1


if __name__ == "__main__":
    sys.exit(main())
