# Storage abstraction (P1)

## Classes

```
IStorageSource                 read() / readExact() / readSectors()  — non-virtual, shared validation
 └─ DeviceBackedSource         geometry checks, request splitting, aligned bounce buffer
     ├─ PhysicalDiskSource     \\.\PhysicalDriveN, sector-aligned I/O
     └─ DiskImageSource        raw image file, no alignment requirement
        ▲
        │ IDeviceOpener / IDeviceIo      (platform seam)
        │
   src/storage/windows/        Win32: CreateFileW(GENERIC_READ), ReadFile with OVERLAPPED offsets, IOCTLs
   tests/support/              MemoryDeviceIo / MockDeviceOpener (simulated disks)
```

Physical disks and images share `DeviceBackedSource`, so the same request gets the same answer from both.
`tests/integration/storage/source_contract_test.cpp` runs one test suite against every source type.

`DiskLister` (`include/storage/disk_list.hpp`, P19) lists the physical disks attached, by number, with their
vendor, product, removability, bus, size and the drive letters of their volumes, for a user interface to choose a
source from. `makePlatformDiskLister()` asks Windows through handles that cannot read or change anything (desired
access 0); see `src/storage/windows/windows_disk_list.cpp` for its Windows assumptions.

## Read contract

Every source validates reads the same way, in `IStorageSource::read`, before the implementation sees the
request:

| Request | Result |
| --- | --- |
| Source not open | `NotOpen` |
| Buffer larger than `kMaxReadSize` (64 MiB) | `InvalidArgument` |
| Offset of 2^63 or more (negative for Windows) | `InvalidArgument` |
| `offset > size`, or range extends past the end (overflow-safe) | `OutOfRange` (the read is never shortened) |
| Zero-length read with `offset <= size` | `Success`; the device is not touched |
| Device returned fewer bytes without an error | `PartialRead`, with the valid prefix in `bytesRead` |
| Device error | `IoError` with the Win32 code and the number of good bytes before the error |
| Implementation reports more bytes than requested | `InternalError` |

`readSectors` checks `first * sectorSize` and `count * sectorSize` for overflow. `readExact` turns anything
other than `Success` into an `Error`.

## Geometry validation

When a source is opened, its geometry is rejected unless all of these hold:

- the logical and physical sector sizes are powers of two between 512 and 65536;
- the alignment is a power of two no larger than 65536;
- the size is below 2^63;
- for devices that need aligned I/O, the size is a multiple of the alignment.

Physical disks must also have a non-zero size that is a multiple of the sector size. Their alignment is
never less than the sector size, even if the driver reports less.

Image files may end in a partial sector. Byte reads can reach it; whole-sector reads of it return
`OutOfRange`.
