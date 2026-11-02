"""Make (or update) a MoonToon Sub-Material Table asset from build_table.py's JSON, bake it, and optionally
put it on a merged material instance (MoonToon T22). Needs the MoonToonEditor module (UMoonToonSubMaterialTable).

Runs inside the Unreal Editor. Set before running:
    REPORT_JSON = "C:/.../T_SubMat_Down.json"   # build_table.py's report (rows -> overrides)
    TABLE       = "/Game/.../ST_SubMat_Down"     # the table asset; its texture is T_ST_SubMat_Down next to it
    APPLY_TO    = None or "/Game/.../MI_SubMat_Down"   # optional: switch on + texture parameter
    CHECK       = True                           # compare the C++ packing with submat_common.build_texels

Row i of the table is row i of the report (the i-th slot of the group). Only the overridden fields are set.
Nothing world-owned is kept in globals.
"""
import gc
import json
import os
import struct
import sys

import unreal

for _k in [k for k, v in list(globals().items()) if isinstance(v, unreal.Object)]:
    del globals()[_k]
gc.collect()

_HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else \
    os.path.join(unreal.Paths.project_plugins_dir(), "MoonToon", "Tools", "SubMaterial")
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import submat_common as S  # noqa: E402

# field -> (override property, [value properties]) on unreal.MoonToonSubMaterialRow
COLOR_FIELDS = {"ShadowColor": "shadow_color", "BaseColorTint": "base_color_tint", "OutlineColorScale": "outline_color_scale"}
FLOAT_FIELDS = {
    "Metallic": "metallic", "Specular": "specular", "Roughness": "roughness", "Reflection": "reflection",
    "Anisotropy": "anisotropy", "RampUVOffset": "ramp_uv_offset", "EmissiveScale": "emissive_scale",
    "BaseColorTintStrength": "base_color_tint_strength", "OutlineWidthScale": "outline_width_scale",
    "ScalarA": "scalar_a", "ScalarB": "scalar_b", "ScalarC": "scalar_c", "ScalarD": "scalar_d",
    "ScalarE": "scalar_e", "ScalarF": "scalar_f", "ScalarG": "scalar_g",
    "VectorX": "vector_x", "VectorY": "vector_y", "VectorZ": "vector_z", "VectorW": "vector_w",
}
OVERRIDE = {"BaseColorTintStrength": "override_base_color_tint"}


def _f32(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def _row(entry):
    r = unreal.MoonToonSubMaterialRow()
    r.set_editor_property("name", "%s (slot %s)" % (entry.get("slot_name", ""), entry.get("slot", "")))
    for field, value in entry["overrides"].items():
        if field in COLOR_FIELDS:
            v = list(value) + [1.0] * (4 - len(value))
            r.set_editor_property(COLOR_FIELDS[field], unreal.LinearColor(v[0], v[1], v[2], 1.0))
            r.set_editor_property("override_" + COLOR_FIELDS[field], True)
        elif field in FLOAT_FIELDS:
            r.set_editor_property(FLOAT_FIELDS[field], float(value))
            r.set_editor_property(OVERRIDE.get(field, "override_" + FLOAT_FIELDS[field]), True)
        else:
            raise ValueError("unknown field %s" % field)
    return r


def _make(report_json, table_path, apply_to, check):
    report = json.load(open(report_json, encoding="utf-8"))
    eal = unreal.EditorAssetLibrary
    if eal.does_asset_exist(table_path):
        table = unreal.load_asset(table_path)
    else:
        folder, name = table_path.rsplit("/", 1)
        table = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, folder, unreal.MoonToonSubMaterialTable, unreal.MoonToonSubMaterialTableFactory())
    table.set_editor_property("rows", [_row(e) for e in report["rows"]])
    tex = table.bake_texture()
    eal.save_loaded_asset(table, False)
    if tex:
        eal.save_loaded_asset(tex, False)
    print("table %s: %d rows, baked %s" % (table_path, len(report["rows"]), tex.get_path_name() if tex else None))

    if check:
        packed = table.pack_texels()
        ref = S.build_texels([e["overrides"] for e in report["rows"]])
        bad = []
        for r in range(S.ROWS):
            for c in range(S.COLUMNS):
                p = packed[r * S.COLUMNS + c]
                got = (p.r, p.g, p.b, p.a)
                want = tuple(_f32(x) for x in ref[r][c])
                if tuple(_f32(x) for x in got) != want:
                    bad.append((r, c, got, want))
        print("C++ packing vs submat_common.build_texels: %s" % ("identical (16 x 8 x 4 floats)" if not bad else "%d texels differ, first %s" % (len(bad), bad[:3])))

    if apply_to:
        mi = unreal.load_asset(apply_to)
        ok = table.apply_to_material_instance(mi)
        eal.save_loaded_asset(mi, False)
        print("applied to %s: %s" % (apply_to, ok))


_make(REPORT_JSON, TABLE, globals().get("APPLY_TO"), globals().get("CHECK", True))
gc.collect()
