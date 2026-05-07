// ─── check.c — Mol type checker ──────────────────────────────────────────────
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ─── built-in type singletons ─────────────────────────────────────────────────

Type *ty_word;
Type *ty_int;
Type *ty_uint;
Type *ty_i8,  *ty_u8;
Type *ty_i16, *ty_u16;
Type *ty_i32, *ty_u32;
Type *ty_i64, *ty_u64;
Type *ty_float32;
Type *ty_float64;
Type *ty_atom;
Type *ty_bool;
Type *ty_string;
Type *ty_type;

// ─── type constructors ────────────────────────────────────────────────────────

static Type *ty_new(Arena *a, TyKind kind) {
    Type *t = arena_calloc(a, sizeof(Type));
    t->kind = kind;
    return t;
}

static Type *ty_int_new(Arena *a, IntKind k) {
    Type *t = ty_new(a, TY_INT);
    t->int_kind = k;
    return t;
}

static Type *ty_ptr_new(Arena *a, Type *inner) {
    Type *t = ty_new(a, TY_PTR);
    t->ptr_inner = inner;
    return t;
}

static Type *ty_array_new(Arena *a, Type *inner, int size) {
    Type *t = ty_new(a, TY_ARRAY);
    t->array.inner = inner;
    t->array.size  = size;
    return t;
}

static Type *ty_tuple_new(Arena *a, Field *fields, int nfields) {
    Type *t = ty_new(a, TY_TUPLE);
    t->tuple.fields  = fields;
    t->tuple.nfields = nfields;
    return t;
}

static Type *ty_proc_new(Arena *a, Type *in, Type *out) {
    Type *t = ty_new(a, TY_PROC);
    t->proc.in  = in;
    t->proc.out = out;
    return t;
}

static Type *ty_const_new(Arena *a, Type *inner) {
    Type *t = ty_new(a, TY_CONST);
    t->const_inner = inner;
    return t;
}

// ─── scope ────────────────────────────────────────────────────────────────────

static Scope *scope_new(Arena *a, Scope *parent) {
    Scope *s = arena_calloc(a, sizeof(Scope));
    s->parent = parent;
    return s;
}

static void scope_define(Arena *a, Scope *s, Atom name, SymKind kind, Type *type) {
    uint32_t idx = (uint32_t)(name.hash % SCOPE_BUCKETS);
    Symbol *sym  = arena_calloc(a, sizeof(Symbol));
    sym->name    = name;
    sym->kind    = kind;
    sym->type    = type;
    sym->next    = s->buckets[idx];
    s->buckets[idx] = sym;
}

static Symbol *scope_lookup(Scope *s, uint64_t hash) {
    uint32_t idx = (uint32_t)(hash % SCOPE_BUCKETS);
    for (Scope *cur = s; cur; cur = cur->parent) {
        for (Symbol *sym = cur->buckets[idx]; sym; sym = sym->next)
            if (sym->name.hash == hash) return sym;
    }
    return NULL;
}

// ─── error ────────────────────────────────────────────────────────────────────

static void check_error(Loc loc, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: error: ", loc.file, loc.line, loc.col);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#include <stdarg.h>

// ─── type compatibility ───────────────────────────────────────────────────────
// TY_WORD is compatible with everything (untyped word).
// Otherwise types must match structurally.

static int ty_compat(Type *expected, Type *got) {
    if (!expected || !got)       return 1;
    if (expected == got)         return 1;
    if (got->kind  == TY_WORD)   return 1;
    if (expected->kind == TY_WORD) return 1;
    if (expected->kind != got->kind) return 0;
    switch (expected->kind) {
        case TY_INT:   return expected->int_kind == got->int_kind;
        case TY_FLOAT: return 1; // float32/float64 — both are TY_FLOAT, coerce freely
        case TY_PTR:   return ty_compat(expected->ptr_inner, got->ptr_inner);
        case TY_ARRAY:
            return expected->array.size == got->array.size
                && ty_compat(expected->array.inner, got->array.inner);
        case TY_RECORD: return expected->record == got->record;
        case TY_ATOM:   return 1;
        case TY_BOOL:   return 1;
        case TY_TYPE:   return 1;
        case TY_CONST:  return ty_compat(expected->const_inner, got->const_inner);
        case TY_TUPLE:
            if (expected->tuple.nfields != got->tuple.nfields) return 0;
            for (int i = 0; i < expected->tuple.nfields; i++)
                if (!ty_compat(expected->tuple.fields[i].type,
                               got->tuple.fields[i].type)) return 0;
            return 1;
        case TY_PROC:
            return ty_compat(expected->proc.in,  got->proc.in)
                && ty_compat(expected->proc.out, got->proc.out);
        default: return 0;
    }
}

// ─── resolve a type-expression node to a Type* ───────────────────────────────

static Type *resolve_type(Checker *c, Node *n);

static Field *resolve_fields(Checker *c, Node **field_nodes, int n, int *out_n) {
    Field *fields = arena_alloc(c->arena, sizeof(Field) * (size_t)n);
    for (int i = 0; i < n; i++) {
        Node *fd = field_nodes[i];
        if (fd->kind != NODE_FIELD_DECL)
            check_error(fd->loc, "expected field declaration");
        fields[i].name   = fd->fielddecl.name;
        fields[i].type   = fd->fielddecl.type
                         ? resolve_type(c, fd->fielddecl.type)
                         : ty_word;
        fields[i].offset = i * 8;  // word-sized, packed
    }
    *out_n = n;
    return fields;
}

static Type *resolve_type(Checker *c, Node *n) {
    if (!n) return ty_word;
    switch (n->kind) {
        case NODE_TYPE_NAME: {
            uint64_t h = n->type_name.hash;
            // check built-ins by name hash
#define NAME_IS(s) (h == atom_hash(s, strlen(s)))
            if (NAME_IS("word"))    return ty_word;
            if (NAME_IS("int"))     return ty_int;
            if (NAME_IS("uint"))    return ty_uint;
            if (NAME_IS("i8"))      return ty_i8;
            if (NAME_IS("u8"))      return ty_u8;
            if (NAME_IS("i16"))     return ty_i16;
            if (NAME_IS("u16"))     return ty_u16;
            if (NAME_IS("i32"))     return ty_i32;
            if (NAME_IS("u32"))     return ty_u32;
            if (NAME_IS("i64"))     return ty_i64;
            if (NAME_IS("u64"))     return ty_u64;
            if (NAME_IS("float"))   return ty_float64;
            if (NAME_IS("float32")) return ty_float32;
            if (NAME_IS("float64")) return ty_float64;
            if (NAME_IS("atom"))    return ty_atom;
            if (NAME_IS("bool"))    return ty_bool;
            if (NAME_IS("string"))  return ty_string;
            if (NAME_IS("type"))    return ty_type;
#undef NAME_IS
            // user-defined record type
            Symbol *sym = scope_lookup(c->scope, h);
            if (!sym)
                check_error(n->loc, "unknown type '%s'", n->type_name.name);
            if (sym->kind != SYM_RECORD)
                check_error(n->loc, "'%s' is not a type", n->type_name.name);
            return sym->type;
        }
        case NODE_TYPE_PTR:
            return ty_ptr_new(c->arena, resolve_type(c, n->ptr_inner));
        case NODE_TYPE_ARRAY: {
            int size = -1;
            if (n->type_array.size) {
                if (n->type_array.size->kind != NODE_INT_LIT)
                    check_error(n->loc, "array size must be a constant integer");
                size = (int)n->type_array.size->intlit.value;
            }
            return ty_array_new(c->arena,
                                resolve_type(c, n->type_array.inner), size);
        }
        case NODE_TYPE_TUPLE: {
            int nf;
            Field *fields = resolve_fields(c,
                n->type_tuple.fields, n->type_tuple.nfields, &nf);
            return ty_tuple_new(c->arena, fields, nf);
        }
        case NODE_TYPE_CONST:
            return ty_const_new(c->arena, resolve_type(c, n->const_inner));
        default:
            check_error(n->loc, "expected type expression");
            return NULL;
    }
}

// ─── forward declarations ─────────────────────────────────────────────────────

static void  check_stmt(Checker *c, Node *n);
static void  check_block(Checker *c, Node *n);

// ─── register built-in types in the global scope ─────────────────────────────

static void register_builtins(Checker *c) {
#define DEF_BUILTIN(name_str, type_ptr) do { \
    Atom a = { atom_hash(name_str, strlen(name_str)), name_str }; \
    scope_define(c->arena, c->scope, a, SYM_RECORD, (type_ptr)); \
} while(0)
    // primitive types are resolved by hash in resolve_type so we don't
    // strictly need to register them in scope, but registering lets user
    // code reference them as identifiers in expressions.
    DEF_BUILTIN("word",    ty_word);
    DEF_BUILTIN("int",     ty_int);
    DEF_BUILTIN("uint",    ty_uint);
    DEF_BUILTIN("float",   ty_float64);
    DEF_BUILTIN("float32", ty_float32);
    DEF_BUILTIN("atom",    ty_atom);
    DEF_BUILTIN("string",  ty_string);
#undef DEF_BUILTIN
}

// ─── checker_init ─────────────────────────────────────────────────────────────

void checker_init(Checker *c, Arena *arena) {
    memset(c, 0, sizeof(*c));
    c->arena = arena;
    c->scope = scope_new(arena, NULL);

    // allocate built-in type singletons into the compiler arena
    ty_word    = ty_new(arena, TY_WORD);
    ty_int     = ty_int_new(arena, INT_KIND_INT);
    ty_uint    = ty_int_new(arena, INT_KIND_UINT);
    ty_i8      = ty_int_new(arena, INT_KIND_I8);
    ty_u8      = ty_int_new(arena, INT_KIND_U8);
    ty_i16     = ty_int_new(arena, INT_KIND_I16);
    ty_u16     = ty_int_new(arena, INT_KIND_U16);
    ty_i32     = ty_int_new(arena, INT_KIND_I32);
    ty_u32     = ty_int_new(arena, INT_KIND_U32);
    ty_i64     = ty_int_new(arena, INT_KIND_I64);
    ty_u64     = ty_int_new(arena, INT_KIND_U64);
    ty_float32 = ty_new(arena, TY_FLOAT);
    ty_float64 = ty_new(arena, TY_FLOAT);
    ty_atom    = ty_new(arena, TY_ATOM);
    ty_bool    = ty_new(arena, TY_BOOL);
    ty_type    = ty_new(arena, TY_TYPE);

    // string is a built-in fixed record: (length uint, data u8!)
    RecordDef *strdef = arena_calloc(arena, sizeof(RecordDef));
    strdef->name      = (Atom){ atom_hash("string", 6), "string" };
    strdef->nfields   = 2;
    strdef->fields    = arena_alloc(arena, sizeof(Field) * 2);
    strdef->fields[0] = (Field){ {atom_hash("length",6),"length"}, ty_uint,  0 };
    strdef->fields[1] = (Field){ {atom_hash("data",  4),"data"},
                                  ty_ptr_new(arena, ty_u8), 8 };
    ty_string         = ty_new(arena, TY_RECORD);
    ty_string->record = strdef;

    register_builtins(c);
}

// ─── first pass: register all top-level names ─────────────────────────────────
// Mol is multi-pass — names may be used before declaration.

static void register_toplevel(Checker *c, Node *n) {
    switch (n->kind) {
        case NODE_RECORD_DECL: {
            // build the RecordDef
            RecordDef *def = arena_calloc(c->arena, sizeof(RecordDef));
            def->name       = n->record.name;
            def->extensible = n->record.extensible;
            int nf;
            def->fields  = resolve_fields(c,
                n->record.fields, n->record.nfields, &nf);
            def->nfields = nf;

            // process dependent case block if present
            if (n->record.case_block) {
                Node *cb = n->record.case_block;
                // cb is NODE_CASE; subject is the governing field expr
                def->nvariants      = cb->casee.narms;
                def->variant_tags   = arena_alloc(c->arena,
                    sizeof(uint64_t) * (size_t)cb->casee.narms);
                def->variant_fields = arena_alloc(c->arena,
                    sizeof(Field*)  * (size_t)cb->casee.narms);
                def->variant_nfields = arena_alloc(c->arena,
                    sizeof(int)     * (size_t)cb->casee.narms);

                for (int i = 0; i < cb->casee.narms; i++) {
                    Node *arm = cb->casee.arms[i];
                    // pattern is NODE_ATOM (or NULL for wildcard)
                    def->variant_tags[i] = arm->arm.pattern
                        ? arm->arm.pattern->atom.hash
                        : 0;
                    // body is a NODE_TYPE_TUPLE of fields
                    Node *body = arm->arm.body;
                    int vnf;
                    def->variant_fields[i]  = resolve_fields(c,
                        body->type_tuple.fields, body->type_tuple.nfields, &vnf);
                    def->variant_nfields[i] = vnf;
                }
            }

            Type *rec_type   = ty_new(c->arena, TY_RECORD);
            rec_type->record = def;
            scope_define(c->arena, c->scope, n->record.name, SYM_RECORD, rec_type);
            break;
        }
        case NODE_PROC_DECL: {
            Type *in  = n->proc.in_tuple  ? resolve_type(c, n->proc.in_tuple)  : ty_tuple_new(c->arena, NULL, 0);
            Type *out = n->proc.out_tuple ? resolve_type(c, n->proc.out_tuple) : ty_tuple_new(c->arena, NULL, 0);
            Type *pt  = ty_proc_new(c->arena, in, out);
            if (n->proc.is_const)
                pt = ty_const_new(c->arena, pt);
            scope_define(c->arena, c->scope,
                         n->proc.name,
                         n->proc.is_const ? SYM_CONST : SYM_PROC,
                         pt);
            break;
        }
        case NODE_VAR_DECL:
        case NODE_LET_DECL: {
            Type *t = n->vardecl.type
                    ? resolve_type(c, n->vardecl.type)
                    : ty_word;
            scope_define(c->arena, c->scope, n->vardecl.name,
                         n->kind == NODE_LET_DECL ? SYM_LET : SYM_VAR, t);
            break;
        }
        default: break;
    }
}

// ─── expression type checker ──────────────────────────────────────────────────

Type *check_expr(Checker *c, Node *n) {
    switch (n->kind) {

        case NODE_INT_LIT: {
            switch (n->intlit.suffix) {
                case INT_UNSUFFIXED: return ty_int;
                case INT_U:          return ty_uint;
                case INT_I8:         return ty_i8;
                case INT_U8:         return ty_u8;
                case INT_I16:        return ty_i16;
                case INT_U16:        return ty_u16;
                case INT_I32:        return ty_i32;
                case INT_U32:        return ty_u32;
                case INT_I64:        return ty_i64;
                case INT_U64:        return ty_u64;
                default:             return ty_int;
            }
        }

        case NODE_FLOAT_LIT:  return ty_float64;
        case NODE_STRING_LIT: return ty_string;
        case NODE_ATOM:       return ty_atom;

        case NODE_IDENT: {
            Symbol *sym = scope_lookup(c->scope, n->ident.hash);
            if (!sym)
                check_error(n->loc, "undefined name '%s'", n->ident.name);
            return sym->type;
        }

        case NODE_ARRAY_LIT: {
            Type *elem = ty_word;
            for (int i = 0; i < n->arraylit.nelems; i++) {
                Type *t = check_expr(c, n->arraylit.elems[i]);
                if (i == 0) elem = t;
                else if (!ty_compat(elem, t))
                    check_error(n->loc, "array literal element type mismatch");
            }
            return ty_array_new(c->arena, elem, n->arraylit.nelems);
        }

        case NODE_MOL_LIT: {
            // positional or named — return TY_TUPLE with inferred field types
            Field *fields = arena_alloc(c->arena,
                sizeof(Field) * (size_t)n->mollit.nfields);
            for (int i = 0; i < n->mollit.nfields; i++) {
                Node *f = n->mollit.fields[i];
                if (f->kind == NODE_FIELD) {
                    // named: ast_field(arena, loc, val, name, len)
                    fields[i].name   = f->field.field;
                    fields[i].type   = check_expr(c, f->field.lhs);
                    fields[i].offset = i * 8;
                } else {
                    fields[i].name   = (Atom){0, NULL};
                    fields[i].type   = check_expr(c, f);
                    fields[i].offset = i * 8;
                }
            }
            return ty_tuple_new(c->arena, fields, n->mollit.nfields);
        }

        case NODE_FIELD: {
            Type *lhs_ty = check_expr(c, n->field.lhs);
            uint64_t fhash = n->field.field.hash;

            // unwrap pointer for field access on pointer-to-record
            if (lhs_ty->kind == TY_PTR) lhs_ty = lhs_ty->ptr_inner;

            if (lhs_ty->kind == TY_RECORD) {
                RecordDef *def = lhs_ty->record;
                // search fixed fields
                for (int i = 0; i < def->nfields; i++)
                    if (def->fields[i].name.hash == fhash)
                        return def->fields[i].type;
                // search variant fields across all variants
                for (int v = 0; v < def->nvariants; v++)
                    for (int i = 0; i < def->variant_nfields[v]; i++)
                        if (def->variant_fields[v][i].name.hash == fhash)
                            return def->variant_fields[v][i].type;
                // extendible records allow unknown fields — return word
                if (def->extensible) return ty_word;
                check_error(n->loc, "record '%s' has no field '%s'",
                            def->name.name, n->field.field.name);
            }

            // TY_WORD / TY_TUPLE — untyped field access, return word
            if (lhs_ty->kind == TY_WORD || lhs_ty->kind == TY_TUPLE)
                return ty_word;

            // const type field access: s:T where T is a const type field
            if (lhs_ty->kind == TY_CONST)
                return ty_type;

            check_error(n->loc, "field access on non-record type");
            return ty_word;
        }

        case NODE_INDEX: {
            Type *lhs_ty = check_expr(c, n->index.lhs);
            Type *idx_ty = check_expr(c, n->index.idx);
            (void)idx_ty;  // must be integral — checked loosely via word compat
            if (lhs_ty->kind == TY_ARRAY)  return lhs_ty->array.inner;
            if (lhs_ty->kind == TY_PTR)    return lhs_ty->ptr_inner;
            if (lhs_ty->kind == TY_WORD)   return ty_word;
            check_error(n->loc, "index on non-array type");
            return ty_word;
        }

        case NODE_CALL: {
            Type *callee_ty = check_expr(c, n->call.callee);
            // unwrap const wrapper for const proc calls
            if (callee_ty->kind == TY_CONST) callee_ty = callee_ty->const_inner;
            if (callee_ty->kind != TY_PROC)
                check_error(n->loc, "call to non-procedure");
            Type *in = callee_ty->proc.in;
            // check arg count
            if (in->tuple.nfields != n->call.nargs)
                check_error(n->loc,
                    "procedure expects %d arguments, got %d",
                    in->tuple.nfields, n->call.nargs);
            // check arg types
            for (int i = 0; i < n->call.nargs; i++) {
                Type *arg_ty = check_expr(c, n->call.args[i]);
                if (!ty_compat(in->tuple.fields[i].type, arg_ty))
                    check_error(n->call.args[i]->loc,
                        "argument %d type mismatch", i + 1);
            }
            // return type: single field → unwrap, multi → tuple, none → word
            Type *out = callee_ty->proc.out;
            if (out->tuple.nfields == 0) return ty_word;
            if (out->tuple.nfields == 1) return out->tuple.fields[0].type;
            return out;
        }

        case NODE_ASSIGN: {
            Type *lhs_ty = check_expr(c, n->assign.lhs);
            Type *rhs_ty = check_expr(c, n->assign.rhs);
            // let bindings are read-only at runtime
            if (n->assign.lhs->kind == NODE_IDENT) {
                Symbol *sym = scope_lookup(c->scope,
                                           n->assign.lhs->ident.hash);
                if (sym && sym->kind == SYM_LET)
                    check_error(n->loc, "cannot assign to let binding '%s'",
                                n->assign.lhs->ident.name);
            }
            if (!ty_compat(lhs_ty, rhs_ty))
                check_error(n->loc, "assignment type mismatch");
            return lhs_ty;
        }

        case NODE_BINOP: {
            Type *lty = check_expr(c, n->binop.lhs);
            Type *rty = check_expr(c, n->binop.rhs);
            // comparison operators always yield bool (nonzero/zero word)
            switch (n->binop.op) {
                case BOP_EQ: case BOP_NEQ:
                case BOP_LT: case BOP_LTE:
                case BOP_GT: case BOP_GTE:
                    return ty_bool;
                default: break;
            }
            // float arithmetic
            if (lty->kind == TY_FLOAT || rty->kind == TY_FLOAT)
                return ty_float64;
            // otherwise propagate left type
            if (!ty_compat(lty, rty))
                check_error(n->loc, "binary operator type mismatch");
            return lty;
        }

        case NODE_UNOP: {
            Type *t = check_expr(c, n->unop.operand);
            return t;
        }

        case NODE_DEREF: {
            Type *t = check_expr(c, n->operand);
            if (t->kind == TY_PTR)  return t->ptr_inner;
            if (t->kind == TY_WORD) return ty_word;
            check_error(n->loc, "dereference of non-pointer");
            return ty_word;
        }

        case NODE_ADDROF: {
            Type *t = check_expr(c, n->operand);
            return ty_ptr_new(c->arena, t);
        }

        default:
            check_error(n->loc, "unexpected node in expression");
            return ty_word;
    }
}

// ─── statement checker ────────────────────────────────────────────────────────

static void check_stmt(Checker *c, Node *n) {
    switch (n->kind) {

        case NODE_VAR_DECL:
        case NODE_LET_DECL: {
            Type *decl_ty = n->vardecl.type
                          ? resolve_type(c, n->vardecl.type)
                          : ty_word;
            if (n->vardecl.init) {
                Type *init_ty = check_expr(c, n->vardecl.init);
                if (n->vardecl.type && !ty_compat(decl_ty, init_ty))
                    check_error(n->loc,
                        "initialiser type does not match declaration");
                if (!n->vardecl.type) decl_ty = init_ty;
            }
            scope_define(c->arena, c->scope, n->vardecl.name,
                         n->kind == NODE_LET_DECL ? SYM_LET : SYM_VAR,
                         decl_ty);
            break;
        }

        case NODE_IF: {
            check_expr(c, n->iff.cond);  // any nonzero value is truthy
            Scope *saved = c->scope;
            c->scope = scope_new(c->arena, saved);
            check_block(c, n->iff.then);
            c->scope = saved;
            if (n->iff.els) {
                c->scope = scope_new(c->arena, saved);
                check_block(c, n->iff.els);
                c->scope = saved;
            }
            break;
        }

        case NODE_WHILE: {
            check_expr(c, n->whilee.cond);
            Scope *saved = c->scope;
            c->scope = scope_new(c->arena, saved);
            int saved_loop = c->in_loop;
            c->in_loop = 1;
            check_block(c, n->whilee.body);
            c->in_loop = saved_loop;
            c->scope = saved;
            break;
        }

        case NODE_LOOP: {
            Scope *saved = c->scope;
            c->scope = scope_new(c->arena, saved);
            int saved_loop = c->in_loop;
            c->in_loop = 1;
            check_block(c, n->loop_body);
            c->in_loop = saved_loop;
            c->scope = saved;
            break;
        }

        case NODE_FOR: {
            Type *iter_ty = check_expr(c, n->forr.iter);
            Scope *saved  = c->scope;
            c->scope = scope_new(c->arena, saved);
            int saved_loop = c->in_loop;
            c->in_loop = 1;

            // bind loop variables
            Type *val_ty = ty_word;
            Type *key_ty = ty_word;
            if (iter_ty->kind == TY_ARRAY) {
                val_ty = iter_ty->array.inner;
                key_ty = ty_uint;
            } else if (iter_ty->kind == TY_RECORD && iter_ty->record->extensible) {
                // extendible record: yields (atom, word) pairs
                val_ty = ty_word;
                key_ty = ty_atom;
            }

            if (n->forr.key)
                scope_define(c->arena, c->scope, n->forr.key->ident, SYM_LET, key_ty);
            scope_define(c->arena, c->scope, n->forr.val->ident, SYM_LET, val_ty);

            check_block(c, n->forr.body);
            c->in_loop = saved_loop;
            c->scope   = saved;
            break;
        }

        case NODE_CASE: {
            Type *subj_ty = check_expr(c, n->casee.subject);
            for (int i = 0; i < n->casee.narms; i++) {
                Node *arm = n->casee.arms[i];
                Scope *saved = c->scope;
                c->scope = scope_new(c->arena, saved);

                // if the subject is a dependent record and the pattern is
                // a known variant, bind that variant's fields into scope
                if (arm->arm.pattern
                    && subj_ty->kind == TY_RECORD
                    && arm->arm.pattern->kind == NODE_ATOM) {
                    RecordDef *def = subj_ty->record;
                    uint64_t tag   = arm->arm.pattern->atom.hash;
                    for (int v = 0; v < def->nvariants; v++) {
                        if (def->variant_tags[v] == tag) {
                            for (int f = 0; f < def->variant_nfields[v]; f++) {
                                Field *fl = &def->variant_fields[v][f];
                                scope_define(c->arena, c->scope,
                                             fl->name, SYM_LET, fl->type);
                            }
                            break;
                        }
                    }
                }

                if (arm->arm.body->kind == NODE_BLOCK)
                    check_block(c, arm->arm.body);
                else
                    check_stmt(c, arm->arm.body);
                c->scope = saved;
            }
            break;
        }

        case NODE_EXIT:
            if (!c->in_loop)
                check_error(n->loc, "'exit' used outside of loop");
            break;

        case NODE_BLOCK:
            check_block(c, n);
            break;

        default:
            // expression statement
            check_expr(c, n);
            break;
    }
}

static void check_block(Checker *c, Node *n) {
    if (!n) return;
    if (n->kind != NODE_BLOCK) { check_stmt(c, n); return; }
    for (int i = 0; i < n->block.nstmts; i++)
        check_stmt(c, n->block.stmts[i]);
}

// ─── proc body checker ────────────────────────────────────────────────────────

static void check_proc(Checker *c, Node *n) {
    if (!n->proc.body) return;  // forward declaration

    Scope *saved     = c->scope;
    Type  *saved_out = c->current_out;
    int    saved_const = c->in_const_proc;

    c->scope        = scope_new(c->arena, saved);
    c->in_const_proc = n->proc.is_const;

    // bind input parameters into scope
    Node *in_tup = n->proc.in_tuple;
    if (in_tup && in_tup->kind == NODE_TYPE_TUPLE) {
        for (int i = 0; i < in_tup->type_tuple.nfields; i++) {
            Node *fd = in_tup->type_tuple.fields[i];
            Type *ft = fd->fielddecl.type
                     ? resolve_type(c, fd->fielddecl.type)
                     : ty_word;
            scope_define(c->arena, c->scope,
                         fd->fielddecl.name, SYM_LET, ft);
        }
    }

    // bind output slots into scope as var (writable)
    Node *out_tup = n->proc.out_tuple;
    if (out_tup && out_tup->kind == NODE_TYPE_TUPLE) {
        for (int i = 0; i < out_tup->type_tuple.nfields; i++) {
            Node *fd = out_tup->type_tuple.fields[i];
            Type *ft = fd->fielddecl.type
                     ? resolve_type(c, fd->fielddecl.type)
                     : ty_word;
            scope_define(c->arena, c->scope,
                         fd->fielddecl.name, SYM_VAR, ft);
            // record the output type for return checking
            c->current_out = ft;
        }
    }

    check_block(c, n->proc.body);

    c->scope         = saved;
    c->current_out   = saved_out;
    c->in_const_proc = saved_const;
}

// ─── check_file ───────────────────────────────────────────────────────────────

void check_file(Checker *c, Node *file) {
    if (file->kind != NODE_BLOCK)
        check_error(file->loc, "expected top-level block");

    // first pass: register all top-level names so forward references work
    for (int i = 0; i < file->block.nstmts; i++)
        register_toplevel(c, file->block.stmts[i]);

    // second pass: check bodies
    for (int i = 0; i < file->block.nstmts; i++) {
        Node *n = file->block.stmts[i];
        switch (n->kind) {
            case NODE_PROC_DECL:
                check_proc(c, n);
                break;
            case NODE_VAR_DECL:
            case NODE_LET_DECL:
                check_stmt(c, n);
                break;
            case NODE_RECORD_DECL:
                break;  // fully handled in register_toplevel
            default:
                check_error(n->loc, "unexpected node at top level");
        }
    }
}
