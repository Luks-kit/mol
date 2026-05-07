#pragma once
// ─── arena.h — compiler arena allocator ──────────────────────────────────────
//
// A simple chain-of-blocks arena. All allocations are bump-pointer within the
// current block. When a block is exhausted a new one is chained. The entire
// arena is freed in one call — no individual frees, ever.
//
// Alignment: all allocations are 8-byte aligned.
// Thread safety: none — one arena per compiler context, single-threaded use.
//
// Usage:
//   Arena a;
//   arena_init(&a, 64 * 1024);
//   Node *n = arena_alloc(&a, sizeof(Node));
//   arena_free(&a);

#include <stddef.h>
#include <stdint.h>

#define ARENA_DEFAULT_BLOCK (64 * 1024)  // 64 KB default block size
#define ARENA_ALIGN         8

typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock {
    ArenaBlock *next;
    size_t      cap;
    size_t      used;
    uint8_t     data[];   // flexible array — block storage follows header
};

typedef struct {
    ArenaBlock *head;     // current block (most recently allocated)
    size_t      blksz;    // default block size for new blocks
    size_t      total;    // total bytes allocated across all blocks (debug)
} Arena;

// Initialise arena. blksz is the default block size; pass 0 for the default.
void  arena_init (Arena *a, size_t blksz);

// Allocate sz bytes, 8-byte aligned. Never returns NULL — aborts on OOM.
void *arena_alloc(Arena *a, size_t sz);

// Allocate sz bytes, zero-initialised.
void *arena_calloc(Arena *a, size_t sz);

// Duplicate a string into the arena.
char *arena_strdup(Arena *a, const char *s);

// Duplicate a string of known length into the arena (no strlen).
char *arena_strndup(Arena *a, const char *s, size_t len);

// Free all blocks. Arena is left in an uninitialised state.
void  arena_free (Arena *a);

// Save/restore arena position for temporary scratch allocations.
// Only safe if no block boundaries have been crossed between save and restore.
typedef struct { ArenaBlock *block; size_t used; } ArenaMark;
ArenaMark arena_mark   (Arena *a);
void      arena_restore(Arena *a, ArenaMark m);
