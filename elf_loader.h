#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

#define ET_EXEC    2
#define EM_386     3
#define PT_LOAD    1
#define ELFCLASS32 1

/* User program virtual address space layout (within the 4MB identity map) */
#define USER_LOAD_BASE   0x300000U  /* ELF segments load here */
#define USER_STACK_TOP   0x3F0000U  /* User stack grows down from here */
#define USER_STACK_PAGES 8
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_PAGES * 0x1000U)
/* Only the top of the stack (holding argc/argv) is mapped eagerly; the rest of
 * the stack region is filled on demand by the page-fault handler. */
#define USER_STACK_PREMAP 2

/* User heap grows up from just above the loaded ELF image toward the stack.
 * One guard page is kept between the heap ceiling and the stack bottom. */
#define USER_HEAP_MAX (USER_STACK_BOTTOM - 0x1000U)

/* Extended user region for mmap(), placed just above the kernel identity map
 * (0-32MB) so it never collides with kernel memory. Backed by a private page
 * table at directory entry 8 and filled on demand. This is what lets user
 * programs address memory beyond the original 4MB low window. */
#define USER_EXT_BASE 0x2000000U            /* 32MB */
#define USER_EXT_TOP  0x2400000U            /* 36MB (one 4MB page table) */
#define USER_EXT_DIR  8                     /* page-directory index for USER_EXT_BASE */

#define MAX_ARGS 16  /* max argv[] entries passed to a user program */

#include "paging.h"

/* Load an ELF image into a fresh user address space with argc/argv set up on
 * its stack. Returns the address space (and entry/esp/heap_base via the out
 * params), or NULL on failure. Does not create a process — used by both spawn
 * and exec. */
address_space_t *elf_load_image(const char *name, int argc, const char **argv,
                                uint32_t *entry, uint32_t *user_esp,
                                uint32_t *heap_base);

/* Spawn/exec with explicit argc/argv (argv[0] is the program name). */
int32_t elf_spawn_argv(int argc, const char **argv);
int     elf_exec_argv(int argc, const char **argv);

/* Convenience wrappers: argv = {name}, argc = 1. */
int32_t elf_spawn(const char *name);
int     elf_exec(const char *name);

#endif
