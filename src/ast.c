// ─── ast.c — Mol AST constructors ────────────────────────────────────────────
#include "ast.h"
#include <string.h>

// ─── FNV-1a 64-bit ────────────────────────────────────────────────────────────
uint64_t atom_hash(const char *name, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// ─── internal helpers ─────────────────────────────────────────────────────────

static Node *node_new(Arena *a, NodeKind kind, Loc loc) {
    Node *n = arena_calloc(a, sizeof(Node));
    n->kind = kind;
    n->loc  = loc;
    return n;
}

// Intern an atom name and compute its hash.
static Atom make_atom(Arena *a, const char *name, size_t len) {
    return (Atom){
        .hash = atom_hash(name, len),
        .name = arena_strndup(a, name, len),
    };
}

// Copy a Node* array into the arena.
static Node **copy_nodes(Arena *a, Node **src, int n) {
    if (!n) return NULL;
    Node **dst = arena_alloc(a, sizeof(Node *) * (size_t)n);
    memcpy(dst, src, sizeof(Node *) * (size_t)n);
    return dst;
}

// ─── atoms / literals ─────────────────────────────────────────────────────────

Node *ast_atom(Arena *a, Loc loc, const char *name, size_t len) {
    Node *n  = node_new(a, NODE_ATOM, loc);
    n->atom  = make_atom(a, name, len);
    return n;
}

Node *ast_ident(Arena *a, Loc loc, const char *name, size_t len) {
    Node *n  = node_new(a, NODE_IDENT, loc);
    n->ident = make_atom(a, name, len);
    return n;
}

Node *ast_intlit(Arena *a, Loc loc, uint64_t value, IntSuffix suffix) {
    Node *n        = node_new(a, NODE_INT_LIT, loc);
    n->intlit.value  = value;
    n->intlit.suffix = suffix;
    return n;
}

Node *ast_floatlit(Arena *a, Loc loc, double value) {
    Node *n      = node_new(a, NODE_FLOAT_LIT, loc);
    n->floatlit  = value;
    return n;
}

Node *ast_strlit(Arena *a, Loc loc, const char *data, size_t len) {
    Node *n         = node_new(a, NODE_STRING_LIT, loc);
    n->strlit.data  = arena_strndup(a, data, len);
    n->strlit.len   = len;
    return n;
}

Node *ast_arraylit(Arena *a, Loc loc, Node **elems, int nelems) {
    Node *n            = node_new(a, NODE_ARRAY_LIT, loc);
    n->arraylit.elems  = copy_nodes(a, elems, nelems);
    n->arraylit.nelems = nelems;
    return n;
}

Node *ast_mollit(Arena *a, Loc loc, Node **fields, int nfields) {
    Node *n           = node_new(a, NODE_MOL_LIT, loc);
    n->mollit.fields  = copy_nodes(a, fields, nfields);
    n->mollit.nfields = nfields;
    return n;
}

// ─── expressions ──────────────────────────────────────────────────────────────

Node *ast_field(Arena *a, Loc loc, Node *lhs, const char *name, size_t len) {
    Node *n       = node_new(a, NODE_FIELD, loc);
    n->field.lhs  = lhs;
    n->field.field = make_atom(a, name, len);
    return n;
}

Node *ast_index(Arena *a, Loc loc, Node *lhs, Node *idx) {
    Node *n       = node_new(a, NODE_INDEX, loc);
    n->index.lhs  = lhs;
    n->index.idx  = idx;
    return n;
}

Node *ast_call(Arena *a, Loc loc, Node *callee, Node **args, int nargs) {
    Node *n       = node_new(a, NODE_CALL, loc);
    n->call.callee = callee;
    n->call.args   = copy_nodes(a, args, nargs);
    n->call.nargs  = nargs;
    return n;
}

Node *ast_assign(Arena *a, Loc loc, Node *lhs, Node *rhs) {
    Node *n        = node_new(a, NODE_ASSIGN, loc);
    n->assign.lhs  = lhs;
    n->assign.rhs  = rhs;
    return n;
}

Node *ast_binop(Arena *a, Loc loc, BinOp op, Node *lhs, Node *rhs) {
    Node *n        = node_new(a, NODE_BINOP, loc);
    n->binop.op    = op;
    n->binop.lhs   = lhs;
    n->binop.rhs   = rhs;
    return n;
}

Node *ast_unop(Arena *a, Loc loc, UnOp op, Node *operand) {
    Node *n        = node_new(a, NODE_UNOP, loc);
    n->unop.op      = op;
    n->unop.operand = operand;
    return n;
}

Node *ast_deref(Arena *a, Loc loc, Node *operand) {
    Node *n    = node_new(a, NODE_DEREF, loc);
    n->operand = operand;
    return n;
}

Node *ast_addrof(Arena *a, Loc loc, Node *operand) {
    Node *n    = node_new(a, NODE_ADDROF, loc);
    n->operand = operand;
    return n;
}

// ─── statements ───────────────────────────────────────────────────────────────

Node *ast_if(Arena *a, Loc loc, Node *cond, Node *then, Node *els) {
    Node *n      = node_new(a, NODE_IF, loc);
    n->iff.cond  = cond;
    n->iff.then  = then;
    n->iff.els   = els;
    return n;
}

Node *ast_while(Arena *a, Loc loc, Node *cond, Node *body) {
    Node *n          = node_new(a, NODE_WHILE, loc);
    n->whilee.cond   = cond;
    n->whilee.body   = body;
    return n;
}

Node *ast_loop(Arena *a, Loc loc, Node *body) {
    Node *n       = node_new(a, NODE_LOOP, loc);
    n->loop_body  = body;
    return n;
}

Node *ast_for(Arena *a, Loc loc, Node *key, Node *val, Node *iter, Node *body) {
    Node *n       = node_new(a, NODE_FOR, loc);
    n->forr.key   = key;
    n->forr.val   = val;
    n->forr.iter  = iter;
    n->forr.body  = body;
    return n;
}

Node *ast_case(Arena *a, Loc loc, Node *subject, Node **arms, int narms) {
    Node *n          = node_new(a, NODE_CASE, loc);
    n->casee.subject = subject;
    n->casee.arms    = copy_nodes(a, arms, narms);
    n->casee.narms   = narms;
    return n;
}

Node *ast_case_arm(Arena *a, Loc loc, Node *pattern, Node *body) {
    Node *n       = node_new(a, NODE_CASE_ARM, loc);
    n->arm.pattern = pattern;   // NULL → wildcard _
    n->arm.body    = body;
    return n;
}

Node *ast_exit(Arena *a, Loc loc) {
    return node_new(a, NODE_EXIT, loc);
}

Node *ast_block(Arena *a, Loc loc, Node **stmts, int nstmts) {
    Node *n           = node_new(a, NODE_BLOCK, loc);
    n->block.stmts    = copy_nodes(a, stmts, nstmts);
    n->block.nstmts   = nstmts;
    return n;
}

// ─── declarations ─────────────────────────────────────────────────────────────

Node *ast_record_decl(Arena *a, Loc loc,
                      const char *name, size_t namelen,
                      Node **fields, int nfields,
                      int extensible, Node *case_block) {
    Node *n               = node_new(a, NODE_RECORD_DECL, loc);
    n->record.name        = make_atom(a, name, namelen);
    n->record.fields      = copy_nodes(a, fields, nfields);
    n->record.nfields     = nfields;
    n->record.extensible  = extensible;
    n->record.case_block  = case_block;
    return n;
}

Node *ast_proc_decl(Arena *a, Loc loc,
                    const char *name, size_t namelen,
                    Node *in_tuple, Node *out_tuple,
                    Node *body, int is_const) {
    Node *n             = node_new(a, NODE_PROC_DECL, loc);
    n->proc.name        = make_atom(a, name, namelen);
    n->proc.in_tuple    = in_tuple;
    n->proc.out_tuple   = out_tuple;
    n->proc.body        = body;
    n->proc.is_const    = is_const;
    return n;
}

Node *ast_var_decl(Arena *a, Loc loc,
                   const char *name, size_t namelen,
                   Node *type, Node *init, int is_let) {
    Node *n             = node_new(a, is_let ? NODE_LET_DECL : NODE_VAR_DECL, loc);
    n->vardecl.name     = make_atom(a, name, namelen);
    n->vardecl.type     = type;
    n->vardecl.init     = init;
    return n;
}

Node *ast_field_decl(Arena *a, Loc loc,
                     const char *name, size_t namelen, Node *type) {
    Node *n              = node_new(a, NODE_FIELD_DECL, loc);
    n->fielddecl.name    = make_atom(a, name, namelen);
    n->fielddecl.type    = type;
    return n;
}

// ─── type expressions ─────────────────────────────────────────────────────────

Node *ast_type_name(Arena *a, Loc loc, const char *name, size_t len) {
    Node *n      = node_new(a, NODE_TYPE_NAME, loc);
    n->type_name = make_atom(a, name, len);
    return n;
}

Node *ast_type_ptr(Arena *a, Loc loc, Node *inner) {
    Node *n       = node_new(a, NODE_TYPE_PTR, loc);
    n->ptr_inner  = inner;
    return n;
}

Node *ast_type_array(Arena *a, Loc loc, Node *size, Node *inner) {
    Node *n                  = node_new(a, NODE_TYPE_ARRAY, loc);
    n->type_array.size       = size;   // NULL → []T
    n->type_array.inner      = inner;
    return n;
}

Node *ast_type_tuple(Arena *a, Loc loc, Node **fields, int nfields) {
    Node *n                  = node_new(a, NODE_TYPE_TUPLE, loc);
    n->type_tuple.fields     = copy_nodes(a, fields, nfields);
    n->type_tuple.nfields    = nfields;
    return n;
}

Node *ast_type_const(Arena *a, Loc loc, Node *inner) {
    Node *n        = node_new(a, NODE_TYPE_CONST, loc);
    n->const_inner = inner;
    return n;
}
