#!/usr/bin/env python3
"""Check the hook contract against the trunk and the modules, in a second rather than in a boot.

Two failure modes, both of which cost a four-minute editor boot to discover, and neither of which
the preprocessor or a token-level wiring proof can see:

  STALE   -- a FToonShadeContext field copied before the trunk has finished writing the local it
             came from. The modules read a value the shading never used. It compiles, it runs, and
             it is merely a different colour. P4 shipped this for LightBanded.

  MISSING -- a module reads Ctx.<field> or Policy.<field> that the struct does not have. The
             contract lives in one file and the modules in others, so adding the reader without the
             field is two edits, and a patch that lands only the second one fails nowhere but in the
             compiler. P5b shipped this for Policy.Stage.

    python Tools/check_context_staleness.py

Exit 0 if clean.

STALE is the rung the v3 ladder was missing. The wiring proof compares module bodies against the
branch they came from after un-prefixing `Ctx.`, so `Ctx.LightBanded` and `LightBanded` are the same
token and it passes -- by construction it cannot see that the two hold different values.
"""
import io
import os
import re
import sys

CONTEXT = r"F:\UnrealEngine\UE_Moon\Engine\Shaders\Private\Toon\ToonShadeContext.ush"
TRUNK = r"F:\UnrealEngine\UE_Moon\Engine\Shaders\Private\Toon\ToonShadingModel.ush"
MODULE_DIRS = [os.path.join(os.path.dirname(CONTEXT), d) for d in ("Modifiers", "Features")]

# The phase setters, in the order they must be called. A field assigned by one of these is copied at
# that point; a field assigned in MakeToonShadeContext is copied at construction.
PHASES = ["MakeToonShadeContext", "ToonShadeContext_SetShading", "ToonShadeContext_SetBxDF"]

# Locals whose name collides with a context field but which are not its source.
EXEMPT = {"GBuffer", "ToonGBuffer", "AreaLight", "ToonLighting", "ToonView", "Context"}

# Trunk helpers that WRITE their first argument. Listed rather than guessed: "appears as the first
# argument of a call" also describes every read, and treating those as writes reported the diffuse
# hook's own dispatch argument as a stale field.
MUTATORS = {"ColorSaturationPowerAndScale"}

COMMENT = re.compile(r"//[^\n]*")


def context_field_phases(src):
    """field name -> index into PHASES of the setter that assigns Ctx.<field>."""
    out = {}
    for i, setter in enumerate(PHASES):
        m = re.search(re.escape(setter) + r"\((?:.|\n)*?\n\}", src)
        if not m:
            raise SystemExit("%s: %s not found" % (CONTEXT, setter))
        # LAST setter wins: if a field is assigned in two of them, the later assignment is the value
        # a module actually reads, so that is the phase it belongs to. Which is also why "assigned
        # in Make as well" is not by itself a bug -- the stale shape is assigned in Make and NOT
        # re-assigned later.
        for field in re.findall(r"Ctx\.(\w+)\s*=", m.group(0)):
            out[field] = i
    return out


def struct_members(src, struct):
    m = re.search(r"struct\s+" + struct + r"\s*\{(.*?)\n\};", src, re.S)
    if not m:
        raise SystemExit("struct %s not found in %s" % (struct, CONTEXT))
    return set(re.findall(r"\b(\w+)\s*;", COMMENT.sub("", m.group(1))))


def trunk_body():
    """(lines, phase call line per PHASES entry) for ToonBxDF."""
    lines = io.open(TRUNK, encoding="utf-8").read().split("\n")
    calls = []
    for setter in PHASES:
        hit = [i for i, l in enumerate(lines) if setter + "(" in l and "//" not in l.split(setter)[0]]
        if not hit:
            raise SystemExit("%s: no call to %s" % (TRUNK, setter))
        calls.append(hit[0])
    return lines, calls


def check_stale(src):
    fields = context_field_phases(src)
    lines, calls = trunk_body()

    problems = []
    for field, phase in sorted(fields.items()):
        if field in EXEMPT:
            continue
        copied_at = calls[phase]
        assign = re.compile(r"^\s*(?:const\s+)?(?:\w+\s+)?" + re.escape(field) + r"\s*[-+*/]?=[^=]"
                            r"|^\s*(?:%s)\(\s*%s\s*,"
                            % ("|".join(sorted(MUTATORS)), re.escape(field)))
        late = [i for i, l in enumerate(lines) if assign.match(l) and i > copied_at]
        if late:
            problems.append("STALE    %s: copied into the context at line %d (%s), but the trunk "
                            "still writes it at line %s -- the modules read a snapshot from before "
                            "that" % (field, copied_at + 1, PHASES[phase],
                                      ", ".join(str(i + 1) for i in late)))
    return problems, len(fields)


def check_module_fields(src):
    """Every Ctx.<field> and Policy.<field> a module reads must exist in the contract structs."""
    ctx = struct_members(src, "FToonShadeContext")
    policies = struct_members(src, "FToonModifierPolicy") | struct_members(src, "FToonFeaturePolicy")

    problems, checked = [], 0
    for d in MODULE_DIRS:
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".ush"):
                continue
            checked += 1
            body = COMMENT.sub("", io.open(os.path.join(d, name), encoding="utf-8").read())
            for field in sorted(set(re.findall(r"\bCtx\.(\w+)", body))):
                if field not in ctx:
                    problems.append("MISSING  %s: Ctx.%s is not a member of FToonShadeContext"
                                    % (name, field))
            for field in sorted(set(re.findall(r"\bPolicy\.(\w+)", body))):
                if field not in policies:
                    problems.append("MISSING  %s: Policy.%s is in neither policy struct"
                                    % (name, field))
    return problems, checked


def main():
    src = io.open(CONTEXT, encoding="utf-8").read()
    stale, n_fields = check_stale(src)
    missing, n_modules = check_module_fields(src)
    problems = stale + missing

    for p in problems:
        print(p)
    print("%d context fields against ToonBxDF, %d modules against the contract: %s"
          % (n_fields, n_modules, "OK" if not problems else "%d PROBLEM(S)" % len(problems)))
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
