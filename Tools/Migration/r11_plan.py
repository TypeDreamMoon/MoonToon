"""R11 batch 1 migration PLAN, from the offline snapshot taken before the regen (r11_census.py).
usage: python -I r11_plan.py <snap_before.json> <plan.json>

Every op keeps an instance's current result under the regenerated material:
  B4  a static switch that became a 0/1 scalar of the same name: each switch OVERRIDE becomes a scalar
      override on the same instance, so inheritance is mirrored exactly.
  B1  "Enable Global Mask Map" / "Enable Specular Color Map" / "Enable Shadow Group ID Map" (new, default off):
      on wherever the instance's effective map is assigned and is not the default texture.
  B2  "Shadow Color From Base Color Map" (new, default off): on wherever the effective Base Color Map and
      Shadow Color Map are the same texture.
  A12 "<X> Ramp Offset Mask Source" (default 1 -> 0 = neutral): the old 1 is kept wherever the offset is
      live (intensity != 0 and read from the global mask).
  A6 / A4 renames: the old parameter's override is copied to the new name.
B1 / B2 / A12 are planned top-down, and an op is written only where an instance's wanted value differs from
what it would inherit after the parents' ops."""
import json
import sys

snap = json.load(open(sys.argv[1], encoding="utf-8"))
D = snap["defaults"]
ROOTS = ("/MoonToon/Materials/Core/M_MoonToon", "/MoonToon/Materials/Core/M_MoonToonOutline")
mis = {k: v for k, v in snap["mis"].items() if v["root"] in ROOTS}

B4 = ["Use Anisotropy", "Invert Roughness Channel", "Is Face", "Is Hair", "Use Screen Space Depth Test Hair Shadow",
      "Flip Left Right", "Distance Field Facial Shadow Map R+G Average", "Head Forward From Baked Face UV"]
DEFAULT_TEX = {
    "Global Mask Map": "/MoonToon/Textures/T_White_Linear",
    "Specular Color Map": "/Engine/EngineResources/WhiteSquareTexture",
    "Shadow Group ID Map": "/MoonToon/Textures/T_Black_Linear",
}
B1 = [("Enable Global Mask Map", "Global Mask Map"), ("Enable Specular Color Map", "Specular Color Map"),
      ("Enable Shadow Group ID Map", "Shadow Group ID Map")]

ops = []


def op(mi, kind, name, value, why):
    ops.append([mi, kind, name, value, why])


def depth(k):
    return len(mis[k]["chain"])


order = sorted(mis, key=depth)  # parents (shorter chains) first


def eff_tex(k, name):
    return mis[k]["effective"]["textures"].get(name)


def eff_sw(k, name):
    return mis[k]["effective"]["switches"].get(name, D.get(name, False))


def eff_sc(k, name, default):
    return mis[k]["effective"]["scalars"].get(name, default)


def parent_in(k):
    p = mis[k]["parent"]
    return p if p in mis else None


def top_down(new_name, wanted_fn, new_default, kind, why, relevant_fn=lambda k: True):
    """Write an override where the wanted value differs from what the instance would inherit."""
    new_eff = {}
    for k in order:
        p = parent_in(k)
        inherited = new_eff[p] if p else new_default
        if relevant_fn(k):
            want = wanted_fn(k)
            if want != inherited:
                op(k, kind, new_name, want, why)
                new_eff[k] = want
                continue
        new_eff[k] = inherited


# B4 -- mirror every override
for k in order:
    for n in B4:
        if n in mis[k]["switches"]:
            op(k, "s", n, 1.0 if mis[k]["switches"][n] else 0.0, "B4 switch->scalar")

# B1
for sw, tex in B1:
    top_down(sw, lambda k, tex=tex: bool(eff_tex(k, tex)) and eff_tex(k, tex) != DEFAULT_TEX[tex], False, "w", "B1 " + tex)

# B2
top_down("Shadow Color From Base Color Map",
         lambda k: bool(eff_tex(k, "Base Color Map")) and eff_tex(k, "Base Color Map") == eff_tex(k, "Shadow Color Map"),
         False, "w", "B2 same texture")

# A12 -- keep the old source (default 1) where the offset is live
for ident in ("Diffuse Ramp Offset", "Specular Ramp Offset"):
    src = ident + " Mask Source"
    live = lambda k, ident=ident: eff_sc(k, ident + " Intensity", 0.0) != 0.0 and eff_sw(k, ident + " From Global Mask Map")
    top_down(src, lambda k, src=src: eff_sc(k, src, 1.0), 0.0, "s", "A12 keep source", relevant_fn=live)

# renames
for k in order:
    v = mis[k]
    if "BodyBaseColor" in v["textures"]:
        op(k, "t", "Stockings Skin Color Map", v["textures"]["BodyBaseColor"], "A6 rename")
    if "KajiyaSecondaryMaskChannel" in v["vectors"]:
        op(k, "v", "Toon Kajiya Strand Shift Channel", v["vectors"]["KajiyaSecondaryMaskChannel"], "A4 rename")
    if "Kajiya Secondary Mask Source" in v["scalars"]:
        op(k, "s", "Toon Kajiya Strand Shift Mask Source", v["scalars"]["Kajiya Secondary Mask Source"], "A4 rename")

summary = {}
for o in ops:
    summary.setdefault(o[4], 0)
    summary[o[4]] += 1
touched = sorted(set(o[0] for o in ops))
json.dump({"ops": ops, "summary": summary, "touched": touched}, open(sys.argv[2], "w", encoding="utf-8"), ensure_ascii=False, indent=1)
out = ["ops %d on %d instances" % (len(ops), len(touched))]
for k2, n in sorted(summary.items()):
    out.append("  %-24s %d" % (k2, n))
by_folder = {}
for t in touched:
    f = "/".join(t.split("/")[:4])
    by_folder[f] = by_folder.get(f, 0) + 1
out.append("instances by folder:")
for f, n in sorted(by_folder.items(), key=lambda x: -x[1]):
    out.append("  %4d %s" % (n, f))
sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8"))
