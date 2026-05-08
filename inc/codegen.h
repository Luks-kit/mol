#pragma once
// ─── codegen.h — Mol → DVM assembly text emitter ─────────────────────────────
#include "arena.h"
#include "ast.h"
#include "check.h"

// ─── Assembly text buffer ─────────────────────────────────────────────────────

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} AsmBuf;

// ─── Deferred string literal ──────────────────────────────────────────────────

typedef struct StrLit StrLit;
struct StrLit {
    char     label[64];   // label emitted in .cnst section
    char    *data;        // raw bytes (arena-allocated, not NUL-terminated)
    size_t   len;
    StrLit  *next;
};

// ─── Local variable ───────────────────────────────────────────────────────────

typedef struct Local Local;
struct Local {
    Atom    name;
    Type   *type;
    int     bp_offset;
    int     is_float;
    Local  *next;
};

// ─── Local scope ─────────────────────────────────────────────────────────────

typedef struct LocalScope LocalScope;
struct LocalScope {
    Local      *locals;
    LocalScope *parent;
    int         next_offset;
};

// ─── Loop context ─────────────────────────────────────────────────────────────

typedef struct LoopCtx LoopCtx;
struct LoopCtx {
    char     exit_label[64];
    char     top_label[64];
    LoopCtx *parent;
};

// ─── Codegen context ─────────────────────────────────────────────────────────

typedef struct {
    Arena      *arena;
    AsmBuf      out;
    LocalScope *scope;
    LoopCtx    *loop;
    int         label_counter;
    int         frame_size;
    char        cur_proc[64];   // name of the proc being compiled — for label scoping
    StrLit     *strlits;        // linked list of deferred string literals
} CGen;

// ─── Public API ───────────────────────────────────────────────────────────────

void cgen_init(CGen *g, Arena *arena);
const char *cgen_file(CGen *g, Checker *c, Node *file);
