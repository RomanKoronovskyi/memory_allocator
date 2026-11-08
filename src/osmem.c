#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include "osmem.h"
#include "block_meta.h"

#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define META_SIZE (ALIGN(sizeof(struct block_meta)))
#define MMAP_THRESHOLD (128 * 1024)

#ifndef STATUS_FREE
#define STATUS_FREE   0
#define STATUS_ALLOC  1
#define STATUS_MAPPED 2
#endif

struct block_meta *head = NULL;
struct block_meta *tail = NULL;
int heap_preallocated = 0;

static void coalesce_all_free_blocks() {
    if (!head) return;
    struct block_meta *curr = head;
    while (curr) {
        if (curr->status == STATUS_FREE && curr->next && curr->next->status == STATUS_FREE) {
            curr->size += curr->next->size + META_SIZE;
            curr->next = curr->next->next;
            if (curr->next) {
                curr->next->prev = curr;
            } else {
                tail = curr;
            }
        } else {
            curr = curr->next;
        }
    }
}

static void split_block(struct block_meta *block, size_t size) {
    size_t aligned_size = ALIGN(size);
    if (block->size >= aligned_size + META_SIZE + 8) {
        struct block_meta *new_block = (struct block_meta *)((char *)block + META_SIZE + aligned_size);
        new_block->size = block->size - aligned_size - META_SIZE;
        new_block->status = STATUS_FREE;
        new_block->prev = block;
        new_block->next = block->next;
        
        if (block->next) {
            block->next->prev = new_block;
        } else {
            tail = new_block;
        }
        
        block->next = new_block;
        block->size = aligned_size;
    }
}

static struct block_meta *find_best_fit(size_t size) {
    coalesce_all_free_blocks();
    
    struct block_meta *curr = head;
    struct block_meta *best = NULL;
    size_t aligned_size = ALIGN(size);
    
    while (curr) {
        if (curr->status == STATUS_FREE && curr->size >= aligned_size) {
            if (!best || curr->size < best->size) {
                best = curr;
            }
        }
        curr = curr->next;
    }
    
    return best;
}

static struct block_meta *request_space_mmap(size_t size) {
    size_t aligned_size = ALIGN(size);
    size_t alloc_size = aligned_size + META_SIZE;
    void *ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return NULL;
    
    struct block_meta *block = (struct block_meta *)ptr;
    block->size = aligned_size;
    block->status = STATUS_MAPPED;
    block->prev = NULL;
    block->next = NULL;
    return block;
}

static struct block_meta *request_space_brk(size_t size) {
    size_t aligned_size = ALIGN(size);
    
    if (!heap_preallocated) {
        size_t alloc_size = MMAP_THRESHOLD;
        void *ptr = sbrk(alloc_size);
        if (ptr == (void *)-1) return NULL;
        
        struct block_meta *block = (struct block_meta *)ptr;
        block->size = alloc_size - META_SIZE;
        block->status = STATUS_ALLOC;
        block->prev = tail;
        block->next = NULL;
        
        if (tail) tail->next = block;
        if (!head) head = block;
        tail = block;
        
        heap_preallocated = 1;
        split_block(block, size);
        return block;
    }
    
    if (tail && tail->status == STATUS_FREE) {
        size_t expand_size = aligned_size - tail->size;
        void *ptr = sbrk(expand_size);
        if (ptr == (void *)-1) return NULL;
        
        tail->size = aligned_size;
        tail->status = STATUS_ALLOC;
        return tail;
    } else {
        size_t alloc_size = aligned_size + META_SIZE;
        void *ptr = sbrk(alloc_size);
        if (ptr == (void *)-1) return NULL;
        
        struct block_meta *block = (struct block_meta *)ptr;
        block->size = aligned_size;
        block->status = STATUS_ALLOC;
        block->prev = tail;
        block->next = NULL;
        
        if (tail) tail->next = block;
        if (!head) head = block;
        tail = block;
        
        return block;
    }
}

void *os_malloc(size_t size) {
    if (size == 0) return NULL;
    
    size_t aligned_size = ALIGN(size);
    
    if (aligned_size + META_SIZE >= MMAP_THRESHOLD) {
        struct block_meta *block = request_space_mmap(size);
        if (!block) return NULL;
        return (void *)((char *)block + META_SIZE);
    }
    
    struct block_meta *best_fit = find_best_fit(size);
    if (best_fit) {
        best_fit->status = STATUS_ALLOC;
        split_block(best_fit, size);
        return (void *)((char *)best_fit + META_SIZE);
    }
    
    struct block_meta *new_block = request_space_brk(size);
    if (!new_block) return NULL;
    return (void *)((char *)new_block + META_SIZE);
}

void *os_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;
    
    size_t total_size = nmemb * size;
    size_t aligned_size = ALIGN(total_size);
    size_t page_size = getpagesize();
    
    if (aligned_size + META_SIZE >= page_size) {
        struct block_meta *block = request_space_mmap(total_size);
        if (!block) return NULL;
        memset((char *)block + META_SIZE, 0, aligned_size);
        return (void *)((char *)block + META_SIZE);
    }
    
    struct block_meta *best_fit = find_best_fit(total_size);
    if (best_fit) {
        best_fit->status = STATUS_ALLOC;
        split_block(best_fit, total_size);
        memset((char *)best_fit + META_SIZE, 0, aligned_size);
        return (void *)((char *)best_fit + META_SIZE);
    }
    
    struct block_meta *new_block = request_space_brk(total_size);
    if (!new_block) return NULL;
    memset((char *)new_block + META_SIZE, 0, aligned_size);
    return (void *)((char *)new_block + META_SIZE);
}

void *os_realloc(void *ptr, size_t size) {
    if (!ptr) return os_malloc(size);
    if (size == 0) {
        os_free(ptr);
        return NULL;
    }
    
    struct block_meta *block = (struct block_meta *)((char *)ptr - META_SIZE);
    if (block->status == STATUS_FREE) return NULL;
    
    size_t aligned_size = ALIGN(size);
    
    if (aligned_size + META_SIZE >= MMAP_THRESHOLD && block->status != STATUS_MAPPED) {
        void *new_ptr = os_malloc(size);
        if (!new_ptr) return NULL;
        size_t copy_size = block->size < size ? block->size : size;
        memcpy(new_ptr, ptr, copy_size);
        os_free(ptr);
        return new_ptr;
    }
    
    if (block->status == STATUS_MAPPED) {
        void *new_ptr = os_malloc(size);
        if (!new_ptr) return NULL;
        size_t copy_size = block->size < size ? block->size : size;
        memcpy(new_ptr, ptr, copy_size);
        os_free(ptr);
        return new_ptr;
    }
    
    if (aligned_size == block->size) {
        return ptr;
    }
    
    if (aligned_size < block->size) {
        split_block(block, size);
        return ptr;
    }
    
    struct block_meta *curr = block->next;
    while (curr && curr->status == STATUS_FREE) {
        block->size += curr->size + META_SIZE;
        block->next = curr->next;
        if (curr->next) curr->next->prev = block;
        else tail = block;
        
        if (block->size >= aligned_size) {
            split_block(block, size);
            return ptr;
        }
        curr = block->next;
    }
    
    if (block == tail) {
        size_t expand_size = aligned_size - block->size;
        void *res = sbrk(expand_size);
        if (res == (void *)-1) {
            void *new_ptr = os_malloc(size);
            if (!new_ptr) return NULL;
            memcpy(new_ptr, ptr, block->size);
            os_free(ptr);
            return new_ptr;
        }
        block->size = aligned_size;
        return ptr;
    }
    
    void *new_ptr = os_malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);
    os_free(ptr);
    
    return new_ptr;
}

void os_free(void *ptr) {
    if (!ptr) return;
    
    struct block_meta *block = (struct block_meta *)((char *)ptr - META_SIZE);
    if (block->status == STATUS_MAPPED) {
        munmap(block, block->size + META_SIZE);
    } else {
        block->status = STATUS_FREE;
    }
}
