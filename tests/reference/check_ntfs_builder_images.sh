#!/bin/sh
# Checks the NTFS test builder's images with ntfs-3g's ntfsprogs, an
# independent NTFS implementation, reading only. The images and their
# manifests come from NtfsBuilderExport.WritesImages (see
# docs/testing/testing.md).
#
# For every image: ntfsinfo must mount it; every "file" line's record must
# read back (ntfscat) with the listed size, CRC-32 and name (ntfsinfo); every
# "deleted" line's record must be recoverable by ntfsundelete with the listed
# size and CRC-32.
#
# Usage: check_ntfs_builder_images.sh <directory of .img and .manifest files>
# Needs ntfsinfo, ntfscat and ntfsundelete on PATH, and python3 (for CRC-32).
set -u

# Prints "<size> <crc32>" of the first $1 bytes of stdin.
crc() {
    python3 -c 'import sys, zlib; d = sys.stdin.buffer.read()[:int(sys.argv[1])]; print(len(d), format(zlib.crc32(d), "08x"))' "$1"
}

failed=0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tab=$(printf '\t')
for image in "$1"/*.img; do
    name=$(basename "$image")
    if ! ntfsinfo -m "$image" > /dev/null 2>&1; then
        echo "NOT MOUNTABLE: $name"
        ntfsinfo -m "$image" 2>&1 | head -3 | sed 's/^/    /'
        failed=1
        continue
    fi
    problems=0
    while IFS=$tab read -r kind record size expected filename; do
        if [ "$kind" = file ]; then
            got=$(ntfscat -i "$record" "$image" 2> /dev/null | crc "$size")
            if ! ntfsinfo -i "$record" "$image" 2> /dev/null | grep -qF "'$filename'"; then
                echo "    record $record: name is not '$filename'"
                problems=1
            fi
        else
            # ntfsundelete writes whole clusters; only the file's size counts.
            rm -rf "${work:?}"/*
            ntfsundelete -u -i "$record" -d "$work" "$image" > /dev/null 2>&1
            got=$(cat "$work"/* 2> /dev/null | crc "$size")
        fi
        if [ "$got" != "$size $expected" ]; then
            echo "    $kind record $record ('$filename'): got '$got', expected '$size $expected'"
            problems=1
        fi
    done < "${image%.img}.manifest"
    if [ $problems -eq 0 ]; then
        echo "consistent:   $name"
    else
        echo "INCONSISTENT: $name"
        failed=1
    fi
done
exit $failed
