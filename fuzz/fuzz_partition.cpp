// Fuzz target: partition tables (MBR with extended partitions, GPT with its
// backup, superfloppy boot records). The input is the start of a disk; the
// first byte picks the sector size (512 or 4096).

#include "fuzz_support.hpp"
#include "partition/partition_table.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    if (size < 1) {
        return 0;
    }
    const std::uint32_t sectorSize = (data[0] & 1) != 0 ? 4096 : 512;
    fuzz::FuzzSource source(fuzz::bytesOf(data + 1, size - 1), sectorSize);
    fuzz::require(source.open().ok(), "opening the source");
    const Result<partition::PartitionTable> table = partition::readPartitionTable(source);
    if (table.ok()) {
        // Every partition the table reports lies inside the device.
        for (const partition::Partition& found : table->partitions) {
            fuzz::require(found.offset <= source.size() && found.size <= source.size() - found.offset,
                          "a partition beyond the device");
        }
    }
    return 0;
}
