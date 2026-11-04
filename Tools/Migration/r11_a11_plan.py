"""R11 A11 migration PLAN: "Opacity Map" and "SA Map" sample through a LinearColor sampler (defaults T_White_Linear /
T_Black_Linear). An instance override that holds the OLD engine default (sRGB WhiteSquareTexture / Black) reads the
same value as the new default but fails the sampler check, so it is cleared -- or, where a parent overrides the map
with something else, replaced by the new linear default so the instance still reads white / black.
Any other override is left alone and listed: that texture must be imported linear (sRGB off, Default or BC7).

usage: python -I r11_a11_plan.py <snapshot.json from r11_census.py> <plan.json>
Apply with r11_apply.py (op kind "x" = clear a texture override, only if it still holds the expected texture)."""
import json
import sys

snap = json.load(open(sys.argv[1], encoding="utf-8"))
ROOTS = ("/MoonToon/Materials/Core/M_MoonToon", "/MoonToon/Materials/Core/M_MoonToonOutline")
mis = {k: v for k, v in snap["mis"].items() if v["root"] in ROOTS}
MAPS = {
    # name: (old engine default, new linear default)
    "Opacity Map": ("/Engine/EngineResources/WhiteSquareTexture", "/MoonToon/Textures/T_White_Linear"),
    "SA Map": ("/Engine/EngineResources/Black", "/MoonToon/Textures/T_Black_Linear"),
}
order = sorted(mis, key=lambda k: len(mis[k]["chain"]))  # parents first

ops, keep = [], []
for name, (old, new) in MAPS.items():
    after = {}  # instance -> its own override after the migration (None = none)
    for k in order:
        own = mis[k]["textures"].get(name)
        p = mis[k]["parent"]
        # what the instance would inherit after its parents' ops
        inherited = None
        while p in mis:
            if after.get(p) is not None:
                inherited = after[p]
                break
            p = mis[p]["parent"]
        if own is None:
            after[k] = None
        elif own == old:
            if inherited is None or inherited in (old, new):
                ops.append([k, "x", name, old, "A11 clear old default"])
                after[k] = None
            else:
                ops.append([k, "t", name, new, "A11 old default over a parent's map"])
                after[k] = new
        else:
            after[k] = own
            if own != new:
                keep.append([k, name, own])

summary = {}
for o in ops:
    summary[o[4]] = summary.get(o[4], 0) + 1
touched = sorted(set(o[0] for o in ops))
json.dump({"ops": ops, "summary": summary, "touched": touched, "kept_needs_linear": keep},
          open(sys.argv[2], "w", encoding="utf-8"), ensure_ascii=False, indent=1)
out = ["ops %d on %d instances" % (len(ops), len(touched))]
for k2, n in sorted(summary.items()):
    out.append("  %-36s %d" % (k2, n))
out.append("kept (texture must be linear): %d" % len(keep))
for k, name, t in keep:
    out.append("  %s  %s = %s" % (k, name, t))
sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8"))
