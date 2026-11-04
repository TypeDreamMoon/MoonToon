#!/usr/bin/env python3
"""Lint the material parameters declared in the MoonToon DShader sources (R11 T57a).

Every parameter an artist sees in a M_MoonToon / M_MoonToonOutline instance (and the plugin's other
materials) is declared somewhere under DShader/, in one of three spellings:
  - a 1.x property bag:   ScalarParameter Ident = 0.5 [ParameterName="X"; Group="..."; SortPriority=3; ...];
  - a 1.x node:           UE.Expression(Class="StaticSwitchParameter", ..., Group="...", SortPriority=3, ...)
  - a .dss doc block:     /// @name X   @group ...   @sort 3   @slider 0 1   +   /// @desc ...
The same display name may be declared at several sites (functions that each read it); the MI panel shows
ONE row for it, so every site must agree.

Fails (exit 1) on:
  G1  a group that is not in GROUPS below (the approved T57a map) -- including an unnumbered one;
  G2  one display name under two different groups, or two different sort priorities, across its sites;
  S1  a parameter without SortPriority;
  S2  two parameters with the same (group, sort);
  D1  no description;
  D2  a description that is not bilingual (needs Chinese text AND an English sentence);
  D3  a texture parameter whose description does not say its colour space (sRGB / linear);
  R1  a scalar parameter without a slider (SliderMin / SliderMax, Slider(a, b) or @slider).

usage: python Tools/lint_params.py [--json out.json] [--quiet]
Called by gen_feature_functions.py after it writes; run it before committing any DShader change.
"""
import argparse
import collections
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DSHADER = os.path.normpath(os.path.join(HERE, "..", "DShader"))

# The approved parameter group map (R11 T57a, 11-04). A subgroup is "<group>|<sub>" and its parent must be here.
GROUPS = [
    "00 - Global", "00 - Global|Global Mask Map",
    "01 - Base Color",
    "02 - Normal",
    "03 - Opacity & Translucency", "03 - Opacity & Translucency|Hair Tip",
    "10 - Diffuse", "10 - Diffuse|Shadow Color LUT",
    "11 - Specular",
    "12 - Rim Light",
    "13 - Shadows",
    "14 - Emissive",
    "15 - Sub Material",
    "16 - Coplanar Layer",
    "20 - Face: SDF Shadow", "20 - Face: SDF Shadow|Full Angle", "20 - Face: SDF Shadow|Skin Shading",
    "21 - Face: See Through Overlay",
    "22 - Eye|Parallax", "22 - Eye|Highlight", "22 - Eye|Sparkle", "22 - Eye|Shading",
    "30 - Shading Feature (pick one)",
    "31 - Feature: Default",
    "32 - Feature: PBR Specular",
    "33 - Feature: Hair Kajiya",
    "34 - Feature: Hair Toon Kajiya",
    "35 - Feature: Hair Highlight Mask",
    "36 - Feature: Angel Ring", "36 - Feature: Angel Ring|Mode A", "36 - Feature: Angel Ring|Mode B",
    "37 - Feature: Skin",
    "38 - Feature: Stockings",
    "39 - Feature: Cloth Velvet",
    "40 - Feature: Metal",
    "41 - Feature: Emissive Ink",
    "44 - Matcap",
    "45 - Oil Film", "45 - Oil Film|Thin Film",
    "50 - FX", "51 - FX: Glitter", "52 - FX: Flow Light", "53 - FX: Reveal", "54 - FX: Pulse",
    "55 - FX: View Hue Shift", "56 - FX: Dissolve", "57 - FX: Wet", "58 - FX: Hologram", "59 - FX: Flicker",
    "60 - Inner Outline",
    "70 - Outline", "71 - Outline v2",
    "80 - Bangs Shadow",
    "90 - Debug", "90 - Debug|Strand Preview",
]

KINDS = ("ScalarParameter|VectorParameter|TextureObjectParameter|TextureSampleParameter2D|StaticSwitchParameter|"
         "StaticBoolParameter|ChannelMaskParameter|StaticComponentMaskParameter|CurveAtlasRowParameter|"
         "TextureObjectParameterCube|DoubleVectorParameter")
TEXTURE_KINDS = ("TextureObjectParameter", "TextureSampleParameter2D", "TextureObjectParameterCube", "Texture2D")
SCALAR_KINDS = ("ScalarParameter", "float")
CJK = re.compile(r"[一-鿿]")
EN_WORDS = re.compile(r"[A-Za-z]{3,}(?:\s+[A-Za-z(][A-Za-z0-9,.()'/-]*){2,}")
COLOUR_SPACE = re.compile(r"sRGB|线性|linear|Linear|颜色空间|color space|colour space", re.I)


def _attr(body, key, sep=None):
    """The value of `key=` in an attribute body, OUTSIDE quoted strings (a description may contain 'Group=')."""
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == '"':
            i = _skip_string(body, i)
            continue
        if body.startswith(key, i) and (i == 0 or not (body[i - 1].isalnum() or body[i - 1] == "_")):
            j = i + len(key)
            while j < n and body[j] in " \t\r\n":
                j += 1
            if j < n and body[j] == "=":
                j += 1
                while j < n and body[j] in " \t\r\n":
                    j += 1
                if j < n and body[j] == '"':
                    end = _skip_string(body, j)
                    return body[j + 1:end - 1]
                m = re.match(r"[-\w.]+", body[j:])
                return m.group(0) if m else None
        i += 1
    return None


def _skip_string(src, i):
    """Index just past the string literal that starts at src[i] == '"' (backslash escapes honoured)."""
    i += 1
    while i < len(src):
        if src[i] == "\\":
            i += 2
            continue
        if src[i] == '"':
            return i + 1
        i += 1
    return i


def match_close(src, i, open_ch, close_ch):
    """src[i] == open_ch; the index of its matching close_ch, skipping string literals and // comments."""
    depth = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '"':
            i = _skip_string(src, i)
            continue
        if c == "/" and src.startswith("//", i):
            nl = src.find("\n", i)
            i = n if nl < 0 else nl
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def iter_bag_decls(src):
    """(kind, ident, body_start, body_end, decl_start) for every 1.x property-bag parameter declaration."""
    for m in re.finditer(r"\b(%s)\s+(\w+)\s*=\s*[^\[;\n]*?\[" % KINDS, src):
        lb = m.end() - 1
        rb = match_close(src, lb, "[", "]")
        if rb > 0:
            yield m.group(1), m.group(2), lb + 1, rb, m.start()


def iter_node_decls(src):
    """(kind, body_start, body_end, decl_start) for every UE.Expression(Class="...Parameter...") node."""
    for m in re.finditer(r'UE\.Expression\(\s*Class\s*=\s*"(\w*Parameter\w*)"', src):
        lp = src.index("(", m.start())
        rp = match_close(src, lp, "(", ")")
        if rp > 0:
            yield m.group(1), lp + 1, rp, m.start()


def collect(root):
    sites = []
    for f in sorted(glob.glob(os.path.join(root, "**", "*.ds*"), recursive=True)):
        rel = os.path.relpath(f, root).replace("\\", "/")
        if rel.startswith("Decompiled") or "/Decompiled/" in rel:
            continue
        src = open(f, encoding="utf-8", errors="replace").read()
        if f.endswith(".dss"):
            for m in re.finditer(r"((?:^[ \t]*///[^\n]*\n)+)[ \t]*(?:uniform\s+|static\s+)?([\w<>]+)\s+(\w+)", src, re.M):
                doc = m.group(1)
                if "@group" not in doc and "@name" not in doc:
                    continue
                name = re.search(r"@name\s+(.+?)(?=\s+@|\s*$)", doc, re.M)
                group = re.search(r"@group\s+(.+?)(?=\s+@|\s*$)", doc, re.M)
                sort = re.search(r"@sort\s+(-?\d+)", doc)
                desc = re.search(r"@desc\s+(.+)", doc)
                sites.append(dict(file=rel, kind=m.group(2), name=(name.group(1).strip() if name else m.group(3)),
                                  group=group.group(1).strip() if group else None,
                                  sort=int(sort.group(1)) if sort else None,
                                  desc=desc.group(1).strip() if desc else "",
                                  slider=("@slider" in doc), line=src.count("\n", 0, m.start()) + 1))
            continue
        for kind, ident, b0, b1, d0 in iter_bag_decls(src):
            body = src[b0:b1]
            sort = _attr(body, "SortPriority")
            sites.append(dict(file=rel, kind=kind, name=_attr(body, "ParameterName") or ident,
                              group=_attr(body, "Group"), sort=int(sort) if sort else None,
                              desc=_attr(body, "Description") or _attr(body, "Desc") or "",
                              slider=_attr(body, "SliderMin") is not None or _attr(body, "SliderMax") is not None
                              or bool(re.search(r"(?<![\w])Slider\s*\(", body)),
                              line=src.count("\n", 0, d0) + 1))
        for kind, b0, b1, d0 in iter_node_decls(src):
            body = src[b0:b1]
            name = _attr(body, "ParameterName")
            if name is None:
                continue
            sort = _attr(body, "SortPriority")
            sites.append(dict(file=rel, kind=kind, name=name, group=_attr(body, "Group"),
                              sort=int(sort) if sort else None,
                              desc=_attr(body, "Desc") or _attr(body, "Description") or "",
                              slider=_attr(body, "SliderMin") is not None or _attr(body, "SliderMax") is not None,
                              line=src.count("\n", 0, d0) + 1))
    return sites


def lint(sites):
    issues = []
    by_name = collections.OrderedDict()
    for s in sites:
        by_name.setdefault(s["name"], []).append(s)
    allowed = set(GROUPS)
    seen_sort = {}
    for name, ss in by_name.items():
        where = "%s:%d" % (ss[0]["file"], ss[0]["line"])
        groups = sorted(set(s["group"] for s in ss if s["group"]))
        sorts = sorted(set(s["sort"] for s in ss if s["sort"] is not None))
        for g in groups:
            if g not in allowed:
                issues.append(("G1", name, where, "group '%s' is not in the map" % g))
        if len(groups) > 1:
            issues.append(("G2", name, where, "declared under %d groups: %s" % (len(groups), " / ".join(groups))))
        if not groups:
            issues.append(("G1", name, where, "no group"))
        if len(sorts) > 1:
            issues.append(("G2", name, where, "declared with %d sort priorities: %s" % (len(sorts), sorts)))
        if not sorts:
            issues.append(("S1", name, where, "no SortPriority"))
        elif groups:
            key = (groups[0], sorts[0])
            if key in seen_sort and seen_sort[key] != name:
                issues.append(("S2", name, where, "same (group, sort) as '%s': %s" % (seen_sort[key], key)))
            seen_sort.setdefault(key, name)
        desc = max((s["desc"] for s in ss), key=len)
        if not desc:
            issues.append(("D1", name, where, "no description"))
        elif not (CJK.search(desc) and EN_WORDS.search(CJK.sub(" ", desc))):
            issues.append(("D2", name, where, "description is not bilingual"))
        kinds = set(s["kind"] for s in ss)
        if kinds & set(TEXTURE_KINDS) and desc and not COLOUR_SPACE.search(desc):
            issues.append(("D3", name, where, "texture description does not say its colour space"))
        if kinds & set(SCALAR_KINDS) and not any(s["slider"] for s in ss):
            issues.append(("R1", name, where, "scalar without a slider"))
    return issues, by_name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--root", default=DSHADER)
    opts = ap.parse_args()
    sites = collect(opts.root)
    issues, by_name = lint(sites)
    if opts.json:
        json.dump(dict(params=len(by_name), sites=len(sites),
                       issues=[dict(rule=r, name=n, where=w, message=m) for r, n, w, m in issues]),
                  open(opts.json, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    counts = collections.Counter(r for r, _, _, _ in issues)
    if not opts.quiet:
        for r, n, w, m in issues:
            sys.stdout.buffer.write(("%s  %-45s %s  (%s)\n" % (r, n, m, w)).encode("utf-8"))
    print("lint_params: %d parameters, %d sites, %d issues %s"
          % (len(by_name), len(sites), len(issues), dict(sorted(counts.items()))))
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main())
