#pragma once
// ─── check.h — Mol type checker ──────────────────────────────────────────────
#include "arena.h"
#include "ast.h"

// ─── Type kinds ───────────────────────────────────────────────────────────────
typedef enum {
    TY_WORD,        // untyped word — bottom type, compatible with everything
    TY_INT,         // bits — signed/unsigned, various widths
    TY_FLOAT,       // float32 / float64
    TY_ATOM,        // atom
    TY_BOOL,        // nonzero/zero — not a distinct runtime type, checker only
    TY_PTR,         // T!
    TY_ARRAY,       // [N]T (fixed) or []T (dynamic)
    TY_RECORD,      // named fixed or extendible record
    TY_TUPLE,       // anonymous tuple — proc signature fragment
    TY_PROC,        // proc type: in_tuple -> out_tuple
    TY_CONST,       // compile-time value — wraps another type
    TY_TYPE,        // the type of a type (const type)
} TyKind;

// ─── Integer subtype ──────────────────────────────────────────────────────────
typedef enum {
    INT_KIND_INT,   // default signed word
    INT_KIND_UINT,
    INT_KIND_I8,  INT_KIND_U8,
    INT_KIND_I16, INT_KIND_U16,
    INT_KIND_I32, INT_KIND_U32,
    INT_KIND_I64, INT_KIND_U64,
    INT_KIND_CHAR,  // char — byte-width, unsigned
    INT_KIND_RUNE,  // rune — 32-bit unicode codepoint
} IntKind;

// ─── Type ─────────────────────────────────────────────────────────────────────
typedef struct Type Type;
typedef struct Field Field;
typedef struct RecordDef RecordDef;

struct Field {
    Atom  name;
    Type *type;
    int   offset;   // byte offset in fixed record; -1 for extendible fields
};

struct RecordDef {
    Atom      name;
    Field    *fields;
    int       nfields;
    int       extensible;
    Type     *case_field;   // the governing field for dependent variants, or NULL
    // variant table: case_field atom hash → Field* array + count
    // stored as parallel arrays for simplicity
    uint64_t *variant_tags;
    Field   **variant_fields;
    int      *variant_nfields;
    int       nvariants;
};

struct Type {
    TyKind kind;
    int    size;   // byte size, computed once by layout pass in checker
    union {
        // TY_INT
        IntKind int_kind;

        // TY_PTR
        Type *ptr_inner;

        // TY_ARRAY
        struct {
            Type    *inner;
            int      size;    // -1 for dynamic []T
        } array;

        // TY_RECORD
        RecordDef *record;

        // TY_TUPLE
        struct {
            Field *fields;
            int    nfields;
        } tuple;

        // TY_PROC
        struct {
            Type *in;    // TY_TUPLE
            Type *out;   // TY_TUPLE
        } proc;

        // TY_CONST
        Type *const_inner;
    };
};

// ─── Typed node — AST node annotated with its resolved type ──────────────────
// The type checker decorates the AST in a parallel arena-allocated array.
// We use a hash map keyed on Node* pointer.

// ─── Symbol ───────────────────────────────────────────────────────────────────
typedef enum {
    SYM_VAR,    // var binding
    SYM_LET,    // let binding (runtime read-only)
    SYM_CONST,  // const binding (compile-time)
    SYM_PROC,   // procedure
    SYM_RECORD, // record type
    SYM_FIELD,  // record field (looked up via record type, not scope)
} SymKind;

typedef struct Symbol Symbol;
struct Symbol {
    Atom     name;
    SymKind  kind;
    Type    *type;
    Symbol  *next;  // hash chain
};

// ─── Scope ────────────────────────────────────────────────────────────────────
#define SCOPE_BUCKETS 64

typedef struct Scope Scope;
struct Scope {
    Symbol  *buckets[SCOPE_BUCKETS];
    Scope   *parent;
};

// ─── Checker ──────────────────────────────────────────────────────────────────
typedef struct {
    Arena  *arena;
    Scope  *scope;          // current lexical scope
    Type   *current_out;    // return tuple type of the proc being checked
    int     in_const_proc;  // nonzero if inside a const proc
    int     in_loop;        // nonzero if inside loop/while/for (for exit)
} Checker;

// ─── Public API ───────────────────────────────────────────────────────────────

// Initialise checker with a fresh global scope.
void checker_init(Checker *c, Arena *arena);

// Type-check a complete file (NODE_BLOCK of top-level decls).
// Emits errors to stderr and exits on first error.
void check_file(Checker *c, Node *file);

// Resolve a type-expression node to a Type*.
// Used by codegen to get the type of declared fields/params without
// re-running full expression type checking.
Type *resolve_type(Checker *c, Node *n);

// Return the type of an expression node (does not mutate checker state).
Type *check_expr(Checker *c, Node *n);

// ─── Built-in type singletons (initialised by checker_init) ──────────────────
extern Type *ty_word;
extern Type *ty_int;
extern Type *ty_uint;
extern Type *ty_i8,  *ty_u8;
extern Type *ty_i16, *ty_u16;
extern Type *ty_i32, *ty_u32;
extern Type *ty_i64, *ty_u64;
extern Type *ty_float32;
extern Type *ty_float64;
extern Type *ty_atom;
extern Type *ty_bool;
extern Type *ty_string;
extern Type *ty_type;   // the type of a type atom
