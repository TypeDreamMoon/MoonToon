"""Move M_MoonToon instances from the old Hair Highlight Mask feature to the new angel ring (T17, G08).

Runs inside the Unreal Editor (Python console, `py`, or UnrealBridge). Set before running:

    ROOTS = ["/Game"]        # content folders to scan (recursive)
    DRY_RUN = True           # True: only report; False: change and save

Per instance whose static switch "Is HairHighlightMask" is on (set on the instance or inherited):
  * "Is HairHighlightMask" off, "Is AngelRing" on;
  * "Angel Ring Legacy Mask" on, "Angel Ring Legacy Mask Map" = the Global Mask Map the old mask came
    from ("Hair Highlight Mask Source" 1/2/3). The legacy mode reads .r; any other channel is reported
    and left for a hand fix (repack the mask or point the map at a texture with it in .r);
  * "Angel Ring Intensity" = the old intensity, "Angel Ring Shadow Floor" = the old shadow retention.
The old look is not reproduced exactly: the ring now slides with the view (Mode A view slide) and is
lit by the trunk's angel-ring rules (main light only, shadow floor, dark band). Check each instance.
"""
import unreal

MEL = unreal.MaterialEditingLibrary
BASE = "/MoonToon/Materials/Core/M_MoonToon"
GLOBAL_MASK_NAMES = {1: ["Global Mask Map", "Global Mask Map 1"], 2: ["Global Mask Map 2"], 3: ["Global Mask Map 3"]}


def _scalar(mi, names, default=None):
    have = [str(n) for n in MEL.get_scalar_parameter_names(mi.get_base_material())]
    for n in names:
        if n in have:
            return MEL.get_material_instance_scalar_parameter_value(mi, n)
    return default


def _texture(mi, names):
    have = [str(n) for n in MEL.get_texture_parameter_names(mi.get_base_material())]
    for n in names:
        if n in have:
            return MEL.get_material_instance_texture_parameter_value(mi, n)
    return None


def _channel(mi):
    try:
        names = [str(n) for n in MEL.get_vector_parameter_names(mi.get_base_material())]
        if "Hair Highlight Mask Channel" in names:
            c = MEL.get_material_instance_vector_parameter_value(mi, "Hair Highlight Mask Channel")
            return (c.r, c.g, c.b, c.a)
    except Exception:
        pass
    return None


def run(roots, dry_run):
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    found = 0
    for root in roots:
        for data in ar.get_assets_by_path(root, recursive=True):
            if str(data.asset_class_path.asset_name) != "MaterialInstanceConstant":
                continue
            mi = unreal.load_asset(str(data.package_name))
            base = mi.get_base_material()
            if base is None or base.get_path_name().split(".")[0] != BASE:
                continue
            if not MEL.get_material_instance_static_switch_parameter_value(mi, "Is HairHighlightMask"):
                continue
            found += 1
            source = int(round(_scalar(mi, ["Hair Highlight Mask Source"], 1.0) or 1.0))
            mask_tex = _texture(mi, GLOBAL_MASK_NAMES.get(source, []))
            channel = _channel(mi)
            intensity = _scalar(mi, ["In_Hair_HighlightMaskIntensity", "In Hair HighlightMaskIntensity"], 0.0)
            shadow = _scalar(mi, ["In_Hair_HighlightMaskShadowIntensity", "In Hair HighlightMaskShadowIntensity"], 0.0)
            note = ""
            if shadow is not None and not (0.0 <= shadow <= 1.0):
                note += " shadow %.3g clamped to the 0-1 floor" % shadow
            if channel is not None and channel[0] < 0.5:
                note += " CHANNEL %s is not R -- fix the legacy mask map by hand" % (channel,)
            print("%s %s: source %d map %s intensity %s shadow %s%s" % ("would migrate" if dry_run else "migrate",
                  data.package_name, source, mask_tex.get_name() if mask_tex else None, intensity, shadow, note))
            if dry_run:
                continue
            MEL.set_material_instance_static_switch_parameter_value(mi, "Is HairHighlightMask", False)
            MEL.set_material_instance_static_switch_parameter_value(mi, "Is AngelRing", True)
            MEL.set_material_instance_static_switch_parameter_value(mi, "Angel Ring Legacy Mask", True)
            if mask_tex:
                MEL.set_material_instance_texture_parameter_value(mi, "Angel Ring Legacy Mask Map", mask_tex)
            MEL.set_material_instance_scalar_parameter_value(mi, "Angel Ring Intensity", float(intensity or 0.0))
            # The old retention could exceed 1; the floor is a 0..1 lerp weight.
            MEL.set_material_instance_scalar_parameter_value(mi, "Angel Ring Shadow Floor", min(max(float(shadow or 0.0), 0.0), 1.0))
            MEL.update_material_instance(mi)
            unreal.EditorAssetLibrary.save_loaded_asset(mi, False)
    print("%d instance(s) %s" % (found, "found" if dry_run else "migrated"))


run(globals().get("ROOTS", ["/Game"]), globals().get("DRY_RUN", True))
