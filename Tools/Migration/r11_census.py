"""R11 census + snapshot (offline, no editor): every MaterialInstanceConstant under the given content roots, its
parent chain, its own overrides and its EFFECTIVE static switches, plus the unique static-permutation count.

usage: python -I r11_census.py <out.json> [--defaults <dshader_dir>] <content_root>=<mount> ...
  e.g. r11_census.py snap.json --defaults .../Plugins/MoonToon/DShader \
         I:/.../DevTest/Content=/Game I:/.../Plugins/MoonToon/Content=/MoonToon

The snapshot is the migration's source of truth: it is read from disk BEFORE the regen, so a migration does not
depend on the editor still holding overrides of switches that the regenerated material no longer has.
Switch defaults come from the DShader sources (StaticSwitchParameter declarations and UE.Expression switch nodes),
so an override that equals the default counts the same as no override -- the compiled code is the same."""
import collections
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uasset_mi_dump as U  # noqa: E402

ROOTS_OF_INTEREST = ("/MoonToon/Materials/Core/M_MoonToon", "/MoonToon/Materials/Core/M_MoonToonOutline")


def switch_defaults(dshader_dir):
    """ParameterName -> default bool, from every .dsf/.dsm/.dss under the DShader folder."""
    out = {}
    for f in glob.glob(os.path.join(dshader_dir, "**", "*.ds*"), recursive=True):
        t = open(f, encoding="utf-8", errors="replace").read()
        # Properties-block form: StaticSwitchParameter Ident = false [ ParameterName="X"; ... ]
        for m in re.finditer(r"StaticSwitchParameter\s+(\w+)\s*=\s*(true|false)\s*\[(.*?)\];", t, re.S):
            pn = re.search(r'ParameterName="([^"]+)"', m.group(3))
            out.setdefault(pn.group(1) if pn else m.group(1), m.group(2) == "true")
        # Graph form: UE.Expression(Class="StaticSwitchParameter", ..., ParameterName="X", DefaultValue=true)
        for m in re.finditer(r'UE\.Expression\(\s*Class="StaticSwitchParameter"(.*?)\);', t, re.S):
            body = m.group(1)
            pn = re.search(r'ParameterName="([^"]+)"', body)
            if not pn:
                continue
            dv = re.search(r"DefaultValue\s*=\s*(true|false)", body)
            out.setdefault(pn.group(1), bool(dv and dv.group(1) == "true"))
    return out


def pkg_of(path, roots):
    p = path.replace("\\", "/")
    for r, mount in roots:
        if p.lower().startswith(r.lower() + "/"):
            return mount + p[len(r):][:-len(".uasset")]
    return None


def main():
    args = sys.argv[1:]
    out_path = args.pop(0)
    dsh = None
    if args and args[0] == "--defaults":
        args.pop(0)
        dsh = args.pop(0)
    roots = [tuple(a.split("=", 1)) for a in args]
    roots = [(r.replace("\\", "/").rstrip("/"), m.rstrip("/")) for r, m in roots]
    defaults = switch_defaults(dsh) if dsh else {}

    mis, errors = {}, []
    for r, mount in roots:
        for dp, dn, fn in os.walk(r):
            if any(x in dp.replace("\\", "/") for x in ("/__ExternalActors__", "/__ExternalObjects__", "/Developers")):
                continue
            for f in fn:
                if not f.endswith(".uasset"):
                    continue
                p = os.path.join(dp, f)
                try:
                    if os.path.getsize(p) > 8_000_000:
                        continue
                    with open(p, "rb") as h:
                        head = h.read()
                    if b"MaterialInstanceConstant" not in head:
                        continue
                    pk = U.Pkg(p)
                    if not any(e["cls"] and e["cls"].endswith("MaterialInstanceConstant") for e in pk.exports):
                        continue
                    props = pk.mi()
                    s = U.summarize(props)
                except Exception as e:  # noqa: BLE001
                    errors.append([p, repr(e)[:200]])
                    continue
                pkgname = pkg_of(p, roots) or pk.package_name
                parent = (s["parent"] or "").split(".")[0] or None
                mis[pkgname] = {
                    "file": p.replace("\\", "/"),
                    "parent": parent,
                    "scalars": s["scalars"], "vectors": s["vectors"],
                    "textures": {k: (v.split(":")[0].split(".")[0] if isinstance(v, str) else v) for k, v in s["textures"].items()},
                    "switches": {k: v["value"] for k, v in s["switches"].items() if v["override"]},
                    "base_overrides": s["raw_other"].get("BasePropertyOverrides"),
                    "has_static_permutation": bool(s["raw_other"].get("bHasStaticPermutationResource")),
                }

    def chain(pk):
        c, q, seen = [], pk, set()
        while q in mis and q not in seen:
            seen.add(q); c.append(q); q = mis[q]["parent"]
        return c, q  # leaf..top, root material (or unresolved)

    for pk, d in mis.items():
        c, root = chain(pk)
        d["root"] = root
        d["chain"] = c
        eff = {"scalars": {}, "vectors": {}, "textures": {}, "switches": {}}
        for q in reversed(c):  # top first, leaf last wins
            for sec in eff:
                eff[sec].update(mis[q][sec])
        d["effective"] = eff
        # static key: effective switch values, normalised against the material default
        sw = {}
        for n, v in eff["switches"].items():
            # A switch the sources no longer declare is a stale override (the material dropped it): it
            # compiles nothing, so it does not split the permutation.
            if defaults and n not in defaults:
                continue
            if n in defaults and defaults[n] == v:
                continue
            sw[n] = v
        d["static_key"] = sorted(sw.items())
        bo = None
        for q in reversed(c):
            if mis[q]["base_overrides"]:
                bo = mis[q]["base_overrides"]
        d["base_key"] = json.dumps(bo, sort_keys=True, default=str) if bo else ""

    interest = {k: v for k, v in mis.items() if v["root"] in ROOTS_OF_INTEREST}
    report = collections.OrderedDict()
    for root in ROOTS_OF_INTEREST:
        group = {k: v for k, v in interest.items() if v["root"] == root}
        prod = {k: v for k, v in group.items() if not any(x in k for x in ("/MoonToonStyleTest/", "/_Spike/", "/ToonSubstrateV2/"))}
        ww = {k: v for k, v in group.items() if "/Test_ww_yy/" in k}
        key = lambda v: (tuple(v["static_key"]), v["base_key"])  # noqa: E731
        report[root] = {
            "mis": len(group),
            "unique_static_combos_all": len(set(key(v) for v in group.values())),
            "unique_static_combos_prod": len(set(key(v) for v in prod.values())),
            "prod_mis": len(prod),
            "unique_static_combos_ww_yy": len(set(key(v) for v in ww.values())),
            "ww_yy_mis": len(ww),
        }
    json.dump({"defaults": defaults, "report": report, "errors": errors, "mis": mis}, open(out_path, "w", encoding="utf-8"),
              ensure_ascii=False, indent=1, default=str)
    sys.stdout.buffer.write((json.dumps(report, indent=1) + "\nparsed %d MIs, %d in the two chains, %d errors\n" % (
        len(mis), len(interest), len(errors))).encode("utf-8"))


if __name__ == "__main__":
    main()
