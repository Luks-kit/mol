// ─── ir.c — Mol IR builder + lowering pass ───────────────────────────────────
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ═══════════════════════════════════════════════════════════════════════════════
// Builder
// ═══════════════════════════════════════════════════════════════════════════════

void ir_init(IrProg *p, Arena *arena) {
    memset(p, 0, sizeof(*p));
    p->arena = arena;
}

Var ir_tmp(IrProg *p, Type *type) {
    char buf[32];
    snprintf(buf, sizeof(buf), "t%d", p->tmp_counter++);
    return (Var){ .name = { atom_hash(buf, strlen(buf)),
                            arena_strdup(p->arena, buf) },
                  .type = type };
}

LabelId ir_label(IrProg *p) {
    return p->label_counter++;
}

const char *ir_str_lit(IrProg *p, const char *data, size_t len) {
    for (IrStr *s = p->strs; s; s = s->next)
        if (s->len == len && memcmp(s->data, data, len) == 0)
            return s->label;
    IrStr *s   = arena_alloc(p->arena, sizeof(IrStr));
    snprintf(s->label, sizeof(s->label), "str_%d", p->tmp_counter++);
    s->data    = arena_strndup(p->arena, data, len);
    s->len     = len;
    s->next    = p->strs;
    p->strs    = s;
    return s->label;
}

IrProc *ir_proc_begin(IrProg *p, const char *name, int is_extern) {
    if (p->nprocs >= p->proc_cap) {
        p->proc_cap = p->proc_cap ? p->proc_cap * 2 : 16;
        p->procs    = arena_alloc(p->arena,
                        sizeof(IrProc) * (size_t)p->proc_cap);
    }
    IrProc *proc = &p->procs[p->nprocs++];
    memset(proc, 0, sizeof(*proc));
    snprintf(proc->name, sizeof(proc->name), "%s", name);
    proc->is_extern = is_extern;
    return proc;
}

void ir_emit(IrProc *proc, Arena *arena, IrInstr instr) {
    if (proc->ninstr >= proc->cap) {
        int      newcap = proc->cap ? proc->cap * 2 : 32;
        IrInstr *nb     = arena_alloc(arena,
                            sizeof(IrInstr) * (size_t)newcap);
        if (proc->instrs)
            memcpy(nb, proc->instrs,
                   sizeof(IrInstr) * (size_t)proc->ninstr);
        proc->instrs = nb;
        proc->cap    = newcap;
    }
    proc->instrs[proc->ninstr++] = instr;
}

Var ir_emit_dst(IrProc *proc, Arena *arena, IrInstr instr) {
    Var dst = instr.dst;
    ir_emit(proc, arena, instr);
    return dst;
}

// ─── instruction zero-initialiser ─────────────────────────────────────────────
static IrInstr mk(IrOp op) {
    IrInstr i; memset(&i, 0, sizeof(i));
    i.op  = op;
    i.dst = VAR_NULL;
    i.a   = val_none();
    i.b   = val_none();
    return i;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Lowering: typed AST → IrProg
// ═══════════════════════════════════════════════════════════════════════════════

// ─── lowering scope ───────────────────────────────────────────────────────────
typedef struct LScope LScope;
struct LScope {
    Var    *vars;
    int     nvars;
    int     cap;
    LScope *parent;
};

typedef struct {
    IrProg  *p;
    Checker *c;
    IrProc  *proc;
    LScope  *scope;
    LabelId  loop_exit;
    LabelId  loop_top;
} Lower;

static void lscope_push(Lower *l) {
    LScope *s = arena_alloc(l->p->arena, sizeof(LScope));
    s->vars   = NULL;
    s->nvars  = 0;
    s->cap    = 0;
    s->parent = l->scope;
    l->scope  = s;
}

static void lscope_pop(Lower *l) {
    l->scope = l->scope->parent;
}

static void lscope_def(Lower *l, Var v) {
    LScope *s = l->scope;
    if (s->nvars >= s->cap) {
        int  newcap = s->cap ? s->cap * 2 : 8;
        Var *nb     = arena_alloc(l->p->arena,
                        sizeof(Var) * (size_t)newcap);
        if (s->vars)
            memcpy(nb, s->vars, sizeof(Var) * (size_t)s->nvars);
        s->vars = nb;
        s->cap  = newcap;
    }
    s->vars[s->nvars++] = v;
}

static Var lscope_find(Lower *l, uint64_t hash) {
    for (LScope *s = l->scope; s; s = s->parent)
        for (int i = 0; i < s->nvars; i++)
            if (s->vars[i].name.hash == hash)
                return s->vars[i];
    return VAR_NULL;
}

// ─── emit conveniences ────────────────────────────────────────────────────────
static void emit(Lower *l, IrInstr i) {
    ir_emit(l->proc, l->p->arena, i);
}
static Var emit_dst(Lower *l, IrInstr i) {
    return ir_emit_dst(l->proc, l->p->arena, i);
}
static Var tmp(Lower *l, Type *t) {
    return ir_tmp(l->p, t);
}
static LabelId fresh_label(Lower *l) {
    return ir_label(l->p);
}
static void emit_label(Lower *l, LabelId id) {
    IrInstr i  = mk(IR_LABEL);
    i.label_id = id;
    emit(l, i);
}
static void emit_jmp(Lower *l, LabelId id) {
    IrInstr i  = mk(IR_JMP);
    i.label_id = id;
    emit(l, i);
}
static void emit_jifn(Lower *l, Val cond, LabelId id) {
    IrInstr i  = mk(IR_JIFN);
    i.a        = cond;
    i.label_id = id;
    emit(l, i);
}

// ─── emit an IR_ALLOCA and return the pointer var ─────────────────────────────
//
// IR_ALLOCA reserves `nbytes` of stack storage.
// The dst var's VALUE is the base address of that storage.
// Codegen translates this to:  lea dst, [bp + slot_offset]
// where the slot is sized to `nbytes` (not just 8).
//
// Callers use the returned var directly as a pointer — no subsequent
// IR_ADDR needed.
static Var emit_alloca(Lower *l, Type *t, size_t nbytes) {
    Var ptr    = tmp(l, NULL);   // ptr type — value IS the address
    IrInstr i  = mk(IR_ALLOCA);
    i.dst      = ptr;
    i.nbytes   = nbytes;
    (void)t;   // kept for documentation; codegen uses nbytes
    emit(l, i);
    return ptr;
}

// ─── field offset helpers ─────────────────────────────────────────────────────
static int field_offset(RecordDef *def, uint64_t fhash) {
    for (int i = 0; i < def->nfields; i++)
        if (def->fields[i].name.hash == fhash)
            return def->fields[i].offset;
    for (int v = 0; v < def->nvariants; v++)
        for (int i = 0; i < def->variant_nfields[v]; i++)
            if (def->variant_fields[v][i].name.hash == fhash)
                return def->variant_fields[v][i].offset;
    return 0;
}

static size_t agg_size(Type *t) {
    if (!t) return 8;
    return (size_t)(t->size > 0 ? t->size : 8);
}

// ─── forward declarations ─────────────────────────────────────────────────────
static Val  lower_expr (Lower *l, Node *n);
static void lower_stmt (Lower *l, Node *n);
static void lower_block(Lower *l, Node *n);

// ─── lvalue → pointer var ─────────────────────────────────────────────────────
//
// Every lvalue case returns a Var whose value is an address.
// For variables that were created by IR_ALLOCA their var value already
// IS their address, so we can return val_var(v) directly via IR_ADDR.
// IR_ADDR in codegen does:  lea dst, [bp + slot]  which is correct for
// both scalar slots and alloca-backed aggregate slots.
static Var lower_lval(Lower *l, Node *n) {
    switch (n->kind) {

        case NODE_IDENT: {
            Var v = lscope_find(l, n->ident.hash);
            if (!var_valid(v)) {
                fprintf(stderr, "lower: undefined '%s'\n", n->ident.name);
                abort();
            }
            // Emit IR_ADDR: dst = &v  →  codegen: lea dst, [bp+slot(v)]
            // Works identically whether v was created by IR_ALLOCA or IR_COPY.
            IrInstr i = mk(IR_ADDR);
            i.dst     = tmp(l, NULL);
            i.a       = val_var(v);
            return emit_dst(l, i);
        }

        case NODE_DEREF: {
            // *ptr as lvalue — the pointer itself is the address
            Val v = lower_expr(l, n->operand);
            if (v.kind != VAL_VAR) {
                fprintf(stderr, "lower: deref lval requires a var operand\n");
                abort();
            }
            return v.var;
        }

        case NODE_INDEX: {
            Val base_ptr = lower_expr(l, n->index.lhs);
            Val idx      = lower_expr(l, n->index.idx);
            int scale    = 8;
            if (n->index.lhs->type
                && n->index.lhs->type->kind == TY_ARRAY
                && n->index.lhs->type->array.inner)
                scale = n->index.lhs->type->array.inner->size;
            IrInstr i     = mk(IR_INDEX_ADDR);
            i.dst         = tmp(l, NULL);
            i.a           = base_ptr;
            i.b           = idx;
            i.index_scale = scale;
            return emit_dst(l, i);
        }

        case NODE_FIELD: {
            Type *base_ty = n->field.lhs->type
                          ? n->field.lhs->type : ty_word;
            Val base_ptr;
            if (base_ty->kind == TY_PTR) {
                // lhs is already a pointer — load it as a value
                base_ptr = lower_expr(l, n->field.lhs);
            } else {
                // lhs is an aggregate — take its address
                base_ptr = val_var(lower_lval(l, n->field.lhs));
            }
            int off = 0;
            if (base_ty->kind == TY_RECORD) {
                off = field_offset(base_ty->record,
                                   n->field.field.hash);
            } else if (base_ty->kind == TY_PTR
                       && base_ty->ptr_inner
                       && base_ty->ptr_inner->kind == TY_RECORD) {
                off = field_offset(base_ty->ptr_inner->record,
                                   n->field.field.hash);
            }
            IrInstr i   = mk(IR_FIELD_ADDR);
            i.dst       = tmp(l, NULL);
            i.a         = base_ptr;
            i.field_off = off;
            return emit_dst(l, i);
        }

        default:
            fprintf(stderr, "lower: not an lvalue (kind=%d)\n", n->kind);
            abort();
    }
}

// ─── expression → Val ─────────────────────────────────────────────────────────
static Val lower_expr(Lower *l, Node *n) {
    switch (n->kind) {

        case NODE_INT_LIT:
            return val_imm(n->intlit.value);

        case NODE_FLOAT_LIT:
            return val_fimm(n->floatlit);

        case NODE_ATOM:
            return val_atom(n->atom.hash);

        case NODE_IDENT: {
            Var v = lscope_find(l, n->ident.hash);
            if (var_valid(v)) return val_var(v);
            // Global proc / extern — fabricate a stand-in var so
            // IR_CALL can reference it by name.
            Var gv = (Var){ n->ident, n->type };
            return val_var(gv);
        }

        case NODE_FIELD:
        case NODE_INDEX: {
            // address-then-load for field/index rvalues
            Var ptr   = lower_lval(l, n);
            IrInstr i = mk(IR_LOAD);
            i.dst     = tmp(l, n->type);
            i.a       = val_var(ptr);
            return val_var(emit_dst(l, i));
        }

        case NODE_DEREF: {
            Val ptr   = lower_expr(l, n->operand);
            IrInstr i = mk(IR_LOAD);
            i.dst     = tmp(l, n->type);
            i.a       = ptr;
            return val_var(emit_dst(l, i));
        }

        case NODE_ADDROF: {
            Var ptr = lower_lval(l, n->operand);
            return val_var(ptr);
        }

        case NODE_BINOP: {
            Type *lty = n->binop.lhs->type ? n->binop.lhs->type : ty_word;
            int   flt = lty && lty->kind == TY_FLOAT;
            int   sgn = !flt && lty && lty->kind == TY_INT
                        && (lty->int_kind == INT_KIND_INT
                            || lty->int_kind == INT_KIND_I8
                            || lty->int_kind == INT_KIND_I16
                            || lty->int_kind == INT_KIND_I32
                            || lty->int_kind == INT_KIND_I64);
            Val a = lower_expr(l, n->binop.lhs);
            Val b = lower_expr(l, n->binop.rhs);

            static const IrOp int_ops[] = {
                [BOP_ADD]=IR_ADD, [BOP_SUB]=IR_SUB,
                [BOP_MUL]=IR_MUL, [BOP_DIV]=IR_DIV, [BOP_MOD]=IR_MOD,
                [BOP_AND]=IR_AND, [BOP_OR]=IR_OR,   [BOP_XOR]=IR_XOR,
                [BOP_SHL]=IR_SHL, [BOP_SHR]=IR_SHR, [BOP_SAR]=IR_SAR,
                [BOP_EQ]=IR_EQ,   [BOP_NEQ]=IR_NEQ,
                [BOP_LT]=IR_LT,   [BOP_LTE]=IR_LTE,
                [BOP_GT]=IR_GT,   [BOP_GTE]=IR_GTE,
            };
            static const IrOp flt_ops[] = {
                [BOP_ADD]=IR_FADD, [BOP_SUB]=IR_FSUB,
                [BOP_MUL]=IR_FMUL, [BOP_DIV]=IR_FDIV,
                [BOP_EQ]=IR_FEQ,   [BOP_NEQ]=IR_FNEQ,
                [BOP_LT]=IR_FLT,   [BOP_LTE]=IR_FLTE,
                [BOP_GT]=IR_FGT,   [BOP_GTE]=IR_FGTE,
            };
            static const IrOp sgn_ops[] = {
                [BOP_MUL]=IR_IMUL, [BOP_DIV]=IR_IDIV,
            };

            IrOp op;
            if (flt) {
                op = flt_ops[n->binop.op];
            } else if (sgn && (n->binop.op == BOP_MUL
                               || n->binop.op == BOP_DIV)) {
                op = sgn_ops[n->binop.op];
            } else {
                op = int_ops[n->binop.op];
            }
            IrInstr i = mk(op);
            i.dst = tmp(l, n->type ? n->type : ty_int);
            i.a   = a;
            i.b   = b;
            return val_var(emit_dst(l, i));
        }

        case NODE_UNOP: {
            Val src   = lower_expr(l, n->unop.operand);
            IrOp op   = (n->unop.op == UOP_NEG) ? IR_NEG : IR_NOT;
            IrInstr i = mk(op);
            i.dst     = tmp(l, n->type);
            i.a       = src;
            return val_var(emit_dst(l, i));
        }

        case NODE_CALL: {
            IrInstr i       = mk(IR_CALL);
            i.dst           = VAR_NULL;
            i.call.callee   = lower_expr(l, n->call.callee);
            i.call.nargs    = n->call.nargs;
            i.call.nrets    = 0;
            if (n->call.nargs > IR_MAX_ARGS) {
                fprintf(stderr, "lower: too many args\n"); abort();
            }
            for (int k = 0; k < n->call.nargs; k++)
                i.call.args[k] = lower_expr(l, n->call.args[k]);

            Type *ct = n->call.callee->type;
            if (ct && ct->kind == TY_PROC
                && ct->proc.out->tuple.nfields > 0) {
                int nr = ct->proc.out->tuple.nfields;
                if (nr > IR_MAX_RETS) nr = IR_MAX_RETS;
                i.call.nrets = nr;
                for (int k = 0; k < nr; k++)
                    i.call.rets[k] = tmp(l,
                        ct->proc.out->tuple.fields[k].type);
            }
            emit(l, i);
            if (i.call.nrets > 0) return val_var(i.call.rets[0]);
            return val_none();
        }

        case NODE_ASSIGN: {
            Val rhs   = lower_expr(l, n->assign.rhs);
            Var ptr   = lower_lval(l, n->assign.lhs);
            IrInstr i = mk(IR_STORE);
            i.a       = val_var(ptr);
            i.b       = rhs;
            emit(l, i);
            return rhs;
        }

        case NODE_ARRAY_LIT: {
            // IR_ALLOCA gives us both the storage and its address in one var.
            int    ne      = n->arraylit.nelems;
            int    esz     = (n->type && n->type->kind == TY_ARRAY
                              && n->type->array.inner)
                           ? n->type->array.inner->size : 8;
            size_t tot     = (size_t)(ne * esz);
            if (tot < 8) tot = 8;

            // base is a pointer var; its value == address of the storage
            Var base = emit_alloca(l, n->type, tot);

            for (int k = 0; k < ne; k++) {
                Val elem  = lower_expr(l, n->arraylit.elems[k]);
                IrInstr ia = mk(IR_FIELD_ADDR);
                ia.dst     = tmp(l, NULL);
                ia.a       = val_var(base);
                ia.field_off = (int64_t)(k * esz);
                Var eptr   = emit_dst(l, ia);
                IrInstr is = mk(IR_STORE);
                is.a       = val_var(eptr);
                is.b       = elem;
                emit(l, is);
            }
            return val_var(base);
        }

        case NODE_MOL_LIT: {
            size_t sz  = agg_size(n->type);
            Var    base = emit_alloca(l, n->type, sz);

            for (int k = 0; k < n->mollit.nfields; k++) {
                Node *f   = n->mollit.fields[k];
                // named field:  f is NODE_FIELD, f->field.lhs is the value
                // positional:   f is the value node itself
                Node *val_node = (f->kind == NODE_FIELD)
                               ? f->field.lhs : f;
                Val v      = lower_expr(l, val_node);

                int64_t off = (int64_t)(k * 8);
                if (f->kind == NODE_FIELD
                    && n->type && n->type->kind == TY_RECORD) {
                    off = (int64_t)field_offset(n->type->record,
                                               f->field.field.hash);
                }
                IrInstr ia  = mk(IR_FIELD_ADDR);
                ia.dst      = tmp(l, NULL);
                ia.a        = val_var(base);
                ia.field_off = off;
                Var fptr    = emit_dst(l, ia);
                IrInstr is  = mk(IR_STORE);
                is.a        = val_var(fptr);
                is.b        = v;
                emit(l, is);
            }
            return val_var(base);
        }

        case NODE_STRING_LIT:
            fprintf(stderr, "lower: string literal used as expression — "
                            "must be a var/let initialiser\n");
            abort();

        default:
            fprintf(stderr, "lower: unhandled expr kind=%d\n", n->kind);
            abort();
    }
}

// ─── statement lowering ───────────────────────────────────────────────────────
static void lower_stmt(Lower *l, Node *n) {
    switch (n->kind) {

        case NODE_VAR_DECL:
        case NODE_LET_DECL: {
            Type *ty = n->type
                     ? n->type
                     : (n->vardecl.init && n->vardecl.init->type
                        ? n->vardecl.init->type : ty_word);

            Var v = (Var){ n->vardecl.name, ty };
            lscope_def(l, v);

            if (!n->vardecl.init) break;

            // ── string literal initialiser ─────────────────────────────
            if (n->vardecl.init->kind == NODE_STRING_LIT) {
                const char *slbl = ir_str_lit(l->p,
                    n->vardecl.init->strlit.data,
                    n->vardecl.init->strlit.len);
                // v is the string var (16-byte record: length + data ptr).
                // We emit IR_ALLOCA to reserve 16 bytes for it, then
                // IR_STR_INIT to fill length and data ptr.
                IrInstr al = mk(IR_ALLOCA);
                al.dst     = v;
                al.nbytes  = 16;   // string = (uint length, u8! data)
                emit(l, al);
                IrInstr si = mk(IR_STR_INIT);
                si.dst     = v;
                snprintf(si.str_label, sizeof(si.str_label), "%s", slbl);
                emit(l, si);
                break;
            }

            // ── aggregate initialiser (mol lit, array lit, call → record) ─
            if (ty && (ty->kind == TY_RECORD || ty->kind == TY_ARRAY)) {
                // Reserve stack space for v with its full size.
                IrInstr al = mk(IR_ALLOCA);
                al.dst     = v;
                al.nbytes  = agg_size(ty);
                emit(l, al);

                Val rhs = lower_expr(l, n->vardecl.init);
                // rhs is a pointer to a temporary aggregate (from
                // NODE_ARRAY_LIT / NODE_MOL_LIT / IR_ALLOCA-backed call ret).
                // Copy it into v's storage.
                IrInstr ia = mk(IR_ADDR);
                ia.dst     = tmp(l, NULL);
                ia.a       = val_var(v);
                Var dst_ptr = emit_dst(l, ia);
                IrInstr mc  = mk(IR_MEMCPY);
                mc.a        = val_var(dst_ptr);
                mc.b        = rhs;
                mc.nbytes   = agg_size(ty);
                emit(l, mc);
                break;
            }

            // ── scalar initialiser ─────────────────────────────────────
            Val rhs   = lower_expr(l, n->vardecl.init);
            IrInstr i = mk(IR_COPY);
            i.dst     = v;
            i.a       = rhs;
            emit(l, i);
            break;
        }

        case NODE_ASSIGN:
            lower_expr(l, n);
            break;

        case NODE_IF: {
            Val     cond    = lower_expr(l, n->iff.cond);
            LabelId else_id = fresh_label(l);
            LabelId end_id  = fresh_label(l);
            emit_jifn(l, cond, n->iff.els ? else_id : end_id);
            lscope_push(l);
            lower_block(l, n->iff.then);
            lscope_pop(l);
            if (n->iff.els) {
                emit_jmp(l, end_id);
                emit_label(l, else_id);
                lscope_push(l);
                lower_block(l, n->iff.els);
                lscope_pop(l);
            }
            emit_label(l, end_id);
            break;
        }

        case NODE_WHILE: {
            LabelId top_id  = fresh_label(l);
            LabelId exit_id = fresh_label(l);
            LabelId sv_exit = l->loop_exit;
            LabelId sv_top  = l->loop_top;
            l->loop_exit    = exit_id;
            l->loop_top     = top_id;
            emit_label(l, top_id);
            Val cond = lower_expr(l, n->whilee.cond);
            emit_jifn(l, cond, exit_id);
            lscope_push(l);
            lower_block(l, n->whilee.body);
            lscope_pop(l);
            emit_jmp(l, top_id);
            emit_label(l, exit_id);
            l->loop_exit = sv_exit;
            l->loop_top  = sv_top;
            break;
        }

        case NODE_LOOP: {
            LabelId top_id  = fresh_label(l);
            LabelId exit_id = fresh_label(l);
            LabelId sv_exit = l->loop_exit;
            LabelId sv_top  = l->loop_top;
            l->loop_exit    = exit_id;
            l->loop_top     = top_id;
            emit_label(l, top_id);
            lscope_push(l);
            lower_block(l, n->loop_body);
            lscope_pop(l);
            emit_jmp(l, top_id);
            emit_label(l, exit_id);
            l->loop_exit = sv_exit;
            l->loop_top  = sv_top;
            break;
        }

        case NODE_FOR: {
            // Iterating an array: iter is (length uint, data ptr)
            LabelId top_id  = fresh_label(l);
            LabelId exit_id = fresh_label(l);
            LabelId sv_exit = l->loop_exit;
            LabelId sv_top  = l->loop_top;
            l->loop_exit    = exit_id;
            l->loop_top     = top_id;

            Val iter = lower_expr(l, n->forr.iter);

            // index = 0
            Var idx       = tmp(l, ty_uint);
            lscope_def(l, idx);
            IrInstr izero = mk(IR_COPY);
            izero.dst     = idx;
            izero.a       = val_imm(0);
            emit(l, izero);

            // len = iter[0]  (length field of the array struct)
            Var len = tmp(l, ty_uint);
            {
                IrInstr fa  = mk(IR_FIELD_ADDR);
                fa.dst      = tmp(l, NULL);
                fa.a        = iter;
                fa.field_off = 0;
                Var lp      = emit_dst(l, fa);
                IrInstr ld  = mk(IR_LOAD);
                ld.dst      = len;
                ld.a        = val_var(lp);
                emit(l, ld);
            }

            emit_label(l, top_id);

            // if idx >= len → exit
            IrInstr cmp = mk(IR_GTE);
            cmp.dst     = tmp(l, ty_bool);
            cmp.a       = val_var(idx);
            cmp.b       = val_var(len);
            Var cond    = emit_dst(l, cmp);
            {
                IrInstr j  = mk(IR_JIF);
                j.a        = val_var(cond);
                j.label_id = exit_id;
                emit(l, j);
            }

            lscope_push(l);

            if (n->forr.key) {
                Var kv    = (Var){ n->forr.key->ident, ty_uint };
                lscope_def(l, kv);
                IrInstr cp = mk(IR_COPY);
                cp.dst     = kv;
                cp.a       = val_var(idx);
                emit(l, cp);
            }

            // data_ptr = iter[8]
            Var data_ptr = tmp(l, NULL);
            {
                IrInstr fa   = mk(IR_FIELD_ADDR);
                fa.dst       = tmp(l, NULL);
                fa.a         = iter;
                fa.field_off = 8;
                Var dp       = emit_dst(l, fa);
                IrInstr ld   = mk(IR_LOAD);
                ld.dst       = data_ptr;
                ld.a         = val_var(dp);
                emit(l, ld);
            }

            // elem_addr = data_ptr + idx * elem_size
            int elem_sz = 8;
            if (n->forr.iter->type
                && n->forr.iter->type->kind == TY_ARRAY
                && n->forr.iter->type->array.inner)
                elem_sz = n->forr.iter->type->array.inner->size;
            IrInstr ia    = mk(IR_INDEX_ADDR);
            ia.dst        = tmp(l, NULL);
            ia.a          = val_var(data_ptr);
            ia.b          = val_var(idx);
            ia.index_scale = elem_sz;
            Var eaddr     = emit_dst(l, ia);

            // val = *elem_addr
            Type *val_ty = n->forr.val->type
                         ? n->forr.val->type : ty_word;
            Var vv       = (Var){ n->forr.val->ident, val_ty };
            lscope_def(l, vv);
            IrInstr ld2  = mk(IR_LOAD);
            ld2.dst      = vv;
            ld2.a        = val_var(eaddr);
            emit(l, ld2);

            lower_block(l, n->forr.body);
            lscope_pop(l);

            // idx++
            IrInstr inc = mk(IR_ADD);
            inc.dst     = idx;
            inc.a       = val_var(idx);
            inc.b       = val_imm(1);
            emit(l, inc);
            emit_jmp(l, top_id);
            emit_label(l, exit_id);
            l->loop_exit = sv_exit;
            l->loop_top  = sv_top;
            break;
        }

        case NODE_CASE: {
            LabelId end_id = fresh_label(l);
            Val subj       = lower_expr(l, n->casee.subject);
            for (int i = 0; i < n->casee.narms; i++) {
                Node   *arm  = n->casee.arms[i];
                LabelId next = fresh_label(l);
                if (arm->arm.pattern) {
                    IrInstr eq = mk(IR_EQ);
                    eq.dst     = tmp(l, ty_bool);
                    eq.a       = subj;
                    eq.b       = val_atom(arm->arm.pattern->atom.hash);
                    Var match  = emit_dst(l, eq);
                    IrInstr j  = mk(IR_JIFN);
                    j.a        = val_var(match);
                    j.label_id = next;
                    emit(l, j);
                }
                lscope_push(l);
                if (arm->arm.body->kind == NODE_BLOCK)
                    lower_block(l, arm->arm.body);
                else
                    lower_stmt(l, arm->arm.body);
                lscope_pop(l);
                emit_jmp(l, end_id);
                emit_label(l, next);
            }
            emit_label(l, end_id);
            break;
        }

        case NODE_EXIT:
            if (l->loop_exit == LABEL_INVALID) {
                fputs("lower: exit outside loop\n", stderr); abort();
            }
            emit_jmp(l, l->loop_exit);
            break;

        case NODE_BLOCK:
            lower_block(l, n);
            break;

        default:
            lower_expr(l, n);
            break;
    }
}

static void lower_block(Lower *l, Node *n) {
    if (!n) return;
    if (n->kind != NODE_BLOCK) { lower_stmt(l, n); return; }
    for (int i = 0; i < n->block.nstmts; i++)
        lower_stmt(l, n->block.stmts[i]);
}

// ─── procedure lowering ───────────────────────────────────────────────────────
static void lower_proc(Lower *l, Node *n) {
    l->proc = ir_proc_begin(l->p, n->proc.name.name,
                            n->proc.body ? 0 : 1);
    if (!n->proc.body) return;

    l->loop_exit = LABEL_INVALID;
    l->loop_top  = LABEL_INVALID;
    lscope_push(l);

    // input parameters: IR_PARAM copies from [bp+8+i*8] into a scalar slot
    Node *in = n->proc.in_tuple;
    if (in && in->kind == NODE_TYPE_TUPLE) {
        for (int i = 0; i < in->type_tuple.nfields; i++) {
            Node *fd = in->type_tuple.fields[i];
            Type *ft = fd->fielddecl.type
                     ? resolve_type(l->c, fd->fielddecl.type) : ty_word;
            Var v    = (Var){ fd->fielddecl.name, ft };
            lscope_def(l, v);
            IrInstr pi  = mk(IR_PARAM);
            pi.dst       = v;
            pi.param_idx = i;
            emit(l, pi);
        }
    }

    // output slots: plain scalar vars, zero-initialised implicitly
    Node *ou = n->proc.out_tuple;
    if (ou && ou->kind == NODE_TYPE_TUPLE) {
        for (int i = 0; i < ou->type_tuple.nfields; i++) {
            Node *fd = ou->type_tuple.fields[i];
            Type *ft = fd->fielddecl.type
                     ? resolve_type(l->c, fd->fielddecl.type) : ty_word;
            Var v    = (Var){ fd->fielddecl.name, ft };
            lscope_def(l, v);
        }
    }

    lower_block(l, n->proc.body);

    // IR_RET with output values
    IrInstr ret   = mk(IR_RET);
    ret.ret.nvals = 0;
    if (ou && ou->kind == NODE_TYPE_TUPLE) {
        int nr = ou->type_tuple.nfields;
        if (nr > IR_MAX_RETS) nr = IR_MAX_RETS;
        ret.ret.nvals = nr;
        for (int i = 0; i < nr; i++) {
            Node *fd = ou->type_tuple.fields[i];
            Var   v  = lscope_find(l, fd->fielddecl.name.hash);
            ret.ret.vals[i] = var_valid(v) ? val_var(v) : val_imm(0);
        }
    }
    emit(l, ret);
    lscope_pop(l);
}

// ─── ir_lower ─────────────────────────────────────────────────────────────────
void ir_lower(IrProg *p, Checker *c, Node *file) {
    if (file->kind != NODE_BLOCK) {
        fputs("lower: expected top-level block\n", stderr); abort();
    }
    Lower l;
    memset(&l, 0, sizeof(l));
    l.p         = p;
    l.c         = c;
    l.loop_exit = LABEL_INVALID;
    l.loop_top  = LABEL_INVALID;
    for (int i = 0; i < file->block.nstmts; i++) {
        Node *n = file->block.stmts[i];
        if (n->kind == NODE_PROC_DECL) {
            l.scope = NULL;
            lower_proc(&l, n);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// IR dump
// ═══════════════════════════════════════════════════════════════════════════════

static void dump_val(Val v, FILE *f) {
    switch (v.kind) {
        case VAL_VAR:  fprintf(f, "%s",
                           v.var.name.name ? v.var.name.name : "?"); break;
        case VAL_IMM:  fprintf(f, "%llu",
                           (unsigned long long)v.imm);               break;
        case VAL_FIMM: fprintf(f, "%g", v.fimm);                     break;
        case VAL_ATOM: fprintf(f, "atom(0x%llx)",
                           (unsigned long long)v.atom_hash);         break;
        case VAL_NONE: fprintf(f, "-");                               break;
        default:       fprintf(f, "?");                               break;
    }
}

static const char *irop_name(IrOp op) {
    switch (op) {
        case IR_PARAM:      return "param";
        case IR_CONST:      return "const";
        case IR_FCONST:     return "fconst";
        case IR_ATOM:       return "atom";
        case IR_COPY:       return "copy";
        case IR_ALLOCA:     return "alloca";
        case IR_ADDR:       return "addr";
        case IR_FIELD_ADDR: return "field_addr";
        case IR_INDEX_ADDR: return "index_addr";
        case IR_LOAD:       return "load";
        case IR_STORE:      return "store";
        case IR_MEMCPY:     return "memcpy";
        case IR_STR_INIT:   return "str_init";
        case IR_ADD:        return "add";
        case IR_SUB:        return "sub";
        case IR_MUL:        return "mul";
        case IR_IMUL:       return "imul";
        case IR_DIV:        return "div";
        case IR_IDIV:       return "idiv";
        case IR_MOD:        return "mod";
        case IR_NEG:        return "neg";
        case IR_AND:        return "and";
        case IR_OR:         return "or";
        case IR_XOR:        return "xor";
        case IR_NOT:        return "not";
        case IR_SHL:        return "shl";
        case IR_SHR:        return "shr";
        case IR_SAR:        return "sar";
        case IR_FADD:       return "fadd";
        case IR_FSUB:       return "fsub";
        case IR_FMUL:       return "fmul";
        case IR_FDIV:       return "fdiv";
        case IR_ITOF:       return "itof";
        case IR_FTOI:       return "ftoi";
        case IR_EQ:         return "eq";
        case IR_NEQ:        return "neq";
        case IR_LT:         return "lt";
        case IR_LTE:        return "lte";
        case IR_GT:         return "gt";
        case IR_GTE:        return "gte";
        case IR_FEQ:        return "feq";
        case IR_FNEQ:       return "fneq";
        case IR_FLT:        return "flt";
        case IR_FLTE:       return "flte";
        case IR_FGT:        return "fgt";
        case IR_FGTE:       return "fgte";
        case IR_LABEL:      return "label";
        case IR_JMP:        return "jmp";
        case IR_JIF:        return "jif";
        case IR_JIFN:       return "jifn";
        case IR_CALL:       return "call";
        case IR_RET:        return "ret";
        default:            return "?";
    }
}

static char *escape_bytes(const char *s, size_t len) {
    size_t olen = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\n': case '\t': case '\r':
            case '\b': case '\f': case '\v':
            case '\\': case '"':  case '\'':
                olen += 2; break;
            case '\0': olen += 4; break;
            default:
                olen += (c < 0x20 || c > 0x7e) ? 4 : 1;
                break;
        }
    }
    char *out = malloc(olen + 1);
    char *dst = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\n': *dst++='\\'; *dst++='n';  break;
            case '\t': *dst++='\\'; *dst++='t';  break;
            case '\r': *dst++='\\'; *dst++='r';  break;
            case '\b': *dst++='\\'; *dst++='b';  break;
            case '\f': *dst++='\\'; *dst++='f';  break;
            case '\v': *dst++='\\'; *dst++='v';  break;
            case '\\': *dst++='\\'; *dst++='\\'; break;
            case '"':  *dst++='\\'; *dst++='"';  break;
            case '\'': *dst++='\\'; *dst++='\''; break;
            case '\0': *dst++='\\'; *dst++='0';  break;
            default:
                if (c < 0x20 || c > 0x7e)
                    dst += sprintf(dst, "\\x%02x", c);
                else
                    *dst++ = (char)c;
                break;
        }
    }
    *dst = '\0';
    return out;
}

void ir_dump(IrProg *p, FILE *f) {
    for (int pi = 0; pi < p->nprocs; pi++) {
        IrProc *proc = &p->procs[pi];
        if (proc->is_extern) {
            fprintf(f, "extern %s\n\n", proc->name);
            continue;
        }
        fprintf(f, "proc %s\n", proc->name);
        for (int ii = 0; ii < proc->ninstr; ii++) {
            IrInstr *n = &proc->instrs[ii];
            if (n->op == IR_LABEL) {
                fprintf(f, "L%u:\n", n->label_id);
                continue;
            }
            fprintf(f, "    ");
            if (var_valid(n->dst))
                fprintf(f, "%s = ", n->dst.name.name);
            fprintf(f, "%s", irop_name(n->op));
            switch (n->op) {
                case IR_PARAM:
                    fprintf(f, " [%d]", n->param_idx);
                    break;
                case IR_ALLOCA:
                    fprintf(f, " %zu bytes", n->nbytes);
                    break;
                case IR_CONST:
                    fprintf(f, " %llu", (unsigned long long)n->a.imm);
                    break;
                case IR_FCONST:
                    fprintf(f, " %g", n->a.fimm);
                    break;
                case IR_ATOM:
                    fprintf(f, " 0x%llx",
                            (unsigned long long)n->a.atom_hash);
                    break;
                case IR_STR_INIT:
                    fprintf(f, " \"%s\"", n->str_label);
                    break;
                case IR_ADDR:
                    fprintf(f, " &");
                    dump_val(n->a, f);
                    break;
                case IR_FIELD_ADDR:
                    fprintf(f, " ");
                    dump_val(n->a, f);
                    fprintf(f, " +%lld", (long long)n->field_off);
                    break;
                case IR_INDEX_ADDR:
                    fprintf(f, " ");
                    dump_val(n->a, f);
                    fprintf(f, "[");
                    dump_val(n->b, f);
                    fprintf(f, "] scale=%d", n->index_scale);
                    break;
                case IR_MEMCPY:
                    fprintf(f, " ");
                    dump_val(n->a, f);
                    fprintf(f, " <- ");
                    dump_val(n->b, f);
                    fprintf(f, " (%zu bytes)", n->nbytes);
                    break;
                case IR_STORE:
                    fprintf(f, " *");
                    dump_val(n->a, f);
                    fprintf(f, " = ");
                    dump_val(n->b, f);
                    break;
                case IR_JMP:
                    fprintf(f, " L%u", n->label_id);
                    break;
                case IR_JIF:
                case IR_JIFN:
                    fprintf(f, " ");
                    dump_val(n->a, f);
                    fprintf(f, " -> L%u", n->label_id);
                    break;
                case IR_CALL:
                    fprintf(f, " ");
                    dump_val(n->call.callee, f);
                    fprintf(f, "(");
                    for (int k = 0; k < n->call.nargs; k++) {
                        if (k) fprintf(f, ", ");
                        dump_val(n->call.args[k], f);
                    }
                    fprintf(f, ")");
                    if (n->call.nrets > 0) {
                        fprintf(f, " -> (");
                        for (int k = 0; k < n->call.nrets; k++) {
                            if (k) fprintf(f, ", ");
                            fprintf(f, "%s",
                                    n->call.rets[k].name.name);
                        }
                        fprintf(f, ")");
                    }
                    break;
                case IR_RET:
                    if (n->ret.nvals > 0) {
                        fprintf(f, " ");
                        for (int k = 0; k < n->ret.nvals; k++) {
                            if (k) fprintf(f, ", ");
                            dump_val(n->ret.vals[k], f);
                        }
                    }
                    break;
                default:
                    if (n->a.kind != VAL_NONE) {
                        fprintf(f, " "); dump_val(n->a, f);
                    }
                    if (n->b.kind != VAL_NONE) {
                        fprintf(f, ", "); dump_val(n->b, f);
                    }
                    break;
            }
            fprintf(f, "\n");
        }
        fprintf(f, "\n");
    }
    if (p->strs) {
        fprintf(f, "; string literals\n");
        for (IrStr *s = p->strs; s; s = s->next) {
            char *esc = escape_bytes(s->data, s->len);
            fprintf(f, "  %s: len=%zu data=\"%s\"\n",
                    s->label, s->len, esc);
            free(esc);
        }
    }
}
