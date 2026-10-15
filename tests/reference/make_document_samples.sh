#!/bin/sh
# Writes qpdf's variants of mspdf_shapes.pdf (make_document_samples.ps1) into
# the same folder, for tests/support/document_samples.cpp:
#   qpdf_linearized.pdf          linearized ("fast web view")
#   qpdf_objstm.pdf              object streams and a cross-reference stream (PDF 1.5)
#   qpdf_linearized_objstm.pdf   both, as Acrobat saves for the web
#   qpdf_aes256.pdf              encrypted with AES-256 (an empty user password)
#
# Usage: make_document_samples.sh <folder of make_document_samples.ps1's files>
# Needs qpdf (11.9.0 made the embedded samples).
set -eu
cd "$1"
in=mspdf_shapes.pdf
qpdf --deterministic-id --linearize "$in" qpdf_linearized.pdf
qpdf --deterministic-id --object-streams=generate "$in" qpdf_objstm.pdf
qpdf --deterministic-id --linearize --object-streams=generate "$in" qpdf_linearized_objstm.pdf
qpdf --encrypt --user-password= --owner-password=owner --bits=256 -- "$in" qpdf_aes256.pdf
for file in qpdf_*.pdf; do
    qpdf --check "$file" > /dev/null
done
ls -l qpdf_*.pdf
