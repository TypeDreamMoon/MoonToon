"""Dump the material slots of one texture group (the slots to merge into one material) to JSON.

Runs inside the Unreal Editor. Set before running:
    MESH  = "/Game/.../SK_Character"     # SkeletalMesh
    SLOTS = [14, 15, 16]                  # material slot indices of the group
    OUT   = "C:/temp/group.json"

Per slot: the material instance, its blend mode and two-sidedness, every static switch, and the effective
value of every scalar / vector parameter. build_table.py reads it. Nothing world-owned is kept in globals.
"""
import gc
import json

import unreal

for _k in [k for k, v in list(globals().items()) if isinstance(v, unreal.Object)]:
    del globals()[_k]
gc.collect()


def _dump(mesh_path, slots, out_path):
    mel = unreal.MaterialEditingLibrary
    mesh = unreal.load_asset(mesh_path)
    mats = mesh.get_editor_property("materials")
    out = {"mesh": mesh_path, "slots": []}
    for i in slots:
        mi = mats[i].get_editor_property("material_interface")
        entry = {"slot": i, "slot_name": str(mats[i].get_editor_property("material_slot_name")),
                 "mi": mi.get_path_name().split(".")[0], "blend": str(mi.get_blend_mode()).split(".")[-1].split(":")[0],
                 "switches": {}, "scalars": {}, "vectors": {}}
        bo = mi.get_editor_property("base_property_overrides")
        entry["two_sided"] = bool(bo.get_editor_property("two_sided")) if bo.get_editor_property("override_two_sided") else None
        for n in mel.get_static_switch_parameter_names(mi):
            entry["switches"][str(n)] = bool(mel.get_material_instance_static_switch_parameter_value(mi, n))
        for n in mel.get_scalar_parameter_names(mi):
            entry["scalars"][str(n)] = float(mel.get_material_instance_scalar_parameter_value(mi, n))
        for n in mel.get_vector_parameter_names(mi):
            c = mel.get_material_instance_vector_parameter_value(mi, n)
            entry["vectors"][str(n)] = [c.r, c.g, c.b, c.a]
        out["slots"].append(entry)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("dumped %d slots -> %s" % (len(slots), out_path))


_dump(MESH, SLOTS, OUT)
gc.collect()
