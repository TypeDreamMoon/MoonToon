"""Compare an offline .uasset MI dump (uasset_mi_dump.py) with an editor dump (wm_midump.py).
usage: python -I uasset_mi_cmp.py <offline.json> <editor.json> [editor_key_map: offkey=editorkey ...]"""
import json
import sys

off = json.load(open(sys.argv[1], encoding="utf-8"))
ed = json.load(open(sys.argv[2], encoding="utf-8"))
keymap = dict(a.split("=", 1) for a in sys.argv[3:])


def norm_tex(v):
    if not isinstance(v, str):
        return v
    return v.split(":")[0].split(".")[0]


def show(v):
    if isinstance(v, str) and "/" in v:
        return v.split("/")[-1]
    return v


out = []
for k, v in off.items():
    ek = keymap.get(k, k)
    if ek not in ed:
        out.append("== %s: no editor dump under %s" % (k, ek))
        continue
    b = ed[ek]
    out.append("== %s  (vs editor dump %s)" % (k, ek))
    out.append("   parent offline=%s editor=%s" % (show(v["parent"]), show(b["parent"])))
    for sec in ("scalars", "vectors", "textures"):
        o, e = dict(v[sec]), dict(b[sec])
        if sec == "textures":
            o = {x: norm_tex(y) for x, y in o.items()}
            e = {x: norm_tex(y) for x, y in e.items()}
        n_same = 0
        for x in sorted(set(o) | set(e)):
            a, c = o.get(x, "-"), e.get(x, "-")
            if isinstance(a, float) and isinstance(c, float) and abs(a - c) < 1e-4:
                n_same += 1
                continue
            if isinstance(a, list) and isinstance(c, list) and len(a) == len(c) and all(abs(p - q) < 1e-4 for p, q in zip(a, c)):
                n_same += 1
                continue
            if a == c:
                n_same += 1
                continue
            out.append("   %-8s %-34s offline: %-30s editor: %s" % (sec, x, show(a), show(c)))
        out.append("   %-8s %d identical" % (sec, n_same))
    on_o = set(x for x, s in v["switches"].items() if s["value"])
    on_e = set(x for x, s in b["switches"].items() if s)
    for x in sorted(on_o - on_e):
        out.append("   switch   %-34s offline ON, editor off" % x)
    for x in sorted(on_e - on_o):
        out.append("   switch   %-34s offline off, editor ON" % x)
    out.append("   switches on: %d offline / %d editor; overridden offline: %d" % (
        len(on_o), len(on_e), sum(1 for s in v["switches"].values() if s["override"])))
    out.append("   other top-level offline: %s" % v["other_top_level"])
sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8"))
