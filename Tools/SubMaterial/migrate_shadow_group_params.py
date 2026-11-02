"""Migrate material instances to the renamed shadow group ID parameters of MF_MoonToonBaseInput (MoonToon T22).

The per-part ray-traced shadow ID ("ID Map(R)", "ID Offset", ...) was renamed to "Shadow Group ID ..." so it is
not confused with the sub-material ID. Instances that override an old name lose that override when the
material function is renamed, so the overrides are captured first and written back under the new names.

Runs inside the Unreal Editor, in two passes. Set before running:
    MODE     = "scan"  -- dry run, BEFORE the rename: finds every MaterialInstanceConstant whose material uses
                          MF_MoonToonBaseInput, lists the old-name overrides and writes them to MANIFEST.
                          Changes nothing.
               "apply" -- AFTER the rename: writes each captured override under its new name, drops the old
                          entry and saves the instance. Refuses an instance whose material does not have the
                          new name yet.
    MANIFEST = "<path>.json"   (default: <Project>/Saved/MoonToon/ShadowGroupMigration.json)

Scalar / vector / texture overrides are read from the instance's own override arrays (exact). Static switches
have no such array in Python, so a switch counts as overridden when its value differs from the parent's.
Nothing world-owned is kept in globals.
"""
import gc
import json
import os

import unreal

for _k in [k for k, v in list(globals().items()) if isinstance(v, unreal.Object)]:
    del globals()[_k]
gc.collect()

RENAMES = {
    "ID Map(R)": "Shadow Group ID Map",
    "ID Offset": "Shadow Group ID Offset",
    "ID Channel": "Shadow Group ID Channel",
    "ID Map UV Channel": "Shadow Group ID Map UV Channel",
    "ID From Global Mask Map": "Shadow Group ID From Global Mask Map",
    "ID From Vertex Color": "Shadow Group ID From Vertex Color",
}
SWITCHES = ("ID From Global Mask Map", "ID From Vertex Color")
BASE_INPUT = "/MoonToon/MaterialFunctions/MF_MoonToonBaseInput"
ARRAYS = (("scalar", "scalar_parameter_values"), ("vector", "vector_parameter_values"),
          ("texture", "texture_parameter_values"))


def _instances():
    """Package names of every MaterialInstanceConstant whose material (through any chain) uses BASE_INPUT."""
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    opts = unreal.AssetRegistryDependencyOptions(include_soft_package_references=False,
                                                include_hard_package_references=True,
                                                include_searchable_names=False,
                                                include_soft_management_references=False,
                                                include_hard_management_references=False)
    seen, todo, out = set(), [BASE_INPUT], []
    while todo:
        p = todo.pop()
        for r in ar.get_referencers(p, opts) or []:
            r = str(r)
            if r in seen:
                continue
            seen.add(r)
            todo.append(r)
            for ad in ar.get_assets_by_package_name(r):
                if str(ad.asset_class_path.asset_name) == "MaterialInstanceConstant":
                    out.append(r)
    return sorted(set(out))


def _value(kind, v):
    if kind == "vector":
        return [v.r, v.g, v.b, v.a]
    if kind == "texture":
        return v.get_path_name() if v else None
    return float(v)


def _parent_switch(mi, name):
    p = mi.get_editor_property("parent")
    mel = unreal.MaterialEditingLibrary
    if isinstance(p, unreal.MaterialInstance):
        return bool(mel.get_material_instance_static_switch_parameter_value(p, name))
    return bool(mel.get_material_default_static_switch_parameter_value(p, name)) if p else False


def _scan(manifest):
    mel = unreal.MaterialEditingLibrary
    found = []
    pkgs = _instances()
    for pkg in pkgs:
        mi = unreal.load_asset(pkg)
        if not isinstance(mi, unreal.MaterialInstanceConstant):
            continue
        entry = {"mi": pkg, "overrides": []}
        for kind, prop in ARRAYS:
            for pv in mi.get_editor_property(prop):
                name = str(pv.get_editor_property("parameter_info").get_editor_property("name"))
                if name in RENAMES:
                    entry["overrides"].append({"kind": kind, "old": name, "new": RENAMES[name],
                                               "value": _value(kind, pv.get_editor_property("parameter_value"))})
        names = {str(n) for n in mel.get_static_switch_parameter_names(mi)}
        for name in SWITCHES:
            if name in names:
                v = bool(mel.get_material_instance_static_switch_parameter_value(mi, name))
                if v != _parent_switch(mi, name):
                    entry["overrides"].append({"kind": "switch", "old": name, "new": RENAMES[name], "value": v})
        if entry["overrides"]:
            found.append(entry)
    os.makedirs(os.path.dirname(manifest), exist_ok=True)
    with open(manifest, "w", encoding="utf-8") as f:
        json.dump({"renames": RENAMES, "scanned": len(pkgs), "instances": found}, f, ensure_ascii=False, indent=1)
    print("scanned %d instances using %s; %d override an old name -> %s" % (len(pkgs), BASE_INPUT, len(found), manifest))
    for e in found:
        for o in e["overrides"]:
            print("  %s  %s = %s" % (e["mi"], o["old"], o["value"]))


def _apply(manifest):
    mel = unreal.MaterialEditingLibrary
    data = json.load(open(manifest, encoding="utf-8"))
    done, refused = 0, []
    for e in data["instances"]:
        mi = unreal.load_asset(e["mi"])
        have = {str(n) for n in mel.get_scalar_parameter_names(mi)} | {str(n) for n in mel.get_vector_parameter_names(mi)} \
            | {str(n) for n in mel.get_texture_parameter_names(mi)} | {str(n) for n in mel.get_static_switch_parameter_names(mi)}
        missing = [o["new"] for o in e["overrides"] if o["new"] not in have]
        if missing:
            refused.append((e["mi"], missing))
            continue
        for o in e["overrides"]:
            k, n, v = o["kind"], o["new"], o["value"]
            if k == "scalar":
                mel.set_material_instance_scalar_parameter_value(mi, n, v)
            elif k == "vector":
                mel.set_material_instance_vector_parameter_value(mi, n, unreal.LinearColor(*v))
            elif k == "texture":
                mel.set_material_instance_texture_parameter_value(mi, n, unreal.load_asset(v) if v else None)
            else:
                mel.set_material_instance_static_switch_parameter_value(mi, n, v)
        for kind, prop in ARRAYS:
            arr = mi.get_editor_property(prop)
            keep = [pv for pv in arr
                    if str(pv.get_editor_property("parameter_info").get_editor_property("name")) not in RENAMES]
            if len(keep) != len(arr):
                mi.set_editor_property(prop, keep)
        mel.update_material_instance(mi)
        unreal.EditorAssetLibrary.save_loaded_asset(mi, False)
        done += 1
        print("  migrated %s: %s" % (e["mi"], ", ".join("%s=%s" % (o["new"], o["value"]) for o in e["overrides"])))
    print("migrated %d of %d instances" % (done, len(data["instances"])))
    for mi, missing in refused:
        print("  REFUSED %s: the material has no %s yet (rename MF_MoonToonBaseInput first)" % (mi, missing))


def _main():
    manifest = globals().get("MANIFEST") or os.path.join(unreal.Paths.project_saved_dir(), "MoonToon",
                                                         "ShadowGroupMigration.json")
    mode = globals().get("MODE", "scan")
    if mode == "scan":
        _scan(manifest)
    elif mode == "apply":
        _apply(manifest)
    else:
        print("MODE must be scan or apply")


_main()
gc.collect()
