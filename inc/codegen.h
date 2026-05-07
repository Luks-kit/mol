#pragma once
// ─── codegen.h — Mol → DVM assembly text emitter ─────────────────────────────
//
// Emits textual DVM assembly suitable for asm_compile().
// One proc → one labelled block. Locals live on the stack frame (ENTER/LEAVE).
// All values are word-sized (8 bytes). Floats use fp[] registers.
//
// Register conventions:
//   ax — primary result / scratch
//   bx — secondary operand
//   cx — tertiary / address scratch
//   dx — loop state (iter base)
//   fp0/fp1 — float operands; fp0 = float result
//
// Calling convention:
//   Caller pushes args left-to-right before CALL/CALLR.
//   Callee reads args from [bp+16], [bp+24], ... (above saved bp).
//   Return value in ax (or fp0 for floats).

#include "arena.h"
#include "ast.h"
#include "check.h"

// ─── Assembly text buffer ─────────────────────────────────────────────────────

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} AsmBuf;

// ─── Local variable ───────────────────────────────────────────────────────────

typedef struct Local Local;
struct Local {
    Atom    name;
    Type   *type;
    int     bp_offset;   // signed, negative (below bp)
    int     is_float;
    Local  *next;
};

// ─── Local scope ─────────────────────────────────────────────────────────────

typedef struct LocalScope LocalScope;
struct LocalScope {
    Local      *locals;
    LocalScope *parent;
    int         next_offset;  // next available bp offset (grows negative)
};

// ─── Loop context (for exit) ──────────────────────────────────────────────────

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
    int         label_counter;   // monotonic, for unique labels
    int         frame_size;      // current proc's frame size in bytes
    size_t      frame_patch_pos; // position of "enter N" size in out.buf to backpatch
} CGen;

// ─── Public API ───────────────────────────────────────────────────────────────

void cgen_init(CGen *g, Arena *arena);

// Emit assembly text for a complete type-checked file.
// Returns a NUL-terminated assembly string (valid for the arena's lifetime).
const char *cgen_file(CGen *g, Checker *c, Node *file);
