#include <stdlib.h>
#include <stdio.h>
#include "region.h"

// ---------------------------------------------------------------------
// Pooling. OP_CALL/OP_RETURN create and destroy one Region (and its
// first block) per call, so under deep/hot recursion this was previously
// a malloc+malloc / free+free pair per call -- real syscall traffic once
// glibc routes 16KiB-class blocks through mmap/munmap. Both free lists
// below are simple LIFO singly-linked stacks reusing the struct's own
// `next`/`enclosing` field as the link, so pooling costs no extra memory
// per node. Bounded by peak concurrent call depth; never grows without
// bound the way live allocations could, since nothing is pooled until an
// equal regionDestroy() has already happened.
// ---------------------------------------------------------------------

static RegionBlock* blockPool = NULL;      // free list of standard-size (REGION_BLOCK_SIZE) blocks only
static Region* regionPool = NULL;          // free list of Region structs

static RegionBlock* allocateBlock(size_t minCapacity) {
    // Only pull from the pool for standard-size requests -- an oversized
    // single allocation (minCapacity > REGION_BLOCK_SIZE) always gets a
    // fresh, exactly-sized malloc, same as before pooling existed, so a
    // single huge allocation can't permanently inflate the pool's block
    // size for everyone after it.
    if (minCapacity <= REGION_BLOCK_SIZE && blockPool != NULL) {
        RegionBlock* block = blockPool;
        blockPool = blockPool->next;
        block->next = NULL;
        block->used = 0;
        // capacity is already REGION_BLOCK_SIZE from when this block was
        // first allocated; every pooled block is standard-size by
        // construction (see regionDestroy).
        return block;
    }

    size_t capacity = REGION_BLOCK_SIZE;
    if (capacity < minCapacity) capacity = minCapacity; // oversized single allocation
    RegionBlock* block = malloc(sizeof(RegionBlock) + capacity);
    if (block == NULL) {
        fprintf(stderr, "region: out of memory allocating %zu-byte block.\n", capacity);
        exit(70);
    }
    block->next = NULL;
    block->used = 0;
    block->capacity = capacity;
    return block;
}

Region* regionCreate(Region* enclosing) {
    Region* region;
    if (regionPool != NULL) {
        region = regionPool;
        regionPool = regionPool->enclosing;
    } else {
        region = malloc(sizeof(Region));
        if (region == NULL) {
            fprintf(stderr, "region: out of memory allocating Region.\n");
            exit(70);
        }
    }
    region->enclosing = enclosing;
    // Lazy block allocation: don't grab a block until something is
    // actually allocated into this region (see regionAlloc). A leaf-ish
    // function that returns without ever concatenating a string or
    // building a list -- e.g. pure arithmetic/recursion -- never touches
    // regionAlloc at all, so it now costs zero block allocations (pooled
    // or otherwise) instead of one guaranteed block per call.
    region->head = NULL;
    region->liveAllocations = 0;
    region->totalBytes = 0;
    return region;
}

// Round up to pointer alignment so bump-allocated structs (ObjString etc.)
// never end up on an unaligned boundary within the arena.
static size_t alignUp(size_t n) {
    const size_t align = sizeof(void*);
    return (n + (align - 1)) & ~(align - 1);
}

void* regionAlloc(Region* region, size_t size) {
    size_t aligned = alignUp(size);
    RegionBlock* block = region->head;
    if (block == NULL) {
        // First allocation this region has ever needed -- grab its
        // initial block now (see regionCreate's lazy-head comment).
        block = allocateBlock(aligned);
        region->head = block;
    } else if (block->used + aligned > block->capacity) {
        // Current block is full (or too small for this allocation); chain
        // a fresh block. The old block stays alive (still referenced by
        // ->next) until regionDestroy walks the whole chain.
        RegionBlock* fresh = allocateBlock(aligned);
        fresh->next = block;
        region->head = fresh;
        block = fresh;
    }
    void* ptr = block->data + block->used;
    block->used += aligned;
    region->liveAllocations++;
    region->totalBytes += aligned;
    return ptr;
}

void regionDestroy(Region* region) {
    if (region == NULL) return;
    // region->head may still be NULL here (lazy allocation, see
    // regionCreate/regionAlloc): a region that never had anything
    // allocated into it simply has no blocks to walk/pool.
    RegionBlock* block = region->head;
    while (block != NULL) {
        RegionBlock* next = block->next;
        if (block->capacity == REGION_BLOCK_SIZE) {
            // Standard-size block: return it to the pool instead of
            // freeing, so the next regionCreate()/regionAlloc() growth
            // in this or any other region can reuse it without touching
            // the system allocator.
            block->next = blockPool;
            blockPool = block;
        } else {
            // Oversized block (a single allocation bigger than
            // REGION_BLOCK_SIZE) -- pooling it would mean every future
            // pool hit hands out an oversized block for a standard-size
            // request, wasting the extra space forever. Just free it,
            // exactly as before pooling existed.
            free(block);
        }
        block = next;
    }
    // Region struct itself always goes back to the pool (fixed size,
    // no such thing as "oversized" here).
    region->enclosing = regionPool;
    regionPool = region;
}

void regionPoolDrain(void) {
    RegionBlock* block = blockPool;
    while (block != NULL) {
        RegionBlock* next = block->next;
        free(block);
        block = next;
    }
    blockPool = NULL;

    Region* region = regionPool;
    while (region != NULL) {
        Region* next = region->enclosing;
        free(region);
        region = next;
    }
    regionPool = NULL;
}

RegionCheckpoint regionCheckpoint(Region* region) {
    RegionCheckpoint cp;
    if (region == NULL || region->head == NULL) {
        cp.block = NULL;
        cp.used = 0;
        return cp;
    }
    cp.block = region->head;
    cp.used = region->head->used;
    return cp;
}

void regionRewind(Region* region, RegionCheckpoint checkpoint) {
    if (region == NULL || region->head == NULL) return;
    RegionBlock* block = region->head;
    if (checkpoint.block == NULL) {
        // Checkpoint was taken before this region had any block at all
        // (lazy allocation, see regionCreate) -- rewinding all the way
        // means freeing every block chained since, same walk as
        // regionDestroy but returning the Region struct itself to the
        // caller instead of pooling it.
        while (block != NULL) {
            RegionBlock* next = block->next;
            if (block->capacity == REGION_BLOCK_SIZE) {
                block->next = blockPool;
                blockPool = block;
            } else {
                free(block);
            }
            block = next;
        }
        region->head = NULL;
        region->liveAllocations = 0;
        region->totalBytes = 0;
        return;
    }
    // Free/pool every block allocated after the checkpoint's block (the
    // checkpoint's own block is kept -- just rewound to its recorded
    // `used` offset below).
    while (block != NULL && block != checkpoint.block) {
        RegionBlock* next = block->next;
        if (block->capacity == REGION_BLOCK_SIZE) {
            block->next = blockPool;
            blockPool = block;
        } else {
            free(block);
        }
        block = next;
    }
    // block now == checkpoint.block (it must still be reachable: the
    // checkpoint's block is never itself freed by regionAlloc, only
    // superseded as region->head by newer blocks chained in front of it).
    region->head = checkpoint.block;
    checkpoint.block->used = checkpoint.used;
    // liveAllocations/totalBytes are diagnostic-only counters (see
    // regionGetStats) -- not rewound precisely here since nothing reads
    // them mid-loop; left as-is rather than tracking exact deltas.
}

RegionStats regionGetStats(Region* region) {
    RegionStats stats;
    if (region == NULL) {
        stats.liveAllocations = 0;
        stats.totalBytes = 0;
        return stats;
    }
    stats.liveAllocations = region->liveAllocations;
    stats.totalBytes = region->totalBytes;
    return stats;
}
