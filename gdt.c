#include "gdt.h"

extern void gdt_flush(uint32_t);
extern void tss_flush(void);

gdt_entry_t gdt_entries[6];
gdt_ptr_t   gdt_ptr;
tss_entry_t tss_entry;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;
    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;
    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access      = access;
}

static void write_tss(int32_t num, uint16_t ss0, uint32_t esp0) {
    uint32_t base = (uint32_t) &tss_entry;
    uint32_t limit = base + sizeof(tss_entry_t);

    gdt_set_gate(num, base, limit, 0xE9, 0x00);
    
    /* Ensure the TSS is initially zero'd */
    for(uint32_t i = 0; i < sizeof(tss_entry_t); i++) ((uint8_t*)&tss_entry)[i] = 0;

    tss_entry.ss0  = ss0;  // Set the kernel stack segment
    tss_entry.esp0 = esp0; // Set the kernel stack pointer
    tss_entry.cs   = 0x08 | 0x3; // 0x08 = Kernel code. +3 = RPL 3
    tss_entry.ss = tss_entry.ds = tss_entry.es = tss_entry.fs = tss_entry.gs = 0x10 | 0x3;
}

void set_kernel_stack(uint32_t stack) {
    tss_entry.esp0 = stack;
}

void gdt_install(void) {
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 6) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    gdt_set_gate(0, 0, 0, 0, 0);                // Null segment
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Kernel Code
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Kernel Data
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User Code
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User Data
    
    write_tss(5, 0x10, 0x0); // TSS

    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush();
}
