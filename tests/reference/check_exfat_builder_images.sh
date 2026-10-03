#!/bin/sh
# Checks the exFAT test builder's images with fsck.exfat (exfatprogs), an
# independent implementation, in read-only mode. The images come from
# ExFatBuilderExport.WritesImages (see docs/testing/testing.md).
#
# Usage: check_exfat_builder_images.sh <directory of .img files>
# Set FSCK_EXFAT to use an fsck.exfat that is not on PATH.
set -u

fsck=${FSCK_EXFAT:-fsck.exfat}
failed=0
for image in "$1"/*.img; do
    # -n: answer "no" to every repair question; the image is never modified.
    if "$fsck" -n "$image" > /dev/null 2>&1; then
        echo "clean:     $(basename "$image")"
    else
        echo "NOT CLEAN: $(basename "$image")"
        "$fsck" -n -v "$image" 2>&1 | grep -iE "error|not correct|corrupt" | sed 's/^/    /'
        failed=1
    fi
done
exit $failed
