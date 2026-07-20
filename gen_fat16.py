#!/usr/bin/env python3
"""Generate a tiny but standard FAT16-format disk image for miniOS to mount.

Layout (512-byte sectors, 1 sector per cluster, single FAT):
  sector 0      boot sector + BPB
  sector 1      FAT
  sector 2      root directory (16 entries)
  sector 3..    data region (cluster 2 = sector 3, cluster N = sector 1+N)

Files:
  /HELLO.TXT, /README.TXT, /DOCS/NOTE.TXT
"""
import struct
import sys

SECTOR = 512
TOTAL_SECTORS = 64
SECTORS_PER_CLUSTER = 1
RESERVED = 1
NUM_FATS = 1
ROOT_ENTRIES = 16
SECTORS_PER_FAT = 1

ROOT_DIR_LBA = RESERVED + NUM_FATS * SECTORS_PER_FAT          # 2
ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR  # 1
DATA_LBA = ROOT_DIR_LBA + ROOT_DIR_SECTORS                     # 3


def boot_sector():
    b = bytearray(SECTOR)
    b[0:3] = bytes([0xEB, 0x3C, 0x90])      # jump
    b[3:11] = b"MINIOS  "                    # OEM name
    struct.pack_into("<H", b, 11, SECTOR)            # bytes per sector
    b[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", b, 14, RESERVED)
    b[16] = NUM_FATS
    struct.pack_into("<H", b, 17, ROOT_ENTRIES)
    struct.pack_into("<H", b, 19, TOTAL_SECTORS)
    b[21] = 0xF8                             # media descriptor
    struct.pack_into("<H", b, 22, SECTORS_PER_FAT)
    struct.pack_into("<H", b, 24, 32)        # sectors per track
    struct.pack_into("<H", b, 26, 2)         # heads
    b[36] = 0x80                             # drive number
    b[38] = 0x29                             # extended boot signature
    struct.pack_into("<I", b, 39, 0x12345678)
    b[43:54] = b"MINIOS FAT "                # volume label (11)
    b[54:62] = b"FAT16   "                   # fs type (8)
    b[510] = 0x55
    b[511] = 0xAA
    return b


def fat_sector(chains):
    """chains: list of cluster numbers that are end-of-chain (single-cluster)."""
    b = bytearray(SECTOR)
    struct.pack_into("<H", b, 0, 0xFFF8)     # entry 0: media
    struct.pack_into("<H", b, 2, 0xFFFF)     # entry 1: reserved
    for cl in chains:
        struct.pack_into("<H", b, cl * 2, 0xFFFF)  # end of chain
    return b


def dir_entry(name8, ext3, attr, cluster, size):
    e = bytearray(32)
    e[0:8] = name8.encode("ascii").ljust(8, b" ")[:8]
    e[8:11] = ext3.encode("ascii").ljust(3, b" ")[:3]
    e[11] = attr
    struct.pack_into("<H", e, 26, cluster)
    struct.pack_into("<I", e, 28, size)
    return e


ATTR_DIR = 0x10
ATTR_FILE = 0x20

hello = b"Hello from FAT16!\n"
readme = b"miniOS FAT16 driver works.\n"
note = b"nested fat note\n"

# Cluster assignment: 2=HELLO, 3=README, 4=DOCS dir, 5=NOTE
root = bytearray(ROOT_ENTRIES * 32)
root[0:32] = dir_entry("HELLO", "TXT", ATTR_FILE, 2, len(hello))
root[32:64] = dir_entry("README", "TXT", ATTR_FILE, 3, len(readme))
root[64:96] = dir_entry("DOCS", "", ATTR_DIR, 4, 0)

docs = bytearray(SECTOR)
docs[0:32] = dir_entry(".", "", ATTR_DIR, 4, 0)
docs[32:64] = dir_entry("..", "", ATTR_DIR, 0, 0)
docs[64:96] = dir_entry("NOTE", "TXT", ATTR_FILE, 5, len(note))

image = bytearray(TOTAL_SECTORS * SECTOR)
image[0:SECTOR] = boot_sector()
image[SECTOR:2 * SECTOR] = fat_sector([2, 3, 4, 5])
image[ROOT_DIR_LBA * SECTOR:ROOT_DIR_LBA * SECTOR + len(root)] = root

def put_cluster(cluster, data):
    lba = DATA_LBA + (cluster - 2) * SECTORS_PER_CLUSTER
    off = lba * SECTOR
    image[off:off + len(data)] = data

put_cluster(2, hello)
put_cluster(3, readme)
put_cluster(4, docs)
put_cluster(5, note)

out = sys.argv[1] if len(sys.argv) > 1 else "fat16.img"
with open(out, "wb") as f:
    f.write(image)
