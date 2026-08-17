#include "elf_loader.h"
#include "ramfs.h"
#include "paging.h"
#include "process.h"
#include "vga.h"
#include "utils.h"

static int read_exact(fs_node_t *file, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (size == 0) return 1;
    if (offset > file->length || size > file->length - offset) return 0;
    return read_fs(file, offset, size, buffer) == size;
}

static int elf_valid(const Elf32_Ehdr *h) {
    return h->e_ident[0] == 0x7f &&
           h->e_ident[1] == 'E'  &&
           h->e_ident[2] == 'L'  &&
           h->e_ident[3] == 'F'  &&
           h->e_ident[4] == ELFCLASS32 &&
           h->e_machine  == EM_386 &&
           h->e_type     == ET_EXEC;
}

static int read_program_header(fs_node_t *file, const Elf32_Ehdr *ehdr,
                               uint32_t index, Elf32_Phdr *phdr) {
    return read_exact(file,
                      ehdr->e_phoff + index * sizeof(Elf32_Phdr),
                      sizeof(Elf32_Phdr),
                      (uint8_t *)phdr);
}

/* Bounds every PT_LOAD segment must satisfy: it has to land inside the user
 * image region, and the bytes it claims to occupy in the file have to be there.
 * Each comparison is written so the subtraction cannot wrap.
 *
 * This is checked TWICE on purpose -- once in validate_segments() so a bad
 * image is rejected before any address space is built, and again in
 * load_segments() right before the values are used. The two passes re-read the
 * headers from the file, and the open reference the loader holds only stops the
 * file being unlinked, not rewritten: another process can modify it in between
 * (the loader yields on every timer tick). Without the second check such a
 * rewrite would hand an unvalidated p_vaddr straight to paging_map_user_page(),
 * which does no range checking of its own. */
static int phdr_in_user_range(const Elf32_Phdr *phdr, uint32_t file_length) {
    return phdr->p_vaddr >= USER_LOAD_BASE &&
           phdr->p_vaddr < USER_STACK_BOTTOM &&
           phdr->p_memsz <= USER_STACK_BOTTOM - phdr->p_vaddr &&
           phdr->p_filesz <= phdr->p_memsz &&
           phdr->p_offset <= file_length &&
           phdr->p_filesz <= file_length - phdr->p_offset;
}

static int phdr_maps_entry(const Elf32_Phdr *phdr, uint32_t entry) {
    return entry >= phdr->p_vaddr && entry - phdr->p_vaddr < phdr->p_memsz;
}

static int validate_segments(fs_node_t *file, const Elf32_Ehdr *ehdr) {
    int entry_mapped = 0;

    for (uint32_t i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr phdr;
        if (!read_program_header(file, ehdr, i, &phdr)) {
            terminal_writestring("exec: cannot read program header\n");
            return 0;
        }

        if (phdr.p_type != PT_LOAD) continue;

        if (!phdr_in_user_range(&phdr, file->length)) {
            terminal_writestring("exec: segment out of user range\n");
            return 0;
        }

        if (phdr_maps_entry(&phdr, ehdr->e_entry)) entry_mapped = 1;
    }

    if (!entry_mapped) {
        terminal_writestring("exec: entry point is not mapped\n");
        return 0;
    }
    return 1;
}

static int load_segments(address_space_t *space, fs_node_t *file,
                         const Elf32_Ehdr *ehdr, uint32_t *heap_base_out) {
    uint8_t buffer[256];
    uint32_t heap_base = USER_LOAD_BASE;
    int entry_mapped = 0;

    for (uint32_t i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr phdr;

        if (!read_program_header(file, ehdr, i, &phdr)) return 0;
        if (phdr.p_type != PT_LOAD) continue;

        /* Re-check: this is a second read of the header, and the file may have
         * been rewritten since validate_segments() approved it. See
         * phdr_in_user_range(). */
        if (!phdr_in_user_range(&phdr, file->length)) {
            terminal_writestring("exec: segment out of user range\n");
            return 0;
        }
        if (phdr_maps_entry(&phdr, ehdr->e_entry)) entry_mapped = 1;

        if (phdr.p_vaddr + phdr.p_memsz > heap_base)
            heap_base = phdr.p_vaddr + phdr.p_memsz;

        uint32_t page_start = phdr.p_vaddr & ~0xFFFU;
        uint32_t page_end = (phdr.p_vaddr + phdr.p_memsz + 0xFFFU) & ~0xFFFU;
        for (uint32_t addr = page_start; addr < page_end; addr += 0x1000U) {
            if (!paging_map_user_page(space, addr)) {
                terminal_writestring("exec: cannot allocate user page\n");
                return 0;
            }
        }

        if (!paging_zero_user(space, phdr.p_vaddr, phdr.p_memsz)) return 0;

        for (uint32_t copied = 0; copied < phdr.p_filesz;) {
            uint32_t chunk = phdr.p_filesz - copied;
            if (chunk > sizeof(buffer)) chunk = sizeof(buffer);

            if (!read_exact(file, phdr.p_offset + copied, chunk, buffer) ||
                !paging_copy_to_user(space, phdr.p_vaddr + copied,
                                     buffer, chunk)) {
                terminal_writestring("exec: cannot read segment\n");
                return 0;
            }
            copied += chunk;
        }
    }

    if (!entry_mapped) {
        terminal_writestring("exec: entry point is not mapped\n");
        return 0;
    }

    /* Heap starts on the first page above the highest loaded segment. */
    *heap_base_out = (heap_base + 0xFFFU) & ~0xFFFU;
    return 1;
}

/*
 * Copy argc/argv strings into the user stack, build the argv[] pointer array,
 * and push argc. Returns the new user ESP (pointing at argc), or 0 on failure.
 *
 * Stack layout built from top down (high to low):
 *   [USER_STACK_TOP - 4]     guard word
 *   string[argc-1]\0 ... string[0]\0   (4-byte aligned)
 *   argv[argc] = NULL        (4 bytes)
 *   argv[argc-1]             (4 bytes)
 *   ...
 *   argv[0]                  (4 bytes)
 *   argc          <- returned esp
 */
static uint32_t setup_user_argv(address_space_t *space,
                                int argc, const char **argv) {
    uint32_t ptrs[MAX_ARGS + 1];
    uint32_t esp = USER_STACK_TOP - 4;
    uint32_t null_val = 0;
    uint32_t argc_val;
    int i;

    if (argc > MAX_ARGS) argc = MAX_ARGS;

    /* Copy each string onto the stack from top to bottom. */
    for (i = argc - 1; i >= 0; i--) {
        uint32_t len = 0;
        while (argv[i][len]) len++;
        len++;                   /* include null terminator */
        esp -= len;
        esp &= ~3U;              /* align down to 4 bytes */
        if (!paging_copy_to_user(space, esp,
                                 (const uint8_t *)argv[i], len))
            return 0;
        ptrs[i] = esp;
    }

    /* NULL sentinel (argv[argc]) */
    esp -= 4;
    if (!paging_copy_to_user(space, esp, (const uint8_t *)&null_val, 4))
        return 0;

    /* argv pointer array, index argc-1 down to 0 */
    for (i = argc - 1; i >= 0; i--) {
        esp -= 4;
        if (!paging_copy_to_user(space, esp,
                                 (const uint8_t *)&ptrs[i], 4))
            return 0;
    }

    /* argc */
    esp -= 4;
    argc_val = (uint32_t)argc;
    if (!paging_copy_to_user(space, esp, (const uint8_t *)&argc_val, 4))
        return 0;

    return esp;
}

/* Build the address space from an already-resolved, already-referenced file.
 * Split out from elf_load_image() so that the open reference taken there is
 * released on exactly one path, no matter which of the failure exits below
 * runs. */
static address_space_t *elf_load_from_node(fs_node_t *file, const char *name,
                                           int argc, const char **argv,
                                           uint32_t *entry, uint32_t *user_esp,
                                           uint32_t *heap_base_out) {
    address_space_t *space;
    Elf32_Ehdr ehdr;
    uint32_t esp;
    uint32_t heap_base = USER_LOAD_BASE;

    (void)name;

    if (!read_exact(file, 0, sizeof(Elf32_Ehdr), (uint8_t *)&ehdr) ||
        !elf_valid(&ehdr)) {
        terminal_writestring("exec: not a valid ELF32/i386\n");
        return NULL;
    }
    if (ehdr.e_phentsize != sizeof(Elf32_Phdr) ||
        ehdr.e_phoff > file->length ||
        ehdr.e_phnum > (file->length - ehdr.e_phoff) / sizeof(Elf32_Phdr)) {
        terminal_writestring("exec: malformed program headers\n");
        return NULL;
    }
    if (!validate_segments(file, &ehdr)) return NULL;

    space = paging_create_user_address_space();
    if (!space) {
        terminal_writestring("exec: cannot create user address space\n");
        return NULL;
    }
    if (!load_segments(space, file, &ehdr, &heap_base)) {
        paging_destroy_user_address_space(space);
        return NULL;
    }

    /* Map only the top of the stack (where argc/argv live); the rest of the
     * stack region is paged in on demand when the program touches it. */
    for (int i = 0; i < USER_STACK_PREMAP; i++) {
        if (!paging_map_user_page(space,
                                  USER_STACK_TOP - (uint32_t)(i + 1) * 0x1000U)) {
            terminal_writestring("exec: cannot allocate user stack\n");
            paging_destroy_user_address_space(space);
            return NULL;
        }
    }

    /* Write argc/argv onto the user stack */
    esp = setup_user_argv(space, argc, argv);
    if (!esp) {
        terminal_writestring("exec: cannot set up argv\n");
        paging_destroy_user_address_space(space);
        return NULL;
    }

    *entry = ehdr.e_entry;
    *user_esp = esp;
    *heap_base_out = heap_base;
    return space;
}

address_space_t *elf_load_image(const char *name, int argc, const char **argv,
                                uint32_t *entry, uint32_t *user_esp,
                                uint32_t *heap_base_out) {
    /* Resolve through the VFS, not just RAMFS, so a program can be executed
     * from any mounted filesystem (e.g. "fat/prog" or "/disk/prog") and not
     * only from the embedded RAMFS image. Everything below already reads the
     * file through the generic read_fs()/node->length interface, so no backend
     * knows or cares which filesystem the image came from.
     *
     * Names without a leading '/' still resolve from the root, exactly as the
     * previous RAMFS-only lookup did. This is deliberately NOT relative to the
     * caller's working directory: bare command names must keep working after a
     * `cd` (ush does `cd fat` and then runs `cat`, which lives at /cat). */
    fs_node_t *file = resolve_fs(name);
    address_space_t *space;

    if (file && file->flags != FS_FILE) file = NULL;   /* directories are not images */

    if (!file) {
        terminal_writestring("exec: not found: ");
        terminal_writestring(name);
        terminal_writestring("\n");
        return NULL;
    }

    /* Hold an open reference for the whole load. Loading reads the file many
     * times (header, every program header, then each segment in 256-byte
     * chunks) and yields on every timer tick in between, so without this the
     * image can be unlinked mid-load: RAMFS would kfree() the node and this
     * loader would keep using it, calling node->read read out of freed heap
     * memory. That is the same class of kernel use-after-free as F11, and it is
     * reachable from the shell (`cp hello prog &` ... `rm prog`). All three
     * filesystems refuse to unlink a file that is open, so the reference is
     * what makes the load safe rather than any timing assumption. */
    open_fs(file);
    space = elf_load_from_node(file, name, argc, argv,
                               entry, user_esp, heap_base_out);
    close_fs(file);
    return space;
}

/* Internal: load the named ELF, set up argv on the stack, and spawn a task. */
static int32_t elf_load_and_spawn(const char *name,
                                  int argc, const char **argv) {
    uint32_t entry, user_esp, heap_base;
    address_space_t *space = elf_load_image(name, argc, argv,
                                            &entry, &user_esp, &heap_base);
    int32_t pid;

    if (!space) return -1;

    pid = process_launch(entry, user_esp, space, name, heap_base);
    if (pid < 0) {
        terminal_writestring("exec: create_task failed\n");
        paging_destroy_user_address_space(space);
        return -1;
    }
    return pid;
}

/* Public API ---------------------------------------------------------------- */

int32_t elf_spawn_argv(int argc, const char **argv) {
    if (argc < 1 || !argv || !argv[0]) return -1;
    return elf_load_and_spawn(argv[0], argc, argv);
}

int elf_exec_argv(int argc, const char **argv) {
    int32_t pid;
    if (argc < 1 || !argv || !argv[0]) return -1;
    pid = elf_load_and_spawn(argv[0], argc, argv);
    if (pid < 0) return -1;
    process_wait(pid);
    return 0;
}

int32_t elf_spawn(const char *name) {
    return elf_load_and_spawn(name, 1, &name);
}

int elf_exec(const char *name) {
    int32_t pid = elf_load_and_spawn(name, 1, &name);
    if (pid < 0) return -1;
    process_wait(pid);
    return 0;
}
