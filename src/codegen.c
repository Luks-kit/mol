// ─── codegen.c — Mol → DVM assembly text emitter ─────────────────────────────
//
// Register discipline:
//   ax  — primary result, call return value
//   bx  — value operand lhs / element scratch
//   cx  — value operand rhs
//   dx  — loop iter base (preserved across loop body)
//   di  — lval/address scratch (NEVER holds a value word)
//   si  — secondary address scratch (assign lhs addr)
//   ex  — float spill address scratch
//   fx  — spare
//   fp0 — float primary / result
//   fp1 — float secondary operand
//
// lval always goes into di or si. value expressions never write di/si.
// This means cgen_lval(n, "di") then cgen_expr(rhs, "ax") is always safe.

#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ─── AsmBuf ───────────────────────────────────────────────────────────────────

static void abuf_init(AsmBuf *b) {
    b->cap = 65536;
    b->buf = malloc(b->cap);
    b->len = 0;
    b->buf[0] = '\0';
}

static void abuf_grow(AsmBuf *b, size_t need) {
    while (b->len + need + 1 > b->cap) {
        b->cap *= 2;
        b->buf  = realloc(b->buf, b->cap);
        if (!b->buf) { fputs("mol: codegen OOM\n", stderr); abort(); }
    }
}

static void emit(CGen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    abuf_grow(&g->out, (size_t)needed + 1);
    va_start(ap, fmt);
    vsnprintf(g->out.buf + g->out.len, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    g->out.len += (size_t)needed;
}

// ─── label generation ─────────────────────────────────────────────────────────

static void fresh_label(CGen *g, const char *prefix, char *out, size_t outsz) {
    snprintf(out, outsz, "%s_%s%d",
             g->cur_proc[0] ? g->cur_proc : "top",
             prefix, g->label_counter++);
}

// ─── string literal table ─────────────────────────────────────────────────────
// Strings are collected during code emission and flushed to .cnst at end.
// The parser stores the string as raw source bytes (escape sequences already
// interpreted by parse_string / lex_scan), so we emit raw bytes here.

static const char *strlit_register(CGen *g, const char *data, size_t len) {
    StrLit *s  = arena_alloc(g->arena, sizeof(StrLit));
    snprintf(s->label, sizeof(s->label), "str_%d", g->label_counter++);
    s->data    = arena_strndup(g->arena, data, len);
    s->len     = len;
    s->next    = g->strlits;
    g->strlits = s;
    return s->label;
}

static void emit_strlits(CGen *g) {
    if (!g->strlits) return;
    int count = 0;
    for (StrLit *s = g->strlits; s; s = s->next) count++;
    StrLit **arr = arena_alloc(g->arena, sizeof(StrLit *) * (size_t)count);
    int i = count - 1;
    for (StrLit *s = g->strlits; s; s = s->next) arr[i--] = s;

    emit(g, "\n.cnst\n");
    for (int j = 0; j < count; j++) {
        StrLit *s = arr[j];
        emit(g, "global %s\n%s:\n", s->label, s->label);
        emit(g, "  dq %zu\n", s->len);   // length prefix
        emit(g, "  ds \"%.*s\"\n", s->len, s->data);
    }
}

// ─── scope management ────────────────────────────────────────────────────────

static void scope_push(CGen *g) {
    LocalScope *s  = arena_alloc(g->arena, sizeof(LocalScope));
    s->locals      = NULL;
    s->parent      = g->scope;
    s->next_offset = g->scope ? g->scope->next_offset : 0;
    g->scope       = s;
}

static void scope_pop(CGen *g) {
    g->scope = g->scope->parent;
}

static Local *local_define(CGen *g, Atom name, Type *type, int is_float) {
    Local *l     = arena_alloc(g->arena, sizeof(Local));
    l->name      = name;
    l->type      = type;
    l->is_float  = is_float;
    g->scope->next_offset -= 8;
    l->bp_offset = g->scope->next_offset;
    if (-l->bp_offset > g->frame_size)
        g->frame_size = -l->bp_offset;
    l->next           = g->scope->locals;
    g->scope->locals  = l;
    return l;
}

static Local *local_lookup(CGen *g, uint64_t hash) {
    for (LocalScope *s = g->scope; s; s = s->parent)
        for (Local *l = s->locals; l; l = l->next)
            if (l->name.hash == hash) return l;
    return NULL;
}

// ─── load / store ─────────────────────────────────────────────────────────────
// Float locals: spill address goes into ex (never a value register).

static void load_local(CGen *g, Local *l, const char *dst) {
    if (l->is_float) {
        emit(g, "  lea ex, [bp%+d]\n", l->bp_offset);
        emit(g, "  fmovm %s, [ex]\n", dst);
    } else {
        emit(g, "  mov %s, [bp%+d]\n", dst, l->bp_offset);
    }
}

static void store_local(CGen *g, Local *l, const char *src) {
    if (l->is_float) {
        emit(g, "  lea ex, [bp%+d]\n", l->bp_offset);
        emit(g, "  fmovs [ex], %s\n", src);
    } else {
        emit(g, "  mov [bp%+d], %s\n", l->bp_offset, src);
    }
}

// ─── type helpers ─────────────────────────────────────────────────────────────

static int type_is_float(Type *t)  { return t && t->kind == TY_FLOAT; }

static int type_is_signed(Type *t) {
    if (!t || t->kind != TY_INT) return 1;
    switch (t->int_kind) {
        case INT_KIND_UINT: case INT_KIND_U8:  case INT_KIND_U16:
        case INT_KIND_U32:  case INT_KIND_U64: return 0;
        default: return 1;
    }
}

// ─── forward declarations ─────────────────────────────────────────────────────

static void cgen_expr (CGen *g, Checker *c, Node *n, const char *dst);
static void cgen_expr_f(CGen *g, Checker *c, Node *n, const char *fdst);
static void cgen_lval (CGen *g, Checker *c, Node *n, const char *ar);
static void cgen_stmt (CGen *g, Checker *c, Node *n);
static void cgen_block(CGen *g, Checker *c, Node *n);

// ─── lvalue address — always into an address register (di or si) ──────────────

static void cgen_lval(CGen *g, Checker *c, Node *n, const char *ar) {
    switch (n->kind) {
        case NODE_IDENT: {
            Local *l = local_lookup(g, n->ident.hash);
            if (!l) { fprintf(stderr, "codegen: undefined local '%s'\n", n->ident.name); abort(); }
            emit(g, "  lea %s, [bp%+d]\n", ar, l->bp_offset);
            break;
        }
        case NODE_DEREF:
            // lval of p! is just the pointer value
            cgen_expr(g, c, n->operand, ar);
            break;
        case NODE_INDEX:
            // ar = base_addr + idx*8
            cgen_lval(g, c, n->index.lhs, ar);
            cgen_expr(g, c, n->index.idx, "bx");
            emit(g, "  mov cx, 8\n");
            emit(g, "  mul bx, cx\n");
            emit(g, "  add %s, bx\n", ar);
            break;
        case NODE_FIELD: {
            Type *base_ty = n->field.lhs->type;
            if (!base_ty) base_ty = ty_word;
            if (base_ty->kind == TY_PTR) {
                cgen_expr(g, c, n->field.lhs, ar);
                base_ty = base_ty->ptr_inner;
            } else {
                cgen_lval(g, c, n->field.lhs, ar);
            }
            if (base_ty->kind == TY_RECORD) {
                RecordDef *def   = base_ty->record;
                uint64_t   fhash = n->field.field.hash;
                for (int i = 0; i < def->nfields; i++)
                    if (def->fields[i].name.hash == fhash) {
                        int off = def->fields[i].offset;
                        if (off) emit(g, "  lea %s, [%s+%d]\n", ar, ar, off);
                        return;
                    }
                for (int v = 0; v < def->nvariants; v++)
                    for (int i = 0; i < def->variant_nfields[v]; i++)
                        if (def->variant_fields[v][i].name.hash == fhash) {
                            int off = def->variant_fields[v][i].offset;
                            if (off) emit(g, "  lea %s, [%s+%d]\n", ar, ar, off);
                            return;
                        }
                if (def->extensible)
                    emit(g, "  ; TODO ext field 0x%llx '%s'\n",
                         (unsigned long long)fhash, n->field.field.name);
            }
            break;
        }
        default:
            fprintf(stderr, "codegen: not an lvalue (kind=%d)\n", n->kind);
            abort();
    }
}

// ─── expression codegen ───────────────────────────────────────────────────────

static void cgen_expr(CGen *g, Checker *c, Node *n, const char *dst) {
    switch (n->kind) {

        case NODE_INT_LIT:
            emit(g, "  mov %s, %llu\n", dst, (unsigned long long)n->intlit.value);
            break;

        case NODE_FLOAT_LIT:
            // spill fp0 through ex (address reg — safe)
            emit(g, "  fmovi fp0, %g\n", n->floatlit);
            emit(g, "  lea ex, [bp-8]\n");
            emit(g, "  fmovs [ex], fp0\n");
            emit(g, "  mov %s, [ex]\n", dst);
            break;

        case NODE_ATOM:
            emit(g, "  mov %s, 0x%llx  ; :%s\n",
                 dst, (unsigned long long)n->atom.hash, n->atom.name);
            break;

        case NODE_STRING_LIT: {
            const char *lbl = strlit_register(g, n->strlit.data, n->strlit.len);
            emit(g, "  mov %s, %s\n", dst, lbl);
            break;
        }

        case NODE_IDENT: {
            Local *l = local_lookup(g, n->ident.hash);
            if (l) load_local(g, l, dst);
            else   emit(g, "  mov %s, %s\n", dst, n->ident.name);
            break;
        }

        case NODE_FIELD:
        case NODE_INDEX:
            // address into di, load value from it
            cgen_lval(g, c, n, "di");
            emit(g, "  mov %s, [di]\n", dst);
            break;

        case NODE_DEREF:
            cgen_expr(g, c, n->operand, dst);
            emit(g, "  mov %s, [%s]\n", dst, dst);
            break;

        case NODE_ADDROF:
            // address into di, move to dst
            cgen_lval(g, c, n->operand, "di");
            emit(g, "  mov %s, di\n", dst);
            break;

        case NODE_BINOP: {
            Type *lty      = n->binop.lhs->type ? n->binop.lhs->type : ty_word;
            int   is_float  = type_is_float(lty);
            int   is_signed = type_is_signed(lty);

            if (is_float) {
                cgen_expr_f(g, c, n->binop.lhs, "fp0");
                cgen_expr_f(g, c, n->binop.rhs, "fp1");
                int is_cmp = 0;
                switch (n->binop.op) {
                    case BOP_ADD: emit(g, "  fadd fp0, fp1\n"); break;
                    case BOP_SUB: emit(g, "  fsub fp0, fp1\n"); break;
                    case BOP_MUL: emit(g, "  fmul fp0, fp1\n"); break;
                    case BOP_DIV: emit(g, "  fdiv fp0, fp1\n"); break;
                    default: emit(g, "  fcmp fp0, fp1\n"); is_cmp = 1; break;
                }
                if (!is_cmp) {
                    // float result → dst via ex spill
                    emit(g, "  lea ex, [bp-8]\n");
                    emit(g, "  fmovs [ex], fp0\n");
                    emit(g, "  mov %s, [ex]\n", dst);
                    break;
                }
                // fall through to comparison result emission below
                goto emit_cmp;
            }

            cgen_expr(g, c, n->binop.lhs, "bx");
            cgen_expr(g, c, n->binop.rhs, "cx");

            switch (n->binop.op) {
                case BOP_ADD: emit(g, "  add bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_SUB: emit(g, "  sub bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_MUL: emit(g, "  %s bx, cx\n  mov %s, bx\n", is_signed?"imul":"mul",  dst); break;
                case BOP_DIV: emit(g, "  %s bx, cx\n  mov %s, bx\n", is_signed?"idiv":"div",  dst); break;
                case BOP_MOD: emit(g, "  mod bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_AND: emit(g, "  and bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_OR:  emit(g, "  or  bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_XOR: emit(g, "  xor bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_SHL: emit(g, "  shl bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_SHR: emit(g, "  shr bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_SAR: emit(g, "  sar bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_EQ: case BOP_NEQ:
                case BOP_LT: case BOP_LTE:
                case BOP_GT: case BOP_GTE:
                    emit(g, "  cmp bx, cx\n");
                emit_cmp: {
                    char t_lbl[64], end_lbl[64];
                    fresh_label(g, "ct", t_lbl,   sizeof(t_lbl));
                    fresh_label(g, "cf", end_lbl,  sizeof(end_lbl));
                    const char *jop;
                    switch (n->binop.op) {
                        case BOP_EQ:  jop = "je";  break;
                        case BOP_NEQ: jop = "jne"; break;
                        case BOP_LT:  jop = "jl";  break;
                        case BOP_LTE: jop = "jle"; break;
                        case BOP_GT:  jop = "jg";  break;
                        case BOP_GTE: jop = "jge"; break;
                        default:      jop = "je";  break;
                    }
                    emit(g, "  %s %s\n", jop, t_lbl);
                    emit(g, "  mov %s, 0\n  jmp %s\n", dst, end_lbl);
                    emit(g, "%s:\n  mov %s, 1\n", t_lbl, dst);
                    emit(g, "%s:\n", end_lbl);
                    break;
                }
                default:
                    fprintf(stderr, "codegen: unhandled binop %d\n", n->binop.op);
                    abort();
            }
            break;
        }

        case NODE_UNOP:
            cgen_expr(g, c, n->unop.operand, dst);
            switch (n->unop.op) {
                case UOP_NEG: emit(g, "  neg %s\n", dst); break;
                case UOP_NOT: emit(g, "  not %s\n", dst); break;
            }
            break;

        case NODE_CALL:
            // args pushed right-to-left so first arg is on top after all pushes
            for (int i = n->call.nargs - 1; i >= 0; i--) {
                cgen_expr(g, c, n->call.args[i], "ax");
                emit(g, "  push ax\n");
            }
            if (n->call.callee->kind == NODE_IDENT) {
                Local *l = local_lookup(g, n->call.callee->ident.hash);
                if (l) {
                    load_local(g, l, "ax");
                    emit(g, "  callr ax, mol_err_stub\n");
                } else {
                    emit(g, "  call %s, mol_err_stub\n", n->call.callee->ident.name);
                }
            } else {
                cgen_expr(g, c, n->call.callee, "ax");
                emit(g, "  callr ax, mol_err_stub\n");
            }
            if (strcmp(dst, "ax")) emit(g, "  mov %s, ax\n", dst);
            break;

        case NODE_ASSIGN:
            // rhs into ax, lhs address into si (different from di so nested fields are safe)
            cgen_expr(g, c, n->assign.rhs, "ax");
            cgen_lval(g, c, n->assign.lhs, "si");
            emit(g, "  mov [si], ax\n");
            if (strcmp(dst, "ax")) emit(g, "  mov %s, ax\n", dst);
            break;

        case NODE_ARRAY_LIT: {
            // allocate on stack, base in di
            int ne = n->arraylit.nelems;
            emit(g, "  mov ax, %d\n", ne * 8);
            emit(g, "  alloca\n");
            emit(g, "  mov di, ax\n");
            for (int i = 0; i < ne; i++) {
                cgen_expr(g, c, n->arraylit.elems[i], "bx");
                emit(g, "  mov [di+%d], bx\n", i * 8);
            }
            if (strcmp(dst, "di")) emit(g, "  mov %s, di\n", dst);
            break;
        }

        case NODE_MOL_LIT: {
            int nf = n->mollit.nfields;
            emit(g, "  mov ax, %d\n", nf * 8);
            emit(g, "  alloca\n");
            emit(g, "  mov di, ax\n");
            for (int i = 0; i < nf; i++) {
                Node *f   = n->mollit.fields[i];
                Node *val = (f->kind == NODE_FIELD) ? f->field.lhs : f;
                cgen_expr(g, c, val, "bx");
                emit(g, "  mov [di+%d], bx\n", i * 8);
            }
            if (strcmp(dst, "di")) emit(g, "  mov %s, di\n", dst);
            break;
        }

        default:
            fprintf(stderr, "codegen: unhandled expr node kind=%d\n", n->kind);
            abort();
    }
}

// ─── float expression — result in fp[fdst] ────────────────────────────────────

static void cgen_expr_f(CGen *g, Checker *c, Node *n, const char *fdst) {
    switch (n->kind) {
        case NODE_FLOAT_LIT:
            emit(g, "  fmovi %s, %g\n", fdst, n->floatlit);
            break;
        case NODE_IDENT: {
            Local *l = local_lookup(g, n->ident.hash);
            if (l && l->is_float) {
                emit(g, "  lea ex, [bp%+d]\n", l->bp_offset);
                emit(g, "  fmovm %s, [ex]\n", fdst);
            } else {
                cgen_expr(g, c, n, "ax");
                emit(g, "  itof %s, ax\n", fdst);
            }
            break;
        }
        default:
            cgen_expr(g, c, n, "ax");
            emit(g, "  itof %s, ax\n", fdst);
            break;
    }
}

// ─── statements ───────────────────────────────────────────────────────────────

static void cgen_stmt(CGen *g, Checker *c, Node *n) {
    switch (n->kind) {

        case NODE_VAR_DECL:
        case NODE_LET_DECL: {
            Type *ty = n->type ? n->type
                     : (n->vardecl.init && n->vardecl.init->type)
                       ? n->vardecl.init->type : ty_word;
            int is_float = type_is_float(ty);
            Local *l = local_define(g, n->vardecl.name, ty, is_float);
            if (n->vardecl.init) {
                if (is_float) {
                    cgen_expr_f(g, c, n->vardecl.init, "fp0");
                    store_local(g, l, "fp0");
                } else {
                    cgen_expr(g, c, n->vardecl.init, "ax");
                    store_local(g, l, "ax");
                }
            }
            break;
        }

        case NODE_ASSIGN:
            cgen_expr(g, c, n, "ax");
            break;

        case NODE_IF: {
            char else_lbl[64], end_lbl[64];
            fresh_label(g, "else", else_lbl, sizeof(else_lbl));
            fresh_label(g, "fi",   end_lbl,  sizeof(end_lbl));

            cgen_expr(g, c, n->iff.cond, "ax");
            emit(g, "  mov bx, 0\n  cmp ax, bx\n");
            emit(g, "  je %s\n", n->iff.els ? else_lbl : end_lbl);

            scope_push(g); cgen_block(g, c, n->iff.then); scope_pop(g);

            if (n->iff.els) {
                emit(g, "  jmp %s\n%s:\n", end_lbl, else_lbl);
                scope_push(g); cgen_block(g, c, n->iff.els); scope_pop(g);
            }
            emit(g, "%s:\n", end_lbl);
            break;
        }

        case NODE_WHILE: {
            char top_lbl[64], exit_lbl[64];
            fresh_label(g, "wt", top_lbl,  sizeof(top_lbl));
            fresh_label(g, "we", exit_lbl, sizeof(exit_lbl));
            LoopCtx lctx;
            snprintf(lctx.exit_label, 64, "%s", exit_lbl);
            snprintf(lctx.top_label,  64, "%s", top_lbl);
            lctx.parent = g->loop; g->loop = &lctx;

            emit(g, "%s:\n", top_lbl);
            cgen_expr(g, c, n->whilee.cond, "ax");
            emit(g, "  mov bx, 0\n  cmp ax, bx\n  je %s\n", exit_lbl);
            scope_push(g); cgen_block(g, c, n->whilee.body); scope_pop(g);
            emit(g, "  jmp %s\n%s:\n", top_lbl, exit_lbl);
            g->loop = lctx.parent;
            break;
        }

        case NODE_LOOP: {
            char top_lbl[64], exit_lbl[64];
            fresh_label(g, "lt", top_lbl,  sizeof(top_lbl));
            fresh_label(g, "le", exit_lbl, sizeof(exit_lbl));
            LoopCtx lctx;
            snprintf(lctx.exit_label, 64, "%s", exit_lbl);
            snprintf(lctx.top_label,  64, "%s", top_lbl);
            lctx.parent = g->loop; g->loop = &lctx;

            emit(g, "%s:\n", top_lbl);
            scope_push(g); cgen_block(g, c, n->loop_body); scope_pop(g);
            emit(g, "  jmp %s\n%s:\n", top_lbl, exit_lbl);
            g->loop = lctx.parent;
            break;
        }

        case NODE_FOR: {
            // for [k,] v in iter do body end
            // iter: (length uint, data ptr) — length-prefixed array
            char top_lbl[64], exit_lbl[64];
            fresh_label(g, "ft", top_lbl,  sizeof(top_lbl));
            fresh_label(g, "fe", exit_lbl, sizeof(exit_lbl));
            LoopCtx lctx;
            snprintf(lctx.exit_label, 64, "%s", exit_lbl);
            snprintf(lctx.top_label,  64, "%s", top_lbl);
            lctx.parent = g->loop; g->loop = &lctx;

            // iter base into dx — preserved across loop body
            cgen_expr(g, c, n->forr.iter, "dx");

            // index counter local
            char idx_buf[48];
            snprintf(idx_buf, sizeof(idx_buf), "__fori%d", g->label_counter);
            Atom idx_atom = { atom_hash(idx_buf, strlen(idx_buf)), idx_buf };
            Local *idx_l  = local_define(g, idx_atom, ty_uint, 0);
            emit(g, "  mov ax, 0\n");
            store_local(g, idx_l, "ax");

            // cx = length (at [dx+0]) — reloaded each iteration top
            emit(g, "%s:\n", top_lbl);
            load_local(g, idx_l, "ax");
            emit(g, "  mov bx, [dx]\n");   // bx = length
            emit(g, "  cmp ax, bx\n  jge %s\n", exit_lbl);

            scope_push(g);

            if (n->forr.key) {
                Local *kl = local_define(g, n->forr.key->ident, ty_uint, 0);
                load_local(g, idx_l, "ax");
                store_local(g, kl, "ax");
            }

            // val = data[i]: data ptr at [dx+8]
            load_local(g, idx_l, "bx");
            emit(g, "  mov cx, 8\n  mul bx, cx\n");  // bx = i*8
            emit(g, "  mov di, [dx+8]\n");            // di = data ptr
            emit(g, "  add di, bx\n");                // di = &data[i]
            emit(g, "  mov ax, [di]\n");
            Local *vl = local_define(g, n->forr.val->ident, ty_word, 0);
            store_local(g, vl, "ax");

            cgen_block(g, c, n->forr.body);
            scope_pop(g);

            // i++
            load_local(g, idx_l, "ax");
            emit(g, "  mov bx, 1\n  add ax, bx\n");
            store_local(g, idx_l, "ax");
            emit(g, "  jmp %s\n%s:\n", top_lbl, exit_lbl);
            g->loop = lctx.parent;
            break;
        }

        case NODE_CASE: {
            char end_lbl[64];
            fresh_label(g, "ce", end_lbl, sizeof(end_lbl));
            // subject in bx — avoid ax so arm bodies can use ax freely
            cgen_expr(g, c, n->casee.subject, "bx");

            for (int i = 0; i < n->casee.narms; i++) {
                Node *arm = n->casee.arms[i];
                char next_lbl[64];
                fresh_label(g, "ca", next_lbl, sizeof(next_lbl));

                if (arm->arm.pattern) {
                    emit(g, "  mov cx, 0x%llx  ; :%s\n",
                         (unsigned long long)arm->arm.pattern->atom.hash,
                         arm->arm.pattern->atom.name);
                    emit(g, "  cmp bx, cx\n  jne %s\n", next_lbl);
                }

                scope_push(g);
                if (arm->arm.body->kind == NODE_BLOCK)
                    cgen_block(g, c, arm->arm.body);
                else
                    cgen_stmt(g, c, arm->arm.body);
                scope_pop(g);

                emit(g, "  jmp %s\n%s:\n", end_lbl, next_lbl);
            }
            emit(g, "%s:\n", end_lbl);
            break;
        }

        case NODE_EXIT:
            if (!g->loop) { fputs("codegen: exit outside loop\n", stderr); abort(); }
            emit(g, "  jmp %s\n", g->loop->exit_label);
            break;

        case NODE_BLOCK:
            cgen_block(g, c, n);
            break;

        default:
            cgen_expr(g, c, n, "ax");
            break;
    }
}

static void cgen_block(CGen *g, Checker *c, Node *n) {
    if (!n) return;
    if (n->kind != NODE_BLOCK) { cgen_stmt(g, c, n); return; }
    for (int i = 0; i < n->block.nstmts; i++)
        cgen_stmt(g, c, n->block.stmts[i]);
}

// ─── proc codegen ─────────────────────────────────────────────────────────────

static void cgen_proc(CGen *g, Checker *c, Node *n) {
    if (!n->proc.body) return;

    g->frame_size = 0;
    g->scope      = NULL;
    g->loop       = NULL;
    snprintf(g->cur_proc, sizeof(g->cur_proc), "%s", n->proc.name.name);
    scope_push(g);

    emit(g, "\nglobal %s\n%s:\n", n->proc.name.name, n->proc.name.name);

    // emit ENTER with placeholder frame size — backpatched after body
    size_t enter_pos = g->out.len;
    emit(g, "  enter 0000\n");  // "  enter " = 8 chars, "0000" = placeholder

    // bind input params: shadow stack owns return addr, so:
    //   [bp+0] = saved bp
    //   [bp+8] = arg0, [bp+16] = arg1, ...
    Node *in_tup = n->proc.in_tuple;
    if (in_tup && in_tup->kind == NODE_TYPE_TUPLE) {
        int np = in_tup->type_tuple.nfields;
        for (int i = 0; i < np; i++) {
            Node *fd = in_tup->type_tuple.fields[i];
            Type *ft = fd->fielddecl.type
                     ? resolve_type(c, fd->fielddecl.type) : ty_word;
            Local *l = local_define(g, fd->fielddecl.name, ft, type_is_float(ft));
            emit(g, "  mov ax, [bp+%d]\n", 8 + i * 8);
            store_local(g, l, "ax");
        }
    }

    // bind output slots as writable locals
    Node *out_tup = n->proc.out_tuple;
    if (out_tup && out_tup->kind == NODE_TYPE_TUPLE) {
        for (int i = 0; i < out_tup->type_tuple.nfields; i++) {
            Node *fd = out_tup->type_tuple.fields[i];
            Type *ft = fd->fielddecl.type
                     ? resolve_type(c, fd->fielddecl.type) : ty_word;
            local_define(g, fd->fielddecl.name, ft, type_is_float(ft));
        }
    }

    cgen_block(g, c, n->proc.body);

    // load first output into ax before return
    if (out_tup && out_tup->kind == NODE_TYPE_TUPLE
        && out_tup->type_tuple.nfields > 0) {
        Node *fd = out_tup->type_tuple.fields[0];
        Local *l = local_lookup(g, fd->fielddecl.name.hash);
        if (l) load_local(g, l, "ax");
    }

    emit(g, "  leave\n  ret\n");

    // backpatch frame size: "  enter " is 8 chars, placeholder at enter_pos+8
    char patch[8];
    snprintf(patch, sizeof(patch), "%-4d", g->frame_size);
    memcpy(g->out.buf + enter_pos + 8, patch, 4);

    scope_pop(g);
    g->cur_proc[0] = '\0';
}

// ─── init / file ──────────────────────────────────────────────────────────────

void cgen_init(CGen *g, Arena *arena) {
    memset(g, 0, sizeof(*g));
    g->arena = arena;
    abuf_init(&g->out);
}

const char *cgen_file(CGen *g, Checker *c, Node *file) {
    if (file->kind != NODE_BLOCK) {
        fputs("codegen: expected top-level block\n", stderr); abort();
    }

    emit(g, "; Mol generated assembly\n.text\n");

    for (int i = 0; i < file->block.nstmts; i++) {
        Node *n = file->block.stmts[i];
        if (n->kind == NODE_PROC_DECL) {
            if (!n->proc.body)
                emit(g, "extern %s\n", n->proc.name.name);
            else
                cgen_proc(g, c, n);
        }
    }
    
    emit(g, "\nglobal mol_err_stub\nmol_err_stub:\n  halt\n");


    emit_strlits(g);
    emit(g, "\n; end\n");
    return g->out.buf;
}
