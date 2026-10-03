# testdata

Nothing in this directory is committed except this README (see `.gitignore`). Disk images are large and
may contain personal data.

The tests generate their data at run time with a fixed seed (`tests/support/test_files.hpp`) and build their
FAT32, exFAT and NTFS volumes in memory, so no files are needed here. Reference images made by other
implementations (see `docs/testing/testing.md`) are generated locally, not committed.

Later phases will add a test-image generator (`tools/test_image_generator`). It will build reproducible
FAT32, exFAT and NTFS images and media corpora in this directory. Never put images of real user devices
here.
