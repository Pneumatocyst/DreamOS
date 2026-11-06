#include "mm/vmm.h"
#include "mm/pmm.h"
#include "arch/io.h"
#include "sched/task.h"

#define ADDR 0x000ffffffffff000ull       /* the physical address inside an entry */

static uint64_t *table_of(uint64_t entry) { return phys_to_virt(entry & ADDR); }

static void *zeroed_page(void) {
    void *p = pmm_alloc(1);
    if (p) memset(p, 0, PAGE_SIZE);
    return p;
}

void vmm_init(void) {
    /* SMAP stops the kernel touching user pages unless it asks first. We copy
       to and from programs directly (after checking their pointers), so turn it off. */
    uint64_t cr4 = read_cr4();
    if (cr4 & (1ull << 21)) write_cr4(cr4 & ~(1ull << 21));
}

uint64_t *vmm_new_space(void) {
    uint64_t *pml4 = zeroed_page();
    if (!pml4) return NULL;
    uint64_t *kernel = phys_to_virt(kernel_cr3() & ADDR);
    for (int i = 256; i < 512; i++) pml4[i] = kernel[i];   /* share the kernel half */
    return pml4;
}

/* Find the last-level entry for va, creating tables on the way if asked. */
static uint64_t *pte_for(uint64_t *pml4, uint64_t va, bool create) {
    uint64_t *t = pml4;
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t *e = &t[(va >> shift) & 511];
        if (!(*e & PTE_P)) {
            if (!create) return NULL;
            uint64_t *n = zeroed_page();
            if (!n) return NULL;
            *e = virt_to_phys(n) | PTE_P | PTE_W | PTE_U;
        }
        t = table_of(*e);
    }
    return &t[(va >> 12) & 511];
}

bool vmm_map_user(uint64_t *pml4, uint64_t va, uint64_t len) {
    if (va >= USER_TOP || len > USER_TOP - va) return false;
    for (uint64_t p = va & ~0xfffull; p < va + len; p += PAGE_SIZE) {
        uint64_t *pte = pte_for(pml4, p, true);
        if (!pte) return false;
        if (*pte & PTE_P) continue;                        /* already there */
        void *page = zeroed_page();
        if (!page) return false;
        *pte = virt_to_phys(page) | PTE_P | PTE_W | PTE_U;
    }
    return true;
}

/* Copy into a program's memory through the kernel's own map of physical RAM,
   so we don't need to switch to the program's page tables to do it. */
bool vmm_copy_to(uint64_t *pml4, uint64_t va, const void *src, uint64_t len) {
    const uint8_t *s = src;
    while (len) {
        uint64_t *pte = pte_for(pml4, va, false);
        if (!pte || !(*pte & PTE_P)) return false;
        uint64_t off = va & 0xfff, n = PAGE_SIZE - off;
        if (n > len) n = len;
        memcpy((uint8_t *)table_of(*pte) + off, s, n);
        va += n; s += n; len -= n;
    }
    return true;
}

bool vmm_user_ok(uint64_t *pml4, uint64_t va, uint64_t len, bool write) {
    if (va >= USER_TOP || len > USER_TOP - va) return false;
    if (!len) return true;
    for (uint64_t p = va & ~0xfffull; p < va + len; p += PAGE_SIZE) {
        uint64_t *pte = pte_for(pml4, p, false);
        if (!pte || !(*pte & PTE_P) || !(*pte & PTE_U)) return false;
        if (write && !(*pte & PTE_W)) return false;
    }
    return true;
}

static uint64_t walk_free(uint64_t *t, int level, bool free_it) {
    uint64_t pages = 0;
    for (int i = 0; i < (level == 4 ? 256 : 512); i++) {     /* level 4: only the user half */
        if (!(t[i] & PTE_P)) continue;
        if (level == 1) { pages++; if (free_it) pmm_free(table_of(t[i]), 1); continue; }
        pages += walk_free(table_of(t[i]), level - 1, free_it);
        if (free_it) pmm_free(table_of(t[i]), 1);
    }
    return pages;
}

void vmm_free_space(uint64_t *pml4) {
    walk_free(pml4, 4, true);
    pmm_free(pml4, 1);
}

uint64_t vmm_user_pages(uint64_t *pml4) { return walk_free(pml4, 4, false); }
