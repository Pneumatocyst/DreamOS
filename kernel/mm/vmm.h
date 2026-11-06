#pragma once
#include "kernel.h"

/* Virtual memory: page tables.
 *
 * x86_64 translates every address through four levels of tables (PML4 -> PDPT ->
 * PD -> PT), 512 entries each, ending at a 4 KiB page of real memory. Each entry
 * carries permission bits; the one that matters here is USER: without it, ring 3
 * code touching the page gets a page fault instead.
 *
 * Every program gets its own top-level table (its "address space"). The top half
 * (entries 256-511) is copied from the kernel's, so the kernel is always there,
 * but those pages aren't marked USER, so programs can't touch them. The bottom
 * half is the program's own. */

#define USER_TOP 0x0000800000000000ull   /* everything below is user space */
#define PTE_P 0x1ull                     /* present */
#define PTE_W 0x2ull                     /* writable */
#define PTE_U 0x4ull                     /* user may access */

void vmm_init(void);
uint64_t *vmm_new_space(void);                       /* a fresh PML4 (kernel-virtual pointer) */
void vmm_free_space(uint64_t *pml4);                 /* frees the user half and the tables */
bool vmm_map_user(uint64_t *pml4, uint64_t va, uint64_t len);           /* fresh zeroed pages */
bool vmm_copy_to(uint64_t *pml4, uint64_t va, const void *src, uint64_t len);
bool vmm_user_ok(uint64_t *pml4, uint64_t va, uint64_t len, bool write); /* safe to touch? */
uint64_t vmm_user_pages(uint64_t *pml4);             /* how many pages a program has */
