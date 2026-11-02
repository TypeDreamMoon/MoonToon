"""Build a sub-material table (MoonToon T22) for one texture group from dump_slot_group.py's JSON.

Run:  python build_table.py <group.json> <out_dir> [--name T_SubMat_Down]

Checks the group can share one material (same blend mode, two-sidedness and static switches), finds the
parameters that differ between the slots, maps each to a table field -- surface parameters directly, the
active feature's In_<Feature>_<Slot> parameters through the feature slot table (the same mapping the
generated MF_ToonFeature_* functions use) -- and writes:

    <out_dir>/<name>.exr    the 8 x 16 table (row i = the i-th slot of the dump)
    <out_dir>/<name>.json   the rows as {field: value} and a report (slot -> row, what each row overrides)

Only differing parameters become overrides: everything else keeps the merged instance's own value.
A differing parameter the table cannot carry (a texture, a parameter of an inactive feature that would map
to the same generic channel, an unsupported surface parameter) is listed, and fails the build unless
--allow-unmapped is given.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import submat_common as S  # noqa: E402
import gen_feature_functions as G  # noqa: E402


def active_feature(switches):
    """The generator feature key the instance shades with (an 'Is <Feature>' switch that is on)."""
    on = [k for k in G.FEATURES if switches.get("Is " + k)]
    if len(on) > 1:
        print("warning: several feature switches on %s; using %s" % (on, on[0]))
    return on[0] if on else "Default"


def feature_param_fields(key, tables):
    """{material parameter name: [field per component]} for one feature."""
    out = {}
    for (t, slot, name) in tables[G.slot_table_for(key)]:
        pname = G.PARAMS[key][name][0]
        out[pname] = list(S.CHANNEL_FIELDS[slot])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("group_json")
    ap.add_argument("out_dir")
    ap.add_argument("--name", default="T_SubMat")
    ap.add_argument("--allow-unmapped", action="store_true")
    a = ap.parse_args()

    g = json.load(open(a.group_json, encoding="utf-8"))
    slots = g["slots"]
    first = slots[0]
    problems = []
    for s in slots[1:]:
        if s["blend"] != first["blend"] or s["two_sided"] != first["two_sided"]:
            problems.append("slot %d: blend / two-sided %s/%s differs from slot %d's %s/%s"
                            % (s["slot"], s["blend"], s["two_sided"], first["slot"], first["blend"], first["two_sided"]))
        for k in set(s["switches"]) | set(first["switches"]):
            if s["switches"].get(k) != first["switches"].get(k):
                problems.append("slot %d: static switch %s differs" % (s["slot"], k))
    if problems:
        print("cannot merge into one material:\n  " + "\n  ".join(problems))
        return 1

    tables = G.parse_slot_tables(G.SLOT_TABLE)
    feature = active_feature(first["switches"])
    fmap = feature_param_fields(feature, tables)

    differing = []
    for kind in ("scalars", "vectors"):
        for k in sorted(set().union(*[set(s[kind]) for s in slots])):
            vals = [json.dumps(s[kind].get(k)) for s in slots]
            if len(set(vals)) > 1:
                differing.append((kind, k))

    # Parameters of features the group does NOT shade with change nothing on screen: leftovers on the
    # instances, not something to carry. Listed, never mapped (they would land on the active feature's
    # generic channels and mean something else there).
    inactive = set()
    for key in G.FEATURES:
        if key != feature:
            inactive |= set(feature_param_fields(key, tables))
    rows = [{} for _ in slots]
    mapped, unmapped, ignored = [], [], []
    for kind, k in differing:
        if k in S.SURFACE_PARAMS:
            field = S.SURFACE_PARAMS[k]
            for r, s in enumerate(slots):
                v = s[kind][k]
                rows[r][field] = v[:3] if kind == "vectors" else v
            mapped.append((k, field))
        elif k in fmap:
            fields = fmap[k]
            for r, s in enumerate(slots):
                v = s[kind][k]
                vals = v[:len(fields)] if kind == "vectors" else [v]
                for f, x in zip(fields, vals):
                    rows[r][f] = x
            mapped.append((k, "+".join(fields)))
        elif k in inactive:
            ignored.append(k)
        else:
            unmapped.append(k)

    report = {"feature": feature, "slots": [s["slot"] for s in slots], "mapped": mapped, "unmapped": unmapped,
              "ignored_inactive_feature": ignored,
              "rows": [{"row": r, "slot": s["slot"], "slot_name": s["slot_name"], "mi": s["mi"], "overrides": rows[r]}
                       for r, s in enumerate(slots)]}
    print("feature %s; %d slots; %d differing parameters" % (feature, len(slots), len(differing)))
    for k, f in mapped:
        print("  %-36s -> %s" % (k, f))
    if ignored:
        print("  ignored (feature not active, no effect): %s" % ignored)
    if unmapped:
        print("  NOT carried by the table: %s" % unmapped)
        if not a.allow_unmapped:
            return 1

    os.makedirs(a.out_dir, exist_ok=True)
    S.write_exr(os.path.join(a.out_dir, a.name + ".exr"), S.build_texels(rows))
    json.dump(report, open(os.path.join(a.out_dir, a.name + ".json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("wrote %s.exr / .json in %s" % (a.name, a.out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
