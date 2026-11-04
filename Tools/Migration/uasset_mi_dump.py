"""Offline reader for an editor-saved MaterialInstanceConstant .uasset (UE5 file version >= 1012, complete
property type names). Read-only; no editor needed. Prints the MI's overrides in the same shape as wm_midump.py
(scalars / vectors / textures / switches / parent) so the two can be diffed.

usage: python -I uasset_mi_dump.py <file.uasset> [<file2.uasset> ...] > out.json"""
import json
import struct
import sys


class R:
    def __init__(self, b, pos=0):
        self.b, self.p = b, pos

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.p)[0]; self.p += 4; return v

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.p)[0]; self.p += 4; return v

    def i64(self):
        v = struct.unpack_from("<q", self.b, self.p)[0]; self.p += 8; return v

    def f32(self):
        v = struct.unpack_from("<f", self.b, self.p)[0]; self.p += 4; return v

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def raw(self, n):
        v = self.b[self.p:self.p + n]; self.p += n; return v

    def fstr(self):
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:
            s = self.raw(-2 * n).decode("utf-16-le")
        else:
            s = self.raw(n).decode("latin-1")
        return s[:-1]


class Pkg:
    def __init__(self, path):
        b = open(path, "rb").read()
        r = R(b)
        assert r.u32() == 0x9E2A83C1
        legacy = r.i32(); assert legacy in (-8, -9), legacy
        r.i32()  # LegacyUE3Version
        self.ue4 = r.i32(); self.ue5 = r.i32(); r.i32()
        assert self.ue5 >= 1016, self.ue5  # saved hash + complete type names
        r.raw(20); self.total_header = r.i32()
        for _ in range(r.i32()):
            r.raw(20)
        self.package_name = r.fstr()
        self.package_flags = r.u32()
        name_count, name_off = r.i32(), r.i32()
        sop_count, sop_off = r.i32(), r.i32()
        r.fstr()  # LocalizationId (editor package)
        r.i32(); r.i32()  # gatherable text
        exp_count, exp_off = r.i32(), r.i32()
        imp_count, imp_off = r.i32(), r.i32()
        self.b = b
        rn = R(b, name_off)
        self.names = []
        for _ in range(name_count):
            self.names.append(rn.fstr()); rn.u32()
        # soft object paths: FTopLevelAssetPath (2 FNames) + FString subpath
        rs = R(b, sop_off)
        self.soft = []
        for _ in range(sop_count):
            pk, an = self.fname(rs), self.fname(rs)
            self.soft.append("%s.%s%s" % (pk, an, (":" + rs.fstr()) or ""))
        stride = (exp_off - imp_off) // imp_count
        assert stride in (36, 40), stride
        self.imports = []
        for i in range(imp_count):
            ri = R(b, imp_off + i * stride)
            cpkg, cname, outer, oname = self.fname(ri), self.fname(ri), ri.i32(), self.fname(ri)
            self.imports.append((cname, outer, oname))
        self.exports = []
        for i in range(exp_count):
            re_ = R(b, exp_off + i * 112)
            cls, sup, tmpl, outer = re_.i32(), re_.i32(), re_.i32(), re_.i32()
            oname = self.fname(re_); re_.u32()
            size, off = re_.i64(), re_.i64()
            re_.raw(4 * 3 + 4 + 4 + 4 + 4 + 4 + 4 * 5)
            s0, s1 = re_.i64(), re_.i64()
            self.exports.append(dict(cls=self.ref(cls), name=oname, off=off, size=size, s0=s0, s1=s1))

    def fname(self, r):
        i, n = r.i32(), r.i32()
        s = self.names[i]
        return s if n == 0 else "%s_%d" % (s, n - 1)

    def ref(self, idx):
        if idx == 0:
            return None
        if idx < 0:
            parts, k = [], -idx - 1
            while True:
                cname, outer, oname = self.imports[k]
                parts.append(oname)
                if outer == 0:
                    break
                k = -outer - 1
            return ".".join(reversed(parts)) if len(parts) > 1 else parts[0]
        return "export:" + self.exports[idx - 1]["name"] if hasattr(self, "exports") and idx - 1 < len(self.exports) else "export#%d" % idx

    def type_name(self, r):
        nodes, remaining = [], 1
        while remaining:
            nm, inner = self.fname(r), r.i32()
            nodes.append((nm, inner)); remaining += inner - 1

        def build(it):
            nm, inner = next(it)
            return [nm] + [build(it) for _ in range(inner)]
        return build(iter(nodes))

    def tags(self, r, end):
        out = {}
        while r.p < end:
            name = self.fname(r)
            if name == "None":
                break
            t = self.type_name(r)
            size, flags = r.i32(), r.u8()
            idx = r.i32() if flags & 1 else 0
            if flags & 2:
                r.raw(16)
            if flags & 4:
                ext = r.u8()
                if ext & 2:
                    r.u8(); r.u32()
            start = r.p
            val = self.value(r, t, size, flags, start + size)
            r.p = start + size
            key = name if idx == 0 else "%s[%d]" % (name, idx)
            out[key] = val
        return out

    def value(self, r, t, size, flags, end):
        k = t[0]
        if k == "BoolProperty":
            return bool(flags & 0x10)
        if k == "IntProperty":
            return r.i32()
        if k == "FloatProperty":
            return round(r.f32(), 5)
        if k == "DoubleProperty":
            return struct.unpack_from("<d", r.b, r.p)[0]
        if k in ("NameProperty", "EnumProperty"):
            return self.fname(r)
        if k == "ByteProperty":
            return self.fname(r) if size == 8 else r.u8()
        if k == "StrProperty":
            return r.fstr()
        if k in ("ObjectProperty", "ClassProperty"):
            return self.ref(r.i32())
        if k == "SoftObjectProperty":
            i = r.i32()
            return self.soft[i] if 0 <= i < len(self.soft) else "soft#%d" % i
        if k == "StructProperty":
            sname = t[1][0] if len(t) > 1 else "?"
            if flags & 8:
                return self.binary_struct(sname, r.raw(size))
            return self.tags(r, end)
        if k == "ArrayProperty":
            inner = t[1]
            n = r.i32()
            items = []
            for _ in range(n):
                if inner[0] == "StructProperty":
                    if flags & 8:
                        return "<native struct array %d bytes>" % size
                    items.append(self.tags(r, end))
                else:
                    sz = {"IntProperty": 4, "FloatProperty": 4, "ObjectProperty": 4, "NameProperty": 8,
                          "EnumProperty": 8, "BoolProperty": 1, "ByteProperty": 1}.get(inner[0])
                    if sz is None:
                        return "<array of %s, %d bytes>" % (inner[0], size)
                    p0 = r.p
                    if inner[0] == "BoolProperty":
                        items.append(bool(r.u8()))
                    else:
                        items.append(self.value(r, inner, sz, 0, p0 + sz))
                    r.p = p0 + sz
            return items
        return "<%s %d bytes>" % (k, size)

    @staticmethod
    def binary_struct(sname, data):
        if sname.endswith("LinearColor") and len(data) == 16:
            return [round(x, 5) for x in struct.unpack("<4f", data)]
        if sname.endswith("Guid") and len(data) == 16:
            return data.hex()
        return "<%s %d bytes>" % (sname, len(data))

    def mi(self):
        e = [x for x in self.exports if x["cls"] and x["cls"].endswith("MaterialInstanceConstant")][0]
        r = R(self.b, e["off"] + e["s0"])
        ctrl = r.u8()  # EClassSerializationControlExtension (UE5 >= 1011)
        if ctrl & 2:
            r.u8()
        return self.tags(r, e["off"] + e["s1"])


def pname(entry):
    pi = entry.get("ParameterInfo", {})
    return pi.get("Name"), pi.get("Association"), pi.get("Index")


def summarize(props):
    res = {"parent": props.get("Parent")}
    res["scalars"] = {pname(e)[0]: e.get("ParameterValue") for e in props.get("ScalarParameterValues", [])}
    res["vectors"] = {pname(e)[0]: e.get("ParameterValue") for e in props.get("VectorParameterValues", [])}
    res["textures"] = {pname(e)[0]: e.get("ParameterValue") for e in props.get("TextureParameterValues", [])}
    sw = {}
    for holder in ("StaticParametersRuntime", "StaticParameters"):
        sp = props.get(holder)
        if isinstance(sp, dict):
            for e in sp.get("StaticSwitchParameters", []):
                sw[pname(e)[0]] = {"value": e.get("Value", False), "override": e.get("bOverride", False)}
    res["switches"] = sw
    res["other_top_level"] = sorted(k for k in props if k not in (
        "Parent", "ScalarParameterValues", "VectorParameterValues", "TextureParameterValues",
        "StaticParametersRuntime", "StaticParameters"))
    res["raw_other"] = {k: props[k] for k in res["other_top_level"]}
    return res


if __name__ == "__main__":
    out = {}
    for f in sys.argv[1:]:
        p = Pkg(f)
        out[p.package_name] = summarize(p.mi())
    sys.stdout.buffer.write(json.dumps(out, ensure_ascii=False, indent=1, default=str).encode("utf-8"))
