"""Builds a FAT32 image with pyfatfs (an independent FAT implementation) and a
manifest of its active files, for cross-checking the RecoveryEngine parser."""

import os
import random
import sys
import zlib

import fs.path
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

out_dir = sys.argv[1]
image = os.path.join(out_dir, "pyfatfs.img")
manifest = os.path.join(out_dir, "pyfatfs.manifest")
if os.path.exists(image):
    os.remove(image)

size = 48 * 1024 * 1024
with open(image, "wb") as f:
    f.truncate(size)
PyFat().mkfs(image, fat_type=PyFat.FAT_TYPE_FAT32, size=size, label="PYFATFS")

rng = random.Random(1234)
files = {
    "/small.txt": 17,
    "/IMG_0001.JPG": 70000,
    "/DCIM/100MEDIA/Holiday photo from the beach.jpeg": 123456,
    # pyfatfs 1.0.5 writes invalid 8.3 aliases (raw UTF-8) with mismatched
    # checksums for non-ASCII names, so only ASCII long names are used here.
    "/DCIM/100MEDIA/Another rather long name with spaces.png": 4097,
    "/Music/track 01 - intro.mp3": 1,
    "/Music/empty.bin": 0,
    "/Deep/a/b/c/d/nested file.dat": 9000,
}
deleted = "/to be deleted.bin"

expected = []
with PyFatFS(image, encoding="utf-8") as vol:
    for path, length in list(files.items()) + [(deleted, 5000)]:
        vol.makedirs(fs.path.dirname(path), recreate=True)
        data = bytes(rng.getrandbits(8) for _ in range(length))
        with vol.openbin(path, "w") as handle:
            handle.write(data)
        if path != deleted:
            expected.append((path, length, zlib.crc32(data) & 0xFFFFFFFF))
    vol.remove(deleted)

with open(manifest, "w", encoding="utf-8") as m:
    for path, length, crc in expected:
        m.write(f"{path}\t{length}\t{crc:08X}\n")
    # No "deleted" line: pyfatfs compacts the directory on remove() instead of
    # marking the entry 0xE5 as Windows does, so nothing deleted remains.
print("wrote", image, "and", manifest)
