#!/bin/sh
# Checks the document test builders' files with independent readers, so that a
# builder and a format module cannot share a misreading of a specification.
# The files come from DocumentBuilderExport.WritesFiles (see docs/testing/testing.md):
#
#   $env:RECOVERY_DOCUMENT_EXPORT_DIR = "<dir>"
#   ctest --preset msvc-debug -R DocumentBuilderExport
#
# Usage: check_document_builder_files.sh <directory of builder_*.pdf/.doc/.xls/.ppt/.msg>
# Needs qpdf on PATH (qpdf --check: the objects, cross-reference sections and
# streams) and python3 with olefile (every stream of a compound file read to
# its recorded size, any defect olefile notes counted as a failure).
set -u

failed=0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

report() {
    if [ "$1" -eq 0 ]; then
        echo "  ok       $2"
    else
        echo "  FAILED   $2"
        sed 's/^/             /' "$work/out" | head -5
        failed=1
    fi
}

for file in "$1"/*.pdf; do
    [ -e "$file" ] || continue
    # 0: no problem; 3: warnings (a repaired file); 2: errors.
    qpdf --check "$file" > "$work/out" 2>&1
    report $? "qpdf --check $(basename "$file")"
done

for file in "$1"/*.doc "$1"/*.xls "$1"/*.ppt "$1"/*.msg; do
    [ -e "$file" ] || continue
    python3 - "$file" > "$work/out" 2>&1 <<'EOF'
import sys
import olefile

ole = olefile.OleFileIO(sys.argv[1], raise_defects=olefile.DEFECT_INCORRECT)
for path in ole.listdir(streams=True, storages=False):
    size = ole.get_size(path)
    data = ole.openstream(path).read()
    if len(data) != size:
        sys.exit("stream %s: read %d of %d bytes" % ("/".join(path), len(data), size))
if ole.parsing_issues:
    sys.exit("olefile noted: %s" % ole.parsing_issues)
EOF
    report $? "olefile $(basename "$file")"
done

if [ "$failed" -ne 0 ]; then
    echo "some files were refused by an independent reader"
    exit 1
fi
echo "every builder file was accepted"
