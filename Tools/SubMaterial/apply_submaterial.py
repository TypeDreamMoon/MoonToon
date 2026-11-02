"""Import a sub-material table + ID map and make the merged material instance (MoonToon T22).

Runs inside the Unreal Editor. Set before running:
    TABLE_EXR = "C:/.../T_SubMat_Down.exr"     # from build_table.py
    ID_PNG    = "C:/.../T_SubMat_Down_ID.png"  # from bake_submaterial_id.py (None for a constant / UV source)
    DEST      = "/Game/.../SubMaterial"        # folder for the textures and the merged instance
    NAME      = "Down"                         # T_SubMat_<NAME>, T_SubMat_<NAME>_ID, MI_SubMat_<NAME>
    SOURCE_MI = "/Game/.../MI_First_Slot"      # the merged instance starts as a copy of this one
    ASSIGN    = None or {"actor": "MT_WW", "slots": [14, 15, ...]}   # optional: put it on an actor's slots
                                                                     # (the level is not saved)

Texture settings: the table uncompressed float (TC_HDR), the ID map uncompressed 8 bit (TC_Grayscale);
both sRGB off, Nearest, no mips, never streamed -- the material Loads / point-samples them.
"""
import gc

import unreal

for _k in [k for k, v in list(globals().items()) if isinstance(v, unreal.Object)]:
    del globals()[_k]
gc.collect()


def _import(path, dest, name, compression):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", dest)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    tex = unreal.load_asset(dest + "/" + name)
    tex.set_editor_property("srgb", False)
    tex.set_editor_property("compression_settings", compression)
    tex.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    tex.set_editor_property("filter", unreal.TextureFilter.TF_NEAREST)
    tex.set_editor_property("never_stream", True)
    tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_PIXELS2D)
    unreal.EditorAssetLibrary.save_loaded_asset(tex, False)
    return tex


def _apply(table_exr, id_png, dest, name, source_mi, assign):
    mel = unreal.MaterialEditingLibrary
    eal = unreal.EditorAssetLibrary
    table = _import(table_exr, dest, "T_SubMat_" + name, unreal.TextureCompressionSettings.TC_HDR)
    idmap = _import(id_png, dest, "T_SubMat_%s_ID" % name, unreal.TextureCompressionSettings.TC_GRAYSCALE) if id_png else None
    mi_path = "%s/MI_SubMat_%s" % (dest, name)
    if eal.does_asset_exist(mi_path):
        eal.delete_asset(mi_path)
    mi = eal.duplicate_asset(source_mi, mi_path)
    mel.set_material_instance_static_switch_parameter_value(mi, "Use SubMaterial Table", True)
    mel.set_material_instance_texture_parameter_value(mi, "SubMaterial Table", table)
    if idmap:
        mel.set_material_instance_texture_parameter_value(mi, "SubMaterial ID Map", idmap)
        mel.set_material_instance_scalar_parameter_value(mi, "SubMaterial ID Source", 0.0)
    mel.update_material_instance(mi)
    eal.save_loaded_asset(mi, False)
    print("made", mi_path, "errors:", unreal.UnrealBridgeMaterialLibrary.get_material_compile_errors(mi_path, "SM6", "High")
          if hasattr(unreal, "UnrealBridgeMaterialLibrary") else "-")
    if assign:
        act = [x for x in unreal.EditorLevelLibrary.get_all_level_actors() if x.get_actor_label() == assign["actor"]][0]
        comp = act.get_component_by_class(unreal.SkeletalMeshComponent)
        for s in assign["slots"]:
            comp.set_material(s, mi)
        print("assigned to %s slots %s" % (assign["actor"], assign["slots"]))


_apply(TABLE_EXR, globals().get("ID_PNG"), DEST, NAME, SOURCE_MI, globals().get("ASSIGN"))
gc.collect()
