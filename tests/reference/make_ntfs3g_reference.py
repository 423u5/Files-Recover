#!/usr/bin/env python3
"""Builds NTFS reference images with ntfs-3g, an independent NTFS implementation,
and a manifest for each, for cross-checking the RecoveryEngine parser
(NtfsReferenceImageTest; the manifest format is documented there).

Usage: make_ntfs3g_reference.py <output dir>

mkntfs formats each image and ntfs-3g, mounted through FUSE, writes and then
deletes files in it, so the images hold active, fragmented, sparse and
deleted files and directories exactly as another implementation lays them out.
Only image files are touched; no device is ever opened.

Needs mkntfs and ntfs-3g on PATH, sfdisk for the partitioned image, and FUSE.
Without root, the script runs the mount inside an unprivileged user and mount
namespace (unshare -rm). If ntfs-3g then stops with "setgroups failed", set
NTFS3G_PRELOAD to a library whose setgroups() succeeds (see
docs/testing/testing.md).
"""

import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

MIB = 1024 * 1024


def payload(size, seed):
    return random.Random(seed).randbytes(size)


class Volume:
    """A mounted volume that records what the manifest must say about it."""

    def __init__(self, root):
        self.root = root
        self.entries = []  # (kind, path, size, crc) in manifest order

    def _full(self, path):
        return os.path.join(self.root, path.lstrip("/"))

    def mkdir(self, path):
        os.makedirs(self._full(path), exist_ok=True)
        self.entries.append(["dir", path])

    def write(self, path, data, kind="file"):
        with open(self._full(path), "wb") as f:
            f.write(data)
        self.entries.append([kind, path, str(len(data)), format(zlib.crc32(data), "08x")])

    def _mark(self, path, kind):
        for entry in self.entries:
            if entry[1] == path:
                entry[0] = kind

    def delete(self, path, kind="deleted"):
        os.remove(self._full(path))
        self._mark(path, kind)

    def delete_tree(self, path):
        shutil.rmtree(self._full(path))
        prefix = path.rstrip("/") + "/"
        for entry in self.entries:
            if entry[1] == path:
                entry[0] = "deleted_dir"
            elif entry[1].startswith(prefix):
                entry[0] = "deleted_dir" if entry[0] == "dir" else "deleted"


def populate_basic(v):
    v.mkdir("/DCIM")
    v.mkdir("/DCIM/100MEDIA")
    v.mkdir("/Documents")
    v.mkdir("/Old stuff")
    v.write("/small.txt", payload(100, 1))
    v.write("/DCIM/100MEDIA/IMG_0001.JPG", payload(50000, 2))
    v.write("/DCIM/100MEDIA/IMG_0002.JPG", payload(123457, 3))
    v.write("/Documents/Été αβγ Фото 😀.txt", payload(3000, 4))
    v.write("/Documents/" + "long name " * 20 + ".bin", payload(777, 5))
    v.write("/empty.bin", b"")
    v.write("/gone.jpg", payload(40000, 6))
    v.write("/note.txt", payload(300, 7))
    v.write("/Old stuff/a.bin", payload(20000, 8))
    v.write("/Old stuff/b.txt", payload(50, 9))
    # A file with a hole: ntfs-3g stores it as a sparse attribute.
    with open(v._full("/holes.bin"), "wb") as f:
        f.write(payload(4096, 10))
        f.seek(MIB)
        f.write(payload(4096, 11))
    v.entries.append(["sparse", "/holes.bin"])
    v.delete("/gone.jpg")
    v.delete("/note.txt")
    v.delete_tree("/Old stuff")


def populate_fragmented(v):
    # Fill the volume, free every other file, then write a file that can
    # only fit in the holes.
    v.mkdir("/fill")
    names = []
    for i in range(10000):
        name = "/fill/f%04d.bin" % i
        data = payload(64 * 1024, 100 + i)
        try:
            v.write(name, data)
        except OSError:
            os.remove(v._full(name))
            v.entries.pop()
            break
        names.append(name)
    # Create the big file's record first, so that it cannot reuse a record freed below.
    open(v._full("/big.bin"), "wb").close()
    for name in names[::2]:
        v.delete(name, "deleted_any")
    big = payload(len(names) // 2 * 64 * 1024 - 8 * 4096, 7)
    with open(v._full("/big.bin"), "wb") as f:
        f.write(big)
    v.entries.append(["file", "/big.bin", str(len(big)), format(zlib.crc32(big), "08x")])
    v.entries.append(["fragmented", "/big.bin"])


def populate_many(v):
    v.mkdir("/Many")
    rng = random.Random(42)
    for i in range(600):
        v.write("/Many/file%04d.txt" % i, payload(rng.randint(1, 3000), 1000 + i))
    for i in range(0, 600, 5):
        v.delete("/Many/file%04d.txt" % i)


def populate_small(v):
    v.mkdir("/dir")
    v.write("/a.txt", payload(200, 1))
    v.write("/b.bin", payload(30000, 2))
    v.write("/dir/c.bin", payload(5000, 3))
    v.write("/d.bin", payload(7000, 4))
    v.delete("/d.bin")


def populate_large_clusters(v):
    v.write("/a.bin", payload(100000, 1))
    v.write("/b.txt", payload(10, 2))
    v.write("/c.bin", payload(200000, 3))
    v.delete("/c.bin")


# name: (volume MiB, mkntfs options, label, populate, partitioned)
SCENARIOS = {
    "basic": (16, ["-c", "4096"], "REFNTFS", populate_basic, False),
    "fragmented": (8, ["-c", "4096"], "FRAG", populate_fragmented, False),
    "many-files": (32, ["-c", "4096"], "MANY", populate_many, False),
    "cluster512": (8, ["-c", "512"], "SMALLCL", populate_small, False),
    "cluster64k": (64, ["-c", "65536"], "BIGCL", populate_large_clusters, False),
    "sector4k": (16, ["-c", "4096", "-s", "4096"], "SECT4K", populate_small, False),
    "partitioned": (48, ["-c", "4096", "-p", "2048"], "INPART", populate_small, True),
}


def populate(name, image, manifest):
    size_mib, options, label, fill, partitioned = SCENARIOS[name]
    mountpoint = tempfile.mkdtemp()
    env = dict(os.environ)
    if os.environ.get("NTFS3G_PRELOAD"):
        env["LD_PRELOAD"] = os.environ["NTFS3G_PRELOAD"]
    daemon = subprocess.Popen(["ntfs-3g", "-o", "no_detach", image, mountpoint], env=env)
    deadline = time.time() + 20
    while not os.path.ismount(mountpoint):
        if daemon.poll() is not None or time.time() > deadline:
            sys.exit("ntfs-3g failed to mount " + image)
        time.sleep(0.1)
    volume = Volume(mountpoint)
    try:
        fill(volume)
    finally:
        subprocess.run(["umount", mountpoint], check=True)
        daemon.wait(timeout=60)
        os.rmdir(mountpoint)
    cluster = options[options.index("-c") + 1]
    with open(manifest, "w", encoding="utf-8", newline="\n") as f:
        f.write("label\t%s\ncluster_size\t%s\n" % (label, cluster))
        for entry in volume.entries:
            f.write("\t".join(entry) + "\n")


def make(out, name):
    size_mib, options, label, _, partitioned = SCENARIOS[name]
    image = os.path.join(out, name + ".img")
    volume = image + ".volume" if partitioned else image
    for path in (image, volume):
        if os.path.exists(path):
            os.remove(path)
    with open(volume, "wb") as f:
        f.truncate(size_mib * MIB)
    subprocess.run(["mkntfs", "-F", "-f", "-q", "-L", label] + options + [volume], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    command = [sys.executable, os.path.abspath(__file__), "--populate", name, volume,
               os.path.join(out, name + ".manifest")]
    if os.geteuid() != 0:
        command = ["unshare", "-rm"] + command
    subprocess.run(command, check=True)
    if partitioned:
        # An MBR disk with the volume as partition 1 (type 0x07) at sector 2048.
        with open(image, "wb") as f:
            f.truncate((size_mib + 2) * MIB)
        table = "label: dos\nstart=2048, size=%d, type=7\n" % (size_mib * MIB // 512)
        subprocess.run(["sfdisk", "-q", image], input=table.encode(), check=True)
        with open(volume, "rb") as src, open(image, "r+b") as dst:
            dst.seek(2048 * 512)
            shutil.copyfileobj(src, dst)
        os.remove(volume)
    print("made", image)


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--populate":
        populate(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name in SCENARIOS:
        make(out, name)


if __name__ == "__main__":
    main()
