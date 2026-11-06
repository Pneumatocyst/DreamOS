#pragma once
#include "kernel.h"

/* kmalloc/kfree: the kernel's malloc and free. Memory isn't zeroed unless you
   use kzalloc. kfree(NULL) is fine. kfree on anything else kmalloc didn't give
   you (or freeing twice) stops the system with a panic, so bugs can't hide. */
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

struct heap_stats { uint64_t live_allocs, live_bytes, total_allocs; };
void heap_stats(struct heap_stats *s);
