#!/usr/bin/env python3
"""Catch the two classes of breakage the shader PREPROCESSOR structurally cannot.

ppdrv (the stb_preprocess driver used to prove macro expansion) is a preprocessor. It happily emits
`f(X)` where X is declared twenty lines later, and it happily emits an `else` with no `if` -- both
are syntax and scope, which only the HLSL compiler checks, four minutes and a full editor boot away.
v3 shipped one of each before this existed:

  * P4's Kajiya step removed two arms from an if/else chain and left the trailing `else` dangling.
  * P4's tangent hook was called from the first statement of ToonBxDF, above the declaration of the
    id it dispatches on -- 355 shader compiler errors, all cascading from that one line.

    python Tools/check_shader_structure.py <file.ush> [more...]

Exit 0 if clean.

FALSE POSITIVES ARE THE FAILURE MODE, not false negatives -- the compiler is still the authority, so
a check that cries wolf is worse than no check because it trains you to skip the output. The first
cut reported twenty-one findings on ToonBufferCommon.ush (names inside its payload doc block), then
seven more across the legacy helpers (brace-less `if/else`, and parameters of different functions
sharing a name). Both classes are handled below; anything still ambiguous is skipped on purpose.
"""
import io
import re
import sys

DECL = re.compile(r"^(\t+)(?:const\s+)?(?:float|float2|float3|float4|half|half2|half3|half4|uint|int|"
                  r"bool|F[A-Za-z0-9_]+|BxDFContext)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|;)")
# a function definition's opening line: something(...) with no trailing semicolon, at column 0
FUNC_OPEN = re.compile(r"^[A-Za-z_][\w:<>, \t\*&]*\([^;]*$|^[A-Za-z_][\w:<>, \t\*&]*\([^;]*\)\s*$")
KEYWORDS = {"if", "else", "for", "while", "return", "switch", "case", "break",
            "true", "false", "BRANCH", "UNROLL", "struct"}


def strip_comments(text):
    """Blank out /* */ blocks and // tails, keeping line numbering intact.

    Block comments matter more than they sound: ToonBufferCommon.ush documents the entire payload
    layout in one, naming every field. Without this every one of those names reads as code used
    above its declaration.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def prev_code_line(code, i):
    """Index of the nearest non-blank line above i, or -1."""
    j = i - 1
    while j >= 0 and not code[j].strip():
        j -= 1
    return j


def check_dangling_else(code):
    """Every `else` must attach to an `if`.

    Two legal shapes: the previous statement is a closing brace, or the `if` was brace-less and the
    previous statement is its single statement. The second is why "must follow }" was too strict --
    the toon helpers are full of `if (x) return a; else if (y) return b;`. So when the previous line
    ends in `;`, look one further back for an `if (` at the same indent as the `else`.
    """
    problems = []
    for i, l in enumerate(code):
        m = re.match(r"(\s*)else\b", l)
        if not m:
            continue
        indent = m.group(1)
        j = prev_code_line(code, i)
        if j < 0:
            continue
        prev = code[j].rstrip()
        if prev.endswith("}"):
            continue
        if prev.endswith(";"):
            k = prev_code_line(code, j)
            if k >= 0 and re.match(re.escape(indent) + r"(?:\}\s*)?else\b|" + re.escape(indent) + r"if\s*\(",
                                   code[k]):
                continue      # brace-less if (or else if) owning that single statement
        problems.append("line %d: `else` does not attach to an `if` -- previous statement is %r"
                        % (i + 1, prev.strip()[:60]))
    return problems


def check_brace_balance(code):
    depth = sum(l.count("{") - l.count("}") for l in code)
    return [] if depth == 0 else ["braces unbalanced by %+d over the file" % depth]


def function_bodies(code):
    """(start, end) line ranges of top-level function bodies.

    Scoping matters: without it, two functions whose PARAMETERS share a name look like a use before
    a declaration. Only column-0 definitions are considered, which is every function in these files.
    """
    spans, i, n = [], 0, len(code)
    while i < n:
        line = code[i]
        if line.startswith("{") or (line.rstrip().endswith("{") and not line.startswith((" ", "\t"))):
            depth, j, opened = 0, i, False
            while j < n:
                depth += code[j].count("{") - code[j].count("}")
                opened = opened or depth > 0
                if opened and depth == 0:      # the closing line is a bare `}` -- it has no `{`
                    break
                j += 1
            spans.append((i, j))
            i = j + 1
            continue
        i += 1
    return spans


def check_decl_order(lines, code):
    """A local must not be used above the line that declares it, within one function body."""
    problems = []
    for start, end in function_bodies(code):
        body = range(start, min(end + 1, len(code)))
        decls = {}
        for i in body:
            m = DECL.match(code[i])
            if not m:
                continue
            name = m.group(2)
            if name in KEYWORDS:
                continue
            decls[name] = None if name in decls else i     # redeclared = ambiguous, skip
        for name, decl_line in decls.items():
            if decl_line is None:
                continue
            use = re.compile(r"(?<![\w.])" + re.escape(name) + r"(?![\w])")
            for i in body:
                if i >= decl_line:
                    break
                # A statement inside a function body is always indented, so a column-0 line is a
                # signature. Its parameter names are not uses of anything -- counting them was the
                # last false-positive class, and it hit every file with two functions whose
                # parameters share a name.
                if code[i][:1] not in ("\t", " "):
                    continue
                if use.search(code[i]):
                    problems.append("line %d uses %r, declared on line %d:  %s"
                                    % (i + 1, name, decl_line + 1, lines[i].strip()[:60]))
                    break
    return problems


def check(path):
    raw = io.open(path, encoding="utf-8").read()
    lines = raw.split("\n")
    code = strip_comments(raw).split("\n")
    assert len(code) == len(lines), "comment stripping changed the line count"
    return check_dangling_else(code) + check_brace_balance(code) + check_decl_order(lines, code)


def main(paths):
    ok = True
    for path in paths:
        problems = check(path)
        name = path.replace("\\", "/").rsplit("/", 1)[-1]
        print("%-42s %s" % (name, "OK" if not problems else "%d PROBLEM(S)" % len(problems)))
        for p in problems:
            ok = False
            print("    " + p)
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    sys.exit(main(sys.argv[1:]))
