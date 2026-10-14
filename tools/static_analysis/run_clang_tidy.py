"""Runs clang-tidy over the engine (src/) and the CLI (tools/recovery_cli/) (P20).

Usage:
    python tools/static_analysis/run_clang_tidy.py BUILD_DIR OUT_DIR JOBS [--integer] [--filter TEXT]

BUILD_DIR is a configured preset's folder (it holds compile_commands.json, e.g. build/msvc-debug). Writes the
log of each file to OUT_DIR/<n>.log and every finding, once, to OUT_DIR/findings.txt as sorted
"path:line:col: warning: message [check]" lines. --integer runs the integer-overflow pass instead of the
default checks; --filter keeps the files whose path holds TEXT. The clang-tidy of Visual Studio is used unless
the CLANG_TIDY environment variable names another one. docs/testing/validation.md has the triage of P20's runs.
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys

DEFAULT_CLANG_TIDY = r"D:\VS\Product\VC\Tools\Llvm\x64\bin\clang-tidy.exe"

# The P20 checks: the bug-finding families, without the checks that duplicate MSVC's /W4 warnings (narrowing)
# or that only restate style.
CHECKS = [
    "-*",
    "bugprone-*",
    "-bugprone-easily-swappable-parameters",
    "-bugprone-narrowing-conversions",  # MSVC /W4 /w14242 already covers conversions
    "-bugprone-implicit-widening-of-multiplication-result",  # the --integer pass
    "clang-analyzer-*",
    "concurrency-*",
    "cert-err33-c", "cert-err34-c", "cert-err52-cpp", "cert-err58-cpp", "cert-flp30-c", "cert-mem57-cpp",
    "cert-msc50-cpp", "cert-msc51-cpp", "cert-oop57-cpp", "cert-oop58-cpp", "cert-str34-c", "cert-dcl58-cpp",
    "performance-*",
    "-performance-enum-size",
    "misc-redundant-expression", "misc-unconventional-assign-operator", "misc-throw-by-value-catch-by-reference",
    "misc-new-delete-overloads", "misc-uniqueptr-reset-release", "misc-misleading-identifier",
    "misc-misleading-bidirectional", "misc-definitions-in-headers", "misc-static-assert",
]

# The integer-overflow audit: arithmetic done in a narrow type and widened afterwards, loops whose counter is
# narrower than their bound, sign confusion, sizeof mistakes, and undefined results of operators.
INTEGER_CHECKS = [
    "-*",
    "bugprone-implicit-widening-of-multiplication-result",
    "bugprone-misplaced-widening-cast",
    "bugprone-too-small-loop-variable",
    "bugprone-signed-char-misuse",
    "bugprone-sizeof-expression",
    "cert-int09-c",
    "clang-analyzer-core.UndefinedBinaryOperatorResult",
    "clang-analyzer-security.insecureAPI.*",
]

LINE = re.compile(r"^(.*?):(\d+):(\d+): (warning|error): (.*) \[([^\]]+)\]$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build")
    parser.add_argument("out")
    parser.add_argument("jobs", type=int)
    parser.add_argument("--integer", action="store_true")
    parser.add_argument("--filter")
    args = parser.parse_args()

    clang_tidy = os.environ.get("CLANG_TIDY", DEFAULT_CLANG_TIDY)
    checks = ",".join(INTEGER_CHECKS if args.integer else CHECKS)
    build = os.path.abspath(args.build)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    root = os.path.normcase(os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

    with open(os.path.join(build, "compile_commands.json"), encoding="utf-8") as db:
        entries = json.load(db)
    files = set()
    for entry in entries:
        path = os.path.normcase(os.path.abspath(entry["file"]))
        if not path.startswith(root + os.sep):
            continue  # another drive or folder: not the repository's
        rel = path[len(root) + 1:].replace("\\", "/")
        if not (rel.startswith("src/") or rel.startswith("tools/recovery_cli/")):
            continue
        if args.filter and args.filter not in rel:
            continue
        files.add(entry["file"])
    files = sorted(files)
    print(f"{len(files)} files", flush=True)

    def run(index_file):
        index, file = index_file
        command = [clang_tidy, "-p", build, f"--checks={checks}", "--quiet",
                   "--header-filter=.*[/\\\\](include|src|tools)[/\\\\].*",
                   "--extra-arg=-Wno-unknown-argument", "--extra-arg=-Wno-unused-command-line-argument",
                   "--extra-arg=-Wno-everything", file]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
        with open(os.path.join(out, f"{index}.log"), "w", encoding="utf-8") as log:
            log.write(result.stdout)
            log.write(result.stderr)
        found = []
        for line in result.stdout.splitlines():
            match = LINE.match(line.strip())
            if match:
                path = os.path.normcase(os.path.abspath(match.group(1)))
                if path.startswith(root + os.sep):
                    path = path[len(root) + 1:].replace("\\", "/")
                found.append(f"{path}:{match.group(2)}:{match.group(3)}: {match.group(4)}: "
                             f"{match.group(5)} [{match.group(6)}]")
        return found

    findings = set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for done, found in enumerate(pool.map(run, enumerate(files)), start=1):
            findings.update(found)
            if done % 10 == 0 or done == len(files):
                print(f"{done}/{len(files)} done, {len(findings)} findings", flush=True)
    with open(os.path.join(out, "findings.txt"), "w", encoding="utf-8", newline="\n") as report:
        for line in sorted(findings):
            report.write(line + "\n")
    print(f"{len(findings)} findings -> {os.path.join(out, 'findings.txt')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
