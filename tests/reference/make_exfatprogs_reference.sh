#!/bin/sh
# Builds exFAT reference images with mkfs.exfat (exfatprogs, an independent
# exFAT implementation) and a manifest for each, for cross-checking the
# RecoveryEngine parser (ExFatReferenceImageTest).
#
# Usage: make_exfatprogs_reference.sh <output dir>
# Needs mkfs.exfat (set MKFS_EXFAT to use one that is not on PATH) and, for
# the partitioned image, sfdisk. Neither needs root: they only write files.
#
# mkfs.exfat can only format, not add files, so these images check the boot
# region, FAT, allocation bitmap, standard up-case table, label and root
# directory as another implementation writes them.
set -eu

out=$1
mkfs=${MKFS_EXFAT:-mkfs.exfat}
mkdir -p "$out"

make_image() {  # name size-MiB label [mkfs options...]
    name=$1 size=$2 label=$3
    shift 3
    rm -f "$out/$name.img"
    truncate -s "${size}M" "$out/$name.img"
    "$mkfs" -q -L "$label" "$@" "$out/$name.img"
}

manifest() {  # name label cluster-size
    printf 'label\t%s\ncluster_size\t%s\n' "$2" "$3" > "$out/$1.manifest"
}

make_image default 64 REFVOL
manifest default REFVOL 4096

make_image cluster512 32 SMALLCL -c 512
manifest cluster512 SMALLCL 512

make_image cluster128k 128 BIGCL -c 128K
manifest cluster128k BIGCL 131072

label="Réf 📷 ÄÖ"
make_image unicode-label 16 "$label"
manifest unicode-label "$label" 4096

make_image packed-bitmap 64 PACKED --pack-bitmap
manifest packed-bitmap PACKED 4096

# An MBR disk with one exFAT partition (type 0x07) at sector 2048.
make_image partition-volume 48 INPART
rm -f "$out/partitioned.img"
truncate -s 50M "$out/partitioned.img"
printf 'label: dos\nstart=2048, size=98304, type=7\n' | sfdisk -q "$out/partitioned.img"
dd if="$out/partition-volume.img" of="$out/partitioned.img" bs=512 seek=2048 conv=notrunc status=none
rm -f "$out/partition-volume.img"
manifest partitioned INPART 4096
