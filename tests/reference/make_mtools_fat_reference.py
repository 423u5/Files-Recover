#!/usr/bin/env python3
"""Builds FAT12 and FAT16 reference images with mkfs.fat (dosfstools) and
mtools, an independent FAT implementation, and a manifest for each, for
cross-checking the RecoveryEngine parser (Fat32ReferenceImageTest and
RecoveryReferenceImageTest; the manifest format is documented in
tests/filesystem/fat32_reference_image_test.cpp).

Usage: make_mtools_fat_reference.py <output dir>

mkfs.fat formats each image and mtools writes, fragments and deletes files in
it without mounting anything, so the images hold active, fragmented and
deleted files and folders with long and non-ASCII names, laid out by another
implementation. fsck.fat -n checks each volume afterwards. Only image files
are touched; no device is ever opened.

Images: a 1.44 MB FAT12 floppy disk (512-byte clusters, no partition table),
an MBR disk with a FAT12 partition of 4 KiB clusters, and a FAT16 volume of
2 KiB clusters.

Needs mkfs.fat, fsck.fat, sfdisk and the mtools commands (mcopy, mmd, mdel,
mdeltree, mshowfat) on PATH; packages unpacked without root work (see
docs/testing/testing.md).
"""

import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

KIB = 1024
MIB = 1024 * KIB

ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1", LC_ALL="C.UTF-8")


def payload(size, seed):
    return random.Random(seed).randbytes(size)


def run(*args, stdin=None):
    result = subprocess.run(args, env=ENV, input=stdin, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"{' '.join(args)} failed ({result.returncode}): {result.stdout}{result.stderr}")
    return result.stdout


def fresh(path):
    """mkfs.fat -C will not overwrite a file."""
    if os.path.exists(path):
        os.remove(path)
    return path


class Volume:
    """A FAT volume that mtools writes (an image, or a partition of one), and
    what its manifest must say."""

    def __init__(self, image, offset, work):
        self.image = image
        self.offset = offset
        self.target = image if offset == 0 else f"{image}@@{offset}"
        self.work = work
        self.entries = []  # ["file", path, size, crc] or ["deleted", name]
        self.deleted_in = set()  # folders that hold a deleted entry

    def _adding_to(self, path):
        # mtools puts a new entry into the slots of a deleted one when they
        # are enough, and the manifest would then name an entry that is gone.
        folder = os.path.dirname(path)
        if folder in self.deleted_in:
            sys.exit(f"{path}: {folder} holds a deleted entry, which this entry could overwrite")

    def mkdir(self, path):
        self._adding_to(path)
        run("mmd", "-i", self.target, "::" + path)

    def write(self, path, data):
        self._adding_to(path)
        local = os.path.join(self.work, "data")
        with open(local, "wb") as f:
            f.write(data)
        run("mcopy", "-i", self.target, local, "::" + path)
        self.entries.append(["file", path, len(data), zlib.crc32(data)])

    def _deleted(self, name):
        # A deleted 8.3 name loses its first letter; a long name is kept whole,
        # so the manifest can name it.
        if re.fullmatch(r"[A-Z0-9_]{1,8}(\.[A-Z0-9_]{1,3})?", name):
            sys.exit(f"{name}: delete only files with long names")
        self.entries.append(["deleted", name])

    def delete(self, path):
        run("mdel", "-i", self.target, "::" + path)
        self.entries = [e for e in self.entries if not (e[0] == "file" and e[1] == path)]
        self._deleted(os.path.basename(path))
        self.deleted_in.add(os.path.dirname(path))

    def delete_tree(self, path):
        inside = [e for e in self.entries if e[0] == "file" and e[1].startswith(path + "/")]
        run("mdeltree", "-i", self.target, "::" + path)
        self.entries = [e for e in self.entries if e not in inside]
        for entry in inside:
            self._deleted(os.path.basename(entry[1]))
        self._deleted(os.path.basename(path))
        self.deleted_in.update({path, os.path.dirname(path)})

    def fragments(self, path):
        """The file's cluster runs as mshowfat prints them: "<2-21> <42-81>"."""
        shown = run("mshowfat", "-i", self.target, "::" + path)
        return re.findall(r"<\d+(?:-\d+)?>", shown)

    def check(self):
        """fsck.fat -n on the volume's bytes: it reads only a whole device or file."""
        volume = self.image
        if self.offset != 0:
            volume = os.path.join(self.work, "volume.img")
            with open(self.image, "rb") as src, open(volume, "wb") as dst:
                src.seek(self.offset)
                shutil.copyfileobj(src, dst)
        print(run("fsck.fat", "-n", volume).strip().splitlines()[-1])

    def write_manifest(self, path):
        with open(path, "w", encoding="utf-8", newline="\n") as m:
            for entry in self.entries:
                if entry[0] == "file":
                    m.write(f"{entry[1]}\t{entry[2]}\t{entry[3]:08X}\n")
                else:
                    m.write(f"deleted\t{entry[1]}\n")


def require_fragmented(volume, path):
    runs = volume.fragments(path)
    if len(runs) < 2:
        sys.exit(f"{path} was not fragmented: {runs}")
    print(f"  {path}: {' '.join(runs)}")


def floppy(out, work):
    """A 1.44 MB floppy disk: FAT12, 512-byte clusters, no partition table."""
    image = fresh(os.path.join(out, "mtools-fat12-floppy.img"))
    run("mkfs.fat", "-F", "12", "-n", "FLOPPY12", "-i", "12121212", "-C", image, "1440")
    v = Volume(image, 0, work)
    v.write("/README.TXT", payload(1000, 1))
    v.write("/Long file name with spaces.txt", payload(5000, 2))
    v.mkdir("/DOCS")
    v.mkdir("/DOCS/1999")
    v.write("/DOCS/1999/LETTER.WRI", payload(30000, 3))
    v.write("/DOCS/Grüße aus Köln.txt", payload(700, 4))
    v.write("/EMPTY.DAT", b"")
    v.mkdir("/Old letters")
    v.write("/Old letters/Inside note.txt", payload(3000, 10))
    v.write("/Deleted photo.jpg", payload(20 * KIB, 11))
    # A gap, filled by a file (in another folder) that continues after the next one.
    v.mkdir("/MEDIA")
    v.write("/A.BIN", payload(10 * KIB, 5))
    v.write("/Gap filler.bin", payload(10 * KIB, 6))
    v.write("/C.BIN", payload(10 * KIB, 7))
    v.delete("/Gap filler.bin")
    v.write("/MEDIA/Fragmented video.avi", payload(60 * KIB, 8))
    require_fragmented(v, "/MEDIA/Fragmented video.avi")
    # 1200 clusters: entries 341, 682 and 1365 lie across FAT sectors.
    v.write("/MEDIA/Big archive.zip", payload(600 * KIB, 9))
    # Deleted last, so that nothing reuses their clusters.
    v.delete("/Deleted photo.jpg")
    v.delete_tree("/Old letters")
    v.check()
    v.write_manifest(os.path.join(out, "mtools-fat12-floppy.manifest"))


def partitioned(out, work):
    """An MBR disk with one FAT12 partition (type 0x01) of 4 KiB clusters."""
    image = os.path.join(out, "mtools-fat12-partition.img")
    with open(image, "wb") as f:
        f.truncate(13 * MIB)
    run("sfdisk", "--no-reread", "--no-tell-kernel", image, stdin="label: dos\nstart=2048, size=24576, type=1\n")
    run("mkfs.fat", "-F", "12", "-s", "8", "-n", "PART12", "--offset", "2048", image, "12288")
    v = Volume(image, 2048 * 512, work)
    v.mkdir("/Music")
    v.mkdir("/Video")
    v.write("/Music/Track 01 - intro.mp3", payload(1600 * KIB, 21))
    v.write("/Music/Track 02.mp3", payload(1200 * KIB, 22))
    v.delete("/Music/Track 01 - intro.mp3")
    v.write("/Video/Holiday clip.mp4", payload(3 * MIB, 23))
    require_fragmented(v, "/Video/Holiday clip.mp4")
    v.write("/NOTES.TXT", payload(4097, 24))
    v.check()
    v.write_manifest(os.path.join(out, "mtools-fat12-partition.manifest"))


def fat16(out, work):
    """A FAT16 volume of 2 KiB clusters, no partition table."""
    image = fresh(os.path.join(out, "mtools-fat16.img"))
    run("mkfs.fat", "-F", "16", "-s", "4", "-n", "CARD16", "-C", image, str(32 * 1024))
    v = Volume(image, 0, work)
    v.mkdir("/DCIM")
    v.mkdir("/DCIM/100MEDIA")
    v.mkdir("/DCIM/101MEDIA")
    v.write("/Grüße.txt", payload(100, 35))
    v.write("/DCIM/100MEDIA/IMG_0001.JPG", payload(300 * KIB, 31))
    v.write("/DCIM/100MEDIA/Gap photo.jpg", payload(200 * KIB, 32))
    v.write("/DCIM/100MEDIA/IMG_0003.JPG", payload(250 * KIB, 33))
    v.delete("/DCIM/100MEDIA/Gap photo.jpg")
    v.write("/DCIM/101MEDIA/MVI_0004 long clip.mp4", payload(2 * MIB, 34))
    require_fragmented(v, "/DCIM/101MEDIA/MVI_0004 long clip.mp4")
    v.check()
    v.write_manifest(os.path.join(out, "mtools-fat16.manifest"))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        for make in (floppy, partitioned, fat16):
            print(make.__doc__)
            make(out, work)
    print("wrote the images and manifests to", out)


main()
