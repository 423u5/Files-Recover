# Partition detection (P3)

`partition::readPartitionTable(source)` works out how a device is divided into volumes.
`partition::PartitionSource` then exposes one volume as its own `IStorageSource`.

## Detection order

1. **Sector 0 is a volume boot record** (a FAT, exFAT or NTFS boot sector): the scheme is `Unpartitioned`.
   The whole device is one volume. Many USB flash drives are formatted this way (a "superfloppy").
2. **Valid MBR** (signature 0x55AA, and every status byte is 0x00 or 0x80):
   - if it has a 0xEE entry, the scheme is `Gpt`, and the GPT is read;
   - otherwise the scheme is `Mbr`: the primary entries are read and any extended partition's EBR chain
     is walked.
3. **No valid MBR, but a valid GPT**: the scheme is `Gpt`, and a `ProtectiveMbrMissing` issue is recorded.
4. **Nothing recognisable**: the scheme is `Unknown`, with no partitions. Later phases carve the whole device.

If sector 0 can't be read, the parser records the problem and still tries the GPT.

## Validation (all table data is untrusted)

| Check | Result when it fails |
| --- | --- |
| Partition starts at or beyond the end of the device | Dropped (`PartitionOutOfRange`) |
| Partition extends beyond the end of the device | Size clamped, `truncated = true` (`PartitionTruncated`) |
| MBR entry with start LBA 0 or 0 sectors | Skipped (`MbrInvalidEntry`) |
| More than one extended partition | The extra one is skipped |
| EBR outside the extended partition, missing signature, or chain loop | Chain stops (`ExtendedChainInvalid` / `ExtendedChainLoop`) |
| More than 128 logical partitions | Chain stops (`ExtendedChainTooLong`) |
| Logical partition past the end of its container | Clamped |
| GPT header signature, revision 1.x, size 92..sector size, CRC32, own LBA | Header rejected |
| GPT first usable ≤ last usable; entry size a power of two in 128..4096; 1..16384 entries; array ≤ 4 MiB, inside the device and clear of the header; array CRC32 | Header rejected |
| Primary header invalid | Backup header used (`GptPrimaryInvalid`) |
| Both headers invalid | `GptUnavailable`; the MBR entries of a hybrid MBR are used if there are any |
| Primary and backup valid but different | Primary used (`GptHeadersDisagree`) |
| GPT entry with first LBA > last LBA | Skipped (`GptEntryInvalid`) |
| GPT entry outside the usable range | Kept, flagged (`GptEntryInvalid`) |
| Overlapping partitions | Kept, flagged (`PartitionOverlap`) |

All LBA arithmetic is overflow-checked. **Every partition returned lies entirely inside the device.** A fuzz
test in `tests/unit/partition` mutates MBR, EBR and GPT sectors 3,000 times and checks this.

## Filesystem candidates

`Partition::candidates` suggests which filesystems a partition may hold, based on its MBR type byte, GPT
type GUID or boot record signature. For example, 0x07 means exFAT or NTFS, and Microsoft basic data means
FAT, exFAT or NTFS. The filesystem modules make the final decision by validating the boot sector.

## PartitionSource

This is a read-only window onto a byte range of a parent source. Reads use offsets relative to the start
of the partition and go through the normal `IStorageSource` validation. As a result, a filesystem parser
running inside a partition cannot read outside it, whatever its own metadata claims. `open()` fails if the
range isn't inside the parent.
