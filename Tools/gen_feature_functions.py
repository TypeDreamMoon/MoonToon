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
REGISTRY = os.path.join(os.path.dirname(SLOT_TABLE), "MoonToonFeatureRegistry.h")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "DShader", "MaterialFunctions", "Features")

# feature key -> (dsf name, MOON_SHADING_FEATURE_ID_*, parameter group)
#
# Which slot table each id reads is NOT stated here -- it comes from the engine's feature registry
# (MoonToonFeatureRegistry.h: id -> module -> slot table), so this file and the shader cannot
# disagree about it. What IS stated here is the material-facing metadata the engine has no opinion
# about: the .dsf name, the id, and the panel group.
FEATURES = {
    "Default":           ("Default",           "MOON_SHADING_FEATURE_ID_DEFAULT",
                          "31 - Feature: Default"),
    "PBRSpecular":       ("PBRSpecular",       "MOON_SHADING_FEATURE_ID_PBR_SPECULAR",
                          "32 - Feature: PBR Specular"),
    "KajiyaHair":        ("KajiyaHair",        "MOON_SHADING_FEATURE_ID_KAJIYA_HAIR_SPECULAR",
                          "34 - Feature: Hair Kajiya Kay"),
    "ToonKajiyaHair":    ("ToonKajiyaHair",    "MOON_SHADING_FEATURE_ID_TOON_KAJIYA_HAIR_SPECULAR",
                          "35 - Feature: Hair Toon Kajiya Kay"),
    "DFFacialShadow":    ("DFFacialShadow",    "MOON_SHADING_FEATURE_ID_DISTANCE_FIELD_FACIAL_SHADOW",
                          "37 - Feature: SDF Face"),
    "HairHighlightMask": ("HairHighlightMask", "MOON_SHADING_FEATURE_ID_HAIR_HIGHLIGHT_MASK",
                          "33 - Feature: Hair Highlight Mask"),
    "Skin":              ("Skin",              "MOON_SHADING_FEATURE_ID_SKIN",
                          "36 - Feature: Skin"),
    "Stockings":         ("Stockings",         "MOON_SHADING_FEATURE_ID_PBR_STOCKINGS",
                          "38 - Feature: Stockings"),
    # Same slots, same parameter NAMES, same panel group as Stockings -- only the lighting path
    # differs (ToonBxDF sends this id through the diffuse ramp and the per-light cel band). Sharing
    # the names is what makes the switch non-destructive: tick the other box and every tuned value
    # is still there, because there is only one set of parameters underneath both.
    "NPRStockings":      ("NPRStockings",      "MOON_SHADING_FEATURE_ID_NPR_STOCKINGS",
                          "38 - Feature: Stockings"),
    "Eye":               ("Eye",               "MOON_SHADING_FEATURE_ID_EYE",
                          "39 - Feature: Eye"),
    "ClothVelvet":       ("ClothVelvet",       "MOON_SHADING_FEATURE_ID_CLOTH_VELVET",
                          "40 - Feature: Cloth Velvet"),
    "ToonMetal":         ("ToonMetal",         "MOON_SHADING_FEATURE_ID_TOON_METAL",
                          "41 - Feature: Metal"),
    "EmissiveInk":       ("EmissiveInk",       "MOON_SHADING_FEATURE_ID_TOON_EMISSIVE_INK",
                          "42 - Feature: Emissive Ink"),
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


def parse_slot_tables(path, prefix="MOON_TOON_SLOTS_"):
    """table-macro -> [(Type, Slot, Name)], following one level of table aliasing.

    FEATURE tables carry a fourth column since phase 2b-3 -- the value the shader falls back to when
    the record stops carrying that channel -- and MODIFIER tables carry three, because the modifier
    payload was never collapsed and so has nothing to fall back to. Both shapes are accepted here;
    the fourth column is kept separately (parse_slot_defaults) so every caller of this function keeps
    the triple it was written against.
    """
    src = open(path, encoding="utf-8").read()
    tables, alias = {}, {}
    for m in re.finditer(r"#define\s+(%s\w+)\(X\)((?:[^\n]*\\\n)*[^\n]*)" % prefix, src):
        name, body = m.group(1), m.group(2)
        rows = [r[:3] for r in re.findall(
            r"X\(\s*(\w+)\s*,\s*([\w.]+)\s*,\s*(\w+)\s*(?:,[^)]*(?:\([^)]*\))?[^)]*)?\)", body)]
        if rows:
            tables[name] = rows
        else:
            ref = re.search(r"(%s\w+)\(X\)" % prefix, body)
            if ref:
                alias[name] = ref.group(1)
    for a, target in alias.items():
        tables[a] = tables[target]
    return tables


# The profile column that DOES reach a collapsed slot, so the warning can name it rather than saying
# "use a profile" and leaving the artist to find which row.
_COLLAPSED_SLOT_ESCAPE = {
    "FeatureInputScalarE":  "FeatureScalar[4]",
    "FeatureInputScalarF":  "FeatureScalar[5]",
    "FeatureInputScalarG":  "FeatureScalar[6]",
    "FeatureInputVector.w": "FeatureVector 的 A",
}


def parse_perpixel_slots(path):
    """The slots the closure record still carries, off MOONTOON_RECORD_PERPIXEL_SLOTS.

    Read from the engine rather than restated here: it is a fact about the record layout, and the
    record header is where that layout is decided.
    """
    src = open(path, encoding="utf-8").read()
    m = re.search(r"#define\s+MOONTOON_RECORD_PERPIXEL_SLOTS\s+([^\n]*)", src)
    if not m:
        raise SystemExit("%s: MOONTOON_RECORD_PERPIXEL_SLOTS not found -- the record header is the "
                         "single source for which slots survive" % os.path.basename(path))
    return set(m.group(1).split())


def collapsed_slot_warning(slot, perpixel):
    """The sentence appended to a parameter whose slot the record no longer carries."""
    if slot in perpixel:
        return ""
    escape = _COLLAPSED_SLOT_ESCAPE.get(slot, "profile 的 feature 列")
    return (" ⚠ Substrate 下改这里不生效: 记录自 2b-3 起不再携带这条通道, 它由本 feature 的母材质"
            "默认值提供. 要给这个材质一个不同的值, 给它挂一个 MoonToonProfile 并设 %s. "
            "(legacy 存储下仍然生效, 所以两条存储会不一致.)" % escape)


def parse_slot_defaults(path, prefix="MOON_TOON_SLOTS_"):
    """table-macro -> {slot Name: default literal}, for the tables that HAVE a fourth column."""
    src = open(path, encoding="utf-8").read()
    out, alias = {}, {}
    for m in re.finditer(r"#define\s+(%s\w+)\(X\)((?:[^\n]*\\\n)*[^\n]*)" % prefix, src):
        name, body = m.group(1), m.group(2)
        rows = re.findall(
            r"X\(\s*\w+\s*,\s*[\w.]+\s*,\s*(\w+)\s*,\s*((?:[^,()]|\([^)]*\))+?)\s*\)", body)
        if rows:
            out[name] = {n: d.strip() for n, d in rows}
        else:
            ref = re.search(r"(%s\w+)\(X\)" % prefix, body)
            if ref:
                alias[name] = ref.group(1)
    for a, target in alias.items():
        if target in out:
            out[a] = out[target]
    return out


def _numbers(literal):
    """The numeric components of a slot default, whichever spelling it arrived in.

    The engine writes HLSL (`0.4482f`, `float3(1.0f, 1.0f, 1.0f)`) and the generator writes
    DreamShaderLang (`0.4482`, `float4(1.0, 1.0, 1.0, 1.0)`). A float3 slot is authored as a float4
    parameter because that is what a colour picker is, so the trailing alpha is dropped before
    comparing -- it is not a slot.
    """
    body = re.sub(r"^\s*float[234]\s*\(", "(", literal.strip())
    vals = [float(x) for x in re.findall(r"-?\d+(?:\.\d+)?", body)]
    # a float3 slot is authored as a float4 parameter, because that is what a colour picker is
    if len(vals) == 4:
        vals = vals[:3]
    return vals


def check_slot_defaults(defaults):
    """The engine slot table and this generator must agree on every slot default.

    They are the same fact in two places: the shader falls back to the table column when the record
    stops carrying the channel, and the material instance starts at the generator's literal. If they
    drift, a material at its own default stops matching the fallback -- which is the entire premise
    of the phase 2b-3 record narrowing, and would show up as a look change nobody edited.
    """
    problems = []
    for key in FEATURES:
        table = slot_table_for(key)
        if table not in defaults:
            continue
        derived = DERIVED.get(key, {})
        for name, (pname, pdefault) in PARAMS.get(key, {}).items():
            if name not in defaults[table]:
                continue
            if name in derived:
                # The parameter and the slot are different QUANTITIES here, related by a curve --
                # Stockings authors denier and the slot holds the opacity it maps to -- so there is
                # no number to compare. The slot table comments carry that arithmetic; checking it
                # would mean evaluating the generator expression, which is not what this guards.
                continue
            want, got = _numbers(defaults[table][name]), _numbers(pdefault)
            if len(want) != len(got) or any(abs(a - b) > 1e-4 for a, b in zip(want, got)):
                problems.append("%s.%s: slot table says %s, generator parameter %s says %s"
                                % (key, name, defaults[table][name], pname, pdefault))
    return problems


def parse_registry(path):
    """(modules, features) from the engine's MoonToonFeatureRegistry.h.

    modules:  Module -> MOON_TOON_SLOTS_* macro
    features: IdName -> Module   (IdName is the MOON_SHADING_FEATURE_ID_ suffix)
    """
    src = open(path, encoding="utf-8").read()

    def rows(list_macro):
        # Line-scanned rather than one big regex: the table is a backslash-continued #define, and a
        # regex for that has to spell "backslash then newline", which is exactly the kind of escape
        # that silently loses a character on the way into a generator script. (It did.)
        collected, collecting = [], False
        for line in src.splitlines():
            if not collecting and re.match(r"\s*#define\s+%s\(X\)" % list_macro, line):
                collecting = True
            if collecting:
                collected.append(line)
                if not line.rstrip().endswith("\\"):
                    break
        if not collected:
            raise SystemExit("%s: %s not found" % (os.path.basename(path), list_macro))
        return re.findall(r"X\(\s*(\w+)\s*,\s*(\w+)\s*\)", "\n".join(collected))

    return dict(rows("MOON_TOON_MODULE_LIST")), dict(rows("MOON_TOON_FEATURE_LIST"))


_REGISTRY = None


def registry():
    global _REGISTRY
    if _REGISTRY is None:
        _REGISTRY = parse_registry(os.path.normpath(REGISTRY))
    return _REGISTRY


def slot_table_for(feature_key):
    """The MOON_TOON_SLOTS_* macro this feature reads, resolved through the engine registry."""
    modules, features = registry()
    id_name = FEATURES[feature_key][1][len("MOON_SHADING_FEATURE_ID_"):]
    if id_name not in features:
        raise SystemExit("%s: id %s is not in MOON_TOON_FEATURE_LIST -- add it to the registry"
                         % (feature_key, id_name))
    module = features[id_name]
    if module not in modules:
        raise SystemExit("registry: MOON_TOON_FEATURE_LIST routes %s to module %s, which is not in "
                         "MOON_TOON_MODULE_LIST" % (id_name, module))
    return modules[module]


def emit(feature_key, tables):
    dsf_name, id_macro, group = FEATURES[feature_key]
    rows = tables[slot_table_for(feature_key)]
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

    perpixel = parse_perpixel_slots(SLOT_TABLE.replace("MoonToonFeatureSlots.h",
                                                       "MoonToonRecordDefinitions.h"))

    prop_lines, call_lines, pre_lines = [], [], []
    for i, (t, slot, name) in enumerate(rows):
        pname, default = params[name]
        kind = "VectorParameter" if t == "float3" else "ScalarParameter"
        desc = DESC.get(feature_key, {}).get(name) or DESC_COMMON.get(name)
        if not desc:
            raise SystemExit("%s.%s has no description -- add one to DESC or DESC_COMMON"
                             % (feature_key, name))
        # A parameter whose slot the record dropped says so, in the one place an artist is looking
        # when they type a value into it.
        desc = desc + collapsed_slot_warning(slot, perpixel)
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
    for (t, _, name) in tables[slot_table_for(key)]:
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
        % (switch_id(k), switch_label(k), i, FEATURES[k][2].split(": ")[-1])
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


# Ids that deliberately have no material function. Stating them is the whole point: the cross-check
# below fails on any id that exists engine-side but has no .dsf, so a feature cannot be added to the
# registry and then silently left unauthorable -- which is how MATCAP stayed dead for months.
FEATURES_WITHOUT_FUNCTION = {
    "EYEBROW": "reserved and inert; routes to the Default module and has no parameters of its own",
}


def macro_body(path, macro):
    """The text of one X-macro list in the registry, continuation lines included."""
    src = open(path, encoding="utf-8").read()
    collected, collecting = [], False
    for line in src.splitlines():
        if not collecting and re.match(r"\s*#define\s+" + macro + r"\(X\)", line):
            collecting = True
        if collecting:
            collected.append(line)
            if not line.rstrip().endswith("\\"):
                break
    if not collected:
        raise SystemExit("%s: %s not found" % (os.path.basename(path), macro))
    return "\n".join(collected)


def parse_bloom_weight_list(path):
    """The ids in MOON_TOON_FEATURES_WITH_BLOOM_WEIGHT (a one-argument X-macro list)."""
    return re.findall(r"X\(\s*(\w+)\s*\)", macro_body(path, "MOON_TOON_FEATURES_WITH_BLOOM_WEIGHT"))


def check_bloom_weight_list(tables):
    """The bloom-weight list must be exactly the features whose slot table names a BloomWeight slot.

    The list has to exist -- ten features write FeatureInputVector.w and only two mean "bloom weight"
    by it, so it cannot be derived from the layout alone -- but it does not have to be trusted.
    """
    listed = set(parse_bloom_weight_list(os.path.normpath(REGISTRY)))
    modules, features = registry()
    actual = set()
    for id_name, module in features.items():
        if id_name in FEATURES_WITHOUT_FUNCTION:
            # An id no material can select cannot carry an authored bloom weight, so it is not
            # expected in the list even though the module it routes to has the slot. EYEBROW is the
            # case: it shades as Default and so inherits Default's BloomWeight slot, but there is no
            # "Is Eyebrow" switch, so nothing can ever write it -- which is why the old
            # `FeatureId == 0 || FeatureId == 1` excluded it too. Making it authorable removes it
            # from this set, and the check then demands it be listed.
            continue
        rows = tables.get(modules.get(module), [])
        if any(name == "BloomWeight" for (_, _, name) in rows):
            actual.add(id_name)

    problems = []
    for id_name in sorted(listed - actual):
        problems.append("MOON_TOON_FEATURES_WITH_BLOOM_WEIGHT lists %s, but its slot table has no "
                        "BloomWeight slot" % id_name)
    for id_name in sorted(actual - listed):
        problems.append("%s has a BloomWeight slot but is missing from "
                        "MOON_TOON_FEATURES_WITH_BLOOM_WEIGHT (ToonBlur.usf would ignore it)"
                        % id_name)
    return problems


def check_registry_agreement():
    """The engine registry and this generator must describe the same set of features.

    Direction matters, so both are checked: an id with no .dsf is unauthorable, and a .dsf for an id
    the engine does not know is a material function nothing can ever select.
    """
    modules, features = registry()
    authored = {FEATURES[k][1][len("MOON_SHADING_FEATURE_ID_"):]: k for k in FEATURES}
    problems = []

    for id_name in sorted(features):
        if id_name not in authored and id_name not in FEATURES_WITHOUT_FUNCTION:
            problems.append("id %s is in MOON_TOON_FEATURE_LIST but has no FEATURES entry "
                            "(add one, or list it in FEATURES_WITHOUT_FUNCTION with a reason)" % id_name)
    for id_name, key in sorted(authored.items()):
        if id_name not in features:
            problems.append("FEATURES[%r] uses id %s, which is not in MOON_TOON_FEATURE_LIST"
                            % (key, id_name))
    for id_name in sorted(FEATURES_WITHOUT_FUNCTION):
        if id_name not in features:
            problems.append("FEATURES_WITHOUT_FUNCTION lists %s, which is not in MOON_TOON_FEATURE_LIST"
                            % id_name)
        if id_name in authored:
            problems.append("%s is both authored in FEATURES and listed as function-less" % id_name)

    routed = set(features.values())
    for module in sorted(modules):
        if module not in routed:
            problems.append("module %s is in MOON_TOON_MODULE_LIST but no id routes to it" % module)

    return problems


# =================================================================================================
# The MODIFIER axis.
#
# Same machinery as the feature axis above, deliberately: one X-macro slot table in the engine, one
# generated material function per entry, one generated selector. What differs is the fallback --
# a feature always picks one (Default), while a modifier's normal state is NONE, which has no module
# and no function. So the chains below bottom out in a literal zero rather than in a call.

MODIFIER_SLOT_TABLE = os.path.join(os.path.dirname(SLOT_TABLE), "MoonToonModifierSlots.h")
MODIFIER_ID_TABLE = os.path.join(os.path.dirname(SLOT_TABLE), "MoonToonModifierDefinitions.h")
MODIFIER_OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "DShader", "MaterialFunctions", "Modifiers")
MODIFIER_SELECT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "DShader",
                                    "MaterialFunctions", "MF_MoonToonModifierSelect.dsf")

# modifier key -> (dsf name, MOON_TOON_MODIFIER_ID_*, parameter group)
MODIFIERS = {
    "Matcap":  ("Matcap",  "MOON_TOON_MODIFIER_ID_MATCAP",   "44 - Modifier: Matcap"),
    "OilFilm": ("OilFilm", "MOON_TOON_MODIFIER_ID_OIL_FILM", "45 - Modifier: Oil Film"),
}

# Slots a modifier computes by calling a shared function instead of declaring a parameter for.
# Identical in purpose to HELPERS on the feature side, and identical in payoff: everything the call
# needs sits behind the modifier's own static switch, so an instance with the modifier off shows
# none of its rows and fetches none of its textures.
#
# Matcap is entirely this -- all seven of its authoring parameters live in MF_ToonMatcap, which now
# returns one output per SLOT rather than one packed float4, so the packing is stated only in the
# engine's MOON_TOON_MOD_SLOTS_MATCAP table.
MODIFIER_HELPERS = {
    "Matcap": {
        "Color":       "MF_ToonMatcap(NormalTS, OutputIndex=0)",
        "LightingMix": "MF_ToonMatcap(NormalTS, OutputIndex=1)",
    },
}

# Inputs a modifier's function takes, threaded through from the selector.
# (type, function input, selector input, default, description)
MODIFIER_INPUTS = {
    "Matcap": [("float3", "NormalTS", "InNormalTS", "float3(0.0, 0.0, 1.0)",
                "Tangent-space normal, the same one the surface ends up shading with.")],
}

# Slots a modifier declares its own material parameter for, for modifiers that are not a pure helper
# call. slot Name -> (parameter name, default, description). Empty for Matcap by construction --
# all seven of its knobs live in MF_ToonMatcap.
#
# The oil film is the opposite: every slot is a plain number, because its colour is NOT authored
# here. It comes from the specular ramp atlas the material already carries, so the film inherits the
# character's specular grading and Hue Shift only rotates where on that ramp it sits.
MODIFIER_PARAMS = {
    "OilFilm": {
        "Intensity": ("Oil Film Intensity", "1.0",
                      "油膜高光强度. 0 = 完全关闭(着色器会整段跳过), 所以这也是它的开关."),
        "HighlightScale": ("Oil Film Highlight Scale", "1.0",
                           "缩放 GGX 的 alpha, 也就是高光斑的大小. >1 更大更散, <1 更小更聚. "
                           "这是风格化 GGX 的核心旋钮 —— 物理 GGX 收紧时会同时变暗, 这里不会."),
        "FalloffPower": ("Oil Film Falloff Power", "1.0",
                         "高光的锐利度: >1 边缘更硬(接近赛璐璐的一块高光), <1 更软更弥散. "
                         "作用在阴影项之后, 所以越锐利的油膜进入暗部时也衰减得越快."),
        "HueShift": ("Oil Film Hue Shift", "0.0",
                     "在高光 ramp 上旋转油膜的取色位置, [0,1] 循环. "
                     "油膜的颜色随视角在 ramp 上扫过(正视在头, 掠射在尾), 这个值整体平移那段扫描."),
    },
}

MODIFIER_IMPORTS = {"Matcap": ["Shared/ToonFunctions.dsh"]}

MODIFIER_HEADER = """// GENERATED from Engine/Shaders/Shared/MoonToonModifierSlots.h by Tools/gen_feature_functions.py.
// Edit the slot table (for the mapping) or the generator (for names/defaults), not this file.
//
// One modifier, one material function. A modifier is ADDITIVE on top of whatever shading feature the
// material selected, so unlike a feature it does not compete for the 30 group's single choice.
"""


def parse_modifier_list(path):
    """IdName -> Module, out of MOON_TOON_MODIFIER_LIST."""
    src = open(path, encoding="utf-8").read()
    collected, collecting = [], False
    for line in src.splitlines():
        if not collecting and re.match(r"\s*#define\s+MOON_TOON_MODIFIER_LIST\(X\)", line):
            collecting = True
        if collecting:
            collected.append(line)
            if not line.rstrip().endswith("\\"):
                break
    if not collected:
        raise SystemExit("%s: MOON_TOON_MODIFIER_LIST not found" % os.path.basename(path))
    return dict(re.findall(r"X\(\s*(\w+)\s*,\s*(\w+)\s*\)", "\n".join(collected)))


def parse_modifier_ids(path):
    """MOON_TOON_MODIFIER_ID_* -> numeric value."""
    src = open(path, encoding="utf-8").read()
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"#define\s+(MOON_TOON_MODIFIER_ID_\w+)\s+(\d+)", src)}


def modifier_slot_table_for(key):
    """The MOON_TOON_MOD_SLOTS_* macro this modifier reads, via the engine's modifier list."""
    id_name = MODIFIERS[key][1][len("MOON_TOON_MODIFIER_ID_"):]
    listed = parse_modifier_list(os.path.normpath(MODIFIER_ID_TABLE))
    if id_name not in listed:
        raise SystemExit("%s: id %s is not in MOON_TOON_MODIFIER_LIST -- its module does not exist "
                         "yet, so a material function for it would author a value nothing reads"
                         % (key, id_name))
    return "MOON_TOON_MOD_SLOTS_%s" % id_name


def emit_modifier(key, tables):
    dsf_name, id_macro, group = MODIFIERS[key]
    rows = tables[modifier_slot_table_for(key)]
    helpers = MODIFIER_HELPERS.get(key, {})
    params = MODIFIER_PARAMS.get(key, {})

    missing = [n for (_, _, n) in rows if n not in helpers and n not in params]
    if missing:
        raise SystemExit("%s: slot table has %s with neither a helper nor a material parameter"
                         % (key, ", ".join(missing)))

    fn = "ToonModifierWrite_%s" % dsf_name
    args = ",\n".join("\tin %s %s" % ("float3" if t == "float3" else "float", n)
                      for (t, _, n) in rows)
    body = "\n".join("\tTBuffer.%s = %s;" % (slot, name) for (_, slot, name) in rows)

    prop_lines, call_lines = [], []
    for i, (t, _, name) in enumerate(rows):
        if name in helpers:
            call_lines.append("\t\t\t%s," % helpers[name])
            continue
        display, default, desc = params[name]
        # The DSL identifier is derived, not authored: it cannot carry spaces, and having the table
        # state both spellings is one more pair that can drift. ParameterName is what an artist
        # reads, so that is the one the table holds.
        ident = "In_%s_%s" % (dsf_name, name)
        kind = "VectorParameter" if t == "float3" else "ScalarParameter"
        prop_lines.append('		%s %s = %s [ParameterName="%s"; Group="%s"; SortPriority=%d; '
                          'Description="%s"];' % (kind, ident, default, display, group, i, desc))
        call_lines.append("			%s%s," % (ident, ".rgb" if t == "float3" else ""))

    input_lines = ['\t\topt %s %s = %s [Description="%s"];' % (t, fn_in, default, desc)
                   for (t, fn_in, _s, default, desc) in MODIFIER_INPUTS.get(key, [])]
    imports = "".join('\nimport "%s";\n' % h for h in MODIFIER_IMPORTS.get(key, []))

    return """%s%s
Function %s(
%s,
\tout float4 ModifierData)
{
\tFToonBuffer TBuffer = (FToonBuffer)0;
\tInitToonBuffer(TBuffer);

%s

\tModifierData = TBuffer.ModifierInputVector;
}

ShaderFunction(Name="MaterialFunctions/Modifiers/MF_ToonModifier_%s", Root="Plugin.MoonToon")
{
\tSettings = {
\t\tDescription       = "Packs the %s modifier parameters into the ToonFeatureRT4 payload.";
\t\tExposeToLibrary   = true;
\t\tLibraryCategories = "MoonToon|Modifiers";
\t}

\tInputs = {
%s
\t}

\tOutputs = {
\t\tfloat4 ModifierData;
\t}

%s\tGraph = {
\t\t%s(
%s
\t\t\tModifierData);
\t}
}
""" % (MODIFIER_HEADER, imports, fn, args, body, dsf_name, dsf_name,
       "\n".join(input_lines),
       # A modifier whose slots all come from helpers declares no parameters of its own -- Matcap's
       # seven live in MF_ToonMatcap. An empty Properties block would be noise, not information.
       ("\tProperties = {\n%s\n\t}\n\n" % "\n".join(prop_lines)) if prop_lines else "",
       fn, "\n".join(call_lines))


def emit_modifier_selector(tables, ids):
    picks = list(MODIFIERS)

    def decl(k):
        in_block = "\n".join("\t\topt %s %s = %s;" % (t, fn_in, default)
                             for (t, fn_in, _s, default, _d) in MODIFIER_INPUTS.get(k, []))
        return ('VirtualFunction(Name="MF_ToonModifier_%s")\n'
                "{\n"
                "\tOptions = {\n"
                '\t\tAsset = Path(Plugins.MoonToon, "MaterialFunctions/Modifiers/MF_ToonModifier_%s");\n'
                "\t}\n\n"
                "\tInputs = {\n%s\n\t}\n\n"
                "\tOutputs = {\n\t\tfloat4 ModifierData;\n\t}\n"
                "}" % (MODIFIERS[k][0], MODIFIERS[k][0], in_block))

    decls = "\n".join(decl(k) for k in picks)

    sel_inputs, seen = [], set()
    for k in picks:
        for (t, _f, sel_in, default, desc) in MODIFIER_INPUTS.get(k, []):
            if sel_in not in seen:
                seen.add(sel_in)
                sel_inputs.append((t, sel_in, default, desc))
    sel_in_block = "\n".join('\t\topt %s %s = %s [Description="%s"];' % (t, n, default, desc)
                             for (t, n, default, desc) in sel_inputs)

    def switch_id(k):
        return "Is_Modifier_%s" % MODIFIERS[k][0]

    # The panel label comes from the group name's suffix rather than the .dsf name -- same
    # convention as the feature switches, and the reason "Oil Film" can read as two words
    # where the DSL identifier cannot.
    def switch_label(k):
        return MODIFIERS[k][2].split(": ")[-1]

    props = "\n".join(
        '\t\tStaticSwitchParameter %s = false '
        '[ParameterName="Is Modifier %s"; Group="43 - Modifier (pick one)"; SortPriority=%d; '
        'Description="启用 %s 修饰层. 修饰层叠加在着色特征之上, 与任意特征共存 —— 它不占 30 组的单选. '
        '只应勾选一个; 若勾了多个, 本文件中靠前的胜出. 一个都不勾 = 无修饰层, 在面板和着色器里都不花代价."];'
        % (switch_id(k), switch_label(k), i, switch_label(k))
        for i, k in enumerate(picks))

    def call(k):
        args = "".join("%s, " % sel_in for (_t, _f, sel_in, _d, _c) in MODIFIER_INPUTS.get(k, []))
        return "MF_ToonModifier_%s(%sOutputIndex=0)" % (MODIFIERS[k][0], args)

    # Both chains bottom out in the NONE literal rather than in a call: NONE has no module, which is
    # the whole reason "no modifier" costs nothing in the shader as well as in the panel.
    def chain(depth=0):
        if depth == len(picks):
            return "float4(0.0, 0.0, 0.0, 0.0)"
        pad = "\t\t\t" + "\t" * depth
        return ("%s(\n%sTrue = %s,\n%sFalse = %s)"
                % (switch_id(picks[depth]), pad, call(picks[depth]), pad, chain(depth + 1)))

    def id_chain(depth=0):
        if depth == len(picks):
            return "%d.0" % ids["MOON_TOON_MODIFIER_ID_NONE"]
        pad = "\t\t\t" + "\t" * depth
        return ("%s(\n%sTrue = %d.0,\n%sFalse = %s)"
                % (switch_id(picks[depth]), pad, ids[MODIFIERS[picks[depth]][1]], pad,
                   id_chain(depth + 1)))

    return """// GENERATED by Tools/gen_feature_functions.py. Do not edit.
//
// Static modifier selection, the additive counterpart to MF_MoonToonFeatureSelect. Only the selected
// branch survives compilation, and only its parameters survive the material instance panel.
//
// Nothing ticked is the normal state and it is free: ModifierID stays MOON_TOON_MODIFIER_ID_NONE,
// which the engine's dispatcher answers with a switch fall-through and no module at all.

%s

ShaderFunction(Name="MaterialFunctions/MF_MoonToonModifierSelect", Root="Plugin.MoonToon")
{
\tSettings = {
\t\tDescription       = "Selects one toon modifier and packs its parameters into the modifier payload.";
\t\tExposeToLibrary   = true;
\t\tLibraryCategories = "MoonToon";
\t}

\tInputs = {
%s
\t}

\tOutputs = {
\t\t// ToonMaterialOutput pin 3 -> ToonFeatureRT4. What its channels mean is selected by ModifierID.
\t\tfloat4 ModifierData;
\t\t// ToonMaterialOutput pin 4 -> ToonFeatureRT3.w, as a RAW id; ComposeToonBufferD normalizes it
\t\t// into the 8-bit lane. Same static choice that picked the branch above, so the payload and the
\t\t// id that says how to read it cannot disagree.
\t\tfloat ModifierID;
\t}

\tProperties = {
%s
\t}

\tGraph = {
\t\tModifierData = %s;

\t\tModifierID = %s;
\t}
}
""" % (decls, sel_in_block, props, chain(), id_chain())


def check_modifier_agreement(tables):
    """Every listed modifier must have a slot table, a material function and an id -- both ways."""
    problems = []
    listed = parse_modifier_list(os.path.normpath(MODIFIER_ID_TABLE))
    ids = parse_modifier_ids(os.path.normpath(MODIFIER_ID_TABLE))

    authored = {MODIFIERS[k][1][len("MOON_TOON_MODIFIER_ID_"):] for k in MODIFIERS}
    for id_name in listed:
        if id_name not in authored:
            problems.append("MOON_TOON_MODIFIER_LIST has %s, but no MODIFIERS entry writes it -- "
                            "the shader would dispatch to a module no material can select" % id_name)
        if "MOON_TOON_MOD_SLOTS_%s" % id_name not in tables:
            problems.append("MOON_TOON_MODIFIER_LIST has %s with no MOON_TOON_MOD_SLOTS_%s table"
                            % (id_name, id_name))
    for k in MODIFIERS:
        if MODIFIERS[k][1] not in ids:
            problems.append("%s names %s, which MoonToonModifierDefinitions.h does not define"
                            % (k, MODIFIERS[k][1]))
    return problems



TRUNK = os.path.join(os.path.dirname(SLOT_TABLE), "..", "Private", "Toon", "ToonShadingModel.ush")


def check_module_includes():
    """Every module either registry names must exist on disk AND be #included by the trunk.

    An X-macro cannot emit an #include, so the dispatch switches and the include list are two places
    that have to agree, and only one of them is generated. When they disagree the dispatcher emits a
    call to a symbol nothing defined -- which is a shader compile error four minutes into an editor
    boot, and which is exactly how OilFilm first failed.
    """
    problems = []
    src = open(os.path.normpath(TRUNK), encoding="utf-8").read()
    toon_dir = os.path.dirname(os.path.normpath(TRUNK))

    modules, _features = registry()
    wanted = [("Features/ToonFeature_%s.ush" % m) for m in modules]
    wanted += [("Modifiers/ToonModifier_%s.ush" % m)
               for m in parse_modifier_list(os.path.normpath(MODIFIER_ID_TABLE)).values()]

    for rel in wanted:
        if not os.path.exists(os.path.join(toon_dir, rel.replace("/", os.sep))):
            problems.append("%s is named by a registry but does not exist" % rel)
        elif '#include "%s"' % rel not in src:
            problems.append('ToonShadingModel.ush does not #include "%s" -- the dispatcher will '
                            "call into it and the compiler will not find the symbol" % rel)
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="fail if any generated file differs from what is on disk")
    opts = ap.parse_args()

    tables = parse_slot_tables(SLOT_TABLE)
    mod_tables = parse_slot_tables(os.path.normpath(MODIFIER_SLOT_TABLE),
                                   "MOON_TOON_MOD_SLOTS_")

    slot_defaults = parse_slot_defaults(SLOT_TABLE)
    problems = (check_registry_agreement() + check_bloom_weight_list(tables)
                + check_modifier_agreement(mod_tables) + check_module_includes()
                + check_slot_defaults(slot_defaults))
    if problems:
        for problem in problems:
            print("registry/generator disagree: %s" % problem)
        return 1
    out_dir = os.path.normpath(OUT_DIR)
    os.makedirs(out_dir, exist_ok=True)

    targets = [(os.path.join(out_dir, "MF_ToonFeature_%s.dsf" % FEATURES[k][0]), emit(k, tables))
               for k in FEATURES]
    ids = parse_feature_ids(os.path.normpath(ID_TABLE))
    targets.append((os.path.normpath(SELECT_PATH), emit_selector(tables, ids)))

    mod_dir = os.path.normpath(MODIFIER_OUT_DIR)
    os.makedirs(mod_dir, exist_ok=True)
    targets += [(os.path.join(mod_dir, "MF_ToonModifier_%s.dsf" % MODIFIERS[k][0]),
                 emit_modifier(k, mod_tables)) for k in MODIFIERS]
    mod_ids = parse_modifier_ids(os.path.normpath(MODIFIER_ID_TABLE))
    targets.append((os.path.normpath(MODIFIER_SELECT_PATH),
                    emit_modifier_selector(mod_tables, mod_ids)))

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
