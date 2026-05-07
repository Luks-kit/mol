// ─── codegen.c — Mol → DVM assembly text emitter ─────────────────────────────
#include "codegen.h"
#include "check.h"
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

// ─── unique label generation ──────────────────────────────────────────────────

static void fresh_label(CGen *g, const char *prefix, char *out, size_t outsz) {
    snprintf(out, outsz, ".%s%d", prefix, g->label_counter++);
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
    Local *l    = arena_alloc(g->arena, sizeof(Local));
    l->name     = name;
    l->type     = type;
    l->is_float = is_float;
    g->scope->next_offset -= 8;
    l->bp_offset = g->scope->next_offset;
    if (-l->bp_offset > g->frame_size)
        g->frame_size = -l->bp_offset;
    l->next          = g->scope->locals;
    g->scope->locals = l;
    return l;
}

static Local *local_lookup(CGen *g, uint64_t hash) {
    for (LocalScope *s = g->scope; s; s = s->parent)
        for (Local *l = s->locals; l; l = l->next)
            if (l->name.hash == hash) return l;
    return NULL;
}

// ─── load/store helpers ───────────────────────────────────────────────────────

static void load_local(CGen *g, Local *l, const char *dst) {
    if (l->is_float) {
        emit(g, "  lea cx, [bp%+d]\n", l->bp_offset);
        emit(g, "  fmovm %s, [cx]\n", dst);
    } else {
        emit(g, "  mov %s, [bp%+d]\n", dst, l->bp_offset);
    }
}

static void store_local(CGen *g, Local *l, const char *src) {
    if (l->is_float) {
        emit(g, "  lea cx, [bp%+d]\n", l->bp_offset);
        emit(g, "  fmovs [cx], %s\n", src);
    } else {
        emit(g, "  mov [bp%+d], %s\n", l->bp_offset, src);
    }
}

// ─── type helpers ─────────────────────────────────────────────────────────────

static int type_is_float(Type *t) {
    return t && t->kind == TY_FLOAT;
}

static int type_is_signed(Type *t) {
    if (!t || t->kind != TY_INT) return 1; // default signed
    switch (t->int_kind) {
        case INT_KIND_UINT: case INT_KIND_U8: case INT_KIND_U16:
        case INT_KIND_U32:  case INT_KIND_U64: return 0;
        default: return 1;
    }
}

// ─── forward declarations ─────────────────────────────────────────────────────

static void cgen_expr    (CGen *g, Checker *c, Node *n, const char *dst);
static void cgen_expr_f  (CGen *g, Checker *c, Node *n, const char *fdst);
static void cgen_lval    (CGen *g, Checker *c, Node *n, const char *addr_reg);
static void cgen_stmt    (CGen *g, Checker *c, Node *n);
static void cgen_block   (CGen *g, Checker *c, Node *n);

// ─── lvalue address into addr_reg ─────────────────────────────────────────────

static void cgen_lval(CGen *g, Checker *c, Node *n, const char *ar) {
    switch (n->kind) {
        case NODE_IDENT: {
            Local *l = local_lookup(g, n->ident.hash);
            if (!l) {
                fprintf(stderr, "codegen: undefined local '%s'\n", n->ident.name);
                abort();
            }
            emit(g, "  lea %s, [bp%+d]\n", ar, l->bp_offset);
            break;
        }
        case NODE_DEREF:
            cgen_expr(g, c, n->operand, ar);
            break;
        case NODE_INDEX: {
            cgen_lval(g, c, n->index.lhs, ar);
            cgen_expr(g, c, n->index.idx, "bx");
            emit(g, "  mov cx, 8\n");
            emit(g, "  mul bx, cx\n");
            emit(g, "  add %s, bx\n", ar);
            break;
        }
        case NODE_FIELD: {
            Type *base_ty = check_expr(c, n->field.lhs);
            // if pointer, load the pointer then work with its target
            int via_ptr = (base_ty->kind == TY_PTR);
            if (via_ptr) {
                cgen_expr(g, c, n->field.lhs, ar);
                base_ty = base_ty->ptr_inner;
            } else {
                cgen_lval(g, c, n->field.lhs, ar);
            }
            if (base_ty->kind == TY_RECORD) {
                RecordDef *def   = base_ty->record;
                uint64_t   fhash = n->field.field.hash;
                // search fixed fields
                for (int i = 0; i < def->nfields; i++) {
                    if (def->fields[i].name.hash == fhash) {
                        if (def->fields[i].offset)
                            emit(g, "  mov cx, %d\n  add %s, cx\n",
                                 def->fields[i].offset, ar);
                        return;
                    }
                }
                // search variant fields
                for (int v = 0; v < def->nvariants; v++)
                    for (int i = 0; i < def->variant_nfields[v]; i++)
                        if (def->variant_fields[v][i].name.hash == fhash) {
                            int off = def->variant_fields[v][i].offset;
                            if (off) emit(g, "  mov cx, %d\n  add %s, cx\n", off, ar);
                            return;
                        }
                // extendible: emit hash as key, runtime stub TODO
                if (def->extensible) {
                    emit(g, "  mov bx, 0x%llx  ; ext field '%s'\n",
                         (unsigned long long)fhash, n->field.field.name);
                    // TODO: call __mol_ext_field_addr
                }
            }
            break;
        }
        default:
            fprintf(stderr, "codegen: not an lvalue (kind=%d)\n", n->kind);
            abort();
    }
}

// ─── expression codegen — result in GPR dst ──────────────────────────────────

static void cgen_expr(CGen *g, Checker *c, Node *n, const char *dst) {
    switch (n->kind) {

        case NODE_INT_LIT:
            emit(g, "  mov %s, %llu\n", dst, (unsigned long long)n->intlit.value);
            break;

        case NODE_FLOAT_LIT: {
            // load float into fp0 then transfer via stack to dst gpr
            uint64_t raw; memcpy(&raw, &n->floatlit, 8);
            emit(g, "  fmovi fp0, %g\n", n->floatlit);
            emit(g, "  lea cx, [bp-8]\n");
            emit(g, "  fmovs [cx], fp0\n");
            emit(g, "  mov %s, [cx]\n", dst);
            break;
        }

        case NODE_ATOM:
            emit(g, "  mov %s, 0x%llx  ; :%s\n", dst,
                 (unsigned long long)n->atom.hash, n->atom.name);
            break;

        case NODE_STRING_LIT: {
            // emit string into .cnst, reference via reloc
            // generate a unique label for this string literal
            char slbl[64];
            fresh_label(g, "str", slbl, sizeof(slbl));
            // we'll emit the .cnst data at file scope — defer via a side buffer
            // For now emit the label reference; the string section is emitted by cgen_file
            emit(g, "  mov %s, %s\n", dst, slbl);
            // record for deferred .cnst emission — stored as a comment for now
            // Full implementation requires a deferred string table
            emit(g, "  ; string literal deferred: %s\n", slbl);
            break;
        }

        case NODE_IDENT: {
            Local *l = local_lookup(g, n->ident.hash);
            if (l) {
                load_local(g, l, dst);
            } else {
                // global proc or const — emit as label reference
                emit(g, "  mov %s, %s\n", dst, n->ident.name);
            }
            break;
        }

        case NODE_FIELD:
        case NODE_INDEX: {
            cgen_lval(g, c, n, "cx");
            emit(g, "  mov %s, [cx]\n", dst);
            break;
        }

        case NODE_DEREF:
            cgen_expr(g, c, n->operand, dst);
            emit(g, "  mov %s, [%s]\n", dst, dst);
            break;

        case NODE_ADDROF:
            cgen_lval(g, c, n->operand, dst);
            break;

        case NODE_BINOP: {
            Type *lty = check_expr(c, n->binop.lhs);
            int is_float  = type_is_float(lty);
            int is_signed = type_is_signed(lty);

            if (is_float) {
                cgen_expr_f(g, c, n->binop.lhs, "fp0");
                cgen_expr_f(g, c, n->binop.rhs, "fp1");
                switch (n->binop.op) {
                    case BOP_ADD: emit(g, "  fadd fp0, fp1\n"); break;
                    case BOP_SUB: emit(g, "  fsub fp0, fp1\n"); break;
                    case BOP_MUL: emit(g, "  fmul fp0, fp1\n"); break;
                    case BOP_DIV: emit(g, "  fdiv fp0, fp1\n"); break;
                    case BOP_EQ: case BOP_NEQ:
                    case BOP_LT: case BOP_LTE:
                    case BOP_GT: case BOP_GTE: {
                        emit(g, "  fcmp fp0, fp1\n");
                        goto emit_cmp;
                    }
                    default:
                        fprintf(stderr, "codegen: unsupported float binop\n");
                        abort();
                }
                // float result: transfer fp0 → dst via stack slot
                emit(g, "  lea cx, [bp-8]\n");
                emit(g, "  fmovs [cx], fp0\n");
                emit(g, "  mov %s, [cx]\n", dst);
                break;
            }

            cgen_expr(g, c, n->binop.lhs, "bx");
            cgen_expr(g, c, n->binop.rhs, "cx");

            switch (n->binop.op) {
                case BOP_ADD: emit(g, "  add bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_SUB: emit(g, "  sub bx, cx\n  mov %s, bx\n", dst); break;
                case BOP_MUL:
                    emit(g, "  %s bx, cx\n  mov %s, bx\n",
                         is_signed ? "imul" : "mul", dst);
                    break;
                case BOP_DIV:
                    emit(g, "  %s bx, cx\n  mov %s, bx\n",
                         is_signed ? "idiv" : "div", dst);
                    break;
                case BOP_MOD: emit(g, "  mod bx, cx\n  mov %s, bx\n", dst);  break;
                case BOP_AND: emit(g, "  and bx, cx\n  mov %s, bx\n", dst);  break;
                case BOP_OR:  emit(g, "  or  bx, cx\n  mov %s, bx\n", dst);  break;
                case BOP_XOR: emit(g, "  xor bx, cx\n  mov %s, bx\n", dst);  break;
                case BOP_SHL: emit(g, "  shl bx, cx\n  mov %s, bx\n", dst);  break;
                case BOP_SHR: emit(g, "  shr bx, cx\n  mov %s, bx\n", dst);  break;
                case BOP_SAR: emit(g, "  sar bx, cx\n  mov %s, bx\n", dst);  break;
                case BOP_EQ: case BOP_NEQ:
                case BOP_LT: case BOP_LTE:
                case BOP_GT: case BOP_GTE:
                    emit(g, "  cmp bx, cx\n");
                emit_cmp: {
                    char t_lbl[64], end_lbl[64];
                    fresh_label(g, "ct", t_lbl,   sizeof(t_lbl));
                    fresh_label(g, "cf", end_lbl, sizeof(end_lbl));
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
                    emit(g, "  mov %s, 0\n", dst);
                    emit(g, "  jmp %s\n", end_lbl);
                    emit(g, "%s:\n", t_lbl);
                    emit(g, "  mov %s, 1\n", dst);
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

        case NODE_CALL: {
            // push args left-to-right
            for (int i = 0; i < n->call.nargs; i++) {
                cgen_expr(g, c, n->call.args[i], "ax");
                emit(g, "  push ax\n");
            }
            // load callee address then indirect call
            cgen_expr(g, c, n->call.callee, "ax");
            emit(g, "  callr ax, .err_stub\n");
            if (dst[0] != 'a' || dst[1] != 'x')
                emit(g, "  mov %s, ax\n", dst);
            break;
        }

        case NODE_ASSIGN: {
            cgen_expr(g, c, n->assign.rhs, "ax");
            cgen_lval(g, c, n->assign.lhs, "bx");
            emit(g, "  mov [bx], ax\n");
            if (dst[0] != 'a' || dst[1] != 'x')
                emit(g, "  mov %s, ax\n", dst);
            break;
        }

        case NODE_ARRAY_LIT: {
            int ne = n->arraylit.nelems;
            emit(g, "  mov ax, %d\n", ne * 8);
            emit(g, "  alloca\n");
            emit(g, "  mov %s, ax\n", dst);
            for (int i = 0; i < ne; i++) {
                cgen_expr(g, c, n->arraylit.elems[i], "bx");
                emit(g, "  mov [%s+%d], bx\n", dst, i * 8);
            }
            break;
        }

        case NODE_MOL_LIT: {
            int nf = n->mollit.nfields;
            emit(g, "  mov ax, %d\n", nf * 8);
            emit(g, "  alloca\n");
            emit(g, "  mov %s, ax\n", dst);
            for (int i = 0; i < nf; i++) {
                Node *f   = n->mollit.fields[i];
                Node *val = (f->kind == NODE_FIELD) ? f->field.lhs : f;
                cgen_expr(g, c, val, "bx");
                emit(g, "  mov [%s+%d], bx\n", dst, i * 8);
            }
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
            if (l) {
                emit(g, "  lea cx, [bp%+d]\n", l->bp_offset);
                emit(g, "  fmovm %s, [cx]\n", fdst);
            }
            break;
        }
        default:
            // evaluate as integer then convert
            cgen_expr(g, c, n, "ax");
            emit(g, "  itof %s, ax\n", fdst);
            break;
    }
}

// ─── statement codegen ────────────────────────────────────────────────────────

static void cgen_stmt(CGen *g, Checker *c, Node *n) {
    switch (n->kind) {

        case NODE_VAR_DECL:
        case NODE_LET_DECL: {
            Type *ty = ty_word;
            if (n->vardecl.type)
                ty = check_expr(c, n->vardecl.type) ? ty_word : ty_word; // type nodes
            if (n->vardecl.init)
                ty = check_expr(c, n->vardecl.init);
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
            emit(g, "  mov bx, 0\n");
            emit(g, "  cmp ax, bx\n");
            emit(g, "  je %s\n", n->iff.els ? else_lbl : end_lbl);

            scope_push(g);
            cgen_block(g, c, n->iff.then);
            scope_pop(g);

            if (n->iff.els) {
                emit(g, "  jmp %s\n", end_lbl);
                emit(g, "%s:\n", else_lbl);
                scope_push(g);
                cgen_block(g, c, n->iff.els);
                scope_pop(g);
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
            lctx.parent = g->loop;
            g->loop     = &lctx;

            emit(g, "%s:\n", top_lbl);
            cgen_expr(g, c, n->whilee.cond, "ax");
            emit(g, "  mov bx, 0\n");
            emit(g, "  cmp ax, bx\n");
            emit(g, "  je %s\n", exit_lbl);
            scope_push(g);
            cgen_block(g, c, n->whilee.body);
            scope_pop(g);
            emit(g, "  jmp %s\n", top_lbl);
            emit(g, "%s:\n", exit_lbl);
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
            lctx.parent = g->loop;
            g->loop     = &lctx;

            emit(g, "%s:\n", top_lbl);
            scope_push(g);
            cgen_block(g, c, n->loop_body);
            scope_pop(g);
            emit(g, "  jmp %s\n", top_lbl);
            emit(g, "%s:\n", exit_lbl);
            g->loop = lctx.parent;
            break;
        }

        case NODE_FOR: {
            // for [k,] v in iter do body end
            // iter points to (length uint, data ptr) — length-prefixed array
            char top_lbl[64], exit_lbl[64];
            fresh_label(g, "ft", top_lbl,  sizeof(top_lbl));
            fresh_label(g, "fe", exit_lbl, sizeof(exit_lbl));

            LoopCtx lctx;
            snprintf(lctx.exit_label, 64, "%s", exit_lbl);
            snprintf(lctx.top_label,  64, "%s", top_lbl);
            lctx.parent = g->loop;
            g->loop     = &lctx;

            // evaluate iter into dx — keep it there for the loop
            cgen_expr(g, c, n->forr.iter, "dx");

            // allocate index counter
            char idx_name_buf[32];
            snprintf(idx_name_buf, sizeof(idx_name_buf), "__for_i_%d", g->label_counter);
            Atom idx_atom = { atom_hash(idx_name_buf, strlen(idx_name_buf)), idx_name_buf };
            Local *idx_l  = local_define(g, idx_atom, ty_uint, 0);
            emit(g, "  mov ax, 0\n");
            store_local(g, idx_l, "ax");

            // load length from iter[0]
            emit(g, "  mov cx, [dx]\n");  // cx = length

            emit(g, "%s:\n", top_lbl);
            load_local(g, idx_l, "ax");
            emit(g, "  cmp ax, cx\n");
            emit(g, "  jge %s\n", exit_lbl);

            scope_push(g);

            // bind key
            if (n->forr.key) {
                Local *kl = local_define(g, n->forr.key->ident, ty_uint, 0);
                load_local(g, idx_l, "ax");
                store_local(g, kl, "ax");
            }

            // bind value: data ptr at iter[8], val = data[i*8]
            emit(g, "  mov bx, [dx+8]\n");   // bx = data ptr
            load_local(g, idx_l, "ax");
            emit(g, "  mov si, 8\n");
            emit(g, "  mul ax, si\n");
            emit(g, "  add bx, ax\n");
            emit(g, "  mov ax, [bx]\n");
            Local *vl = local_define(g, n->forr.val->ident, ty_word, 0);
            store_local(g, vl, "ax");

            cgen_block(g, c, n->forr.body);
            scope_pop(g);

            // i++
            load_local(g, idx_l, "ax");
            emit(g, "  mov bx, 1\n");
            emit(g, "  add ax, bx\n");
            store_local(g, idx_l, "ax");

            emit(g, "  jmp %s\n", top_lbl);
            emit(g, "%s:\n", exit_lbl);
            g->loop = lctx.parent;
            break;
        }

        case NODE_CASE: {
            char end_lbl[64];
            fresh_label(g, "ce", end_lbl, sizeof(end_lbl));
            cgen_expr(g, c, n->casee.subject, "ax");

            for (int i = 0; i < n->casee.narms; i++) {
                Node *arm = n->casee.arms[i];
                char next_lbl[64];
                fresh_label(g, "ca", next_lbl, sizeof(next_lbl));

                if (arm->arm.pattern) {
                    // compare ax to pattern atom hash
                    emit(g, "  mov bx, 0x%llx  ; :%s\n",
                         (unsigned long long)arm->arm.pattern->atom.hash,
                         arm->arm.pattern->atom.name);
                    emit(g, "  cmp ax, bx\n");
                    emit(g, "  jne %s\n", next_lbl);
                }

                scope_push(g);
                if (arm->arm.body->kind == NODE_BLOCK)
                    cgen_block(g, c, arm->arm.body);
                else
                    cgen_stmt(g, c, arm->arm.body);
                scope_pop(g);

                emit(g, "  jmp %s\n", end_lbl);
                emit(g, "%s:\n", next_lbl);
            }
            emit(g, "%s:\n", end_lbl);
            break;
        }

        case NODE_EXIT:
            if (!g->loop) {
                fputs("codegen: exit outside loop\n", stderr); abort();
            }
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

    // reset per-proc state
    g->frame_size = 0;
    g->scope      = NULL;
    g->loop       = NULL;
    scope_push(g);

    // emit global label
    emit(g, "\nglobal %s\n", n->proc.name.name);
    emit(g, "%s:\n", n->proc.name.name);

    // placeholder ENTER — we'll backpatch frame_size after emitting the body
    // We emit a comment placeholder and replace it post-hoc via string rewrite.
    // Simpler: emit ENTER with a large enough frame and not worry about exact size,
    // OR do a two-pass where we emit body to a temp buffer first.
    // We use the two-pass approach: save current out.len, emit enter with 0,
    // note the position, emit body, then patch.
    size_t enter_pos = g->out.len;
    emit(g, "  enter 0000\n");  // 10 chars for the number field — we'll overwrite

    // bind input parameters — they sit above bp after ENTER
    // bp+0  = saved bp (pushed by ENTER)
    // bp+8  = return addr (on shadow stack, not data stack) — not here
    // args were pushed by caller: first arg at bp+16, second at bp+24, ...
    Node *in_tup = n->proc.in_tuple;
    if (in_tup && in_tup->kind == NODE_TYPE_TUPLE) {
        int np = in_tup->type_tuple.nfields;
        for (int i = 0; i < np; i++) {
            Node *fd = in_tup->type_tuple.fields[i];
            Type *ft = fd->fielddecl.type
                     ? resolve_type(c, fd->fielddecl.type)
                     : ty_word;
            // allocate a local and copy from the above-bp slot
            Local *l = local_define(g, fd->fielddecl.name, ft, type_is_float(ft));
            int above_bp = 16 + i * 8;
            emit(g, "  mov ax, [bp+%d]\n", above_bp);
            store_local(g, l, "ax");
        }
    }

    // bind output slots as writable locals
    Node *out_tup = n->proc.out_tuple;
    if (out_tup && out_tup->kind == NODE_TYPE_TUPLE) {
        for (int i = 0; i < out_tup->type_tuple.nfields; i++) {
            Node *fd = out_tup->type_tuple.fields[i];
            Type *ft = fd->fielddecl.type
                     ? resolve_type(c, fd->fielddecl.type)
                     : ty_word;
            local_define(g, fd->fielddecl.name, ft, type_is_float(ft));
        }
    }

    cgen_block(g, c, n->proc.body);

    // load first output into ax before returning
    if (out_tup && out_tup->kind == NODE_TYPE_TUPLE
        && out_tup->type_tuple.nfields > 0) {
        Node *fd = out_tup->type_tuple.fields[0];
        Local *l = local_lookup(g, fd->fielddecl.name.hash);
        if (l) load_local(g, l, "ax");
    }

    emit(g, "  leave\n");
    emit(g, "  ret\n");

    // backpatch ENTER frame size
    // The enter line looks like "  enter 0000\n"
    // We need to write the actual frame_size into that position.
    // Locate "0000" after "enter " at enter_pos.
    char patch[16];
    int  psz = snprintf(patch, sizeof(patch), "%-4d", g->frame_size);
    // find offset of the number within the emit
    // "  enter " = 8 chars; our placeholder "0000" starts at enter_pos+8
    size_t patch_at = enter_pos + 8;
    for (int i = 0; i < psz && patch_at + (size_t)i < g->out.len; i++)
        g->out.buf[patch_at + i] = patch[i];

    scope_pop(g);
}

// ─── error stub ───────────────────────────────────────────────────────────────
// callr needs an error handler label. Emit a minimal one.

static void emit_err_stub(CGen *g) {
    emit(g, "\nerr_stub:\n");
    emit(g, "  halt\n");
}

// ─── cgen_init / cgen_file ────────────────────────────────────────────────────

void cgen_init(CGen *g, Arena *arena) {
    memset(g, 0, sizeof(*g));
    g->arena = arena;
    abuf_init(&g->out);
}

const char *cgen_file(CGen *g, Checker *c, Node *file) {
    if (file->kind != NODE_BLOCK) {
        fputs("codegen: expected top-level block\n", stderr); abort();
    }

    emit(g, "; Mol generated assembly\n");
    emit(g, ".text\n");

    // emit error stub first so it's always defined
    emit_err_stub(g);

    for (int i = 0; i < file->block.nstmts; i++) {
        Node *n = file->block.stmts[i];
        if (n->kind == NODE_PROC_DECL)
            cgen_proc(g, c, n);
    }

    emit(g, "\n; end\n");
    return g->out.buf;
}
