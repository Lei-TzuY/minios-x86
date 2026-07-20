#!/usr/bin/env python3
import sys
import struct

SECTOR_SIZE = 512
SECTOR_COUNT = 2048
MARKER = b"MINIOS ATA PIO READ TEST\n"
DISKFS_MAGIC = 0x5346534D
DISKFS_VERSION = 2
DISKFS_GENERATION = 7
DISKFS_SUPERBLOCK_LBA = 4
DISKFS_DIRECTORY_LBA = 5
DISKFS_DIRECTORY_SECTORS = 2
DISKFS_DATA_LBA = 16
DISKFS_MAX_FILES = 16
DISKFS_FILE_SECTORS = 4
DISKFS_CHECKSUM_SEED = 0xA5C35A3C
DISKFS_SEED_NAME = b"seed.txt"
DISKFS_SEED_DATA = b"MINIOS DISKFS SEED\n"


def diskfs_checksum(fields: tuple[int, ...]) -> int:
    checksum = DISKFS_CHECKSUM_SEED
    for field in fields:
        checksum ^= field
    return checksum


def write_diskfs(image) -> None:
    fields = (
        DISKFS_MAGIC,
        DISKFS_VERSION,
        DISKFS_GENERATION,
        SECTOR_COUNT,
        DISKFS_DIRECTORY_LBA,
        DISKFS_DIRECTORY_SECTORS,
        DISKFS_DATA_LBA,
        DISKFS_MAX_FILES,
        DISKFS_FILE_SECTORS,
    )
    superblock = struct.pack("<10I", *fields, diskfs_checksum(fields))
    superblock += bytes(SECTOR_SIZE - len(superblock))

    entry = struct.pack(
        "<BBBB32sI", 1, 1, 0xFF, 0, DISKFS_SEED_NAME, len(DISKFS_SEED_DATA)
    )
    directory = entry + bytes(DISKFS_DIRECTORY_SECTORS * SECTOR_SIZE - len(entry))

    image.seek(DISKFS_SUPERBLOCK_LBA * SECTOR_SIZE)
    image.write(superblock)
    image.seek(DISKFS_DIRECTORY_LBA * SECTOR_SIZE)
    image.write(directory)
    image.seek(DISKFS_DATA_LBA * SECTOR_SIZE)
    image.write(DISKFS_SEED_DATA)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: gen_ata_image.py OUTPUT")

    with open(sys.argv[1], "wb") as image:
        image.truncate(SECTOR_SIZE * SECTOR_COUNT)
        image.seek(SECTOR_SIZE)
        image.write(MARKER)
        write_diskfs(image)


if __name__ == "__main__":
    main()
