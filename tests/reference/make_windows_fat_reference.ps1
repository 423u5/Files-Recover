# Makes FAT32 and exFAT reference images with Windows' own drivers, for the
# reference tests (RECOVERY_FAT32_REFERENCE_DIR, RECOVERY_EXFAT_REFERENCE_DIR)
# and to see what Windows does when it deletes (limitations L7, L8, L12, L13):
# (each in a folder of its own, fat32 and exfat, as the reference tests take
# every image of their folder):
#   win_fat32.img  a 600 MB virtual disk, MBR, FAT32 of 4 KiB clusters (about
#                  153,000): a 260 MB file, so that what follows lies beyond
#                  cluster 65,536; a camera folder of 150 photos with long
#                  names (the folder grows over several clusters) of which
#                  every seventh is deleted; a folder of 120 photos and a
#                  subfolder, deleted whole; a voice note, deleted
#   win_exfat.img  a 256 MB virtual disk, MBR, exFAT of 4 KiB clusters: a file
#                  made fragmented by appending to it, deleted (L12); a folder
#                  nearly filled with a note, a folder of photos and forty
#                  small files, of which the note and the photos' folder are
#                  deleted; then a newer file whose four entries do not fit at
#                  the end of the folder's cluster, so that Windows reuses the
#                  deleted entries (an orphaned folder, L13) or extends it
# Each with <name>.manifest, tab-separated lines: active files
# ("<path>\t<size>\t<CRC-32>"), deleted entries ("deleted\t<name>"), deleted
# files whose data is known ("deleted_file\t<path>\t<size>\t<CRC-32>"), and for
# exFAT the label and cluster size ("label\t...", "cluster_size\t...").
# The photos are JPEG files GDI+ encodes; the voice note is 16-bit PCM WAV.
# What is written is flushed to the disk before anything is deleted: Windows
# drops what it has not written yet of a file deleted at once (the first run,
# 2026-10-16, left a deleted folder and the files written just before it as
# zeros on the disk).
#
# It needs an elevated PowerShell: it creates, attaches and formats virtual
# disks. It touches nothing else: each disk is checked to be the file-backed
# virtual disk of the file just created before it is partitioned and
# formatted, and it is detached at the end. Files are never overwritten; a
# disk the script cannot finish is detached and its unfinished file removed.
#
# Usage (elevated): powershell -ExecutionPolicy Bypass -File make_windows_fat_reference.ps1 <output folder>
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated PowerShell: it creates, attaches and formats virtual disks.'
}
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
public static class ReferenceData {
    static readonly uint[] Table = MakeTable();
    static uint[] MakeTable() {
        var table = new uint[256];
        for (uint i = 0; i < 256; i++) {
            uint c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return table;
    }
    public static string Crc32(byte[] data) {
        uint c = 0xFFFFFFFFu;
        foreach (byte b in data) c = Table[(c ^ b) & 0xFF] ^ (c >> 8);
        return (c ^ 0xFFFFFFFFu).ToString("X8");
    }
    static ulong Next(ref ulong x) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; }
    // A photo-like JPEG: noise over a gradient, so it does not compress to nothing.
    public static byte[] Jpeg(int width, int height, ulong seed) {
        ulong x = seed * 0x9E3779B97F4A7C15UL + 1;
        using (var bitmap = new Bitmap(width, height, PixelFormat.Format24bppRgb)) {
            for (int px = 0; px < width; px++) {
                for (int py = 0; py < height; py++) {
                    int noise = (int)(Next(ref x) % 64);
                    bitmap.SetPixel(px, py, Color.FromArgb((px * 3 + noise) % 256, (py * 5 + noise) % 256, (px + py + noise) % 256));
                }
            }
            using (var stream = new MemoryStream()) {
                bitmap.Save(stream, ImageFormat.Jpeg);
                return stream.ToArray();
            }
        }
    }
    // 16-bit mono PCM at 16 kHz.
    public static byte[] Wav(int samples, ulong seed) {
        ulong x = seed * 0x9E3779B97F4A7C15UL + 1;
        var data = new byte[44 + samples * 2];
        Action<int, string> text = (at, s) => { for (int i = 0; i < 4; i++) data[at + i] = (byte)s[i]; };
        Action<int, uint> le32 = (at, v) => { for (int i = 0; i < 4; i++) data[at + i] = (byte)(v >> (8 * i)); };
        text(0, "RIFF"); le32(4, (uint)(36 + samples * 2)); text(8, "WAVE"); text(12, "fmt ");
        le32(16, 16); data[20] = 1; data[22] = 1; le32(24, 16000); le32(28, 32000); data[32] = 2; data[34] = 16;
        text(36, "data"); le32(40, (uint)(samples * 2));
        for (int i = 0; i < samples; i++) {
            short sample = (short)(Math.Sin(i * 0.05) * 8000 + (int)(Next(ref x) % 512) - 256);
            data[44 + 2 * i] = (byte)sample;
            data[45 + 2 * i] = (byte)(sample >> 8);
        }
        return data;
    }
    public static void Filler(string path, long bytes) {
        var block = new byte[1 << 20];
        ulong x = 7;
        for (int i = 0; i < block.Length; i++) block[i] = (byte)Next(ref x);
        using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write)) {
            for (long done = 0; done < bytes; done += block.Length) file.Write(block, 0, block.Length);
        }
    }
}
'@

# Creates a fixed virtual disk of $SizeMB in $Vhd, attaches it, checks that the
# disk Windows attached is that file's, and returns it.
function New-ReferenceDisk([string]$Vhd, [int]$SizeMB) {
    $script = Join-Path $env:TEMP 'make_windows_fat_reference.diskpart'
    Set-Content -Path $script -Encoding ascii -Value "create vdisk file=`"$Vhd`" maximum=$SizeMB type=fixed"
    diskpart /s $script | Out-Null
    $created = $LASTEXITCODE
    Remove-Item $script
    if ($created -ne 0 -or -not (Test-Path $Vhd)) {
        throw "diskpart could not create $Vhd"
    }
    Mount-DiskImage -ImagePath $Vhd | Out-Null
    # The disk reached through the image is the image's; check that it is a
    # file-backed virtual disk of that size, still empty, before formatting.
    $disk = Get-DiskImage -ImagePath $Vhd | Get-Disk
    $isOurs = $disk.BusType -eq 'File Backed Virtual' -and [math]::Abs($disk.Size - [int64]$SizeMB * 1MB) -le 1MB -and
        $disk.PartitionStyle -eq 'RAW'
    if (-not $isOurs) {
        Dismount-DiskImage -ImagePath $Vhd | Out-Null
        throw "disk $($disk.Number) is not the empty virtual disk of $Vhd (bus $($disk.BusType), size $($disk.Size), style $($disk.PartitionStyle))"
    }
    Write-Host "disk $($disk.Number): $($disk.FriendlyName), $($disk.Size) bytes, $($disk.Location)"
    return $disk
}

# Partitions and formats the reference disk; returns the volume's root folder.
# The drive letter comes after the format: File Explorer offers to format a
# volume that has a letter and no filesystem yet.
function Format-ReferenceDisk($Disk, [string]$FileSystem, [int]$ClusterSize, [string]$Label) {
    Initialize-Disk -Number $Disk.Number -PartitionStyle MBR | Out-Null
    $partition = New-Partition -DiskNumber $Disk.Number -UseMaximumSize
    Format-Volume -Partition $partition -FileSystem $FileSystem -AllocationUnitSize $ClusterSize `
        -NewFileSystemLabel $Label -Confirm:$false -Force | Out-Null
    # (Windows may have given it one already, if it mounts new volumes itself.)
    if ("$((Get-Partition -DiskNumber $Disk.Number -PartitionNumber $partition.PartitionNumber).DriveLetter)" -notmatch '^[A-Za-z]$') {
        Add-PartitionAccessPath -DiskNumber $Disk.Number -PartitionNumber $partition.PartitionNumber -AssignDriveLetter |
            Out-Null
    }
    for ($try = 0; $try -lt 20; $try++) {
        $letter = "$((Get-Partition -DiskNumber $Disk.Number -PartitionNumber $partition.PartitionNumber).DriveLetter)"
        if ($letter -match '^[A-Za-z]$') { return "$($letter):\" }
        Start-Sleep -Milliseconds 500
    }
    throw "disk $($Disk.Number) got no drive letter"
}

# Writes $Bytes as $Path (relative to $Root) and returns its manifest line.
function Write-ReferenceFile([string]$Root, [string]$Path, [byte[]]$Bytes) {
    $full = Join-Path $Root $Path
    # (New-Item refuses a drive's root folder, "F:\", as "not of a legal form".)
    [System.IO.Directory]::CreateDirectory((Split-Path $full)) | Out-Null
    [System.IO.File]::WriteAllBytes($full, $Bytes)
    return [pscustomobject]@{ Path = '/' + ($Path -replace '\\', '/'); Size = $Bytes.Length; Crc = [ReferenceData]::Crc32($Bytes) }
}

function Close-ReferenceDisk([string]$Root, [string]$Vhd, [string]$Image, [string[]]$Manifest) {
    Write-VolumeCache -DriveLetter $Root.Substring(0, 1)
    Dismount-DiskImage -ImagePath $Vhd | Out-Null
    Move-Item -LiteralPath $Vhd -Destination $Image
    $text = ($Manifest -join "`n") + "`n"
    [System.IO.File]::WriteAllText([System.IO.Path]::ChangeExtension($Image, '.manifest'), $text,
        (New-Object System.Text.UTF8Encoding $false))
}

# Detaches the unfinished disk of $Vhd and removes its file (a later run makes it again).
function Remove-UnfinishedDisk([string]$Vhd) {
    if (Test-Path -LiteralPath $Vhd) {
        Dismount-DiskImage -ImagePath $Vhd -ErrorAction SilentlyContinue | Out-Null
        Remove-Item -LiteralPath $Vhd -Force -ErrorAction SilentlyContinue
        Write-Host "detached and removed the unfinished $Vhd"
    }
}

# ---------------------------------------------------------------------------
# FAT32 (L7, L8), in <output folder>\fat32
# ---------------------------------------------------------------------------
$folder = Join-Path $Out 'fat32'
New-Item -ItemType Directory -Force $folder | Out-Null
$image = Join-Path $folder 'win_fat32.img'
$vhd = Join-Path $folder 'win_fat32.vhd'
if ((Test-Path $image) -or (Test-Path $vhd)) { throw "$image exists: it is never overwritten" }
try {
    $disk = New-ReferenceDisk $vhd 600
    $root = Format-ReferenceDisk $disk 'FAT32' 4096 'WINFAT32'
    $manifest = @()
    $active = @()
    $deleted = @()

    $active += Write-ReferenceFile $root 'Kept low.jpg' ([ReferenceData]::Jpeg(160, 120, 1))
    [ReferenceData]::Filler((Join-Path $root 'filler.bin'), 260MB)
    $filler = Get-Item (Join-Path $root 'filler.bin')
    $manifest += "/filler.bin`t$($filler.Length)`t$([ReferenceData]::Crc32([System.IO.File]::ReadAllBytes($filler.FullName)))"
    for ($i = 1; $i -le 150; $i++) {
        $file = Write-ReferenceFile $root ('DCIM\100TEST\Photo of the day {0:D3}.jpg' -f $i) ([ReferenceData]::Jpeg(120 + ($i % 9) * 20, 90 + ($i % 7) * 15, 100 + $i))
        if ($i % 7 -eq 0) { $deleted += $file } else { $active += $file }
    }
    for ($i = 1; $i -le 120; $i++) {
        $deleted += Write-ReferenceFile $root ('Trip to delete\Trip photo number {0:D3}.jpg' -f $i) ([ReferenceData]::Jpeg(100 + ($i % 5) * 30, 80 + ($i % 4) * 20, 400 + $i))
    }
    for ($i = 1; $i -le 5; $i++) {
        $deleted += Write-ReferenceFile $root ('Trip to delete\nested\Inner photo {0}.jpg' -f $i) ([ReferenceData]::Jpeg(140, 100, 600 + $i))
    }
    $deleted += Write-ReferenceFile $root 'Voice note.wav' ([ReferenceData]::Wav(40000, 700))
    $active += Write-ReferenceFile $root 'Kept high.jpg' ([ReferenceData]::Jpeg(160, 120, 800))

    # On the disk before anything is deleted; then Windows deletes them (not to
    # the recycle bin).
    Write-VolumeCache -DriveLetter $root.Substring(0, 1)
    foreach ($file in $deleted) {
        if ($file.Path -notlike '/Trip to delete/*') {
            Remove-Item -LiteralPath (Join-Path $root ($file.Path.Substring(1) -replace '/', '\')) -Force
        }
    }
    Remove-Item -LiteralPath (Join-Path $root 'Trip to delete') -Recurse -Force
    foreach ($file in $active) { $manifest += "$($file.Path)`t$($file.Size)`t$($file.Crc)" }
    foreach ($file in $deleted) {
        $manifest += "deleted`t$(Split-Path $file.Path -Leaf)"
        $manifest += "deleted_file`t$($file.Path)`t$($file.Size)`t$($file.Crc)"
    }
    Close-ReferenceDisk $root $vhd $image $manifest
} catch {
    Remove-UnfinishedDisk $vhd
    throw
}
Write-Host "wrote $image"

# ---------------------------------------------------------------------------
# exFAT (L12, L13), in <output folder>\exfat
# ---------------------------------------------------------------------------
$folder = Join-Path $Out 'exfat'
New-Item -ItemType Directory -Force $folder | Out-Null
$image = Join-Path $folder 'win_exfat.img'
$vhd = Join-Path $folder 'win_exfat.vhd'
if ((Test-Path $image) -or (Test-Path $vhd)) { throw "$image exists: it is never overwritten" }
try {
    $disk = New-ReferenceDisk $vhd 256
    $root = Format-ReferenceDisk $disk 'exFAT' 4096 'WINEXFAT'
    $manifest = @("label`tWINEXFAT", "cluster_size`t4096")
    $active = @()
    $deleted = @()

    $text = { param([string]$Words, [int]$Times) [System.Text.Encoding]::ASCII.GetBytes($Words * $Times) }

    # A fragmented file (L12): one cluster, a file written right after it, and
    # two clusters appended, which must go elsewhere (its FAT chain is used).
    $growing = Join-Path $root 'Growing.txt'
    [System.IO.File]::WriteAllBytes($growing, (& $text 'grows ' 680))
    $active += Write-ReferenceFile $root 'Blocker.txt' (& $text 'blocks ' 580)
    $more = & $text 'more ' 1600
    $stream = [System.IO.File]::Open($growing, [System.IO.FileMode]::Append)
    $stream.Write($more, 0, $more.Length)
    $stream.Close()

    # A folder of 126 entries of the 128 its cluster holds: a note (three
    # entries, five clusters of data), a folder of photos (three entries) and
    # forty small files (three entries each).
    Write-ReferenceFile $root 'Album\a.txt' (& $text 'a note ' 2900) | Out-Null
    for ($i = 1; $i -le 6; $i++) {
        $deleted += Write-ReferenceFile $root ('Album\Old Photos\Old photo {0}.jpg' -f $i) ([ReferenceData]::Jpeg(160, 120, 900 + $i))
    }
    for ($i = 1; $i -le 40; $i++) {
        $active += Write-ReferenceFile $root ('Album\fill{0:D2}.txt' -f $i) (& $text "fill $i " 20)
    }
    $active += Write-ReferenceFile $root 'Kept.jpg' ([ReferenceData]::Jpeg(160, 120, 1000))

    Write-VolumeCache -DriveLetter $root.Substring(0, 1)
    Remove-Item -LiteralPath $growing -Force
    Remove-Item -LiteralPath (Join-Path $root 'Album\a.txt') -Force
    Remove-Item -LiteralPath (Join-Path $root 'Album\Old Photos') -Recurse -Force
    Write-VolumeCache -DriveLetter $root.Substring(0, 1)

    # Four entries, which do not fit in the two left at the end of the folder's
    # cluster: Windows appends a new entry set where there is room (seen on the
    # first run), so it reuses the deleted entries now, which orphans the old
    # folder's Stream Extension and File Name entries (L13), or extends the
    # folder. Windows takes the lowest free clusters for data: those of the
    # note and the fragmented file, not the old folder's.
    $active += Write-ReferenceFile $root 'Album\A newer picture name.jpg' ([ReferenceData]::Jpeg(120, 90, 1001))
    foreach ($file in $active) { $manifest += "$($file.Path)`t$($file.Size)`t$($file.Crc)" }
    foreach ($file in $deleted) {
        $manifest += "deleted`t$(Split-Path $file.Path -Leaf)"
        $manifest += "deleted_file`t$($file.Path)`t$($file.Size)`t$($file.Crc)"
    }
    # Its data is not listed: whether its chain survives deletion is L12's question.
    $manifest += "deleted`tGrowing.txt"
    Close-ReferenceDisk $root $vhd $image $manifest
} catch {
    Remove-UnfinishedDisk $vhd
    throw
}
Write-Host "wrote $image"
