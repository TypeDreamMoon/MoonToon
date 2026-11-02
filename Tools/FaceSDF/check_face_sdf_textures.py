"""Check every classic face SDF texture against the import spec (MoonToon T28, Docs/FaceSDF.md). Read-only.

Runs inside the Unreal Editor (UnrealBridge exec or the Python console). Optional, set before running:
    ROOTS = ["/Game", "/MoonToon"]      # where to look for material instances
    GRANDFATHERED = ["/Game/Assets/Meshs/Test_ww_yy/"]   # path prefixes (of the instance OR the texture) whose
                                        # current look depends on the old import (e.g. sRGB on): "keep", not "fix"
    REPORT = "<Project>/Saved/MoonToon/FaceSdfCheck.json"

For every MaterialInstanceConstant with "Is DFFacialShadow" on (and "Use Full Angle SDF" off -- a full-angle
face does not read the classic map) it reports the "Distance Field Facial Shadow Map" texture and the switches
that decide its precision, and flags:
  ERROR  sRGB on                   the map is data; sRGB bends the threshold curve (and its dark end)
  ERROR  BC1 / BC3 / default       5-6 bit block endpoints: terraces and patches along the terminator
  ERROR  R+G Average on a compressed texture   BC7 / BC1 mix R and G per block: the 9 bits are gone
  WARN   BC7 / BC4                 lossy; fine for 8-bit-class maps, not for a 16-bit bake
  WARN   >8-bit texture, "Facial SDF Extended Payload" off   the lights still see 8 bits
  INFO   mips                      the SDF is read at mip 0 (MF_ToonDFFacialShadow), mips only cost memory
Nothing is changed or saved; nothing world-owned is kept in globals."""
import gc
import json
import os

import unreal

for _k in [k for k, v in list(globals().items()) if isinstance(v, unreal.Object)]:
    del globals()[_k]
gc.collect()

TC = unreal.TextureCompressionSettings
PRECISE = {TC.TC_HALF_FLOAT: "R16F", TC.TC_SINGLE_FLOAT: "R32F", TC.TC_GRAYSCALE: "G8/G16 (no compression)",
           TC.TC_VECTOR_DISPLACEMENTMAP: "BGRA8 (no compression)", TC.TC_HDR: "RGBA16F", TC.TC_HDR_F32: "RGBA32F"}
HIGH = {TC.TC_HALF_FLOAT, TC.TC_SINGLE_FLOAT, TC.TC_HDR, TC.TC_HDR_F32}
LOSSY_OK = {TC.TC_ALPHA: "BC4", TC.TC_BC7: "BC7"}
SWITCH_DFF = "Is DFFacialShadow"
SWITCH_RG = "Distance Field Facial Shadow Map R+G Average"
SWITCH_EXT = "Facial SDF Extended Payload"
SWITCH_360 = "Use Full Angle SDF"
TEX = "Distance Field Facial Shadow Map"


KEEP_PREFIXES = []


def _switch(mi, name):
    try:
        return bool(unreal.MaterialEditingLibrary.get_material_instance_static_switch_parameter_value(mi, name))
    except Exception:
        return False


def _check(mi, grandfathered):
    tex = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(mi, TEX)
    if not tex:
        return None
    comp = tex.get_editor_property("compression_settings")
    srgb = tex.get_editor_property("srgb")
    rg = _switch(mi, SWITCH_RG)
    ext = _switch(mi, SWITCH_EXT)
    issues = []
    if srgb:
        issues.append(("ERROR", "sRGB on"))
    if comp in PRECISE:
        fmt = PRECISE[comp]
    elif comp in LOSSY_OK:
        fmt = LOSSY_OK[comp]
        issues.append(("WARN", fmt + " is lossy (fine for an 8-bit-class map, not a 16-bit bake)"))
    else:
        fmt = getattr(comp, "name", str(comp).split(".")[-1])
        issues.append(("ERROR", fmt + ": BC1 / BC3 block endpoints -> terraces along the terminator"))
    if rg and comp != TC.TC_VECTOR_DISPLACEMENTMAP:
        issues.append(("ERROR", "R+G Average needs an uncompressed RGBA8 texture (VectorDisplacementmap)"))
    if (comp in HIGH or rg) and not ext:
        issues.append(("WARN", ">8-bit map but Facial SDF Extended Payload is off: the lights still get 8 bits"))
    if tex.get_editor_property("mip_gen_settings") != unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS:
        issues.append(("INFO", "has mips; the SDF is read at mip 0, mips only cost memory"))
    path = mi.get_path_name().split(".")[0]
    grandfathered = grandfathered or any(tex.get_path_name().startswith(p) for p in KEEP_PREFIXES)
    if grandfathered and any(i[0] == "ERROR" for i in issues):
        issues = [("KEEP", "grandfathered: the current look depends on this import") ] + issues
    return {"mi": path, "texture": tex.get_path_name().split(".")[0], "format": fmt, "srgb": bool(srgb),
            "size": [tex.blueprint_get_size_x(), tex.blueprint_get_size_y()], "rg_average": rg,
            "extended_payload": ext, "issues": issues}


def _run():
    roots = globals().get("ROOTS", ["/Game", "/MoonToon"])
    keep = globals().get("GRANDFATHERED", ["/Game/Assets/Meshs/Test_ww_yy/"])
    KEEP_PREFIXES[:] = keep
    report = globals().get("REPORT", os.path.join(unreal.Paths.project_saved_dir(), "MoonToon", "FaceSdfCheck.json"))
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    rows = []
    for root in roots:
        flt = unreal.ARFilter(class_paths=[unreal.TopLevelAssetPath("/Script/Engine", "MaterialInstanceConstant")],
                              package_paths=[root], recursive_paths=True)
        for data in reg.get_assets(flt):
            mi = data.get_asset()
            if not mi or not _switch(mi, SWITCH_DFF) or _switch(mi, SWITCH_360):
                continue
            row = _check(mi, any(mi.get_path_name().startswith(p) for p in keep))
            if row:
                rows.append(row)
    os.makedirs(os.path.dirname(report), exist_ok=True)
    with open(report, "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=1, ensure_ascii=False)
    worst = {"ERROR": 0, "WARN": 0, "KEEP": 0}
    for r in rows:
        tags = {i[0] for i in r["issues"]}
        level = "KEEP" if "KEEP" in tags else ("ERROR" if "ERROR" in tags else ("WARN" if "WARN" in tags else "OK"))
        worst[level] = worst.get(level, 0) + 1
        print("%-5s %s  <- %s  [%s%s%s%s]" % (level, r["texture"], r["mi"], r["format"], ", sRGB" if r["srgb"] else "",
                                            ", R+G" if r["rg_average"] else "", ", ext" if r["extended_payload"] else ""))
        for lvl, msg in r["issues"]:
            if lvl != "KEEP":
                print("        %s %s" % (lvl, msg))
    print("%d face SDF instances: %s -> %s" % (len(rows), worst, report))


_run()
gc.collect()
