/* Physical memory manager: a bitmap with one bit per 4 KiB page of RAM.
 *   bit = 1  the page is in use (or isn't usable RAM at all)
 *   bit = 0  the page is free
 *
 * At boot, the memory map from Limine tells us which regions are usable RAM.
 * We mark everything used, then free the usable parts. The bitmap itself has
 * to live somewhere, so it goes in the first usable region big enough for it. */
#include "mm/pmm.h"
#include <limine.h>
#include "arch/io.h"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0,
};

static uint64_t hhdm;
static uint8_t *bitmap;
static uint64_t frames;          /* pages the bitmap covers */
static uint64_t usable, used;    /* page counts */
static uint64_t hint;            /* where the last search ended: start the next one there */

uint64_t virt_to_phys(const void *v) { return (uint64_t)v - hhdm; }
void    *phys_to_virt(uint64_t p)    { return (void *)(p + hhdm); }

static bool test(uint64_t f) { return bitmap[f / 8] & (1 << (f % 8)); }
static void set(uint64_t f)  { bitmap[f / 8] |= 1 << (f % 8); }
static void clr(uint64_t f)  { bitmap[f / 8] &= ~(1 << (f % 8)); }

void pmm_init(struct limine_memmap_response *map) {
    if (!hhdm_req.response || !map) panic("no memory map or HHDM from the bootloader");
    hhdm = hhdm_req.response->offset;

    /* how far does usable RAM reach? */
    uint64_t top = 0;
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *e = map->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->base + e->length > top) top = e->base + e->length;
    }
    frames = top / PAGE_SIZE;
    uint64_t bytes = (frames + 7) / 8;

    /* find a home for the bitmap */
    for (uint64_t i = 0; i < map->entry_count && !bitmap; i++) {
        struct limine_memmap_entry *e = map->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bytes) bitmap = phys_to_virt(e->base);
    }
    if (!bitmap) panic("no room for the page bitmap");
    memset(bitmap, 0xff, bytes);                    /* everything used... */

    for (uint64_t i = 0; i < map->entry_count; i++) {   /* ...except usable RAM */
        struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t first = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t last  = (e->base + e->length) / PAGE_SIZE;
        for (uint64_t f = first; f < last; f++) { clr(f); usable++; }
    }

    /* the bitmap's own pages, and page 0 (so a NULL pointer is never valid memory) */
    uint64_t self = virt_to_phys(bitmap) / PAGE_SIZE;
    for (uint64_t f = self; f < self + (bytes + PAGE_SIZE - 1) / PAGE_SIZE; f++) { set(f); used++; }
    if (!test(0)) { set(0); used++; }
}

void *pmm_alloc(size_t pages) {
    if (!pages) return NULL;
    uint64_t fl = irq_save();           /* tasks share the bitmap */
    uint64_t run = 0;
    /* next-fit: scan from the hint, wrap around once */
    for (uint64_t n = 0; n < frames + pages; n++) {
        uint64_t f = (hint + n) % frames;
        if (f == 0) run = 0;                        /* runs can't wrap past the end */
        if (test(f)) { run = 0; continue; }
        if (++run < pages) continue;

        uint64_t start = f + 1 - pages;
        for (uint64_t k = start; k <= f; k++) set(k);
        used += pages;
        hint = f + 1;
        irq_restore(fl);
        return phys_to_virt(start * PAGE_SIZE);
    }
    irq_restore(fl);
    return NULL;
}

void pmm_free(void *addr, size_t pages) {
    uint64_t fl = irq_save();
    uint64_t start = virt_to_phys(addr) / PAGE_SIZE;
    for (uint64_t f = start; f < start + pages; f++) {
        if (!test(f)) panic("pmm_free: freeing a page that was already free");
        clr(f);
    }
    used -= pages;
    irq_restore(fl);
}

uint64_t pmm_total_pages(void) { return usable; }
uint64_t pmm_used_pages(void)  { return used; }
