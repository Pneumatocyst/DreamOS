#pragma once
#include "kernel.h"

#define PAGE_SIZE 4096

struct limine_memmap_response;

/* The physical memory manager: hands out 4 KiB pages of real RAM. */
void  pmm_init(struct limine_memmap_response *map);
void *pmm_alloc(size_t pages);            /* contiguous pages, or NULL if none */
void  pmm_free(void *addr, size_t pages);

uint64_t pmm_total_pages(void);           /* usable RAM, in pages */
uint64_t pmm_used_pages(void);

/* Limine maps all physical memory at a fixed offset ("higher-half direct map"),
   so physical address X can be reached at virtual address X + offset. */
uint64_t virt_to_phys(const void *v);
void    *phys_to_virt(uint64_t p);
