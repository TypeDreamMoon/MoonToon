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
                          "MOON_TOON_SLOTS_DEFAULT",             "31 - Feature: Default"),
    "PBRSpecular":       ("PBRSpecular",       "MOON_SHADING_FEATURE_ID_PBR_SPECULAR",
                          "MOON_TOON_SLOTS_PBR_SPECULAR",        "32 - Feature: PBR Specular"),
    "KajiyaHair":        ("KajiyaHair",        "MOON_SHADING_FEATURE_ID_KAJIYA_HAIR_SPECULAR",
                          "MOON_TOON_SLOTS_KAJIYA_HAIR",         "34 - Feature: Hair Kajiya Kay"),
    "ToonKajiyaHair":    ("ToonKajiyaHair",    "MOON_SHADING_FEATURE_ID_TOON_KAJIYA_HAIR_SPECULAR",
                          "MOON_TOON_SLOTS_KAJIYA_HAIR",         "35 - Feature: Hair Toon Kajiya Kay"),
    "DFFacialShadow":    ("DFFacialShadow",    "MOON_SHADING_FEATURE_ID_DISTANCE_FIELD_FACIAL_SHADOW",
                          "MOON_TOON_SLOTS_SKIN",                "37 - Feature: SDF Face"),
    "HairHighlightMask": ("HairHighlightMask", "MOON_SHADING_FEATURE_ID_HAIR_HIGHLIGHT_MASK",
                          "MOON_TOON_SLOTS_HAIR_HIGHLIGHT_MASK", "33 - Feature: Hair Highlight Mask"),
    "Skin":              ("Skin",              "MOON_SHADING_FEATURE_ID_SKIN",
                          "MOON_TOON_SLOTS_SKIN",                "36 - Feature: Skin"),
    "Stockings":         ("Stockings",         "MOON_SHADING_FEATURE_ID_PBR_STOCKINGS",
                          "MOON_TOON_SLOTS_PBR_STOCKINGS",       "38 - Feature: Stockings"),
    # Same slots, same parameter NAMES, same panel group as Stockings -- only the lighting path
    # differs (ToonBxDF sends this id through the diffuse ramp and the per-light cel band). Sharing
    # the names is what makes the switch non-destructive: tick the other box and every tuned value
    # is still there, because there is only one set of parameters underneath both.
    "NPRStockings":      ("NPRStockings",      "MOON_SHADING_FEATURE_ID_NPR_STOCKINGS",
                          "MOON_TOON_SLOTS_NPR_STOCKINGS",       "38 - Feature: Stockings"),
    "Eye":               ("Eye",               "MOON_SHADING_FEATURE_ID_EYE",
                          "MOON_TOON_SLOTS_EYE",                 "39 - Feature: Eye"),
    "ClothVelvet":       ("ClothVelvet",       "MOON_SHADING_FEATURE_ID_CLOTH_VELVET",
                          "MOON_TOON_SLOTS_CLOTH_VELVET",        "40 - Feature: Cloth Velvet"),
    "ToonMetal":         ("ToonMetal",         "MOON_SHADING_FEATURE_ID_TOON_METAL",
                          "MOON_TOON_SLOTS_TOON_METAL",          "41 - Feature: Metal"),
    "EmissiveInk":       ("EmissiveInk",       "MOON_SHADING_FEATURE_ID_TOON_EMISSIVE_INK",
                          "MOON_TOON_SLOTS_TOON_EMISSIVE_INK",   "42 - Feature: Emissive Ink"),
    # No Matcap entry. A matcap is additive on top of the lighting result (MToon 1.0), so it is a
    # modifier that composes with every feature, not a 14th mutually exclusive one -- as a feature,
    # ticking it turned skin/hair shading OFF. It is authored in MF_MoonToonBaseInput under
    # "44 - Matcap" and rides ToonFeatureRT4. Group 43 is retired with the feature.
}

# The two stocking ids share ONE set of material parameters, by name. UE resolves same-named
# parameters across material functions to a single row, and the static feature switch means only one
# of the two functions is ever compiled in, so there is no ambiguity to resolve at runtime -- what an
# artist gets is a PBR/NPR toggle that keeps their tuning instead of two parallel sets of sliders.
STOCKINGS_PARAMS = {
    "Density":                 ("In_Stockings_Denier", "30.0"),
    "FresnelPower":            ("In_Stockings_FresnelPower", "0.0"),
    "TransmissionStrength":    ("In_Stockings_TransStrength", "0.0"),
    "TransmissionDistortion":  ("In_Stockings_TransDistortion", "0.2"),  # NEW
    "SpecularIntensity":       ("In_Stockings_SpecIntensity", "0.0"),
    "ShadowStrength":          ("In_Stockings_ShadowStrength", "0.0"),
    "GrazingDarkenStrength":   ("In_Stockings_GrazingDarken", "0.0"),
    "SkinColor":               ("In_Stockings_SkinColorRGB", "float4(0.0, 0.0, 0.0, 0.0)"),
    "GrazingSpecBoost":        ("In_Stockings_GrazingSpecBoost", "0.0"),
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
    "Stockings":    STOCKINGS_PARAMS,
    "NPRStockings": STOCKINGS_PARAMS,
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
}

# Slots that can also be driven per-pixel from a map, on top of their constant parameter.
# feature key -> {slot Name: (function input name, selector input name[, combine op])}
#
# The old writer took these as pins and the constant was either added to them (Toon Kajiya's strand
# shifts) or missing entirely (the hair highlight mask, whose In_Hair_HighlightMask parameter was
# declared and then never passed -- dead since it was written). Everything is input + parameter now,
# which reproduces the old result whenever the parameter is at its 0 default.
#
# The combine op defaults to "+", where the map is an offset and an unconnected pin (0) is inert.
# "*" makes the map a modulation instead: the parameter is the overall level and the map is where it
# varies, which is the right shape for a density/denier map -- an unconnected pin reads 1 and the
# slider alone still works. _feature_inputs picks the pin's default to match, so the identity of the
# op and the identity of the default can never disagree.
MAP_INPUTS = {
    "ToonKajiyaHair": {
        "PrimaryShift":   ("InPrimaryStrandShift", "InToonKajiyaPrimaryStrandShift"),
        "SecondaryShift": ("InSecondaryStrandShift", "InToonKajiyaSecondaryStrandShift"),
    },
    "Stockings": {
        "Density":   ("InThickness", "InStockingsThickness", "*",
                      "Per-pixel thickness multiplier from a map, applied to the denier value BEFORE the "
                      "opacity curve. 1 = full denier, 0.5 = half."),
        "SkinColor": ("InSkinColor", "InStockingsSkinColor"),
    },
    "NPRStockings": {
        "Density":   ("InThickness", "InStockingsThickness", "*",
                      "Per-pixel thickness multiplier from a map, applied to the denier value BEFORE the "
                      "opacity curve. 1 = full denier, 0.5 = half."),
        "SkinColor": ("InSkinColor", "InStockingsSkinColor"),
    },
}

# Slots a feature computes for ITSELF by calling a shared function, instead of receiving the
# finished value as a pin from the base input.
#
# The difference is where everything that produces the value lives. As a pin, the sampling happens
# in MF_MoonToonBaseInput: its texture, its channel and its map selection are declared there, which
# means they are on every material's panel and fetched by every material, whichever feature is
# selected. Called from inside the feature, all of it sits behind the feature's own static switch --
# GetVisibleMaterialParametersFromExpression does not descend the branch that was not taken, so the
# rows disappear from the panel, and the translator culls the same branch, so the fetch disappears
# with them. Which is the whole reason feature selection was made static.
#
# feature key -> {slot Name: (call expression, [inputs the call needs], header to import)}, where an
# input is (type, function input name, selector input name, default literal, description).
HELPERS = {
    "HairHighlightMask": {
        "Mask": (
            "MF_ToonHairHighlightMask(GlobalMaskA, GlobalMaskB, GlobalMaskC)",
            [("float4", "GlobalMaskA", "InGlobalMaskA", "float4(1.0, 1.0, 1.0, 1.0)",
              "Packed mask map 1, exactly as the base input sampled it. Read only if Hair Highlight Mask From Global Map is on."),
             ("float4", "GlobalMaskB", "InGlobalMaskB", "float4(1.0, 1.0, 1.0, 1.0)",
              "Packed mask map 2."),
             ("float4", "GlobalMaskC", "InGlobalMaskC", "float4(1.0, 1.0, 1.0, 1.0)",
              "Packed mask map 3.")],
            "Shared/ToonFunctions.dsh",
        ),
    },
    "Stockings": {
        "SpecularIntensity": (
            "MF_ToonStockingsSheenMask(GlobalMaskA, GlobalMaskB, GlobalMaskC)",
            [("float4", "GlobalMaskA", "InGlobalMaskA", "float4(1.0, 1.0, 1.0, 1.0)",
              "The three packed mask maps the base input sampled, already resolved. Which one the weave mask reads is Stockings Sheen Mask Source."),
             ("float4", "GlobalMaskB", "InGlobalMaskB", "float4(1.0, 1.0, 1.0, 1.0)",
              "Packed mask map 2."),
             ("float4", "GlobalMaskC", "InGlobalMaskC", "float4(1.0, 1.0, 1.0, 1.0)",
              "Packed mask map 3.")],
            "Shared/ToonFunctions.dsh",
            "*",
        ),
    },
}
# Both stocking ids share one parameter set, so they share the weave mask too.
HELPERS["NPRStockings"] = HELPERS["Stockings"]



# Slots whose value is an EXPRESSION over the parameter and the map input, emitted as a local in the
# Graph block instead of being passed straight through.
#
# MAP_INPUTS can only combine the two with "+" or "*", and HELPERS can only delegate to a shared
# function. Neither can say "the material parameter is a REAL-WORLD denier and the slot is the
# opacity that implies", which needs a curve between the authored number and the 8-bit slot. Keeping
# it here rather than in the generated .dsf is the whole point: that file says GENERATED at the top,
# so a curve written into it directly is wiped by the next run of this script.
#
# feature key -> {slot Name: (local name, expression, comment lines)}; the expression may reference
# {param} and {map}.
DERIVED = {
    "Stockings": {
        "Density": (
            "StockingsOpacity",
            "1.0 - pow(0.5, max({map} * {param}, 0.0) / 35.0)",
            ["Denier -> opacity, half-life form. Denier is grams per 9000m of filament, i.e. how much",
             "fibre is in the way, so occlusion is Beer-Lambert in it: every extra 35D halves what",
             "still gets through. Written as pow(0.5, D/35) rather than exp(-D/50) because the",
             "language has no exp() and the half-life constant is the readable one -- 35 IS the",
             "half-opacity denier. Unbounded on purpose (no UIMax): 250D saturates at 0.993 by itself.",
             "The thickness map scales denier BEFORE the curve, so a map value of 0.5 means half as",
             "many filaments here, which is what a stretched knee actually is."],
        ),
    },
}
# The two ids share one parameter set, so they share the derivation too.
DERIVED["NPRStockings"] = DERIVED["Stockings"]


def _zero(type_name):
    return {"float": "0.0",
            "float3": "float3(0.0, 0.0, 0.0)",
            "float4": "float4(0.0, 0.0, 0.0, 0.0)"}[type_name]


def _one(type_name):
    return {"float": "1.0",
            "float3": "float3(1.0, 1.0, 1.0)",
            "float4": "float4(1.0, 1.0, 1.0, 1.0)"}[type_name]


def _map_op(maps, name):
    """Combine op for a map-driven slot, defaulting to '+'."""
    return maps[name][2] if len(maps[name]) > 2 else "+"


def _map_identity(type_name, op):
    """The pin default that makes an unconnected map a no-op under `op`."""
    return _one(type_name) if op == "*" else _zero(type_name)


# Artist-facing description per slot. Keyed by slot Name, shared across features that reuse a slot
# table (Kajiya / Toon Kajiya, Skin / SDF Face), then overridden per feature where the meaning
# differs. Shown as the tooltip in the material instance panel.
DESC_COMMON = {
    # Kajiya Kay hair
    "TangentRotate":           "发丝切线绕法线旋转, 单位为 PI 弧度. 仅在材质没有各向异性方向时生效.",
    "PrimaryShift":            "主高光(靠近发根的那道)沿法线的偏移. 正值上移, 负值下移.",
    "SecondaryShift":          "次高光(靠近发梢的那道)沿法线的偏移.",
    "SecondaryMask":           "次高光的遮罩, 0~1. 通常由发丝遮罩贴图驱动.",
    "PrimaryExpScale":         "主高光锐度倍数. 大于 1 更锐, 小于 1 更散.",
    "SecondaryExpScale":       "次高光锐度倍数.",
    "OverallIntensity":        "两道高光的总强度. 0 = 关闭整个 Kajiya 高光.",
    "PrimaryIntensity":        "主高光强度.",
    "SecondaryIntensity":      "次高光强度.",
    "SecondaryShiftScale":     "次高光偏移的额外缩放, 用来拉开两道高光的间距.",
    "ShadowStrength":          "高光受阴影影响的程度. 0 = 阴影里也保持全亮, 1 = 完全跟随阴影.",
    # Skin / SDF face
    "ScatterStrength":         "次表面散射强度. 会把明暗交界线向暗部推, 让皮肤过渡更软.",
    "TransmissionStrength":    "背光透射强度(耳朵、鼻翼的透光).",
    "TransmissionPower":       "背光透射的集中度. 越大越集中在正背光方向.",
    "PrimaryRoughnessScale":   "主高光粗糙度倍数. 小于 1 让高光更小更锐.",
    "SecondaryRoughnessScale": "次高光粗糙度倍数, 通常比主高光大, 做出油光的外圈.",
    "SecondaryRampOffset":     "次高光在 Specular Ramp 上的横向偏移.",
    "OverallSpecIntensity":    "两道高光的总强度. 0 = 没有高光.",
    "PrimaryLobeIntensity":    "主高光瓣强度.",
    "SecondaryLobeIntensity":  "次高光瓣强度.",
    "SpecShadowStrength":      "高光受阴影影响的程度.",
    "WarmTintStrength":        "散射的暖色偏移强度. 越大散射越偏红.",
}

# Shared by both stocking ids, which share their parameters. Wording stays id-neutral for that
# reason -- the same tooltip has to be true whether the material is shading PBR or NPR.
STOCKINGS_DESC = {
    "Density":               "丝袜旦数(D), 直接填现实规格: 10D 极薄, 20D 薄透, 30D 半透, 40~70D 半不透, "
                             "80~120D 不透, 250D 完全不透. 换算成遮蔽率走半衰期曲线 1-pow(0.5, D/35) —— 35D 是半透点, "
                             "每再加 35D 剩余透光减半, 所以数字可以一直往上加不需要上限. 0 = 没有丝袜. "
                             "旦数同时驱动三件事: 遮蔽率、透射量、以及高光锐度(薄的更亮更锐, 厚的更哑). "
                             "逐像素厚度贴图乘在旦数上 —— 膝盖脚跟被撑薄所以旦数低, 袜口和脚尖加固区旦数高.",
    "FresnelPower":          "掠射角衰减的幂次. 越大, 变暗只发生在更接近边缘的地方.",
    "TransmissionStrength":  "背光透射强度 —— 光从腿后面穿过丝袜到达眼睛的量.",
    "TransmissionDistortion":"背光透射的法线扭曲量(DICE GDC 2011 的 Distortion). 0 = 只有光正对着镜头背面时才透光; "
                             "调大以后小腿侧面这种'夹角大、布料厚'的地方也会透出来, 通常 0.1~0.3.",
    "SpecularIntensity":     "高光总强度.",
    "ShadowStrength":        "漫反射受阴影影响的程度.",
    "GrazingDarkenStrength": "掠射角变暗强度, 做出丝袜边缘收深的效果.",
    "SkinColor":             "透出来的皮肤颜色 —— 注意是腿, 不是丝袜. 丝袜自身的颜色用材质的 Base Color. "
                             "全黑 = 未授权, 此时回退成布料色(Density 的混合变成空操作, 但吸收/透射/高光照常).",
    "GrazingSpecBoost":      "掠射角高光增益, 做出边缘的一圈亮光.",
}

DESC = {
    "Default": {
        "MultiBandCelEnable": "大于 0.5 时跳过硬明暗交界, 改由 Diffuse Ramp 贴图自己定义几阶、阶在哪. 0 = 保持原本的二值 cel.",
        "TerminatorWidth":    "每材质的明暗交界宽度, 与逐光源的 ToonLightSmooth 解耦 —— 同一盏灯下皮肤软、布料硬. 0 = 完全交给 Ramp.",
        "BloomWeight":        "该材质的 Bloom 权重, 0 = 未授权(按 1.0 处理). 用来让眼睛发光而皮肤不泛光.",
    },
    "PBRSpecular": {
        "BloomWeight":        "该材质的 Bloom 权重, 0 = 未授权(按 1.0 处理).",
    },
    "HairHighlightMask": {
        "Mask":            "天使环遮罩, 一般由发丝高光贴图驱动. 逐像素输入会与这个常量相加.",
        "Intensity":       "天使环强度. 0 = 关闭.",
        "ShadowIntensity": "天使环在阴影中保留多少. 0 = 阴影里完全消失, 1 = 不受阴影影响.",
        "ViewAnchorBlend": "0 = 天使环锚定在光照角度(旧行为); 1 = 锚定在视角, 环会跟着相机走, 更接近手绘动画的做法.",
    },
    "Stockings": STOCKINGS_DESC,
    "NPRStockings": STOCKINGS_DESC,
    "Eye": {
        "DiffuseWrapStrength":    "眼球漫反射的 wrap 强度, 让受光过渡更柔和.",
        "LimbalDarkenStrength":   "角膜缘变暗强度 —— 视角越偏, 眼球边缘越深.",
        "SpecularShadowStrength": "高光受阴影影响的程度.",
        "PrimaryRoughnessScale":  "主高光(小而锐的那点)粗糙度倍数.",
        "SecondaryRoughnessScale":"次高光(大而柔的那片)粗糙度倍数.",
        "PrimaryIntensity":       "主高光强度.",
        "SecondaryIntensity":     "次高光强度.",
        "EyeTint":                "眼球染色. 全黑 = 未授权, 此时用材质的 Base Color.",
        "SecondaryRampOffset":    "次高光在 Specular Ramp 上的横向偏移.",
    },
    "ClothVelvet": {
        "WrapStrength":          "布料漫反射的 wrap 强度.",
        "SheenPower":            "绒光的幂次. 越大, 绒光越集中在轮廓边缘.",
        "SheenShadowStrength":   "绒光受阴影影响的程度.",
        "RoughnessScale":        "高光粗糙度倍数.",
        "SheenIntensity":        "绒光强度. 0 = 没有绒感.",
        "RetroDiffuseStrength":  "逆反射强度 —— 光从相机方向来时的额外提亮, 天鹅绒的典型特征.",
        "GrazingDarkenStrength": "掠射角变暗强度.",
        "SheenTint":             "绒光颜色.",
        "RimBias":               "绒光在背光面的保底值. 0 = 背光面没有绒光.",
    },
    "ToonMetal": {
        "DiffuseIntensity":    "金属漫反射强度. 金属通常很低.",
        "ShadowSpecularFloor": "阴影中高光的保底值. 0 = 阴影里高光全灭.",
        "RampOffset":          "高光在 Specular Ramp 上的横向偏移, 用来整体调亮或调暗.",
        "RoughnessScale":      "高光粗糙度倍数.",
        "SpecularIntensity":   "高光总强度.",
        "EdgeBoost":           "掠射角高光增益, 做出金属边缘的亮边.",
        "SpecularContrast":    "高光对比度(Ramp 的幂次). 越大高光越硬、越集中.",
        "MetalTint":           "金属高光染色.",
    },
    "EmissiveInk": {
        "LightInfluence":    "受光影响程度. 0 = 完全自发光不受光照, 1 = 正常受光.",
        "ShadowLift":        "阴影提亮 —— 阴影中保留多少亮度. 0 = 阴影全黑.",
        "InkDarkenStrength": "掠射角变暗强度, 做出墨色向边缘收深.",
        "RoughnessScale":    "高光粗糙度倍数.",
        "SpecularIntensity": "高光总强度.",
        "EdgeBoost":         "掠射角高光增益.",
        "InkBlend":          "墨色与材质 Base Color 的混合比. 0 = 完全用 Base Color, 1 = 完全用墨色.",
        "InkTint":           "墨色.",
        "RimBias":           "边缘高光的菲涅尔偏置.",
    },
    "_RetiredMatcap": {
        "Intensity":   "unused -- matcap is no longer a feature",
        "MatcapColor": "unused -- matcap is no longer a feature",
    },
    "DFFacialShadow": {
        "OverallSpecIntensity": "两道高光的总强度. 在这次重构之前这个槽位写不进去, 所以每张 SDF 脸的 toon 高光都是 0 —— 现在可以打开了.",
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
    "NPRStockings":      "float4(1.0, 0.5, 1.0, 1.0)",
    "Eye":               "float4(0.0, 1.0, 1.0, 1.0)",
    "ClothVelvet":       "float4(0.5, 1.0, 0.5, 1.0)",
    "ToonMetal":         "float4(1.0, 0.5, 0.5, 1.0)",
    "EmissiveInk":       "float4(0.5, 0.5, 1.0, 1.0)",
    # Matcap retired as a feature -- no debug colour needed.
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
    helpers = HELPERS.get(feature_key, {})
    derived = DERIVED.get(feature_key, {})

    prop_lines, call_lines, pre_lines = [], [], []
    for i, (t, _, name) in enumerate(rows):
        pname, default = params[name]
        kind = "VectorParameter" if t == "float3" else "ScalarParameter"
        desc = DESC.get(feature_key, {}).get(name) or DESC_COMMON.get(name)
        if not desc:
            raise SystemExit("%s.%s has no description -- add one to DESC or DESC_COMMON"
                             % (feature_key, name))
        prop_lines.append('\t\t%s %s = %s [Group="%s"; SortPriority=%d; Description="%s"];'
                          % (kind, pname, default, group, i, desc))
        expr = "%s%s" % (pname, ".rgb" if t == "float3" else "")
        if name in derived:
            local, tmpl, comment = derived[name]
            if pre_lines:
                pre_lines.append("")
            pre_lines.extend("\t\t// %s" % c for c in comment)
            pre_lines.append("\t\tfloat %s = %s;"
                             % (local, tmpl.format(param=expr,
                                                   map=maps[name][0] if name in maps else "1.0")))
            expr = local
        else:
            if name in maps:
                expr = "%s %s %s" % (maps[name][0], _map_op(maps, name), expr)
            if name in helpers:
                hop = helpers[name][3] if len(helpers[name]) > 3 else "+"
                expr = "%s %s %s" % (helpers[name][0], hop, expr)
        call_lines.append("\t\t\t%s," % expr)

    input_lines = ['\t\topt %s %s = %s [Description="%s"];' % (t, fn_in, default, desc)
                   for (t, fn_in, _, default, desc) in _feature_inputs(feature_key, tables)]

    imports = "".join('\nimport "%s";\n' % spec[2] for spec in helpers.values())

    return """%s%s
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
%s\t\t%s(
%s
\t\t\tToonBufferA, ToonBufferB, ToonBufferC);
\t}
}
""" % (HEADER, imports, fn, args, id_macro, body, dsf_name, dsf_name,
       "\n".join(input_lines), "\n".join(prop_lines),
       ("\n".join(pre_lines) + "\n\n") if pre_lines else "", fn,
       "\n".join(call_lines))


SELECT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "DShader", "MaterialFunctions", "MF_MoonToonFeatureSelect.dsf")

# Default is the fallback rather than a checkbox: an instance with nothing ticked shades as plain
# toon, which is what a fresh material should do.
FALLBACK = "Default"


def _feature_inputs(key, tables):
    """[(type, function input, selector input, default literal, description)], slot-table order."""
    maps = MAP_INPUTS.get(key, {})
    helpers = HELPERS.get(key, {})
    ins = []
    for (t, _, name) in tables[FEATURES[key][2]]:
        if name in maps:
            fn_in, sel_in = maps[name][0], maps[name][1]
            op = _map_op(maps, name)
            t = "float3" if t == "float3" else "float"
            verb = "multiplied into" if op == "*" else "added to"
            # A derived slot's map does not feed the slot directly, so the generic sentence
            # would describe the wrong quantity -- those entries carry their own.
            desc = (maps[name][3] if len(maps[name]) > 3
                    else "Per-pixel %s from a map, %s the parameter." % (name, verb))
            ins.append((t, fn_in, sel_in, _map_identity(t, op), desc))
        if name in helpers:
            ins.extend(helpers[name][1])
    return ins


def parse_feature_ids(path):
    """MOON_SHADING_FEATURE_ID_* -> numeric value, straight from the shared definition header."""
    src = open(path, encoding="utf-8").read()
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"#define\s+(MOON_SHADING_FEATURE_ID_\w+)\s+(\d+)", src)}


def emit_selector(tables, ids):
    picks = [k for k in FEATURES if k != FALLBACK]

    def decl(k):
        ins = _feature_inputs(k, tables)
        in_block = "\n".join('\t\topt %s %s = %s;' % (t, fn_in, default)
                             for (t, fn_in, _, default, _d) in ins)
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
        for (t, _, sel_in, default, desc) in _feature_inputs(k, tables):
            if sel_in not in seen:
                seen.add(sel_in)
                sel_inputs.append((t, sel_in, default, desc))
    sel_in_block = "\n".join('\t\topt %s %s = %s [Description="%s"];' % (t, n, default, desc)
                             for (t, n, default, desc) in sel_inputs)

    # Two spellings of one switch, as close as the languages allow. The DSL identifier has to be an
    # identifier, so it uses an underscore; the panel label is what an artist reads, so it uses a
    # space and matches the plugin's existing "Is Face" / "Is Hair" switches.
    def switch_id(k):
        return "Is_%s" % FEATURES[k][0]

    def switch_label(k):
        return "Is %s" % FEATURES[k][0]

    props = "\n".join(
        '\t\tStaticSwitchParameter %s = false '
        '[ParameterName="%s"; Group="30 - Shading Feature (pick one)"; SortPriority=%d; '
        'Description="启用 %s 着色特征. 只应勾选一个; 若勾了多个, 本文件中靠前的胜出. 一个都不勾 = 普通 toon. '
        '勾选后, 未选中特征的参数会从材质实例面板里消失(静态开关会裁掉未走的分支)."];'
        % (switch_id(k), switch_label(k), i, FEATURES[k][3].split(": ")[-1])
        for i, k in enumerate(picks))

    def call(k, out_index):
        args = "".join("%s, " % sel_in for (_, _, sel_in, _d, _c) in _feature_inputs(k, tables))
        return "MF_ToonFeature_%s(%sOutputIndex=%d)" % (FEATURES[k][0], args, out_index)

    def chain(out_index, depth=0):
        if depth == len(picks):
            return call(FALLBACK, out_index)
        k = picks[depth]
        pad = "\t\t\t" + "\t" * depth
        return ("%s(\n%sTrue = %s,\n%sFalse = %s)"
                % (switch_id(k), pad, call(k, out_index), pad, chain(out_index, depth + 1)))

    def id_chain(depth=0):
        if depth == len(picks):
            return "%d.0" % ids[FEATURES[FALLBACK][1]]
        k = picks[depth]
        pad = "\t\t\t" + "\t" * depth
        return ("%s(\n%sTrue = %d.0,\n%sFalse = %s)"
                % (switch_id(k), pad, ids[FEATURES[k][1]], pad, id_chain(depth + 1)))

    body = "\n\n".join("\t\t%s = %s;" % (name, chain(i))
                       for i, name in enumerate(["ToonBufferA", "ToonBufferB", "ToonBufferC"]))
    def const_chain(values, depth=0):
        if depth == len(picks):
            return values[FALLBACK]
        k = picks[depth]
        pad = "\t\t\t" + "\t" * depth
        return ("%s(\n%sTrue = %s,\n%sFalse = %s)"
                % (switch_id(k), pad, values[k], pad, const_chain(values, depth + 1)))

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
