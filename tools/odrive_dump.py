"""Parse ODrive parameter dumps (stdlib only, no hardware access).

Accepted formats (mixed in one file is fine):

* odrivetool / IPython session, as in ``docs/odrive_dump.txt``::

      In [8]: odrv0.axis0
      Out[8]:
      config:
        enable_watchdog: False (bool)
        calibration_lockin: ...            <- not expanded: ignored
      clear_errors(obj: object_ref)        <- method: ignored
      error: 256 (int32)

  The expression after ``In [n]:`` gives the path prefix of the block.
* flat: ``axis0.motor.config.current_lim = 12.0`` (optionally ``odrv0.``).
* older odrivetool trees with ``name = value (type)``.

parse_dump() returns {path relative to odrv0: value}; parse_dump_report()
also returns the problems found (unparsable lines), so a corrupted dump is
noticed instead of silently producing a partial picture.
"""
import re

_TYPE_SUFFIX = re.compile(r"\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\)\s*$")
_IN_LINE = re.compile(r"^In \[\d+\]:\s*(.*)$")
_OUT_LINE = re.compile(r"^Out\[\d+\]:\s*(.*)$")
_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)*$")


def parse_value(text):
    """'12.0 (float)' -> 12.0, 'True (bool)' -> True, '0x10' -> 16, 'inf' -> inf."""
    t = _TYPE_SUFFIX.sub("", text.strip())
    if t in ("True", "true"):
        return True
    if t in ("False", "false"):
        return False
    if t == "None":
        return None
    try:
        if t.lower().startswith(("0x", "-0x")):
            return int(t, 16)
        if re.fullmatch(r"[+-]?\d+", t):
            return int(t)
        return float(t)                     # also 'inf', '-inf', 'nan'
    except ValueError:
        return t


def _prefix_from_expression(expr):
    expr = expr.strip()
    if expr == "odrv0":
        return ""
    if expr.startswith("odrv0."):
        rest = expr[len("odrv0."):]
        return rest if _NAME.match(rest) else None
    return None                             # not an odrv0 expression: ignore block


def parse_dump_report(text, prefix=""):
    """Return (values, problems). problems = list of (line_number, line, reason)."""
    values = {}
    problems = []
    stack = []                              # (indent, name)
    block_prefix = prefix
    skipping = False                        # inside an Out block we cannot place
    for no, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip("\r\n").rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = _IN_LINE.match(line.strip())
        if m:
            p = _prefix_from_expression(m.group(1)) if m.group(1).strip() else None
            stack = []
            if m.group(1).strip() == "":
                skipping = True             # trailing "In [12]:" prompt
            elif p is None:
                skipping = True
                problems.append((no, line, "unsupported expression, block ignored"))
            else:
                block_prefix = p
                skipping = False
            continue
        if _OUT_LINE.match(line.strip()):
            continue
        if skipping:
            continue
        indent = len(line) - len(line.lstrip(" "))
        body = line.strip()
        while stack and stack[-1][0] >= indent:
            stack.pop()
        # method signatures: "name(obj: object_ref, ...)" [-> result]
        head = re.split(r"[:=]", body, maxsplit=1)[0]
        if "(" in head:
            continue
        if "=" in body and (":" not in body or body.index("=") < body.index(":")):
            name, _, value = body.partition("=")
        elif body.endswith(":"):
            stack.append((indent, body[:-1].strip()))
            continue
        elif ":" in body:
            name, _, value = body.partition(":")
        else:
            problems.append((no, line, "no ':' or '='"))
            continue
        name = name.strip()
        value = value.strip()
        if not _NAME.match(name):
            problems.append((no, line, "invalid name"))
            continue
        if value == "...":
            continue                        # sub-object not expanded here
        rel = [s[1] for s in stack] + [name]
        absolute = rel[0].startswith("odrv0")
        parts = ([block_prefix] if (block_prefix and not absolute) else []) + rel
        path = ".".join(p for p in parts if p)
        if path.startswith("odrv0."):
            path = path[len("odrv0."):]
        parsed = parse_value(value)
        if isinstance(parsed, str) and ("(" in value or ")" in value or value == ""):
            problems.append((no, line, "malformed value"))
            continue
        values[path] = parsed
    return values, problems


def parse_dump(text, prefix=""):
    return parse_dump_report(text, prefix)[0]
