#!/usr/bin/env python3
"""Compares what two scans of the same source recover: the check that a change
to the engine made no file stop being recovered (docs/testing/testing.md,
"Comparing two builds on real data").

Usage:
  python -I compare_reports.py BEFORE.json AFTER.json [--ignore-recovery] [--all]
  python -I compare_reports.py --self-test

BEFORE.json and AFTER.json are session reports (`recovery report --session ID
--format json --output FILE`) of two scans of the same source, usually by the
build before a change and the build after it.

It prints:
1. the scan metrics of both;
2. file by file: every file the BEFORE scan found must be found by the AFTER
   scan in the same or a better condition. A file is its content (SHA-256)
   when the report has one, else where it starts and its name. Files LOST
   (found before, not now) and WORSE (found in a worse condition) are listed
   first, then BETTER and NEW. Conditions from best to worst: COMPLETE,
   UNVERIFIED, PARTIAL, CORRUPTED, AMBIGUOUS, UNRECOVERABLE. A file whose bytes
   changed shows as LOST and NEW: a change to review too;
3. field by field: the other differences in the candidates, volumes and
   duplicate groups, by position (fields that only describe the run, such as
   times, speeds and bytes read, are left out, and so are fields only the AFTER
   build writes). These do not change the exit code.

Exit code: 0 when no file was lost or got worse; 1 when one was (stop and
review before accepting the change); 2 when the reports cannot be read or are
not of the same source.

--ignore-recovery leaves out what recovery jobs wrote (for two reports of one
session taken at different times). --all lists every file and difference, not
only the first ones.

Only reads the two reports; writes nothing.
"""

import json
import sys

REPORT_FORMAT = "recovery-session-report"
# Fields that describe the run, not what it found.
RUN_ONLY = {"elapsedMs", "scanSpeed", "bytesRead", "created", "updated", "time", "folder"}
# Higher is better: what a user gets back.
RANK = {"COMPLETE": 6, "UNVERIFIED": 5, "PARTIAL": 4, "CORRUPTED": 3, "AMBIGUOUS": 2, "UNRECOVERABLE": 1}
SHOWN = 20
SHOWN_FIELDS = 40


class ReportError(Exception):
    pass


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            report = json.load(f)
    except (OSError, ValueError) as error:
        raise ReportError(f"{path}: cannot be read as JSON: {error}") from error
    if not isinstance(report, dict) or report.get("format") != REPORT_FORMAT:
        raise ReportError(f"{path}: not a session report (format {REPORT_FORMAT!r})")
    for part in ("source", "scan", "candidates"):
        if part not in report:
            raise ReportError(f"{path}: the report has no {part!r}")
    return report


def strip(value, dropped):
    if isinstance(value, dict):
        return {k: strip(v, dropped) for k, v in value.items() if k not in dropped}
    if isinstance(value, list):
        return [strip(v, dropped) for v in value]
    return value


def differences(a, b, path, out, limit):
    if limit is not None and len(out) >= limit:
        return
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a:
                out.append(f"{path}.{key}: only AFTER = {b[key]!r}")
            elif key not in b:
                out.append(f"{path}.{key}: only BEFORE = {a[key]!r}")
            else:
                differences(a[key], b[key], f"{path}.{key}", out, limit)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"{path}: {len(a)} items BEFORE, {len(b)} AFTER")
        for i, (x, y) in enumerate(zip(a, b)):
            differences(x, y, f"{path}[{i}]", out, limit)
    elif a != b:
        out.append(f"{path}: BEFORE {a!r} AFTER {b!r}")


def file_key(candidate):
    if candidate.get("sha256"):
        return ("sha256", candidate["sha256"])
    return ("at", candidate.get("sourceOffset"), candidate.get("path") or candidate.get("name"))


def files(report):
    """Each file found -> (rank, condition, description), its best candidate's."""
    best = {}
    for c in report["candidates"]:
        key = file_key(c)
        rank = RANK.get(c.get("condition"), 0)
        if key not in best or rank > best[key][0]:
            where = c.get("path") or c.get("name")
            best[key] = (rank, c.get("condition"), f"#{c.get('id')} {where} ({c.get('size')} bytes)")
    return best


def compare(before, after, ignore_recovery=False, show_all=False):
    """The comparison as lines of text, and the exit code."""
    lines = []
    sb, sa = before["source"], after["source"]
    fb, fa = sb.get("fingerprint", {}), sa.get("fingerprint", {})
    if sb.get("size") != sa.get("size") or fb.get("sha256") != fa.get("sha256"):
        lines.append(f"not the same source: BEFORE {sb.get('path')} ({sb.get('size')} bytes, fingerprint "
                     f"{fb.get('sha256')}), AFTER {sa.get('path')} ({sa.get('size')} bytes, fingerprint "
                     f"{fa.get('sha256')})")
        return lines, 2

    run_only = set(RUN_ONLY) | ({"recovery"} if ignore_recovery else set())
    mb, ma = before["scan"].get("metrics", {}), after["scan"].get("metrics", {})
    added = {k for k in ma if k not in mb}
    lines.append("metrics BEFORE: " + json.dumps({k: v for k, v in mb.items() if k not in run_only}))
    lines.append("metrics AFTER:  " + json.dumps({k: v for k, v in ma.items() if k not in run_only | added}))
    if added:
        lines.append("only the AFTER build writes: " + json.dumps({k: ma[k] for k in sorted(added)}))

    old, new = files(before), files(after)
    lost = [old[k] for k in old if k not in new]
    worse = [(old[k], new[k]) for k in old if k in new and new[k][0] < old[k][0]]
    better = [(old[k], new[k]) for k in old if k in new and new[k][0] > old[k][0]]
    found = [new[k] for k in new if k not in old]
    complete_before = sum(1 for v in old.values() if v[1] == "COMPLETE")
    complete_after = sum(1 for v in new.values() if v[1] == "COMPLETE")
    lines.append(f"files: {len(old)} BEFORE ({complete_before} COMPLETE), {len(new)} AFTER ({complete_after} COMPLETE)")
    lines.append(f"  LOST {len(lost)}, WORSE {len(worse)}, BETTER {len(better)}, NEW {len(found)}")
    shown = None if show_all else SHOWN
    for v in lost[:shown]:
        lines.append(f"  LOST   {v[1]} {v[2]}")
    for b, a in worse[:shown]:
        lines.append(f"  WORSE  {b[1]} -> {a[1]} {b[2]}")
    for b, a in better[:shown]:
        lines.append(f"  BETTER {b[1]} -> {a[1]} {b[2]}")
    for v in found[:shown]:
        lines.append(f"  NEW    {v[1]} {v[2]}")

    for part in ("candidates", "volumes", "duplicateGroups", "partitionScheme"):
        a, b = strip(before.get(part), run_only | added), strip(after.get(part), run_only | added)
        out = []
        differences(a, b, part, out, None if show_all else SHOWN_FIELDS)
        count = f"{len(a)} BEFORE, " if isinstance(a, list) else ""
        lines.append(f"{part}: {count}{'IDENTICAL' if not out else str(len(out)) + '+ differences'}")
        lines.extend("    " + line[:300] for line in out)

    if lost or worse:
        lines.append("RESULT: files were lost or got worse: review before accepting the change")
        return lines, 1
    lines.append("RESULT: no file lost, none worse")
    return lines, 0


def self_test():
    def report(candidates, sha="f" * 64):
        return {"format": REPORT_FORMAT, "source": {"path": "card.img", "size": 1000, "fingerprint": {"sha256": sha}},
                "scan": {"metrics": {"candidates": len(candidates)}}, "candidates": candidates}

    def candidate(id_, condition, sha=None, offset=0, name="x"):
        return {"id": id_, "name": name, "path": "", "size": 10, "sourceOffset": offset, "condition": condition,
                "sha256": sha or ""}

    a, b, c, d, e = "a" * 64, "b" * 64, "c" * 64, "d" * 64, "e" * 64
    before = report([candidate(1, "COMPLETE", a), candidate(2, "COMPLETE", b), candidate(3, "PARTIAL", None, 100, "c"),
                     candidate(4, "PARTIAL", d), candidate(5, "CORRUPTED", c)])
    checks = []

    lines, code = compare(before, json.loads(json.dumps(before)))
    checks.append(("the same report: nothing lost", code == 0 and "  LOST 0, WORSE 0, BETTER 0, NEW 0" in lines))

    after = report([candidate(1, "COMPLETE", a), candidate(3, "CORRUPTED", None, 100, "c"), candidate(4, "COMPLETE", d),
                    candidate(5, "CORRUPTED", c), candidate(6, "COMPLETE", e)])
    lines, code = compare(before, after)
    checks.append(("lost, worse, better and new told apart",
                   code == 1 and "  LOST 1, WORSE 1, BETTER 1, NEW 1" in lines
                   and any(line.startswith("  LOST   COMPLETE #2") for line in lines)
                   and any(line.startswith("  WORSE  PARTIAL -> CORRUPTED #3") for line in lines)))

    duplicate = report(before["candidates"] + [candidate(7, "CORRUPTED", a)])
    lines, code = compare(before, duplicate)
    checks.append(("a file's best candidate speaks for it", code == 0))

    lines, code = compare(before, report(before["candidates"], sha="0" * 64))
    checks.append(("another source is refused", code == 2))

    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(("ok      " if ok else "FAILED  ") + name)
    return 1 if failed else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    options = {"--ignore-recovery", "--all"}
    paths = [a for a in argv if a not in options]
    if len(paths) != 2 or any(a.startswith("--") for a in paths):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        before, after = load(paths[0]), load(paths[1])
    except ReportError as error:
        print(f"compare_reports: {error}", file=sys.stderr)
        return 2
    lines, code = compare(before, after, "--ignore-recovery" in argv, "--all" in argv)
    for line in lines:
        print(line)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
