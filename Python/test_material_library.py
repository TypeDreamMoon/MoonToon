"""Headless check of UMoonToonMaterialLibrary on throwaway transient instances.

Nothing here touches a project asset: every material instance is created in the transient
package, so the test can re-parent and clear overrides freely. It reads two real materials
(M_MoonToon and M_MoonToonOutline) but only as parents, and never saves anything.

Run it without opening the editor:

    "<engine>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "<project>.uproject" ^
        -run=pythonscript -script="<this file>" -unattended -nosplash -nullrhi ^
        -abslog="<somewhere>/pytest.log"

then grep the log for MoonToonTest; the last line is the failure count.
"""
import unreal

FAILURES = []


def check(label, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print("[MoonToonTest] {:<58} {}{}".format(label, status, (" -- " + detail) if detail else ""))
    if not condition:
        FAILURES.append(label)


lib = unreal.MoonToonMaterialLibrary
mel = unreal.MaterialEditingLibrary

toon = unreal.load_asset('/MoonToon/Materials/Core/M_MoonToon')
# M_MoonToonOutline shares MF_MoonToonBaseInput with M_MoonToon, so it declares the same
# parameters -- exactly the case where an override is expected to survive a re-parent.
outline = unreal.load_asset('/MoonToon/Materials/Core/M_MoonToonOutline')
# An unrelated material, for the case where it is not.
stranger = unreal.load_asset('/Engine/EngineMaterials/WorldGridMaterial')
check("load parents", None not in (toon, outline, stranger),
      "{} / {} / {}".format(toon, outline, stranger))

# --- one instance, one override -------------------------------------------------------------
mic = unreal.new_object(unreal.MaterialInstanceConstant)
mel.set_material_instance_parent(mic, toon)
check("parent assigned", mic.get_editor_property('parent') == toon)

# A parameter M_MoonToon really has (it is on MF_MoonToonBaseInput).
mel.set_material_instance_scalar_parameter_value(mic, "Rim Light Intensity", 0.25)
report = lib.describe_instances([mic])
check("override is reported live", "Rim Light Intensity" in report and "orphaned" not in report,
      report.replace("\n", " | ")[:120])

# --- re-parent to a material that shares the parameter set -----------------------------------
report = lib.set_parent_on_instances([mic], outline, True)
check("shared parameter survives a re-parent",
      "1 override(s) kept" in report and "cleared" not in report,
      report.replace("\n", " | ")[:160])
check("value survived", abs(mel.get_material_instance_scalar_parameter_value(
    mic, "Rim Light Intensity") - 0.25) < 1e-4)

# --- re-parent to a material that does not have that parameter -------------------------------
report = lib.set_parent_on_instances([mic], stranger, False)
check("re-parent reports the dead override", "now dead" in report and "Rim Light Intensity" in report,
      report.replace("\n", " | ")[:160])
check("parent actually changed", mic.get_editor_property('parent') == stranger)

report = lib.describe_instances([mic])
check("override is now orphaned", "orphaned" in report, report.replace("\n", " | ")[:120])

# --- clearing orphans ------------------------------------------------------------------------
report = lib.clear_orphaned_overrides([mic])
check("orphan cleared", "Rim Light Intensity" in report,
      report.replace("\n", " | ")[:120])
report = lib.describe_instances([mic])
check("nothing left over", "orphaned" not in report and "overrides : 0" in report,
      report.replace("\n", " | ")[:120])

# --- overrides that survive a re-parent ------------------------------------------------------
keeper = unreal.new_object(unreal.MaterialInstanceConstant)
mel.set_material_instance_parent(keeper, toon)
mel.set_material_instance_scalar_parameter_value(keeper, "Rim Light Intensity", 0.75)

sibling = unreal.new_object(unreal.MaterialInstanceConstant)
mel.set_material_instance_parent(sibling, toon)

report = lib.set_parent_on_instances([keeper], toon, True)
check("same parent is a no-op", "already had this parent" in report,
      report.replace("\n", " | ")[:120])
check("value survived the no-op",
      abs(mel.get_material_instance_scalar_parameter_value(keeper, "Rim Light Intensity") - 0.75) < 1e-4)

# --- copy overrides --------------------------------------------------------------------------
report = lib.copy_overrides(keeper, [sibling])
check("copy reports one parameter", "1 copied" in report, report.replace("\n", " | ")[:120])
check("copied value landed",
      abs(mel.get_material_instance_scalar_parameter_value(sibling, "Rim Light Intensity") - 0.75) < 1e-4)
check("copy created a real override", "Rim Light Intensity" in lib.describe_instances([sibling]))

# --- reset -----------------------------------------------------------------------------------
report = lib.reset_overrides([sibling], [])
check("reset reports one", "1 reset to parent" in report, report.replace("\n", " | ")[:120])
check("reset dropped the override", "overrides : 0" in lib.describe_instances([sibling]))
check("value fell back to the parent's",
      abs(mel.get_material_instance_scalar_parameter_value(sibling, "Rim Light Intensity")
          - mel.get_material_instance_scalar_parameter_value(keeper, "Rim Light Intensity")) > 1e-4
      or mic is None)

# --- the panel's own write path, across several instances -------------------------------------
a = unreal.new_object(unreal.MaterialInstanceConstant)
b = unreal.new_object(unreal.MaterialInstanceConstant)
stray = unreal.new_object(unreal.MaterialInstanceConstant)
for m, p in ((a, toon), (b, toon), (stray, stranger)):
    mel.set_material_instance_parent(m, p)

report = lib.set_scalar_on_instances([a, b, stray], "Rim Light Intensity", 0.4)
check("batch scalar writes the ones that have it", "on 2 of 3 instance(s)" in report,
      report.replace("\n", " | ")[:140])
check("batch scalar skips the stranger", "Not a parameter of" in report)
check("batch scalar landed on both",
      abs(mel.get_material_instance_scalar_parameter_value(a, "Rim Light Intensity") - 0.4) < 1e-4 and
      abs(mel.get_material_instance_scalar_parameter_value(b, "Rim Light Intensity") - 0.4) < 1e-4)
check("stranger untouched", "overrides : 0" in lib.describe_instances([stray]),
      lib.describe_instances([stray]).replace("\n", " | ")[:120])

# --- static switches take a different path out of the instance --------------------------------
sw = unreal.new_object(unreal.MaterialInstanceConstant)
mel.set_material_instance_parent(sw, toon)
report = lib.set_static_switch_on_instances([sw], "Use Custom Diffuse Ramp", True)
check("batch switch reports one", "on 1 of 1 instance(s)" in report, report.replace("\n", " | ")[:120])
check("static switch override reported", "Use Custom Diffuse Ramp" in lib.describe_instances([sw]),
      lib.describe_instances([sw]).replace("\n", " | ")[:140])
# Engine behaviour, not ours: writing ANY static switch on an instance also leaves a dead
# "<name>_NNN" entry in its static parameter set (the number varies per process, and it
# reproduces through the stock MaterialEditingLibrary helper with publishing turned off).
# Assert the shape of it, so a fix upstream -- or a *different* kind of junk -- both show up.
_desc = lib.describe_instances([sw])
check("real switch is live", "live      : Use Custom Diffuse Ramp (switch)" in _desc,
      _desc.replace("\n", " | ")[:140])
check("only the known engine phantom is orphaned",
      ("orphaned" not in _desc) or
      all(part.strip().startswith("Use Custom Diffuse Ramp_")
          for part in _desc.split("orphaned  : ")[1].split("\n")[0].split(",")),
      _desc.replace("\n", " | ")[:180])
report = lib.reset_overrides([sw], [])
check("static switch reset reports one", "1 reset to parent" in report,
      report.replace("\n", " | ")[:120])
check("static switch override gone", "Use Custom Diffuse Ramp" not in lib.describe_instances([sw]),
      lib.describe_instances([sw]).replace("\n", " | ")[:140])

# --- guards ----------------------------------------------------------------------------------
check("no instances is handled", "No material instances" in lib.set_parent_on_instances([], toon, True))
check("no parent is handled", "No parent" in lib.set_parent_on_instances([mic], None, True))

child = unreal.new_object(unreal.MaterialInstanceConstant)
mel.set_material_instance_parent(child, keeper)
report = lib.set_parent_on_instances([keeper], child, True)
check("cyclic parent refused", "SKIPPED" in report, report.replace("\n", " | ")[:140])
check("cyclic parent left alone", keeper.get_editor_property('parent') == toon)

print("[MoonToonTest] ==== {} failure(s) ====".format(len(FAILURES)))
for name in FAILURES:
    print("[MoonToonTest] FAILED: {}".format(name))
