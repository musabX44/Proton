#ifndef PROTON_REGION_H
#define PROTON_REGION_H

#include "common.h"

// ---------------------------------------------------------------------
// LAM Faz 1: region-based memory for short-lived, non-interned heap data.
//
// A Region is a bump-pointer arena: allocate() just advances a pointer,
// destroy() resets it. No malloc/free per allocation, no per-object
// bookkeeping. When a block fills up, a new block is chained on (regions
// can grow), so a single long-running call frame producing lots of
// temporary strings doesn't blow a fixed-size buffer -- it just chains.
//
// Lifetime model (Faz 1): one Region per active CallFrame. The VM creates
// a Region when a function call begins and destroys it unconditionally
// when that call returns (see vm.c: OP_CALL / OP_RETURN). This is coarser
// than per-lexical-scope, but it's exactly as long as the VM already
// tracks call frame lifetime, so it requires no new escape analysis to
// be correct -- see README/design notes for why per-block granularity is
// deferred to a later phase.
//
// What lives in a Region (Faz 1): only heap allocations that are known to
// be non-interned and function-local, currently just the ObjString
// produced by runtime string concatenation and by number/bool-to-string
// coercion (toStringValue in vm.c). Interned string literals are NEVER
// placed in a Region -- they stay on the global/permanent heap exactly as
// before, because interning means sharing across scopes, which a region
// that gets torn down would turn into a dangling pointer. See
// object.c: copyString (interned, permanent) vs regionTakeString
// (region-scoped, not interned).
// ---------------------------------------------------------------------

#define REGION_BLOCK_SIZE (16 * 1024) // 16 KiB per block; chains as needed

typedef struct RegionBlock {
    struct RegionBlock* next; // previous block in this region (for destroy)
    size_t used;
    size_t capacity;
    unsigned char data[]; // flexible array member; block payload
} RegionBlock;

typedef struct Region {
    struct Region* enclosing;  // region stack: the caller's region
    RegionBlock* head;         // most recently allocated block (bump pointer lives here)
    size_t liveAllocations;    // diagnostic counter, see regionStats()
    size_t totalBytes;         // diagnostic counter, see regionStats()
} Region;

// Creates a new region and returns it. `enclosing` is the caller's region
// (NULL for the outermost/main frame) -- this is the link a value walks
// when it escapes its own region (see vm.c: promoteEscapingString). Does
// not touch any global state; caller (VM) is responsible for pushing/
// popping it onto whatever region stack it maintains.
Region* regionCreate(Region* enclosing);

// Bump-allocates `size` bytes from `region`, growing (chaining a new
// block) if the current block doesn't have room. Never returns NULL
// (aborts via allocation failure the same way malloc-based code already
// does elsewhere in this codebase, for consistency).
void* regionAlloc(Region* region, size_t size);

// Frees every block chained onto `region` and then the Region struct
// itself. After this call, every pointer previously returned by
// regionAlloc(region, ...) is invalid -- this is the "destroy" side of
// bump allocation: reset-by-freeing-the-whole-arena, not per-object free.
void regionDestroy(Region* region);

// Diagnostic-only: total live regions currently on the VM's region stack
// and cumulative bytes ever allocated across all regions (never
// decremented; useful for eyeballing whether LAM is actually keeping
// steady-state memory flat across loop iterations vs. the old
// never-freed-until-exit behavior).
typedef struct {
    size_t liveAllocations;
    size_t totalBytes;
} RegionStats;

RegionStats regionGetStats(Region* region);

// Optimization: pooling. regionCreate()/regionDestroy() are called on
// every OP_CALL/OP_RETURN, which was previously a raw malloc+free pair
// per call (two of each, for the Region struct and its first block).
// That's fine functionally but shows up as heavy syscall/allocator
// traffic under call-heavy workloads (e.g. deep recursion), since glibc
// routes large-enough allocations through mmap/munmap rather than the
// small-object arena.
//
// regionPoolDrain() releases every block currently sitting in the free
// list back to the system allocator. Purely an optional cleanup hook
// (e.g. at VM shutdown, or for diagnostics) -- never required for
// correctness, since the pool is bounded by peak concurrent call depth
// and would otherwise just get reused by the next regionCreate().
void regionPoolDrain(void);

// ---------------------------------------------------------------------
// Loop-scope checkpoint/rewind. OP_LOOP marks "one iteration of a while/
// for/continue just completed, jumping back to the top" -- the VM has no
// escape analysis (Faz 2, not implemented), so unlike promoteEscapingValue
// at OP_RETURN, there is no way to tell whether a value allocated during
// the iteration that just finished was assigned somewhere that survives
// past this loop (e.g. a variable declared before the loop and read
// after it). regionRewind() resets the bump pointer unconditionally --
// any such pointer becomes dangling. This is used to cut steady-state
// memory growth for the common case (temporaries fully consumed within
// the iteration), at the cost of that unsound edge case. Not a default
// safety net: call sites are responsible for this tradeoff.
// ---------------------------------------------------------------------
typedef struct {
    RegionBlock* block; // region->head at checkpoint time
    size_t used;         // block->used at checkpoint time
} RegionCheckpoint;

// Captures the current bump-pointer position. `region` may be NULL (no
// allocations happened yet in this frame) -- the checkpoint records that
// as block == NULL and is safe to pass back to regionRewind().
RegionCheckpoint regionCheckpoint(Region* region);

// Resets `region`'s bump pointer back to a previously captured
// checkpoint, freeing (returning to the pool, or actually freeing if
// oversized) every block chained on *after* the checkpoint's block, and
// resetting the checkpoint's own block back to its recorded `used`
// offset. Every pointer handed out by regionAlloc(region, ...) after the
// checkpoint was taken becomes invalid -- see the unsoundness note above.
void regionRewind(Region* region, RegionCheckpoint checkpoint);

#endif
