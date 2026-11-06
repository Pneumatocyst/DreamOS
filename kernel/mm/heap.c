/* The kernel heap, in two tiers, like most real kernels:
 *
 *   small (up to 2 KiB): "size classes". Each class (32, 64 ... 2048 bytes) keeps a
 *     free list of blocks. When a list runs dry, we take a fresh page from the page
 *     allocator and cut it into blocks of that size. Fast, and no searching.
 *
 *   large: straight to the page allocator, rounded up to whole 4 KiB pages.
 *
 * Every block starts with a 16-byte header recording how big it is, so kfree
 * knows where to put it back. The pointer you get is just past the header. */
#include "mm/heap.h"
#include "mm/pmm.h"
#include "arch/io.h"

#define MAGIC_USED 0xd7ea4a11u
#define MAGIC_FREE 0xf7eef7eeu
#define LARGE      0xffff

struct header {
    uint32_t magic;
    uint16_t cls;            /* size class index, or LARGE */
    uint16_t reserved;
    uint64_t size;           /* what the caller asked for */
};
_Static_assert(sizeof(struct header) == 16, "header keeps allocations 16-byte aligned");

struct free_block { struct header h; struct free_block *next; };

static const uint32_t class_size[] = { 32, 64, 128, 256, 512, 1024, 2048 };
#define NCLASSES (int)(sizeof class_size / sizeof class_size[0])
static struct free_block *free_list[NCLASSES];
static struct heap_stats stats;

static bool refill(int c) {
    uint8_t *page = pmm_alloc(1);
    if (!page) return false;
    for (uint32_t off = 0; off + class_size[c] <= PAGE_SIZE; off += class_size[c]) {
        struct free_block *b = (struct free_block *)(page + off);
        b->h.magic = MAGIC_FREE;
        b->h.cls = c;
        b->next = free_list[c];
        free_list[c] = b;
    }
    return true;
}

static size_t large_pages(uint64_t size) {
    return (size + sizeof(struct header) + PAGE_SIZE - 1) / PAGE_SIZE;
}

static void *do_kmalloc(size_t size) {
    if (!size) return NULL;
    size_t need = size + sizeof(struct header);
    struct header *h = NULL;

    for (int c = 0; c < NCLASSES; c++) {
        if (need > class_size[c]) continue;
        if (!free_list[c] && !refill(c)) return NULL;
        struct free_block *b = free_list[c];
        free_list[c] = b->next;
        h = &b->h;
        h->cls = c;
        break;
    }
    if (!h) {
        h = pmm_alloc(large_pages(size));
        if (!h) return NULL;
        h->cls = LARGE;
    }
    h->magic = MAGIC_USED;
    h->size = size;
    stats.live_allocs++;
    stats.total_allocs++;
    stats.live_bytes += size;
    return h + 1;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static struct header *header_of(void *ptr, const char *who) {
    struct header *h = (struct header *)ptr - 1;
    if (h->magic == MAGIC_FREE) panic(who);
    if (h->magic != MAGIC_USED) panic("kfree/krealloc: that pointer didn't come from kmalloc");
    return h;
}

static void do_kfree(void *ptr) {
    if (!ptr) return;
    struct header *h = header_of(ptr, "kfree: freed the same memory twice (double free)");
    stats.live_allocs--;
    stats.live_bytes -= h->size;
    h->magic = MAGIC_FREE;
    if (h->cls == LARGE) {
        pmm_free(h, large_pages(h->size));
    } else {
        struct free_block *b = (struct free_block *)h;
        b->next = free_list[h->cls];
        free_list[h->cls] = b;
    }
}

static void *do_krealloc(void *ptr, size_t size) {
    if (!ptr) return do_kmalloc(size);
    if (!size) { do_kfree(ptr); return NULL; }
    struct header *h = header_of(ptr, "krealloc: memory was already freed");
    size_t room = h->cls == LARGE ? large_pages(h->size) * PAGE_SIZE - sizeof *h
                                  : class_size[h->cls] - sizeof *h;
    if (size <= room) {                     /* still fits: just update the size */
        stats.live_bytes += size - h->size;
        h->size = size;
        return ptr;
    }
    void *n = do_kmalloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, h->size);
    do_kfree(ptr);
    return n;
}

/* Several tasks share the heap, so each call runs with interrupts off:
   no task switch can happen halfway through updating a free list. */
void *kmalloc(size_t size) {
    uint64_t f = irq_save();
    void *p = do_kmalloc(size);
    irq_restore(f);
    return p;
}

void kfree(void *ptr) {
    uint64_t f = irq_save();
    do_kfree(ptr);
    irq_restore(f);
}

void *krealloc(void *ptr, size_t size) {
    uint64_t f = irq_save();
    void *p = do_krealloc(ptr, size);
    irq_restore(f);
    return p;
}

void heap_stats(struct heap_stats *s) { *s = stats; }
