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
ID_TABLE = os.path.join(os.path.dirname(SLOT_TABLE), "MoonToonShadingFeatureDefinitions.h")
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

# Slots that can also be driven per-pixel from a map, on top of their constant parameter.
# feature key -> {slot Name: (function input name, selector input name)}
#
# The old writer took these as pins and the constant was either added to them (Toon Kajiya's strand
# shifts) or missing entirely (the hair highlight mask, whose In_Hair_HighlightMask parameter was
# declared and then never passed -- dead since it was written). Everything is input + parameter now,
# which reproduces the old result whenever the parameter is at its 0 default.
MAP_INPUTS = {
    "HairHighlightMask": {
        "Mask": ("InHighlightMask", "InHairHighlightMask"),
    },
    "ToonKajiyaHair": {
        "PrimaryShift":   ("InPrimaryStrandShift", "InToonKajiyaPrimaryStrandShift"),
        "SecondaryShift": ("InSecondaryStrandShift", "InToonKajiyaSecondaryStrandShift"),
    },
    "Stockings": {
        "BaseColor": ("InBodyColor", "InStockingsBodyColor"),
    },
}

# Colours for the "Debug Shading Feature" base-material view. Kept exactly as the old writer's
# DebugColor branches so the view still reads the same.
DEBUG_COLORS = {
    "Default":           "float4(0.0, 0.0, 0.0, 1.0)",
    "PBRSpecular":       "float4(0.25, 0.25, 0.25, 1.0)",
    "KajiyaHair":        "float4(0.0, 1.0, 0.0, 1.0)",
    "ToonKajiyaHair":    "float4(0.0, 0.0, 1.0, 1.0)",
    "DFFacialShadow":    "float4(1.0, 1.0, 1.0, 1.0)",
    "HairHighlightMask": "float4(1.0, 0.0, 0.0, 1.0)",
    "Skin":              "float4(1.0, 1.0, 0.0, 1.0)",
    "Stockings":         "float4(1.0, 0.0, 1.0, 1.0)",
    "Eye":               "float4(0.0, 1.0, 1.0, 1.0)",
    "ClothVelvet":       "float4(0.5, 1.0, 0.5, 1.0)",
    "ToonMetal":         "float4(1.0, 0.5, 0.5, 1.0)",
    "EmissiveInk":       "float4(0.5, 0.5, 1.0, 1.0)",
    "Matcap":            "float4(1.0, 0.75, 0.0, 1.0)",
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

    maps = MAP_INPUTS.get(feature_key, {})

    prop_lines, call_lines, input_lines = [], [], []
    for i, (t, _, name) in enumerate(rows):
        pname, default = params[name]
        kind = "VectorParameter" if t == "float3" else "ScalarParameter"
        prop_lines.append('\t\t%s %s = %s [Group="%s"; SortPriority=%d];'
                          % (kind, pname, default, group, i))
        expr = "%s%s" % (pname, ".rgb" if t == "float3" else "")
        if name in maps:
            fn_in = maps[name][0]
            input_lines.append(
                '\t\topt %s %s = %s [Description="Per-pixel %s from a map, added to the parameter."];'
                % ("float3" if t == "float3" else "float", fn_in,
                   "float3(0.0, 0.0, 0.0)" if t == "float3" else "0.0", name))
            expr = "%s + %s" % (fn_in, expr)
        call_lines.append("\t\t\t%s," % expr)

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
%s
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
       "\n".join(input_lines), "\n".join(prop_lines), fn, "\n".join(call_lines))


SELECT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "DShader", "MaterialFunctions", "MF_MoonToonFeatureSelect.dsf")

# Default is the fallback rather than a checkbox: an instance with nothing ticked shades as plain
# toon, which is what a fresh material should do.
FALLBACK = "Default"


def _feature_inputs(key, tables):
    """[(type, function input name, selector input name)] in slot-table order."""
    maps = MAP_INPUTS.get(key, {})
    if not maps:
        return []
    order = [n for (_, _, n) in tables[FEATURES[key][2]] if n in maps]
    return [("float3" if dict((n, t) for (t, _, n) in tables[FEATURES[key][2]])[n] == "float3"
             else "float", maps[n][0], maps[n][1]) for n in order]


def parse_feature_ids(path):
    """MOON_SHADING_FEATURE_ID_* -> numeric value, straight from the shared definition header."""
    src = open(path, encoding="utf-8").read()
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"#define\s+(MOON_SHADING_FEATURE_ID_\w+)\s+(\d+)", src)}


def emit_selector(tables, ids):
    picks = [k for k in FEATURES if k != FALLBACK]

    def decl(k):
        ins = _feature_inputs(k, tables)
        in_block = "\n".join('\t\topt %s %s = %s;'
                             % (t, fn_in, "float3(0.0, 0.0, 0.0)" if t == "float3" else "0.0")
                             for (t, fn_in, _) in ins)
        return ('VirtualFunction(Name="MF_ToonFeature_%s")\n'
                "{\n"
                "\tOptions = {\n"
                '\t\tAsset = Path(Plugins.MoonToon, "MaterialFunctions/Features/MF_ToonFeature_%s");\n'
                "\t}\n\n"
                "\tInputs = {\n%s\n\t}\n\n"
                "\tOutputs = {\n\t\tfloat4 ToonBufferA;\n\t\tfloat4 ToonBufferB;\n\t\tfloat4 ToonBufferC;\n\t}\n"
                "}" % (FEATURES[k][0], FEATURES[k][0], in_block))

    decls = "\n".join(decl(k) for k in FEATURES)

    # Selector-level inputs: the union of every feature's map-driven slots, deduplicated.
    sel_inputs, seen = [], set()
    for k in FEATURES:
        for (t, _, sel_in) in _feature_inputs(k, tables):
            if sel_in not in seen:
                seen.add(sel_in)
                sel_inputs.append((t, sel_in))
    sel_in_block = "\n".join(
        '\t\topt %s %s = %s [Description="Per-pixel value from a map; added to the matching parameter."];'
        % (t, n, "float3(0.0, 0.0, 0.0)" if t == "float3" else "0.0") for (t, n) in sel_inputs)

    props = "\n".join(
        '\t\tStaticSwitchParameter Feature_Is_%s = false [Group="MT >> __ << Shading Feature"; SortPriority=%d];'
        % (FEATURES[k][0], i) for i, k in enumerate(picks))

    def call(k, out_index):
        args = "".join("%s, " % sel_in for (_, _, sel_in) in _feature_inputs(k, tables))
        return "MF_ToonFeature_%s(%sOutputIndex=%d)" % (FEATURES[k][0], args, out_index)

    def chain(out_index, depth=0):
        if depth == len(picks):
            return call(FALLBACK, out_index)
        k = picks[depth]
        pad = "\t\t\t" + "\t" * depth
        return ("Feature_Is_%s(\n%sTrue = %s,\n%sFalse = %s)"
                % (FEATURES[k][0], pad, call(k, out_index), pad, chain(out_index, depth + 1)))

    def id_chain(depth=0):
        if depth == len(picks):
            return "%d.0" % ids[FEATURES[FALLBACK][1]]
        k = picks[depth]
        pad = "\t\t\t" + "\t" * depth
        return ("Feature_Is_%s(\n%sTrue = %d.0,\n%sFalse = %s)"
                % (FEATURES[k][0], pad, ids[FEATURES[k][1]], pad, id_chain(depth + 1)))

    body = "\n\n".join("\t\t%s = %s;" % (name, chain(i))
                       for i, name in enumerate(["ToonBufferA", "ToonBufferB", "ToonBufferC"]))
    def const_chain(values, depth=0):
        if depth == len(picks):
            return values[FALLBACK]
        k = picks[depth]
        pad = "\t\t\t" + "\t" * depth
        return ("Feature_Is_%s(\n%sTrue = %s,\n%sFalse = %s)"
                % (FEATURES[k][0], pad, values[k], pad, const_chain(values, depth + 1)))

    body += "\n\n\t\tShadingFeatureID = %s;" % id_chain()
    body += "\n\n\t\tDebugColor = %s;" % const_chain(DEBUG_COLORS)

    return """// GENERATED by Tools/gen_feature_functions.py. Do not edit.
//
// Static feature selection. Only the selected branch survives compilation, and -- the point of
// making the choice static -- only its parameters survive the material instance panel:
// GetVisibleMaterialParametersFromExpression descends the taken side of a static switch and no
// other, so an instance shows ~11 feature rows instead of all 80.
//
// Exactly one switch should be on. If several are, the topmost in this file wins; if none are, the
// material shades as plain toon.

%s

ShaderFunction(Name="MaterialFunctions/MF_MoonToonFeatureSelect", Root="Plugin.MoonToon")
{
\tSettings = {
\t\tDescription       = "Selects one toon shading feature and packs its parameters into ToonBufferA/B/C.";
\t\tExposeToLibrary   = true;
\t\tLibraryCategories = "MoonToon";
\t}

\tInputs = {
%s
\t}

\tOutputs = {
\t\tfloat4 ToonBufferA;
\t\tfloat4 ToonBufferB;
\t\tfloat4 ToonBufferC;
\t\t// The same static choice that picked the branch above, as a raw feature id. Feeding
\t\t// MoonEncodeToonAttributes from here instead of from a separate scalar parameter is what stops
\t\t// TBufferA.x and the material-attribute copy of the id from being able to disagree.
\t\tfloat ShadingFeatureID;
\t\t// Per-feature flat colour for the base material's "Debug Shading Feature" view.
\t\tfloat4 DebugColor;
\t}

\tProperties = {
%s
\t}

\tGraph = {
%s
\t}
}
""" % (decls, sel_in_block, props, body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="fail if any generated file differs from what is on disk")
    opts = ap.parse_args()

    tables = parse_slot_tables(SLOT_TABLE)
    out_dir = os.path.normpath(OUT_DIR)
    os.makedirs(out_dir, exist_ok=True)

    targets = [(os.path.join(out_dir, "MF_ToonFeature_%s.dsf" % FEATURES[k][0]), emit(k, tables))
               for k in FEATURES]
    ids = parse_feature_ids(os.path.normpath(ID_TABLE))
    targets.append((os.path.normpath(SELECT_PATH), emit_selector(tables, ids)))

    stale = []
    for path, text in targets:
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old == text:
            continue
        if opts.check:
            stale.append(os.path.basename(path))
        else:
            open(path, "w", encoding="utf-8", newline="\n").write(text)
            print("wrote %s" % os.path.basename(path))

    if stale:
        print("stale, re-run the generator: %s" % ", ".join(stale))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
