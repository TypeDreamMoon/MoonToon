"""Export one material slot's triangles (reference pose positions + UV0) from a mesh asset to JSON.

Runs inside the Unreal Editor (Python console, `py` command or UnrealBridge); needs the Geometry Script
plugin (Modeling Tools Editor Mode brings it in). Set these before running the file:

    MESH = "/Game/Path/SK_Character"      # SkeletalMesh or StaticMesh
    SLOT = 24                               # material slot index (or its slot name, e.g. "MI_Bangs")
    OUT  = "C:/temp/bangs.json"            # where to write
    UV_CHANNEL = 0                          # optional, default 0

Output: {"mesh", "slot", "uv_channel", "triangles": [[[x, y, z, u, v] * 3], ...]} in the mesh's own
space (cm, Z up). Feed it to bake_hair_tip_mask.py.
"""
import json

import unreal


def _resolve_slot(asset, slot):
    if isinstance(slot, int):
        return slot
    mats = asset.materials if isinstance(asset, unreal.SkeletalMesh) else asset.static_materials
    for i, m in enumerate(mats):
        if str(m.material_slot_name) == str(slot):
            return i
    raise ValueError("no material slot named %s" % slot)


def export(mesh_path, slot, out_path, uv_channel=0):
    asset = unreal.load_asset(mesh_path)
    if asset is None:
        raise ValueError("cannot load " + mesh_path)
    slot_index = _resolve_slot(asset, slot)
    dm = unreal.DynamicMesh()
    opts = unreal.GeometryScriptCopyMeshFromAssetOptions()
    lod = unreal.GeometryScriptMeshReadLOD()
    if isinstance(asset, unreal.SkeletalMesh):
        dm, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_from_skeletal_mesh(asset, dm, opts, lod)
    else:
        dm, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(asset, dm, opts, lod)
    if outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
        raise RuntimeError("copy failed: %s" % outcome)

    Q = unreal.GeometryScript_MeshQueries
    M = unreal.GeometryScript_Materials
    tris = []
    n = Q.get_num_triangle_i_ds(dm)
    for t in range(n):
        mat_id, valid = M.get_triangle_material_id(dm, t)
        if not valid or mat_id != slot_index:
            continue
        okp, p1, p2, p3 = Q.get_triangle_positions(dm, t)
        u1, u2, u3, oku = Q.get_triangle_u_vs(dm, uv_channel, t)
        if not (okp and oku):
            continue
        tris.append([[p.x, p.y, p.z, u.x, u.y] for p, u in ((p1, u1), (p2, u2), (p3, u3))])
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump({"mesh": mesh_path, "slot": slot_index, "uv_channel": uv_channel, "triangles": tris}, f)
    print("exported %d triangles of slot %d (%d in mesh) -> %s" % (len(tris), slot_index, n, out_path))


export(MESH, SLOT, OUT, globals().get("UV_CHANNEL", 0))
