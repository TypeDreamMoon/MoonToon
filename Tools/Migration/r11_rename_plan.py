"""R11 T57b: parameter-rename migration, offline.

  plan    python -I r11_rename_plan.py plan <snapshot_before.json> <renames.json> <plan.json>
  verify  python -I r11_rename_plan.py verify <snapshot_before.json> <snapshot_after.json> <renames.json>
  cleanup python -I r11_rename_plan.py cleanup <snapshot_after.json> <renames.json> <plan.json>   (stale old entries)

The snapshots come from r11_census.py (taken BEFORE the renamed materials are regenerated, and after the
migration is saved). A rename is a mirror: every override an instance stores under an old name is written under
the new name with the same kind and value, so inheritance is reproduced exactly -- no top-down reasoning
needed. Apply the plan in the editor with r11_apply.py, after the regen. The old-name entries left in an
instance are stale (the material no longer has them): UE drops stale static switches on save and ignores the rest.

verify checks, per instance, that every own override and every EFFECTIVE value (the parent chain merged) under
an old name before equals the value under the new name after, and that nothing else changed."""
import json
import sys

KIND = {"scalars": "s", "vectors": "v", "textures": "t", "switches": "w"}


def load(p):
    return json.load(open(p, encoding="utf-8"))


def plan(snap_path, renames_path, out_path):
    snap, ren = load(snap_path), load(renames_path)["renames"]
    ops = []
    for mi, m in snap["mis"].items():
        if not (m.get("root") or "").startswith("/MoonToon/"):
            continue
        for sec, kind in KIND.items():
            for name, value in (m.get(sec) or {}).items():
                if name in ren:
                    ops.append([mi, kind, ren[name], value, "T57b rename %s" % name])
    touched = sorted(set(o[0] for o in ops))
    json.dump({"ops": ops, "touched": touched, "summary": {"T57b rename": len(ops)}},
              open(out_path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("ops %d on %d instances" % (len(ops), len(touched)))


def _close(a, b):
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        return all(abs(float(x) - float(y)) < 1e-5 for x, y in zip(a, b))
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool):
        return abs(a - b) < 1e-5
    return a == b


def verify(before_path, after_path, renames_path):
    b, a, ren = load(before_path), load(after_path), load(renames_path)["renames"]
    olds = set(ren)
    bad, checked, stale = [], 0, 0
    # names that are scalars after the rename: a SWITCH override on one is stale (R11 B4 turned some switches
    # into 0/1 scalars and mirrored their overrides as scalars; the old switch entry is dropped on save).
    scalar_names = set()
    for m in a["mis"].values():
        scalar_names.update((m.get("scalars") or {}).keys())
        scalar_names.update(((m.get("effective") or {}).get("scalars") or {}).keys())
    for mi, mb in b["mis"].items():
        if not (mb.get("root") or "").startswith("/MoonToon/"):
            continue
        ma = a["mis"].get(mi)
        if ma is None:
            bad.append((mi, "missing after"))
            continue
        for view in ("own", "effective"):
            for sec in KIND:
                sb = (mb.get(sec) if view == "own" else mb.get("effective", {}).get(sec)) or {}
                sa = (ma.get(sec) if view == "own" else ma.get("effective", {}).get(sec)) or {}
                for name, value in sb.items():
                    if name in olds:
                        if sec == "switches" and ren[name] in scalar_names and ren[name] not in sa:
                            stale += 1
                            continue
                        checked += 1
                        if not _close(sa.get(ren[name]), value):
                            bad.append((mi, view, sec, name, value, sa.get(ren[name])))
                    elif name not in ren.values() and not _close(sa.get(name), value):
                        if not (sec == "switches" and name not in sa):  # stale switches drop on save
                            bad.append((mi, view, sec, name, value, sa.get(name), "unrelated change"))
    print("checked %d renamed values (%d stale switch entries on what is now a scalar skipped); %d problems"
          % (checked, stale, len(bad)))
    for x in bad[:40]:
        print("  ", x)
    return 1 if bad else 0


def cleanup(snap_after_path, renames_path, out_path):
    """Drop the stale old-name scalar / vector / texture entries an instance still stores after the migration
    (UE does not prune values of parameters the material no longer has). Only where the new name is there too."""
    snap, ren = load(snap_after_path), load(renames_path)["renames"]
    ops = []
    for mi, m in snap["mis"].items():
        if not (m.get("root") or "").startswith("/MoonToon/"):
            continue
        for sec in ("scalars", "vectors", "textures"):
            own = m.get(sec) or {}
            for name in own:
                if name in ren and ren[name] in own:
                    ops.append([mi, "d", name, sec, "T57b drop stale %s" % name])
    touched = sorted(set(o[0] for o in ops))
    json.dump({"ops": ops, "touched": touched, "summary": {"T57b drop stale": len(ops)}},
              open(out_path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("drop ops %d on %d instances" % (len(ops), len(touched)))


if __name__ == "__main__":
    if sys.argv[1] == "plan":
        plan(*sys.argv[2:5])
    elif sys.argv[1] == "cleanup":
        cleanup(*sys.argv[2:5])
    else:
        sys.exit(verify(*sys.argv[2:5]))
