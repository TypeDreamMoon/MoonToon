"""Sub-material table layout and writer (MoonToon T22 / G01), shared by the SubMaterial tools.

The layout is the one MF_ToonSubMaterial.dss reads (DShader/MaterialFunctions/Shared): 8 float4 columns per
row, 16 rows. A field is (column, components, mask bit); the override mask is split into three bytes
(column 0 .w = bits 0-7, column 3 .w = bits 8-15, column 5 .w = bits 16-23) so a half-float texture holds it
exactly.

Pure Python (struct only), so it runs both in the editor's interpreter and outside it.
"""
import struct

COLUMNS = 8
ROWS = 16

# field -> (column, component indices, mask bit). Material parameter names in comments.
FIELDS = {
    "ShadowColor":      (0, (0, 1, 2), 0),    # Shadow Color (rgb)
    "Metallic":         (1, (0,), 1),
    "Specular":         (1, (1,), 2),
    "Roughness":        (1, (2,), 3),
    "Reflection":       (1, (3,), 4),
    "Anisotropy":       (2, (0,), 5),
    "RampUVOffset":     (2, (1,), 6),         # reserved: not consumed yet
    "BaseColorTintStrength": (2, (2,), 7),    # reserved: not consumed yet (with BaseColorTint)
    "EmissiveScale":    (2, (3,), 8),         # multiplies Emissive Intensity
    "BaseColorTint":    (3, (0, 1, 2), 7),    # reserved: not consumed yet
    "ScalarA":          (4, (0,), 9),
    "ScalarB":          (4, (1,), 10),
    "ScalarC":          (4, (2,), 11),
    "ScalarD":          (4, (3,), 12),
    "ScalarE":          (5, (0,), 13),
    "ScalarF":          (5, (1,), 14),
    "ScalarG":          (5, (2,), 15),
    "VectorX":          (6, (0,), 16),
    "VectorY":          (6, (1,), 17),
    "VectorZ":          (6, (2,), 18),
    "VectorW":          (6, (3,), 19),
    "OutlineWidthScale": (7, (0,), 20),       # reserved: not consumed yet
    "OutlineColorScale": (7, (1, 2, 3), 21),  # reserved: not consumed yet
}
MASK_SLOTS = ((0, 3), (3, 3), (5, 3))   # (column, component) of mask bytes 0, 1, 2

# Material parameter name -> field, for the surface parameters of MF_MoonToonBaseInput.
SURFACE_PARAMS = {
    "Shadow Color": "ShadowColor",
    "Metallic": "Metallic",
    "Specular": "Specular",
    "Roughness": "Roughness",
    "Reflection": "Reflection",
    "Anisotropy": "Anisotropy",
}

# Feature slot table channel -> field (Engine/Shaders/Shared/MoonToonFeatureSlots.h).
CHANNEL_FIELDS = {
    "FeatureInputScalarA": ("ScalarA",), "FeatureInputScalarB": ("ScalarB",),
    "FeatureInputScalarC": ("ScalarC",), "FeatureInputScalarD": ("ScalarD",),
    "FeatureInputScalarE": ("ScalarE",), "FeatureInputScalarF": ("ScalarF",),
    "FeatureInputScalarG": ("ScalarG",),
    "FeatureInputVector.x": ("VectorX",), "FeatureInputVector.y": ("VectorY",),
    "FeatureInputVector.z": ("VectorZ",), "FeatureInputVector.w": ("VectorW",),
    "FeatureInputVector.xyz": ("VectorX", "VectorY", "VectorZ"),
}


def build_texels(rows):
    """rows: list (<= 16) of {field: value or [values]}. Returns ROWS x COLUMNS x 4 floats (row-major)."""
    if len(rows) > ROWS:
        raise ValueError("a sub-material table has at most %d rows, got %d" % (ROWS, len(rows)))
    tex = [[[0.0, 0.0, 0.0, 0.0] for _ in range(COLUMNS)] for _ in range(ROWS)]
    for r, row in enumerate(rows):
        mask = 0
        for field, value in row.items():
            if field not in FIELDS:
                raise ValueError("unknown field %s" % field)
            col, comps, bit = FIELDS[field]
            vals = list(value) if isinstance(value, (list, tuple)) else [value]
            if len(vals) < len(comps):
                raise ValueError("%s needs %d values" % (field, len(comps)))
            for c, v in zip(comps, vals):
                tex[r][col][c] = float(v)
            mask |= 1 << bit
        for i, (col, comp) in enumerate(MASK_SLOTS):
            tex[r][col][comp] = float((mask >> (8 * i)) & 0xFF)
    return tex


def write_exr(path, texels):
    """Uncompressed scanline OpenEXR, FLOAT channels A, B, G, R (alphabetical), width COLUMNS, height ROWS."""
    h = len(texels)
    w = len(texels[0])

    def attr(name, typ, data):
        return name.encode() + b"\0" + typ.encode() + b"\0" + struct.pack("<i", len(data)) + data

    chlist = b"".join(c.encode() + b"\0" + struct.pack("<iB3xii", 2, 0, 1, 1) for c in "ABGR") + b"\0"
    header = b"\x76\x2f\x31\x01" + struct.pack("<i", 2)
    header += attr("channels", "chlist", chlist)
    header += attr("compression", "compression", b"\0")
    box = struct.pack("<iiii", 0, 0, w - 1, h - 1)
    header += attr("dataWindow", "box2i", box)
    header += attr("displayWindow", "box2i", box)
    header += attr("lineOrder", "lineOrder", b"\0")
    header += attr("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    header += attr("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
    header += attr("screenWindowWidth", "float", struct.pack("<f", 1.0))
    header += b"\0"
    line_bytes = w * 4 * 4
    table_start = len(header)
    first = table_start + 8 * h
    offsets = b"".join(struct.pack("<Q", first + y * (8 + line_bytes)) for y in range(h))
    body = b""
    for y in range(h):
        data = b""
        for ch in (3, 2, 1, 0):   # A, B, G, R
            data += b"".join(struct.pack("<f", texels[y][x][ch]) for x in range(w))
        body += struct.pack("<ii", y, len(data)) + data
    with open(path, "wb") as f:
        f.write(header + offsets + body)


def read_exr(path):
    """The inverse of write_exr, for checking a written table (uncompressed FLOAT ABGR only)."""
    b = open(path, "rb").read()
    i = 8
    attrs = {}
    while b[i] != 0:
        n = b.index(b"\0", i)
        name = b[i:n].decode()
        t = b.index(b"\0", n + 1)
        size = struct.unpack_from("<i", b, t + 1)[0]
        attrs[name] = b[t + 5:t + 5 + size]
        i = t + 5 + size
    i += 1
    x0, y0, x1, y1 = struct.unpack("<iiii", attrs["dataWindow"])
    w, h = x1 - x0 + 1, y1 - y0 + 1
    offs = struct.unpack_from("<%dQ" % h, b, i)
    tex = [[[0.0] * 4 for _ in range(w)] for _ in range(h)]
    for y, o in enumerate(offs):
        p = o + 8
        for ch in (3, 2, 1, 0):
            for x in range(w):
                tex[y][x][ch] = struct.unpack_from("<f", b, p)[0]
                p += 4
    return tex
