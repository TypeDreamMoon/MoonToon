#!/usr/bin/env python3
"""Generate one DreamShaderLang material function per toon feature.

The (feature x slot) mapping is NOT stated here. It is read out of the engine's slot table,
Engine/Shaders/Shared/MoonToonFeatureSlots.h, so the material-side writer and the shader-side
reader cannot drift -- which is exactly what happened to the four hand-maintained copies this
replaces.

What IS stated here is the material-facing metadata the engine has no opinion about: the parameter
name an artist sees, its group, and its default. Existing parameter names are preserved verbatim so
material instances keep their overrides.

Usage:  python Tools/gen_feature_functions.py [--check]
"""

import argparse
import os
import re
import sys

SLOT_TABLE = r"F:\UnrealEngine\UE_Moon\Engine\Shaders\Shared\MoonToonFeatureSlots.h"
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "DShader", "MaterialFunctions", "Features")

# feature key -> (dsf name, MOON_SHADING_FEATURE_ID_*, slot-table macro, parameter group)
FEATURES = {
    "Default":           ("Default",           "MOON_SHADING_FEATURE_ID_DEFAULT",
                          "MOON_TOON_SLOTS_DEFAULT",             "MT >> 00 << Default"),
    "PBRSpecular":       ("PBRSpecular",       "MOON_SHADING_FEATURE_ID_PBR_SPECULAR",
                          "MOON_TOON_SLOTS_PBR_SPECULAR",        "MT >> 00 << Default"),
    "KajiyaHair":        ("KajiyaHair",        "MOON_SHADING_FEATURE_ID_KAJIYA_HAIR_SPECULAR",
                          "MOON_TOON_SLOTS_KAJIYA_HAIR",         "MT >> 02 << Hair Kajiya Kay"),
    "ToonKajiyaHair":    ("ToonKajiyaHair",    "MOON_SHADING_FEATURE_ID_TOON_KAJIYA_HAIR_SPECULAR",
                          "MOON_TOON_SLOTS_KAJIYA_HAIR",         "MT >> 03 << Hair Toon Kajiya Kay"),
    "DFFacialShadow":    ("DFFacialShadow",    "MOON_SHADING_FEATURE_ID_DISTANCE_FIELD_FACIAL_SHADOW",
                          "MOON_TOON_SLOTS_SKIN",                "MT >> 04b << SDF Face"),
    "HairHighlightMask": ("HairHighlightMask", "MOON_SHADING_FEATURE_ID_HAIR_HIGHLIGHT_MASK",
                          "MOON_TOON_SLOTS_HAIR_HIGHLIGHT_MASK", "MT >> 01 << Hair Highlight Mask"),
    "Skin":              ("Skin",              "MOON_SHADING_FEATURE_ID_SKIN",
                          "MOON_TOON_SLOTS_SKIN",                "MT >> 04 << Skin"),
    "Stockings":         ("Stockings",         "MOON_SHADING_FEATURE_ID_PBR_STOCKINGS",
                          "MOON_TOON_SLOTS_PBR_STOCKINGS",       "MT >> 05 << Stockings"),
    "Eye":               ("Eye",               "MOON_SHADING_FEATURE_ID_EYE",
                          "MOON_TOON_SLOTS_EYE",                 "MT >> 06 << Eye"),
    "ClothVelvet":       ("ClothVelvet",       "MOON_SHADING_FEATURE_ID_CLOTH_VELVET",
                          "MOON_TOON_SLOTS_CLOTH_VELVET",        "MT >> 07 << Cloth Velvet"),
    "ToonMetal":         ("ToonMetal",         "MOON_SHADING_FEATURE_ID_TOON_METAL",
                          "MOON_TOON_SLOTS_TOON_METAL",          "MT >> 08 << Metal"),
    "EmissiveInk":       ("EmissiveInk",       "MOON_SHADING_FEATURE_ID_TOON_EMISSIVE_INK",
                          "MOON_TOON_SLOTS_TOON_EMISSIVE_INK",   "MT >> 09 << Emissive Ink"),
    "Matcap":            ("Matcap",            "MOON_SHADING_FEATURE_ID_MATCAP",
                          "MOON_TOON_SLOTS_MATCAP",              "MT >> 10 << Matcap"),
}

# slot Name (from the engine table) -> (material parameter name, default literal).
# Names that already exist keep their spelling so MI overrides survive; NEW marks a slot the
# material could not reach before this refactor.
PARAMS = {
    "Default": {
        "MultiBandCelEnable":      ("In_Default_MultiBandCel", "0.0"),          # NEW
        "TerminatorWidth":         ("In_Default_TerminatorWidth", "0.0"),       # NEW
        "BloomWeight":             ("In_Default_BloomWeight", "0.0"),           # NEW
    },
    "PBRSpecular": {
        "BloomWeight":             ("In_PBRSpecular_BloomWeight", "0.0"),       # NEW
    },
    "KajiyaHair": {
        "TangentRotate":           ("In_Hair_TangentRotate", "0.0"),
        "SecondaryShift":          ("In_Hair_SecondaryShift", "0.0"),
        "SecondaryMask":           ("In_Hair_SecondaryMask", "0.0"),
        "PrimaryShift":            ("In_Hair_PrimaryShift", "0.0"),
        "PrimaryExpScale":         ("In_Hair_PrimaryExpScale", "1.0"),
        "SecondaryExpScale":       ("In_Hair_SecondaryExpScale", "1.0"),
        "OverallIntensity":        ("In_Hair_OverallSpecIntensity", "0.0"),
        "PrimaryIntensity":        ("In_Hair_PrimaryIntensity", "0.0"),
        "SecondaryIntensity":      ("In_Hair_SecondaryIntensity", "0.0"),
        "SecondaryShiftScale":     ("In_Hair_SecondaryShiftScale", "1.0"),
        "ShadowStrength":          ("In_Hair_ShadowStrength", "0.0"),
    },
    "ToonKajiyaHair": {
        "TangentRotate":           ("In_ToonHair_TangentRotate", "0.0"),
        "SecondaryShift":          ("In_ToonHair_SecondaryShift", "0.0"),
        "SecondaryMask":           ("In_ToonHair_SecondaryMask", "0.0"),
        "PrimaryShift":            ("In_ToonHair_PrimaryStrandShift", "0.0"),
        "PrimaryExpScale":         ("In_ToonHair_PrimaryExpScale", "1.0"),
        "SecondaryExpScale":       ("In_ToonHair_SecondaryExpScale", "1.0"),
        "OverallIntensity":        ("In_ToonHair_OverallSpecIntensity", "0.0"),
        "PrimaryIntensity":        ("In_ToonHair_PrimaryIntensity", "0.0"),
        "SecondaryIntensity":      ("In_ToonHair_SecondaryIntensity", "0.0"),
        "SecondaryShiftScale":     ("In_ToonHair_SecondaryShiftScale", "1.0"),
        "ShadowStrength":          ("In_ToonHair_ShadowStrength", "0.0"),
    },
    # DFF shades through the skin branch, so it needs the skin slots -- and could not write ANY of
    # them before, which is why every SDF face had zero toon specular.
    "DFFacialShadow": {
        "ScatterStrength":         ("In_SdfFace_ScatterStrength", "0.0"),       # NEW
        "TransmissionStrength":    ("In_SdfFace_TransStrength", "0.0"),         # NEW
        "TransmissionPower":       ("In_SdfFace_TransPower", "0.0"),            # NEW
        "PrimaryRoughnessScale":   ("In_SdfFace_PrimaryRoughnessScale", "1.0"), # NEW
        "SecondaryRoughnessScale": ("In_SdfFace_SecondaryRoughnessScale", "1.0"),# NEW
        "SecondaryRampOffset":     ("In_SdfFace_SecondaryRampOffset", "0.0"),   # NEW
        "OverallSpecIntensity":    ("In_SdfFace_OverallSpecIntensity", "0.0"),  # NEW
        "PrimaryLobeIntensity":    ("In_SdfFace_PrimaryLobeIntensity", "0.0"),  # NEW
        "SecondaryLobeIntensity":  ("In_SdfFace_SecondaryLobeIntensity", "0.0"),# NEW
        "SpecShadowStrength":      ("In_SdfFace_SpecShadowStrength", "0.0"),    # NEW
        "WarmTintStrength":        ("In_SdfFace_WarmTintStrength", "0.0"),      # NEW
    },
    "HairHighlightMask": {
        "Mask":                    ("In_Hair_HighlightMask", "0.0"),
        "Intensity":               ("In_Hair_HighlightMaskIntensity", "0.0"),
        "ShadowIntensity":         ("In_Hair_HighlightMaskShadowIntensity", "0.0"),
        "ViewAnchorBlend":         ("In_Hair_HighlightViewAnchorBlend", "0.0"), # NEW
    },
    "Skin": {
        "ScatterStrength":         ("In_Skin_ScatterStrength", "0.0"),
        "TransmissionStrength":    ("In_Skin_TransStrength", "0.0"),
        "TransmissionPower":       ("In_Skin_TransPower", "0.0"),
        "PrimaryRoughnessScale":   ("In_Skin_PrimaryRoughnessScale", "1.0"),
        "SecondaryRoughnessScale": ("In_Skin_SecondaryRoughnessScale", "1.0"),
        "SecondaryRampOffset":     ("In_Skin_SecondaryRampOffset", "0.0"),
        "OverallSpecIntensity":    ("In_Skin_OverallSpecIntensity", "0.0"),
        "PrimaryLobeIntensity":    ("In_Skin_PrimaryLobeIntensity", "0.0"),
        "SecondaryLobeIntensity":  ("In_Skin_SecondaryLobeIntensity", "0.0"),
        "SpecShadowStrength":      ("In_Skin_SpecShadowStrength", "0.0"),
        "WarmTintStrength":        ("In_Skin_WarmTintStrength", "0.0"),
    },
    "Stockings": {
        "Density":                 ("In_Stockings_Density", "0.0"),
        "FresnelPower":            ("In_Stockings_FresnelPower", "0.0"),
        "TransmissionStrength":    ("In_Stockings_TransStrength", "0.0"),
        "RoughnessScale":          ("In_Stockings_RoughnessScale", "1.0"),
        "SpecularIntensity":       ("In_Stockings_SpecIntensity", "0.0"),
        "ShadowStrength":          ("In_Stockings_ShadowStrength", "0.0"),
        "GrazingDarkenStrength":   ("In_Stockings_GrazingDarken", "0.0"),
        "BaseColor":               ("In_Stockings_BodyColorRGB", "float4(0.0, 0.0, 0.0, 0.0)"),
        "GrazingSpecBoost":        ("In_Stockings_GrazingSpecBoost", "0.0"),
    },
    "Eye": {
        "DiffuseWrapStrength":     ("In_Eye_DiffuseWrapStrength", "0.0"),
        "LimbalDarkenStrength":    ("In_Eye_LimbalDarkenStrength", "0.0"),
        "SpecularShadowStrength":  ("In_Eye_SpecularShadowStrength", "0.0"),
        "PrimaryRoughnessScale":   ("In_Eye_PrimaryRoughnessScale", "1.0"),
        "SecondaryRoughnessScale": ("In_Eye_SecondaryRoughnessScale", "1.0"),
        "PrimaryIntensity":        ("In_Eye_PrimaryIntensity", "0.0"),
        "SecondaryIntensity":      ("In_Eye_SecondaryIntensity", "0.0"),
        "EyeTint":                 ("In_Eye_EyeTintRGB", "float4(0.0, 0.0, 0.0, 0.0)"),
        "SecondaryRampOffset":     ("In_Eye_SecondaryRampOffset", "0.0"),
    },
    "ClothVelvet": {
        "WrapStrength":            ("In_ClothVelvet_DiffuseWrapStrength", "0.0"),
        "SheenPower":              ("In_ClothVelvet_SheenPower", "0.0"),
        "SheenShadowStrength":     ("In_ClothVelvet_SheenShadowStrength", "0.0"),
        "RoughnessScale":          ("In_ClothVelvet_RoughnessScale", "1.0"),
        "SheenIntensity":          ("In_ClothVelvet_SheenIntensity", "0.0"),
        "RetroDiffuseStrength":    ("In_ClothVelvet_RetroDiffuseStrength", "0.0"),
        "GrazingDarkenStrength":   ("In_ClothVelvet_GrazingDarkenStrength", "0.0"),
        "SheenTint":               ("In_ClothVelvet_SheenTintRGB", "float4(1.0, 1.0, 1.0, 1.0)"),
        "RimBias":                 ("In_ClothVelvet_RimBias", "0.0"),
    },
    "ToonMetal": {
        "DiffuseIntensity":        ("In_ToonMetal_DiffuseIntensity", "0.0"),
        "ShadowSpecularFloor":     ("In_ToonMetal_ShadowSpecularFloor", "0.0"),
        "RampOffset":              ("In_ToonMetal_RampOffset", "0.0"),
        "RoughnessScale":          ("In_ToonMetal_RoughnessScale", "1.0"),
        "SpecularIntensity":       ("In_ToonMetal_SpecularIntensity", "0.0"),
        "EdgeBoost":               ("In_ToonMetal_EdgeBoost", "0.0"),
        "SpecularContrast":        ("In_ToonMetal_SpecularContrast", "1.0"),
        "MetalTint":               ("In_ToonMetal_MetalTintRGB", "float4(1.0, 1.0, 1.0, 1.0)"),
    },
    "EmissiveInk": {
        "LightInfluence":          ("In_ToonEmissiveInk_LightInfluence", "0.0"),
        "ShadowLift":              ("In_ToonEmissiveInk_ShadowLift", "0.0"),
        "InkDarkenStrength":       ("In_ToonEmissiveInk_InkDarkenStrength", "0.0"),
        "RoughnessScale":          ("In_ToonEmissiveInk_RoughnessScale", "1.0"),
        "SpecularIntensity":       ("In_ToonEmissiveInk_SpecularIntensity", "0.0"),
        "EdgeBoost":               ("In_ToonEmissiveInk_EdgeBoost", "0.0"),
        "InkBlend":                ("In_ToonEmissiveInk_InkBlend", "0.0"),
        "InkTint":                 ("In_ToonEmissiveInk_InkTintRGB", "float4(0.0, 0.0, 0.0, 0.0)"),
        "RimBias":                 ("In_ToonEmissiveInk_RimBias", "0.0"),
    },
    "Matcap": {
        "Intensity":               ("In_Matcap_Intensity", "0.0"),              # NEW
        "MatcapColor":             ("In_Matcap_ColorRGB", "float4(0.0, 0.0, 0.0, 0.0)"),  # NEW
    },
}

HEADER = """// GENERATED from Engine/Shaders/Shared/MoonToonFeatureSlots.h by Tools/gen_feature_functions.py.
// Edit the slot table (for the mapping) or the generator (for names/defaults), not this file.
//
// One toon feature, one material function. It declares only its own slots, so a material instance
// that selected a different feature never shows these parameters -- that pruning is why feature
// selection had to become static.
"""


def parse_slot_tables(path):
    """feature-macro -> [(Type, Slot, Name)], following one level of table aliasing."""
    src = open(path, encoding="utf-8").read()
    tables, alias = {}, {}
    for m in re.finditer(r"#define\s+(MOON_TOON_SLOTS_\w+)\(X\)((?:[^\n]*\\\n)*[^\n]*)", src):
        name, body = m.group(1), m.group(2)
        rows = re.findall(r"X\(\s*(\w+)\s*,\s*([\w.]+)\s*,\s*(\w+)\s*\)", body)
        if rows:
            tables[name] = rows
        else:
            ref = re.search(r"(MOON_TOON_SLOTS_\w+)\(X\)", body)
            if ref:
                alias[name] = ref.group(1)
    for a, target in alias.items():
        tables[a] = tables[target]
    return tables


def emit(feature_key, tables):
    dsf_name, id_macro, table_macro, group = FEATURES[feature_key]
    rows = tables[table_macro]
    params = PARAMS[feature_key]

    missing = [n for (_, _, n) in rows if n not in params]
    if missing:
        raise SystemExit("%s: slot table has %s with no material parameter mapping"
                         % (feature_key, ", ".join(missing)))

    fn = "ToonFeatureWrite_%s" % dsf_name
    args = ",\n".join("\tin %s %s" % ("float3" if t == "float3" else "float", n)
                      for (t, _, n) in rows)
    body = "\n".join("\tTBuffer.%s = %s;" % (slot, name) for (_, slot, name) in rows)

    prop_lines, call_lines = [], []
    for i, (t, _, name) in enumerate(rows):
        pname, default = params[name]
        kind = "VectorParameter" if t == "float3" else "ScalarParameter"
        prop_lines.append('\t\t%s %s = %s [Group="%s"; SortPriority=%d];'
                          % (kind, pname, default, group, i))
        call_lines.append("\t\t\t%s%s," % (pname, ".rgb" if t == "float3" else ""))

    return """%s
Function %s(
%s,
\tout float4 TBufferA, out float4 TBufferB, out float4 TBufferC)
{
\tFToonBuffer TBuffer = (FToonBuffer)0;
\tInitToonBuffer(TBuffer);

\tTBuffer.ShadingFeatureID = %s;
%s

\tEncodeToonBuffer(TBuffer, TBufferA, TBufferB, TBufferC);
}

ShaderFunction(Name="MaterialFunctions/Features/MF_ToonFeature_%s", Root="Plugin.MoonToon")
{
\tSettings = {
\t\tDescription       = "Packs the %s toon feature parameters into ToonBufferA/B/C.";
\t\tExposeToLibrary   = true;
\t\tLibraryCategories = "MoonToon|Features";
\t}

\tInputs = {
\t}

\tOutputs = {
\t\tfloat4 ToonBufferA;
\t\tfloat4 ToonBufferB;
\t\tfloat4 ToonBufferC;
\t}

\tProperties = {
%s
\t}

\tGraph = {
\t\t%s(
%s
\t\t\tToonBufferA, ToonBufferB, ToonBufferC);
\t}
}
""" % (HEADER, fn, args, id_macro, body, dsf_name, dsf_name,
       "\n".join(prop_lines), fn, "\n".join(call_lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="fail if any generated file differs from what is on disk")
    opts = ap.parse_args()

    tables = parse_slot_tables(SLOT_TABLE)
    out_dir = os.path.normpath(OUT_DIR)
    os.makedirs(out_dir, exist_ok=True)

    stale = []
    for key in FEATURES:
        path = os.path.join(out_dir, "MF_ToonFeature_%s.dsf" % FEATURES[key][0])
        text = emit(key, tables)
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old == text:
            continue
        if opts.check:
            stale.append(os.path.basename(path))
        else:
            open(path, "w", encoding="utf-8", newline="\n").write(text)
            print("wrote %s (%d slots)" % (os.path.basename(path), len(tables[FEATURES[key][2]])))

    if stale:
        print("stale, re-run the generator: %s" % ", ".join(stale))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
