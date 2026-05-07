#pragma once
// ─── ast.h — Mol abstract syntax tree ────────────────────────────────────────
#include "arena.h"
#include <stdint.h>

// ─── Source location ──────────────────────────────────────────────────────────
typedef struct {
    const char *file;
    int         line;
    int         col;
} Loc;

// ─── Atom ─────────────────────────────────────────────────────────────────────
// An atom is its hash. The name is kept for error messages and the string table.
typedef struct {
    uint64_t    hash;
    const char *name;  // interned in the compiler arena
} Atom;

// FNV-1a 64-bit hash of a name string.
uint64_t atom_hash(const char *name, size_t len);

// ─── Integer literal suffix ───────────────────────────────────────────────────
typedef enum {
    INT_UNSUFFIXED = 0,
    INT_U,           // u
    INT_I8,  INT_U8,
    INT_I16, INT_U16,
    INT_I32, INT_U32,
    INT_I64, INT_U64,
} IntSuffix;

// ─── Binary operators ─────────────────────────────────────────────────────────
typedef enum {
    BOP_ADD, BOP_SUB, BOP_MUL, BOP_DIV, BOP_MOD,
    BOP_AND, BOP_OR,  BOP_XOR,
    BOP_SHL, BOP_SHR, BOP_SAR,
    BOP_EQ,  BOP_NEQ,
    BOP_LT,  BOP_LTE,
    BOP_GT,  BOP_GTE,
    BOP_LAND, BOP_LOR,  // logical and/or (short-circuit)
} BinOp;

// ─── Unary operators ──────────────────────────────────────────────────────────
typedef enum {
    UOP_NEG,   // -x
    UOP_NOT,   // ~x  (bitwise / logical)
} UnOp;

// ─── Node kinds ───────────────────────────────────────────────────────────────
typedef enum {
    // declarations
    NODE_RECORD_DECL,   // name:(fields)  /  name+:(fields)
    NODE_PROC_DECL,     // name:(in) -> (out) do ... end  [const flag]
    NODE_VAR_DECL,      // var name type  [:= expr]
    NODE_LET_DECL,      // let name type  [:= expr]
    // expressions
    NODE_ATOM,          // :foo
    NODE_INT_LIT,       // 42, 5u, 3i8 ...
    NODE_FLOAT_LIT,     // 3.14
    NODE_STRING_LIT,    // "hello"
    NODE_ARRAY_LIT,     // [a, b, c]
    NODE_MOL_LIT,       // {:x 4.0, :y 2.0}  /  {4.0, 2.0}
    NODE_IDENT,         // foo
    NODE_FIELD,         // p:x
    NODE_INDEX,         // a[i]
    NODE_CALL,          // f(a, b)
    NODE_ASSIGN,        // x := y
    NODE_BINOP,         // x + y
    NODE_UNOP,          // -x
    NODE_DEREF,         // p!  (load from pointer)
    NODE_ADDROF,        // @v  (take address)
    // statements
    NODE_IF,            // if cond do ... [else do ...] end
    NODE_WHILE,         // while cond do ... end
    NODE_LOOP,          // loop do ... end
    NODE_FOR,           // for [k,] v in expr do ... end
    NODE_CASE,          // case expr do arms end  (control flow + record variant)
    NODE_CASE_ARM,      // pattern => body  /  _ => body
    NODE_EXIT,          // exit
    NODE_BLOCK,         // sequence of nodes
    // type expressions
    NODE_TYPE_NAME,     // int, float, atom, point ...
    NODE_TYPE_PTR,      // T!
    NODE_TYPE_ARRAY,    // [N]T  (N == NULL → dynamic []T)
    NODE_TYPE_TUPLE,    // (x T, y T)  — proc signature fragment
    NODE_TYPE_CONST,    // const T
    // field declaration (used inside record/tuple nodes)
    NODE_FIELD_DECL,    // name type  — a single typed field
} NodeKind;

// ─── Node ─────────────────────────────────────────────────────────────────────
typedef struct Node Node;

struct Node {
    NodeKind kind;
    Loc      loc;

    union {
        // ── NODE_ATOM ─────────────────────────────────────────────────────────
        Atom atom;

        // ── NODE_INT_LIT ──────────────────────────────────────────────────────
        struct {
            uint64_t  value;
            IntSuffix suffix;
        } intlit;

        // ── NODE_FLOAT_LIT ────────────────────────────────────────────────────
        double floatlit;

        // ── NODE_STRING_LIT ───────────────────────────────────────────────────
        struct {
            const char *data;
            size_t      len;
        } strlit;

        // ── NODE_ARRAY_LIT ────────────────────────────────────────────────────
        struct {
            Node  **elems;
            int     nelems;
        } arraylit;

        // ── NODE_MOL_LIT ──────────────────────────────────────────────────────
        // Each entry is a NODE_FIELD_DECL (named) or bare expr (positional).
        struct {
            Node  **fields;
            int     nfields;
        } mollit;

        // ── NODE_IDENT ────────────────────────────────────────────────────────
        Atom ident;

        // ── NODE_FIELD — p:x ──────────────────────────────────────────────────
        struct {
            Node *lhs;
            Atom  field;
        } field;

        // ── NODE_INDEX — a[i] ─────────────────────────────────────────────────
        struct {
            Node *lhs;
            Node *idx;
        } index;

        // ── NODE_CALL — f(args) ───────────────────────────────────────────────
        struct {
            Node  *callee;
            Node **args;
            int    nargs;
        } call;

        // ── NODE_ASSIGN — lhs := rhs ──────────────────────────────────────────
        struct {
            Node *lhs;
            Node *rhs;
        } assign;

        // ── NODE_BINOP ────────────────────────────────────────────────────────
        struct {
            BinOp op;
            Node *lhs;
            Node *rhs;
        } binop;

        // ── NODE_UNOP ─────────────────────────────────────────────────────────
        struct {
            UnOp  op;
            Node *operand;
        } unop;

        // ── NODE_DEREF / NODE_ADDROF — single operand ─────────────────────────
        Node *operand;

        // ── NODE_IF ───────────────────────────────────────────────────────────
        struct {
            Node *cond;
            Node *then;
            Node *els;   // NULL if no else branch
        } iff;

        // ── NODE_WHILE ────────────────────────────────────────────────────────
        struct {
            Node *cond;
            Node *body;
        } whilee;

        // ── NODE_LOOP ─────────────────────────────────────────────────────────
        Node *loop_body;

        // ── NODE_FOR ──────────────────────────────────────────────────────────
        struct {
            Node *key;    // binding for key/index — NULL if omitted
            Node *val;    // binding for value
            Node *iter;   // the expression being iterated
            Node *body;
        } forr;

        // ── NODE_CASE ─────────────────────────────────────────────────────────
        struct {
            Node  *subject;
            Node **arms;
            int    narms;
        } casee;

        // ── NODE_CASE_ARM ─────────────────────────────────────────────────────
        struct {
            Node *pattern;  // NODE_ATOM, NODE_MOL_LIT, or NULL for wildcard _
            Node *body;
        } arm;

        // ── NODE_BLOCK ────────────────────────────────────────────────────────
        struct {
            Node **stmts;
            int    nstmts;
        } block;

        // ── NODE_RECORD_DECL ──────────────────────────────────────────────────
        struct {
            Atom    name;
            Node  **fields;     // NODE_FIELD_DECL nodes
            int     nfields;
            int     extensible; // 1 if name+
            Node   *case_block; // NODE_CASE if dependent molecule, else NULL
        } record;

        // ── NODE_PROC_DECL ────────────────────────────────────────────────────
        struct {
            Atom  name;
            Node *in_tuple;    // NODE_TYPE_TUPLE
            Node *out_tuple;   // NODE_TYPE_TUPLE
            Node *body;        // NODE_BLOCK — NULL for forward decls
            int   is_const;
        } proc;

        // ── NODE_VAR_DECL / NODE_LET_DECL ────────────────────────────────────
        struct {
            Atom  name;
            Node *type;    // type expression node — NULL if inferred
            Node *init;    // initialiser expression — NULL if absent
        } vardecl;

        // ── NODE_FIELD_DECL ───────────────────────────────────────────────────
        struct {
            Atom  name;
            Node *type;    // type expression node
        } fielddecl;

        // ── NODE_TYPE_NAME ────────────────────────────────────────────────────
        Atom type_name;

        // ── NODE_TYPE_PTR ─────────────────────────────────────────────────────
        Node *ptr_inner;

        // ── NODE_TYPE_ARRAY ───────────────────────────────────────────────────
        struct {
            Node *size;   // NODE_INT_LIT — NULL for dynamic []T
            Node *inner;
        } type_array;

        // ── NODE_TYPE_TUPLE ───────────────────────────────────────────────────
        struct {
            Node **fields;  // NODE_FIELD_DECL nodes
            int    nfields;
        } type_tuple;

        // ── NODE_TYPE_CONST ───────────────────────────────────────────────────
        Node *const_inner;
    };
};

// ─── Constructor declarations ─────────────────────────────────────────────────

// atoms / literals
Node *ast_atom    (Arena *a, Loc loc, const char *name, size_t len);
Node *ast_ident   (Arena *a, Loc loc, const char *name, size_t len);
Node *ast_intlit  (Arena *a, Loc loc, uint64_t value, IntSuffix suffix);
Node *ast_floatlit(Arena *a, Loc loc, double value);
Node *ast_strlit  (Arena *a, Loc loc, const char *data, size_t len);
Node *ast_arraylit(Arena *a, Loc loc, Node **elems, int nelems);
Node *ast_mollit  (Arena *a, Loc loc, Node **fields, int nfields);

// expressions
Node *ast_field (Arena *a, Loc loc, Node *lhs, const char *name, size_t len);
Node *ast_index (Arena *a, Loc loc, Node *lhs, Node *idx);
Node *ast_call  (Arena *a, Loc loc, Node *callee, Node **args, int nargs);
Node *ast_assign(Arena *a, Loc loc, Node *lhs, Node *rhs);
Node *ast_binop (Arena *a, Loc loc, BinOp op, Node *lhs, Node *rhs);
Node *ast_unop  (Arena *a, Loc loc, UnOp op, Node *operand);
Node *ast_deref (Arena *a, Loc loc, Node *operand);
Node *ast_addrof(Arena *a, Loc loc, Node *operand);

// statements
Node *ast_if      (Arena *a, Loc loc, Node *cond, Node *then, Node *els);
Node *ast_while   (Arena *a, Loc loc, Node *cond, Node *body);
Node *ast_loop    (Arena *a, Loc loc, Node *body);
Node *ast_for     (Arena *a, Loc loc, Node *key, Node *val, Node *iter, Node *body);
Node *ast_case    (Arena *a, Loc loc, Node *subject, Node **arms, int narms);
Node *ast_case_arm(Arena *a, Loc loc, Node *pattern, Node *body);
Node *ast_exit    (Arena *a, Loc loc);
Node *ast_block   (Arena *a, Loc loc, Node **stmts, int nstmts);

// declarations
Node *ast_record_decl(Arena *a, Loc loc,
                      const char *name, size_t namelen,
                      Node **fields, int nfields,
                      int extensible, Node *case_block);
Node *ast_proc_decl  (Arena *a, Loc loc,
                      const char *name, size_t namelen,
                      Node *in_tuple, Node *out_tuple,
                      Node *body, int is_const);
Node *ast_var_decl   (Arena *a, Loc loc,
                      const char *name, size_t namelen,
                      Node *type, Node *init, int is_let);
Node *ast_field_decl (Arena *a, Loc loc,
                      const char *name, size_t namelen, Node *type);

// type expressions
Node *ast_type_name (Arena *a, Loc loc, const char *name, size_t len);
Node *ast_type_ptr  (Arena *a, Loc loc, Node *inner);
Node *ast_type_array(Arena *a, Loc loc, Node *size, Node *inner);
Node *ast_type_tuple(Arena *a, Loc loc, Node **fields, int nfields);
Node *ast_type_const(Arena *a, Loc loc, Node *inner);
