"""R11: apply a migration plan (r11_plan.py, r11_a11_plan.py) in the editor, AFTER the regen.
Op kinds: s scalar, w static switch, t texture, v vector, x clear a texture override (if it holds the given texture).
The MaterialEditingLibrary setters return False even when they work: verify from disk, not from "failed".
Prefix with PLAN = "<plan.json>", LOG = "<log.json>", SAVE = True/False.
Paths come from the plan file (UTF-8) so this script stays ASCII (the bridge mangles non-ASCII source text).
Only the instances the plan names are touched and saved; nothing else, no level."""
import json
import unreal

MEL = unreal.MaterialEditingLibrary
plan = json.load(open(PLAN, encoding="utf-8"))
by_mi = {}
for mi, kind, name, value, why in plan["ops"]:
    by_mi.setdefault(mi, []).append((kind, name, value, why))

log = {"applied": 0, "failed": [], "missing_asset": [], "saved": 0, "save_failed": []}
pkgs = []
for path, ops in by_mi.items():
    obj = unreal.load_asset(path)
    if obj is None:
        log["missing_asset"].append(path)
        continue
    for kind, name, value, why in ops:
        ok = False
        if kind == "s":
            ok = MEL.set_material_instance_scalar_parameter_value(obj, name, float(value))
        elif kind == "w":
            ok = MEL.set_material_instance_static_switch_parameter_value(obj, name, bool(value))
        elif kind == "t":
            tex = unreal.load_asset(value)
            ok = tex is not None and MEL.set_material_instance_texture_parameter_value(obj, name, tex)
        elif kind == "v":
            ok = MEL.set_material_instance_vector_parameter_value(obj, name, unreal.LinearColor(*value))
        elif kind == "d":
            # T57b: drop a stale override (any value) left under a renamed parameter's old name.
            # value names the section: "scalars" / "vectors" / "textures".
            prop = {"scalars": "scalar_parameter_values", "vectors": "vector_parameter_values",
                    "textures": "texture_parameter_values"}[value]
            vals = list(obj.get_editor_property(prop))
            keep = [v for v in vals
                    if str(v.get_editor_property("parameter_info").get_editor_property("name")) != name]
            if len(keep) < len(vals):
                obj.set_editor_property(prop, keep)
                ok = True
        elif kind == "x":
            # A11: clear a texture override, but only while it still holds the texture the plan expects.
            vals = list(obj.get_editor_property("texture_parameter_values"))
            keep = []
            for tv in vals:
                tex = tv.get_editor_property("parameter_value")
                if (str(tv.get_editor_property("parameter_info").get_editor_property("name")) == name
                        and tex is not None and tex.get_path_name().split(".")[0] == value):
                    continue
                keep.append(tv)
            if len(keep) == len(vals) - 1:
                obj.set_editor_property("texture_parameter_values", keep)
                ok = True
        if ok:
            log["applied"] += 1
        else:
            log["failed"].append([path, kind, name, value, why])
    MEL.update_material_instance(obj)
    pkgs.append(obj.get_outermost())

if SAVE and pkgs:
    for p in pkgs:
        assert not p.get_name().endswith("_Showcase") and "/Maps/" not in p.get_name(), p.get_name()
    ok = unreal.EditorLoadingAndSavingUtils.save_packages(pkgs, False)
    log["saved"] = len(pkgs) if ok else 0
    if not ok:
        log["save_failed"] = [p.get_name() for p in pkgs]
json.dump(log, open(LOG, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print("applied %d, failed %d, missing %d, saved %d" % (log["applied"], len(log["failed"]), len(log["missing_asset"]), log["saved"]))
