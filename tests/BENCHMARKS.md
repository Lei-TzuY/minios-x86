# Performance measurements

Run with `make bench`. These numbers back two changes that were originally
made on reasoning alone, so that a reader does not have to take "this is
faster" on faith.

## How to read this

- **memcpy/memset** are *timed* against a byte-at-a-time baseline. Timing is
  host- and compiler-dependent, so the **ratio and its direction** are the
  result, not the absolute nanoseconds. Measured on the x86-64 WSL host under
  `gcc -O1 -fno-builtin`; the kernel itself builds 32-bit at `-O2
  -ffreestanding`, so the real in-kernel figures will differ in magnitude while
  the shape holds. The word version under test is the actual `utils.c` code,
  linked in.
- **RAMFS growth** is *counted*, not timed: reallocations and bytes copied.
  Those are exact algorithmic quantities, independent of host, CPU and
  compiler, so they are the strong evidence here.

## memcpy / memset (word-at-a-time vs byte-at-a-time)

Representative of three consistent runs; ratios were stable to within a few
percent.

| operation | size / alignment        | speedup |
|-----------|-------------------------|---------|
| memset    | 4096, aligned (page)    | ~3.9x   |
| memset    | 512, aligned (sector)   | ~4.6–5.2x |
| memset    | 64, aligned             | ~3.4x   |
| memset    | 4096, dest+1 (unaligned)| ~3.7x   |
| memset    | 7 bytes                 | ~1.0x   |
| memcpy    | 4096, aligned (page)    | ~3.9x   |
| memcpy    | 512, aligned (sector)   | ~4.6x   |
| memcpy    | 64, aligned             | ~3.0x   |
| memcpy    | 4096, dest & src misaligned relative to each other | **~1.0x** |
| memcpy    | 7 bytes                 | ~1.0x   |

What the numbers say, including the parts that are *not* a win:

- The aligned page and sector sizes are exactly the kernel's hot path — every
  demand-paged/COW frame is a zeroed 4 KB page, every ATA transfer is a 512-byte
  sector, every heap block is at least 4-byte aligned. Those get a 3–5x
  speedup, which is the case that matters.
- **memset always benefits**, even from an unaligned start, because it only
  needs the destination aligned (it aligns the head, then fills words).
- **memcpy only benefits when source and destination share alignment.** When
  they are misaligned relative to each other it falls back to the byte loop and
  the speedup is ~1.0x — no gain. This is a deliberate correctness choice (no
  unaligned 32-bit stores) and an honest limitation of the change: it was not
  stated when the optimization was first claimed.
- Tiny sizes (7 bytes) are ~1.0x: the fast path is skipped, so there is no
  regression on small copies.

## RAMFS growth (geometric vs reallocate-on-every-append)

Building one file with N writes of K bytes. `old` is the pre-change behaviour
(reallocate and copy the whole file on every extending write), shown as its
exact closed form; `new` is measured from the shipping code.

| N    | K  | reallocations new / old | growth-copy bytes new / old | copying saved |
|------|----|-------------------------|-----------------------------|---------------|
| 64   | 16 | 5 / 64                  | 960 / 32,256                | 34x           |
| 128  | 16 | 6 / 128                 | 1,984 / 130,048             | 66x           |
| 256  | 16 | 7 / 256                 | 4,032 / 522,240             | 130x          |
| 512  | 16 | 8 / 512                 | 8,128 / 2,093,056           | 258x          |
| 1024 | 8  | 8 / 1024                | 8,128 / 4,190,208           | 516x          |

Reallocations fall from N to about log2(N·K / 64); bytes copied for growth fall
from O(N²) to O(final size). The saving grows without bound as the file does,
which is the whole point of the change — the old code was quadratic to build a
file up incrementally.
