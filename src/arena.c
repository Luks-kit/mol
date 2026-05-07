// ─── arena.c — compiler arena allocator ──────────────────────────────────────
#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ─── internal ─────────────────────────────────────────────────────────────────

static ArenaBlock *block_new(size_t cap) {
    ArenaBlock *b = malloc(sizeof(ArenaBlock) + cap);
    if (!b) {
        fputs("mol: arena out of memory\n", stderr);
        abort();
    }
    b->next = NULL;
    b->cap  = cap;
    b->used = 0;
    return b;
}

static inline size_t align8(size_t n) {
    return (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
}

// ─── public ───────────────────────────────────────────────────────────────────

void arena_init(Arena *a, size_t blksz) {
    a->blksz = blksz ? blksz : ARENA_DEFAULT_BLOCK;
    a->total  = 0;
    a->head   = block_new(a->blksz);
}

void *arena_alloc(Arena *a, size_t sz) {
    sz = align8(sz);
    if (!sz) sz = ARENA_ALIGN;  // never return a zero-size allocation

    // fast path — fits in current block
    if (a->head->used + sz <= a->head->cap) {
        void *p = a->head->data + a->head->used;
        a->head->used += sz;
        a->total      += sz;
        return p;
    }

    // slow path — need a new block
    // if the requested size exceeds the default block size, allocate a
    // dedicated oversized block for it rather than wasting a default block
    size_t newcap = (sz > a->blksz) ? sz : a->blksz;
    ArenaBlock *b = block_new(newcap);
    // prepend so head is always the block we try first
    b->next  = a->head;
    a->head  = b;
    b->used  = sz;
    a->total += sz;
    return b->data;
}

void *arena_calloc(Arena *a, size_t sz) {
    void *p = arena_alloc(a, sz);
    memset(p, 0, sz);
    return p;
}

char *arena_strdup(Arena *a, const char *s) {
    size_t len = strlen(s);
    return arena_strndup(a, s, len);
}

char *arena_strndup(Arena *a, const char *s, size_t len) {
    // +1 for null terminator; allocation is still 8-byte aligned internally
    char *p = arena_alloc(a, len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head  = NULL;
    a->total = 0;
}

ArenaMark arena_mark(Arena *a) {
    return (ArenaMark){ .block = a->head, .used = a->head->used };
}

void arena_restore(Arena *a, ArenaMark m) {
    // free any blocks allocated after the mark
    while (a->head != m.block) {
        ArenaBlock *dead = a->head;
        a->head = dead->next;
        free(dead);
    }
    a->head->used = m.used;
}
