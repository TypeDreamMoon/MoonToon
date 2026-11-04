"""R11 A9: UMoonToonProfile's RimLightIntensity default went 1.0 -> 0.5 (engine), the instance default.
A profile that was ON the old default never stored it (tagged serialisation writes only non-defaults), so it
now loads 0.5. Run in the editor AFTER the engine change, with BEFORE = "<json written by MODE='record' on the
old engine>" -- that pass records every profile's intensity; this pass writes an explicit 1.0 into every
profile that read 1.0 then and reads 0.5 now, and saves only those.
Prefix: MODE = "record" | "apply", BEFORE = "<path>.json".  Paths stay in the JSON (the bridge mangles non-ASCII)."""
import json
import unreal

ar = unreal.AssetRegistryHelpers.get_asset_registry()
flt = unreal.ARFilter(class_paths=[unreal.TopLevelAssetPath("/Script/Engine", "MoonToonProfile")], recursive_classes=True)
profiles = sorted(str(a.package_name) for a in ar.get_assets(flt))

if MODE == "record":
    rows = {}
    for p in profiles:
        s = unreal.load_asset(p).get_editor_property("settings")
        rows[p] = s.get_editor_property("rim_light_intensity")
    json.dump(rows, open(BEFORE, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("recorded", len(rows))
else:
    before = json.load(open(BEFORE, encoding="utf-8"))
    fixed, pkgs = [], []
    for p in profiles:
        o = unreal.load_asset(p)
        s = o.get_editor_property("settings")
        now = s.get_editor_property("rim_light_intensity")
        if abs(before.get(p, -1.0) - 1.0) < 1e-6 and abs(now - 0.5) < 1e-6:
            s.set_editor_property("rim_light_intensity", 1.0)
            o.set_editor_property("settings", s)
            o.modify()
            fixed.append(p)
            pkgs.append(o.get_outermost())
    ok = unreal.EditorLoadingAndSavingUtils.save_packages(pkgs, False) if pkgs else True
    json.dump({"fixed": fixed, "saved": bool(ok)}, open(BEFORE.replace(".json", "_applied.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("fixed", len(fixed), "saved", ok)
