#include <stddef.h>
#include <stdint.h>

/*
 * The ELF loader is a trust boundary: it parses a file the kernel did not
 * produce and turns it into page mappings and an entry point to jump to. Every
 * field it reads -- e_phoff, e_phnum, p_offset, p_vaddr, p_filesz, p_memsz --
 * is attacker-controlled, and a missing bound on any of them means either
 * reading past the file or mapping/writing outside the user window.
 *
 * None of that is reachable from the shell, which only ever runs the well-formed
 * images the build embeds: producing a malformed ELF would mean writing one to
 * the filesystem by hand. So the rejection paths -- the whole point of the
 * validation -- have never been exercised. This builds images in memory
 * instead, one corrupted field at a time.
 *
 * Two properties are checked for every rejection, not just the NULL return:
 *   - a header rejected before the address space is created must not create one;
 *   - a failure after it is created must destroy it, or exec leaks an address
 *     space (and its frames) on every bad image.
 *
 * elf_loader.c is included directly to reach the static validators, and
 * --gc-sections drops elf_spawn/elf_exec and their process_launch/process_wait
 * dependencies, since main only ever calls elf_load_image().
 */

#include "../elf_loader.c"

/* test.h after elf_loader.c so the kernel headers' enums precede the macros
 * <stdio.h> defines. */
#include "test.h"

/* --- the fake file being parsed ------------------------------------------- */

#define EHDR_SZ ((uint32_t)sizeof(Elf32_Ehdr))
#define PHDR_SZ ((uint32_t)sizeof(Elf32_Phdr))
#define IMG_MAX 8192u

/* Segment data sits well past the header table so p_offset can be corrupted in
 * both directions without colliding with the headers. */
#define SEG_OFF    256u
#define SEG_VADDR  USER_LOAD_BASE
#define SEG_FILESZ 64u
#define SEG_MEMSZ  128u

static uint8_t   g_img[IMG_MAX];
static uint32_t  g_img_len;
static fs_node_t g_node;
static int       g_have_file;      /* 0 makes resolve_fs report "not found" */

static Elf32_Ehdr *ehdr(void) { return (Elf32_Ehdr *)g_img; }
static Elf32_Phdr *phdr(uint32_t i) {
    return (Elf32_Phdr *)(g_img + EHDR_SZ) + i;
}

/* A well-formed image with `nphdr` identical PT_LOAD segments covering the
 * entry point. Each test corrupts exactly one field of this. */
static void build_valid(uint32_t nphdr) {
    Elf32_Ehdr *e = ehdr();
    uint32_t i;

    for (i = 0; i < IMG_MAX; i++) g_img[i] = 0;

    e->e_ident[0] = 0x7f;
    e->e_ident[1] = 'E';
    e->e_ident[2] = 'L';
    e->e_ident[3] = 'F';
    e->e_ident[4] = ELFCLASS32;
    e->e_type      = ET_EXEC;
    e->e_machine   = EM_386;
    e->e_version   = 1;
    e->e_entry     = SEG_VADDR;
    e->e_phoff     = EHDR_SZ;
    e->e_ehsize    = (uint16_t)EHDR_SZ;
    e->e_phentsize = (uint16_t)PHDR_SZ;
    e->e_phnum     = (uint16_t)nphdr;

    for (i = 0; i < nphdr; i++) {
        Elf32_Phdr *p = phdr(i);
        p->p_type   = PT_LOAD;
        p->p_offset = SEG_OFF;
        p->p_vaddr  = SEG_VADDR;
        p->p_filesz = SEG_FILESZ;
        p->p_memsz  = SEG_MEMSZ;
    }

    g_img_len    = SEG_OFF + SEG_FILESZ;   /* file ends with the segment data */
    g_node.length = g_img_len;
    g_node.flags  = FS_FILE;
    g_have_file   = 1;
}

/* --- stubs ---------------------------------------------------------------- */

fs_node_t *resolve_fs(const char *path) {
    (void)path;
    return g_have_file ? &g_node : NULL;
}

/* Simulates the file being rewritten underneath the loader: once armed, the
 * program header is corrupted just before the SECOND time it is read, i.e.
 * after validate_segments() has approved it and before load_segments() uses it.
 * See test_rejects_a_header_rewritten_mid_load. */
enum {
    REWRITE_NONE,
    REWRITE_OUT_OF_RANGE,
    REWRITE_ENTRY_UNMAPPED,
};
static int g_rewrite_phdr;
static int g_phdr_reads;

uint32_t read_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    uint32_t i;

    (void)node;

    if (offset == EHDR_SZ && size == PHDR_SZ) {
        if (++g_phdr_reads == 2) {
            if (g_rewrite_phdr == REWRITE_OUT_OF_RANGE)
                phdr(0)->p_vaddr = 0x1000;  /* far outside the user window */
            if (g_rewrite_phdr == REWRITE_ENTRY_UNMAPPED)
                phdr(0)->p_vaddr = SEG_VADDR + 0x2000U; /* valid, but moved */
        }
    }

    if (offset >= g_img_len) return 0;
    if (size > g_img_len - offset) size = g_img_len - offset;
    for (i = 0; i < size; i++) buffer[i] = g_img[offset + i];
    return size;
}

/* The loader holds an open reference for the whole load so the image cannot be
 * unlinked from under it. Counted here to prove it is released on every exit. */
static int g_opens, g_closes;
void open_fs(fs_node_t *node) { (void)node; g_opens++; }
void close_fs(fs_node_t *node) { (void)node; g_closes++; }

/* Every rejection prints a diagnostic; the test asserts on return values. */
void terminal_writestring(const char *s) { (void)s; }

static address_space_t g_space;
static int g_created, g_destroyed, g_mapped;
static int g_map_limit;      /* fail paging_map_user_page past this many pages */

address_space_t *paging_create_user_address_space(void) {
    g_created++;
    return &g_space;
}

void paging_destroy_user_address_space(address_space_t *space) {
    (void)space;
    g_destroyed++;
}

int paging_map_user_page(address_space_t *space, uint32_t vaddr) {
    (void)space; (void)vaddr;
    if (g_map_limit >= 0 && g_mapped >= g_map_limit) return 0;
    g_mapped++;
    return 1;
}

int paging_zero_user(address_space_t *space, uint32_t vaddr, uint32_t size) {
    (void)space; (void)vaddr; (void)size;
    return 1;
}

int paging_copy_to_user(address_space_t *space, uint32_t vaddr,
                        const uint8_t *src, uint32_t size) {
    (void)space; (void)vaddr; (void)src; (void)size;
    return 1;
}

/* --- helpers -------------------------------------------------------------- */

static uint32_t g_entry, g_esp, g_heap;

static address_space_t *load(void) {
    static const char *argv[] = { "prog" };

    g_created = g_destroyed = g_mapped = 0;
    g_opens = g_closes = 0;
    g_phdr_reads = 0;
    g_map_limit = -1;
    g_entry = g_esp = g_heap = 0;
    return elf_load_image("prog", 1, argv, &g_entry, &g_esp, &g_heap);
}

/* A rejection that happens while reading the headers, before any address space
 * exists: nothing to clean up, and nothing may have been created. */
static void CHECK_REJECTED_EARLY(void) {
    CHECK(load() == NULL);
    CHECK_EQ(g_created, 0);
    CHECK_EQ(g_destroyed, 0);
}

static uint32_t page_up(uint32_t v) { return (v + 0xFFFU) & ~0xFFFU; }

/* --- tests ---------------------------------------------------------------- */

static void test_valid_image_loads(void) {
    build_valid(1);

    TEST("a well-formed image loads");
    CHECK(load() != NULL);
    CHECK_EQ(g_entry, SEG_VADDR);
    CHECK_EQ(g_created, 1);
    CHECK_EQ(g_destroyed, 0);          /* success must not tear it down */

    /* The heap starts on the first page above the highest byte of the image. */
    CHECK_EQ(g_heap, page_up(SEG_VADDR + SEG_MEMSZ));

    /* argc/argv are written at the top of the stack, inside the pre-mapped
     * pages -- the rest of the stack is demand-paged and would fault here. */
    CHECK(g_esp < USER_STACK_TOP);
    CHECK(g_esp >= USER_STACK_TOP - USER_STACK_PREMAP * 0x1000U);
}

static void test_rejects_non_elf(void) {
    TEST("magic, class, machine and type are all enforced");
    build_valid(1); ehdr()->e_ident[0] = 0x00; CHECK_REJECTED_EARLY();
    build_valid(1); ehdr()->e_ident[1] = 'X';  CHECK_REJECTED_EARLY();
    build_valid(1); ehdr()->e_ident[2] = 'X';  CHECK_REJECTED_EARLY();
    build_valid(1); ehdr()->e_ident[3] = 'X';  CHECK_REJECTED_EARLY();

    /* A 64-bit or non-x86 image would be decoded with the wrong layout. */
    build_valid(1); ehdr()->e_ident[4] = 2;    CHECK_REJECTED_EARLY();  /* ELFCLASS64 */
    build_valid(1); ehdr()->e_machine  = 62;   CHECK_REJECTED_EARLY();  /* EM_X86_64 */

    /* Only ET_EXEC: a relocatable or shared object has no fixed load address,
     * so its p_vaddr values would be meaningless here. */
    build_valid(1); ehdr()->e_type = 1;        CHECK_REJECTED_EARLY();  /* ET_REL */
    build_valid(1); ehdr()->e_type = 3;        CHECK_REJECTED_EARLY();  /* ET_DYN */
}

static void test_rejects_truncated_file(void) {
    TEST("a file shorter than the ELF header is rejected");
    build_valid(1);
    g_img_len = EHDR_SZ - 1;          /* header itself does not fit */
    g_node.length = g_img_len;
    CHECK_REJECTED_EARLY();

    TEST("an empty file is rejected");
    build_valid(1);
    g_img_len = 0;
    g_node.length = 0;
    CHECK_REJECTED_EARLY();
}

static void test_rejects_bad_header_table(void) {
    TEST("e_phentsize must match the program header size");
    /* A wrong stride would make every subsequent header decode misaligned. */
    build_valid(1); ehdr()->e_phentsize = (uint16_t)(PHDR_SZ - 1); CHECK_REJECTED_EARLY();
    build_valid(1); ehdr()->e_phentsize = (uint16_t)(PHDR_SZ + 1); CHECK_REJECTED_EARLY();

    TEST("the program header table must lie inside the file");
    build_valid(1); ehdr()->e_phoff = g_img_len + 1; CHECK_REJECTED_EARLY();

    /* e_phnum is bounded by how many headers actually fit after e_phoff; the
     * check is a division so a huge count cannot overflow into a small one. */
    TEST("e_phnum is bounded by the space left after e_phoff");
    build_valid(1);
    ehdr()->e_phnum = (uint16_t)((g_img_len - EHDR_SZ) / PHDR_SZ + 1);
    CHECK_REJECTED_EARLY();

    build_valid(1); ehdr()->e_phnum = 0xFFFF; CHECK_REJECTED_EARLY();

    TEST("an impossible e_phnum is rejected before any header is read");
    /* read_exact would refuse the out-of-file headers one by one anyway, so the
     * return value alone cannot tell whether the e_phnum gate exists. What the
     * gate actually buys is that a bogus count is rejected up front instead of
     * driving 65535 attacker-chosen read attempts, so assert the cheap
     * rejection: not a single program header is read. */
    build_valid(1);
    ehdr()->e_phnum = 0xFFFF;
    CHECK_REJECTED_EARLY();
    CHECK_EQ(g_phdr_reads, 0);
}

static void test_rejects_segments_outside_user_window(void) {
    TEST("a segment must start inside the user load window");
    build_valid(1); phdr(0)->p_vaddr = USER_LOAD_BASE - 1;  CHECK_REJECTED_EARLY();
    build_valid(1); phdr(0)->p_vaddr = 0;                   CHECK_REJECTED_EARLY();
    build_valid(1); phdr(0)->p_vaddr = USER_STACK_BOTTOM;   CHECK_REJECTED_EARLY();

    TEST("a segment must not extend into the stack");
    build_valid(1);
    phdr(0)->p_memsz = USER_STACK_BOTTOM - SEG_VADDR + 1;
    CHECK_REJECTED_EARLY();

    /* The largest segment that still fits is accepted, so the bound is not
     * off by one in the safe direction either. */
    TEST("a segment ending exactly at the stack bottom is accepted");
    build_valid(1);
    phdr(0)->p_memsz  = USER_STACK_BOTTOM - SEG_VADDR;
    phdr(0)->p_filesz = 0;                 /* nothing to read from the file */
    CHECK(load() != NULL);

    TEST("p_memsz near 2^32 cannot wrap past the bound");
    build_valid(1);
    phdr(0)->p_memsz = 0xFFFFFFFFU;
    CHECK_REJECTED_EARLY();

    TEST("a later segment's p_memsz cannot wrap past the bound either");
    /* The case above does not actually isolate the overflow: a wrapping
     * p_vaddr + p_memsz always lands BELOW p_vaddr, so the entry-point check
     * rejects that image regardless of how the size bound is written. Mutating
     * the bound to the wrapping form `p_vaddr + p_memsz <= BOTTOM` therefore
     * survived that test.
     *
     * A second segment has no such accidental cover: the entry point is already
     * satisfied by the first one, so the size bound itself is the only thing
     * standing between a crafted p_memsz and load_segments(). Written as
     * `p_memsz <= BOTTOM - p_vaddr` with p_vaddr already known to be below
     * BOTTOM, the subtraction cannot wrap. */
    build_valid(2);
    phdr(1)->p_vaddr  = SEG_VADDR + 0x10000U;   /* still inside the window */
    phdr(1)->p_memsz  = 0xFFFFFFFFU;
    phdr(1)->p_filesz = 0;
    phdr(1)->p_offset = 0;
    CHECK_REJECTED_EARLY();
}

static void test_rejects_segments_outside_file(void) {
    TEST("file-backed bytes must lie inside the file");
    build_valid(1); phdr(0)->p_offset = g_img_len + 1; CHECK_REJECTED_EARLY();

    /* Reading past EOF: offset is valid but offset+filesz is not. */
    build_valid(1); phdr(0)->p_filesz = SEG_FILESZ + 1; CHECK_REJECTED_EARLY();

    TEST("p_filesz may not exceed p_memsz");
    /* Otherwise the copy would run past the memory the segment reserved.
     * The file is grown so those bytes really are present: with the default
     * 320-byte image the file-bounds clause rejects this first and the
     * filesz-vs-memsz clause is never the one under test. */
    build_valid(1);
    g_img_len     = SEG_OFF + 200;
    g_node.length = g_img_len;
    phdr(0)->p_offset = SEG_OFF;
    phdr(0)->p_filesz = 200;           /* inside the file... */
    phdr(0)->p_memsz  = 128;           /* ...but past what the segment reserved */
    CHECK_REJECTED_EARLY();

    /* The same shape, but too big for the file as well: still rejected. */
    build_valid(1);
    phdr(0)->p_filesz = SEG_MEMSZ + 1;
    phdr(0)->p_memsz  = SEG_MEMSZ;
    CHECK_REJECTED_EARLY();

    TEST("p_offset near 2^32 cannot wrap past the file bound");
    build_valid(1); phdr(0)->p_offset = 0xFFFFFFFFU; CHECK_REJECTED_EARLY();
}

static void test_entry_point_must_be_mapped(void) {
    TEST("the entry point must fall inside a loaded segment");
    /* Otherwise the kernel would jump into an unmapped page immediately. */
    build_valid(1); ehdr()->e_entry = SEG_VADDR - 1;         CHECK_REJECTED_EARLY();
    build_valid(1); ehdr()->e_entry = SEG_VADDR + SEG_MEMSZ; CHECK_REJECTED_EARLY();
    build_valid(1); ehdr()->e_entry = 0;                     CHECK_REJECTED_EARLY();

    TEST("the last byte of a segment is still a valid entry point");
    build_valid(1);
    ehdr()->e_entry = SEG_VADDR + SEG_MEMSZ - 1;
    CHECK(load() != NULL);
}

static void test_non_load_segments_are_ignored(void) {
    TEST("non-PT_LOAD headers are skipped, however absurd");
    /* A PT_NOTE/PT_DYNAMIC entry carries no mapping, so its addresses are not
     * constrained -- but it must also not be allowed to fail the load. */
    build_valid(2);
    phdr(1)->p_type   = 4;             /* PT_NOTE */
    phdr(1)->p_vaddr  = 0xFFFFF000U;   /* nowhere near the user window */
    phdr(1)->p_offset = 0xFFFFFF00U;   /* nowhere near inside the file */
    phdr(1)->p_filesz = 0xFFFFFFFFU;
    phdr(1)->p_memsz  = 0xFFFFFFFFU;
    CHECK(load() != NULL);
    CHECK_EQ(g_entry, SEG_VADDR);

    TEST("a non-PT_LOAD segment cannot supply the entry point");
    /* The entry must be covered by something actually mapped. */
    build_valid(2);
    phdr(1)->p_type  = 4;
    phdr(1)->p_vaddr = SEG_VADDR + 0x1000U;
    phdr(1)->p_memsz = 0x1000U;
    phdr(1)->p_filesz = 0;
    phdr(1)->p_offset = 0;
    ehdr()->e_entry  = SEG_VADDR + 0x1000U;   /* inside the PT_NOTE only */
    CHECK_REJECTED_EARLY();
}

static void test_heap_starts_above_the_highest_segment(void) {
    TEST("heap_base clears the highest segment, page aligned");
    build_valid(2);
    phdr(1)->p_vaddr  = SEG_VADDR + 0x2000U;
    phdr(1)->p_offset = SEG_OFF;
    phdr(1)->p_filesz = 16;
    phdr(1)->p_memsz  = 100;
    CHECK(load() != NULL);
    CHECK_EQ(g_heap, page_up(SEG_VADDR + 0x2000U + 100));

    /* Order must not matter: the highest segment wins even if listed first. */
    TEST("segment order does not affect heap_base");
    build_valid(2);
    phdr(0)->p_vaddr  = SEG_VADDR + 0x2000U;
    phdr(0)->p_memsz  = 100;
    phdr(0)->p_filesz = 16;
    phdr(1)->p_vaddr  = SEG_VADDR;
    phdr(1)->p_memsz  = SEG_MEMSZ;
    phdr(1)->p_filesz = SEG_FILESZ;
    ehdr()->e_entry   = SEG_VADDR;
    CHECK(load() != NULL);
    CHECK_EQ(g_heap, page_up(SEG_VADDR + 0x2000U + 100));
}

static void test_missing_or_non_regular_file(void) {
    TEST("a missing file is reported, not loaded");
    build_valid(1);
    g_have_file = 0;
    CHECK_REJECTED_EARLY();

    TEST("a directory is not an executable");
    /* resolve_fs happily returns directories; the loader must filter them. */
    build_valid(1);
    g_node.flags = FS_DIRECTORY;
    CHECK_REJECTED_EARLY();
}

static void test_address_space_released_on_late_failure(void) {
    TEST("a failure after the address space exists still releases it");
    /* This is the leak-prone half: everything up to here failed before
     * paging_create_user_address_space() was ever called. */
    build_valid(1);
    CHECK(load() == NULL || g_map_limit != 0);   /* sanity: baseline loads */

    g_created = g_destroyed = g_mapped = 0;
    g_map_limit = 0;                              /* first mapping fails */
    {
        static const char *argv[] = { "prog" };
        CHECK(elf_load_image("prog", 1, argv, &g_entry, &g_esp, &g_heap) == NULL);
    }
    CHECK_EQ(g_created, 1);
    CHECK_EQ(g_destroyed, 1);                     /* released, not leaked */

    TEST("a stack mapping failure also releases the address space");
    build_valid(1);
    g_created = g_destroyed = g_mapped = 0;
    /* Let the segment pages through, then fail on the stack pre-map. */
    g_map_limit = 1;
    {
        static const char *argv[] = { "prog" };
        CHECK(elf_load_image("prog", 1, argv, &g_entry, &g_esp, &g_heap) == NULL);
    }
    CHECK_EQ(g_created, 1);
    CHECK_EQ(g_destroyed, 1);
}

static void test_open_reference_is_balanced(void) {
    TEST("the image is kept open for the whole load and released after");
    /* Without the reference the file can be unlinked mid-load and the node
     * freed while the loader still reads through it. Releasing it on only some
     * exits would instead pin the file forever, so both counts must match. */
    build_valid(1);
    CHECK(load() != NULL);
    CHECK_EQ(g_opens, 1);
    CHECK_EQ(g_closes, 1);

    TEST("the reference is released on a rejected image too");
    build_valid(1);
    ehdr()->e_ident[1] = 'X';
    CHECK(load() == NULL);
    CHECK_EQ(g_opens, 1);
    CHECK_EQ(g_closes, 1);

    TEST("the reference is released when the load fails late");
    build_valid(1);
    {
        static const char *argv[] = { "prog" };
        g_created = g_destroyed = g_mapped = 0;
        g_opens = g_closes = 0;
        g_phdr_reads = 0;
        g_map_limit = 0;                        /* fail inside load_segments */
        CHECK(elf_load_image("prog", 1, argv, &g_entry, &g_esp, &g_heap) == NULL);
    }
    CHECK_EQ(g_opens, 1);
    CHECK_EQ(g_closes, 1);

    TEST("a file that does not resolve is never opened");
    build_valid(1);
    g_have_file = 0;
    CHECK(load() == NULL);
    CHECK_EQ(g_opens, 0);
    CHECK_EQ(g_closes, 0);
}

static void test_rejects_a_header_rewritten_mid_load(void) {
    TEST("a program header rewritten after validation is caught");
    /* The headers are read twice -- once to validate, once to load -- and the
     * loader yields between them, so another process can rewrite the file in
     * the gap. The open reference stops an unlink, not a rewrite, so the second
     * read has to be re-checked. Otherwise a p_vaddr that was in range when
     * validated reaches paging_map_user_page(), which range-checks nothing. */
    build_valid(1);
    g_rewrite_phdr = REWRITE_OUT_OF_RANGE;
    CHECK(load() == NULL);
    g_rewrite_phdr = REWRITE_NONE;

    /* And the address space built before the corruption was noticed is freed. */
    CHECK_EQ(g_created, 1);
    CHECK_EQ(g_destroyed, 1);
    CHECK_EQ(g_closes, 1);

    TEST("the same image loads when it is not rewritten");
    /* Guards against the rewrite hook itself being what fails the load. */
    build_valid(1);
    CHECK(load() != NULL);

    TEST("a valid rewrite may not leave the entry point unmapped");
    /* Moving the segment to another valid user page still passes the second
     * range check. The second-read segment set must also cover e_entry, or the
     * loader returns an address space that faults on its first instruction. */
    build_valid(1);
    g_rewrite_phdr = REWRITE_ENTRY_UNMAPPED;
    CHECK(load() == NULL);
    g_rewrite_phdr = REWRITE_NONE;
    CHECK_EQ(g_created, 1);
    CHECK_EQ(g_destroyed, 1);
    CHECK_EQ(g_closes, 1);

    TEST("entry coverage may come from another second-read segment");
    build_valid(2);
    g_rewrite_phdr = REWRITE_ENTRY_UNMAPPED; /* only header 0 is rewritten */
    CHECK(load() != NULL);                   /* header 1 still covers e_entry */
    g_rewrite_phdr = REWRITE_NONE;
    CHECK_EQ(g_created, 1);
    CHECK_EQ(g_destroyed, 0);
}

int main(void) {
    test_valid_image_loads();
    test_rejects_non_elf();
    test_rejects_truncated_file();
    test_rejects_bad_header_table();
    test_rejects_segments_outside_user_window();
    test_rejects_segments_outside_file();
    test_entry_point_must_be_mapped();
    test_non_load_segments_are_ignored();
    test_heap_starts_above_the_highest_segment();
    test_missing_or_non_regular_file();
    test_address_space_released_on_late_failure();
    test_open_reference_is_balanced();
    test_rejects_a_header_rewritten_mid_load();
    TEST_REPORT("elf");
}
