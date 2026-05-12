// ─── ir_codegen.c — IrProg → DVM assembly text ───────────────────────────────
//
// Register conventions:
//   ax  — primary GPR result / scratch
//   bx  — LHS operand
//   cx  — RHS operand
//   di  — address (pointer) scratch
//   si  — secondary address scratch
//   ex  — float ↔ GPR spill address (always points into the reserved scratch slot)
//   fp0 — float primary / result
//   fp1 — float secondary operand
//
// Stack frame (bp-relative):
//   [bp + 8 + i*8]  input parameter i   (pushed right-to-left by caller)
//   [bp + 0]        saved bp             (pushed by ENTER)
//   [bp - 8]        reserved float-spill scratch slot  ← NEVER a real var
//   [bp - 16] ...   locals (IR_ALLOCA / scalars), growing downward
//
// Variable model:
//   Scalar vars (IR_COPY dst, IR_PARAM dst, etc.) get an 8-byte slot.
//   IR_ALLOCA vars get a slot of exactly .nbytes (rounded up to 8).
//     Their slot IS the backing storage; their runtime value is that slot's
//     address.  Codegen for IR_ALLOCA:  lea dst_slot ← lea di,[bp+off]
//     then store di into dst_slot so later loads return the address.
//     IR_ADDR of an IR_ALLOCA var therefore returns the same address again.
//
// String layout (.cnst):
//   [label + 0]  dq length   (8 bytes)
//   [label + 8]  raw bytes   (len bytes + 1 null terminator)
//
// IR_STR_INIT fills two consecutive 8-byte words at the string var's slot:
//   [bp + slot]     = length
//   [bp + slot + 8] = pointer to data bytes   ← was slot−8, which was wrong
//
// Caller cleans args: add sp, nargs*8 after CALL.
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ═══════════════════════════════════════════════════════════════════════════════
// Growable output buffer
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct { char *buf; size_t len, cap; } SBuf;

static void sb_init(SBuf *b) {
    b->cap = 65536; b->len = 0;
    b->buf = malloc(b->cap); b->buf[0] = '\0';
}
static void sb_grow(SBuf *b, size_t need) {
    while (b->len + need + 1 > b->cap) {
        b->cap *= 2;
        b->buf  = realloc(b->buf, b->cap);
        if (!b->buf) { fputs("codegen: OOM\n", stderr); abort(); }
    }
}
static void cg_out(SBuf *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_grow(g, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(g->buf + g->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    g->len += (size_t)n;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stack slot map
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
    uint64_t hash;
    int      bp_offset;   // always negative: local is at [bp + bp_offset]
    int      size;        // bytes reserved (multiple of 8)
} Slot;

typedef struct {
    Slot  *slots;
    int    nslots, cap;
    int    next_offset;   // grows more negative with each allocation
    int    frame_size;    // = -next_offset, the total frame size
} SlotMap;

static void slotmap_init(SlotMap *m) { memset(m, 0, sizeof(*m)); }

static Slot *slotmap_find(SlotMap *m, uint64_t hash) {
    for (int i = 0; i < m->nslots; i++)
        if (m->slots[i].hash == hash) return &m->slots[i];
    return NULL;
}

// Reserve `size` bytes (rounded up to 8) for `hash`.
// If already present, returns existing slot (idempotent).
static Slot *slotmap_alloc(SlotMap *m, uint64_t hash, int size) {
    Slot *s = slotmap_find(m, hash);
    if (s) return s;
    size = (size + 7) & ~7;
    if (size < 8) size = 8;
    if (m->nslots >= m->cap) {
        m->cap   = m->cap ? m->cap * 2 : 16;
        m->slots = realloc(m->slots, sizeof(Slot) * (size_t)m->cap);
    }
    m->next_offset -= size;
    s            = &m->slots[m->nslots++];
    s->hash      = hash;
    s->bp_offset = m->next_offset;
    s->size      = size;
    if (-m->next_offset > m->frame_size)
        m->frame_size = -m->next_offset;
    return s;
}

// Allocate an 8-byte slot for a scalar Var.
static Slot *var_slot(SlotMap *m, Var v) {
    int sz = (v.type && v.type->size > 0) ? v.type->size : 8;
    // Clamp scalar slots to 8; aggregates go through slotmap_alloc directly.
    if (sz > 8) sz = 8;
    return slotmap_alloc(m, v.name.hash, sz);
}

// ═══════════════════════════════════════════════════════════════════════════════
// First pass: allocate all stack slots before emitting any code
// ═══════════════════════════════════════════════════════════════════════════════

// Sentinel hash for the reserved float-spill scratch word at [bp-8].
// FNV-1a of the empty string happens to be non-zero and will never collide
// with any real atom name.  We use 0xdeadbeefcafe0000 as a magic value.
#define FLOAT_SCRATCH_HASH UINT64_C(0xdeadbeefcafe0000)

static void alloc_slots(IrProc *proc, SlotMap *m) {
    // Reserve [bp-8] as the float-spill scratch.
    // This must be the first allocation so it lands at offset -8.
    slotmap_alloc(m, FLOAT_SCRATCH_HASH, 8);

    for (int i = 0; i < proc->ninstr; i++) {
        IrInstr *n = &proc->instrs[i];

        if (n->op == IR_ALLOCA) {
            // IR_ALLOCA: the dst var's slot IS the aggregate storage.
            // Allocate exactly nbytes (not just 8).
            if (var_valid(n->dst))
                slotmap_alloc(m, n->dst.name.hash, (int)n->nbytes);
            continue;
        }

        // All other instructions: dst gets an 8-byte scalar slot.
        if (var_valid(n->dst))
            var_slot(m, n->dst);

        // IR_CALL return slots
        if (n->op == IR_CALL) {
            for (int k = 0; k < n->call.nrets; k++)
                var_slot(m, n->call.rets[k]);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Load / store helpers
// ═══════════════════════════════════════════════════════════════════════════════

// Float scratch is at bp_offset == -8 (always, because we reserved it first).
#define FLOAT_SCRATCH_OFF (-8)

static void load_val(SBuf *g, SlotMap *m, Val v, const char *dst_reg) {
    switch (v.kind) {
        case VAL_VAR: {
            Slot *s = slotmap_find(m, v.var.name.hash);
            if (!s) {
                fprintf(stderr, "codegen: undefined var '%s'\n",
                        v.var.name.name ? v.var.name.name : "?");
                abort();
            }
            cg_out(g, "  mov %s, [bp%+d]\n", dst_reg, s->bp_offset);
            break;
        }
        case VAL_IMM:
            cg_out(g, "  mov %s, %llu\n", dst_reg,
                   (unsigned long long)v.imm);
            break;
        case VAL_ATOM:
            cg_out(g, "  mov %s, 0x%llx\n", dst_reg,
                   (unsigned long long)v.atom_hash);
            break;
        case VAL_FIMM:
            // Materialise float immediate through the reserved scratch slot.
            cg_out(g, "  fmovi fp0, %g\n", v.fimm);
            cg_out(g, "  lea ex, [bp%+d]\n", FLOAT_SCRATCH_OFF);
            cg_out(g, "  fmovs [ex], fp0\n");
            cg_out(g, "  mov %s, [bp%+d]\n", dst_reg, FLOAT_SCRATCH_OFF);
            break;
        default:
            fprintf(stderr, "codegen: load_val: bad val kind %d\n", v.kind);
            abort();
    }
}

static void load_val_f(SBuf *g, SlotMap *m, Val v, const char *fdst) {
    if (v.kind == VAL_FIMM) {
        cg_out(g, "  fmovi %s, %g\n", fdst, v.fimm);
        return;
    }
    if (v.kind == VAL_VAR) {
        Slot *s = slotmap_find(m, v.var.name.hash);
        if (s) {
            cg_out(g, "  lea ex, [bp%+d]\n", s->bp_offset);
            cg_out(g, "  fmovm %s, [ex]\n", fdst);
            return;
        }
    }
    // Fallback: load as integer, convert.
    load_val(g, m, v, "ax");
    cg_out(g, "  itof %s, ax\n", fdst);
}

// Store a GPR value into a var's scalar slot (8 bytes).
static void store_var(SBuf *g, SlotMap *m, Var dst, const char *src_reg) {
    Slot *s = var_slot(m, dst);
    cg_out(g, "  mov [bp%+d], %s\n", s->bp_offset, src_reg);
}

// Store a float register into a var's scalar slot via the scratch pointer.
static void store_var_f(SBuf *g, SlotMap *m, Var dst, const char *fsrc) {
    Slot *s = var_slot(m, dst);
    cg_out(g, "  lea ex, [bp%+d]\n", s->bp_offset);
    cg_out(g, "  fmovs [ex], %s\n", fsrc);
}

static int ty_float(Type *t)  { return t && t->kind == TY_FLOAT; }
static int ty_signed(Type *t) {
    if (!t || t->kind != TY_INT) return 1;
    switch (t->int_kind) {
        case INT_KIND_UINT: case INT_KIND_U8:  case INT_KIND_U16:
        case INT_KIND_U32:  case INT_KIND_U64: return 0;
        default: return 1;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Single-instruction emitter
// ═══════════════════════════════════════════════════════════════════════════════

// Global counters for inline comparison labels (per codegen run).
// These are fine as file-statics: each ir_codegen() call starts fresh
// programs, and label names embed the counter so they never collide.
static int g_cmp_ctr  = 0;
static int g_fcmp_ctr = 0;

static void emit_instr(SBuf *g, SlotMap *m, IrInstr *n) {
    switch (n->op) {

        // ── IR_ALLOCA ─────────────────────────────────────────────────────────
        // The dst var's slot already has `nbytes` reserved by alloc_slots.
        // At runtime the var's value must equal the base address of that slot,
        // so we emit:  lea di, [bp+slot]  then store di into the slot itself.
        // After this, any load of the var gives back the slot's address.
        case IR_ALLOCA: {
            if (!var_valid(n->dst)) break;
            Slot *s = slotmap_find(m, n->dst.name.hash);
            if (!s) {
                fprintf(stderr, "codegen: IR_ALLOCA: no slot for '%s'\n",
                        n->dst.name.name ? n->dst.name.name : "?");
                abort();
            }
            cg_out(g, "  lea di, [bp%+d]\n", s->bp_offset);
            cg_out(g, "  mov [bp%+d], di\n", s->bp_offset);
            break;
        }

        // ── IR_PARAM ──────────────────────────────────────────────────────────
        case IR_PARAM: {
            Slot *s = var_slot(m, n->dst);
            // Parameters are at [bp + 8 + param_idx*8] (above saved bp).
            cg_out(g, "  mov ax, [bp%+d]\n", 8 + n->param_idx * 8);
            cg_out(g, "  mov [bp%+d], ax\n", s->bp_offset);
            break;
        }

        // ── constants ─────────────────────────────────────────────────────────
        case IR_CONST:
            if (var_valid(n->dst)) {
                cg_out(g, "  mov ax, %llu\n",
                       (unsigned long long)n->a.imm);
                store_var(g, m, n->dst, "ax");
            }
            break;

        case IR_FCONST:
            if (var_valid(n->dst)) {
                cg_out(g, "  fmovi fp0, %g\n", n->a.fimm);
                store_var_f(g, m, n->dst, "fp0");
            }
            break;

        case IR_ATOM:
            if (var_valid(n->dst)) {
                cg_out(g, "  mov ax, 0x%llx\n",
                       (unsigned long long)n->a.atom_hash);
                store_var(g, m, n->dst, "ax");
            }
            break;

        // ── IR_COPY (scalar) ──────────────────────────────────────────────────
        case IR_COPY: {
            if (!var_valid(n->dst)) break;
            if (ty_float(n->dst.type)) {
                load_val_f(g, m, n->a, "fp0");
                store_var_f(g, m, n->dst, "fp0");
            } else {
                load_val(g, m, n->a, "ax");
                store_var(g, m, n->dst, "ax");
            }
            break;
        }

        // ── IR_ADDR: dst = &var ───────────────────────────────────────────────
        // Looks up the slot for the operand var and emits `lea di, [bp+off]`.
        // Works for both scalar vars and IR_ALLOCA vars (which have larger
        // slots); the address is always the slot's base address.
        case IR_ADDR: {
            if (!var_valid(n->dst)) break;
            if (n->a.kind != VAL_VAR) {
                fprintf(stderr, "codegen: IR_ADDR requires VAL_VAR\n");
                abort();
            }
            Slot *s = slotmap_find(m, n->a.var.name.hash);
            if (!s) {
                fprintf(stderr, "codegen: IR_ADDR: no slot for '%s'\n",
                        n->a.var.name.name ? n->a.var.name.name : "?");
                abort();
            }
            cg_out(g, "  lea di, [bp%+d]\n", s->bp_offset);
            store_var(g, m, n->dst, "di");
            break;
        }

        // ── IR_FIELD_ADDR: dst = base_ptr + offset ────────────────────────────
        case IR_FIELD_ADDR: {
            if (!var_valid(n->dst)) break;
            load_val(g, m, n->a, "di");
            if (n->field_off != 0)
                cg_out(g, "  lea di, [di%+lld]\n",
                       (long long)n->field_off);
            store_var(g, m, n->dst, "di");
            break;
        }

        // ── IR_INDEX_ADDR: dst = base_ptr + idx * scale ───────────────────────
        case IR_INDEX_ADDR: {
            if (!var_valid(n->dst)) break;
            load_val(g, m, n->a, "di");
            load_val(g, m, n->b, "bx");
            int scale = n->index_scale > 0 ? n->index_scale : 8;
            if (scale != 1) {
                cg_out(g, "  mov cx, %d\n", scale);
                cg_out(g, "  mul bx, cx\n");
            }
            cg_out(g, "  add di, bx\n");
            store_var(g, m, n->dst, "di");
            break;
        }

        // ── IR_LOAD: dst = *ptr ───────────────────────────────────────────────
        case IR_LOAD: {
            if (!var_valid(n->dst)) break;
            load_val(g, m, n->a, "di");
            if (ty_float(n->dst.type)) {
                cg_out(g, "  fmovm fp0, [di]\n");
                store_var_f(g, m, n->dst, "fp0");
            } else {
                cg_out(g, "  mov ax, [di]\n");
                store_var(g, m, n->dst, "ax");
            }
            break;
        }

        // ── IR_STORE: *ptr = val ──────────────────────────────────────────────
        case IR_STORE: {
            load_val(g, m, n->a, "si");   // pointer → si
            if (n->b.kind == VAL_VAR && ty_float(n->b.var.type)) {
                load_val_f(g, m, n->b, "fp0");
                cg_out(g, "  fmovs [si], fp0\n");
            } else {
                load_val(g, m, n->b, "ax");
                cg_out(g, "  mov [si], ax\n");
            }
            break;
        }

        // ── IR_MEMCPY: word-granular copy ─────────────────────────────────────
        // For aggregates up to 512 bytes we unroll word-by-word.
        // Larger aggregates require a loop (emit a local label using the global
        // comparison counter, which is cheap and collision-free).
        case IR_MEMCPY: {
            load_val(g, m, n->a, "di");   // dst ptr
            load_val(g, m, n->b, "si");   // src ptr
            size_t nb = n->nbytes;
            if (nb == 0) break;
            if ((nb % 8) != 0) {
                fprintf(stderr,
                    "codegen: IR_MEMCPY nbytes=%zu not word-aligned\n",
                    nb);
                abort();
            }
            if (nb <= 512) {
                for (size_t off = 0; off < nb; off += 8) {
                    cg_out(g, "  mov ax, [si+%zu]\n", off);
                    cg_out(g, "  mov [di+%zu], ax\n", off);
                }
            } else {
                // Emit a counted loop using a unique inline label.
                int id = g_cmp_ctr++;
                cg_out(g, "  mov cx, %zu\n", nb / 8);  // word count
                cg_out(g, "  mov bx, 0\n");             // offset counter
                cg_out(g, "cpy_top_%d:\n", id);
                cg_out(g, "  mov ax, [si]\n");
                cg_out(g, "  mov [di], ax\n");
                cg_out(g, "  lea si, [si+8]\n");
                cg_out(g, "  lea di, [di+8]\n");
                cg_out(g, "  sub cx, 1\n");
                cg_out(g, "  jne cpy_top_%d\n", id);
            }
            break;
        }

        // ── IR_STR_INIT ───────────────────────────────────────────────────────
        // String var layout:
        //   [slot + 0]  = length  (uint)
        //   [slot + 8]  = data pointer  (u8!)
        // cnst section layout:
        //   [label + 0] = dq length
        //   [label + 8] = raw byte data
        case IR_STR_INIT: {
            if (!var_valid(n->dst)) break;
            Slot *s = slotmap_find(m, n->dst.name.hash);
            if (!s) {
                // IR_STR_INIT on a string var that went through IR_ALLOCA
                // in lower_stmt — the slot should exist.
                fprintf(stderr, "codegen: IR_STR_INIT: no slot\n");
                abort();
            }
            cg_out(g, "  mov si, %s\n", n->str_label);
            // length word
            cg_out(g, "  mov ax, [si]\n");
            cg_out(g, "  mov [bp%+d], ax\n", s->bp_offset);
            // data pointer: address of bytes (8 bytes past label)
            cg_out(g, "  lea ax, [si+8]\n");
            cg_out(g, "  mov [bp%+d], ax\n", s->bp_offset + 8);
            break;
        }

        // ── integer binary ops ────────────────────────────────────────────────
        case IR_ADD:  case IR_SUB:
        case IR_MUL:  case IR_IMUL:
        case IR_DIV:  case IR_IDIV:
        case IR_MOD:
        case IR_AND:  case IR_OR:   case IR_XOR:
        case IR_SHL:  case IR_SHR:  case IR_SAR:
        case IR_EQ:   case IR_NEQ:
        case IR_LT:   case IR_LTE:
        case IR_GT:   case IR_GTE: {
            if (!var_valid(n->dst)) break;
            load_val(g, m, n->a, "bx");
            load_val(g, m, n->b, "cx");

            int is_cmp = (n->op >= IR_EQ && n->op <= IR_GTE);
            if (!is_cmp) {
                int sgn = ty_signed(n->dst.type);
                const char *mnem;
                switch (n->op) {
                    case IR_ADD:  mnem = "add";                       break;
                    case IR_SUB:  mnem = "sub";                       break;
                    case IR_MUL:  mnem = sgn ? "imul" : "mul";        break;
                    case IR_IMUL: mnem = "imul";                      break;
                    case IR_DIV:  mnem = sgn ? "idiv" : "div";        break;
                    case IR_IDIV: mnem = "idiv";                      break;
                    case IR_MOD:  mnem = "mod";                       break;
                    case IR_AND:  mnem = "and";                       break;
                    case IR_OR:   mnem = "or";                        break;
                    case IR_XOR:  mnem = "xor";                       break;
                    case IR_SHL:  mnem = "shl";                       break;
                    case IR_SHR:  mnem = "shr";                       break;
                    case IR_SAR:  mnem = "sar";                       break;
                    default:      mnem = "add";                       break;
                }
                cg_out(g, "  %s bx, cx\n", mnem);
                cg_out(g, "  mov ax, bx\n");
            } else {
                int id = g_cmp_ctr++;
                const char *jop;
                switch (n->op) {
                    case IR_EQ:  jop = "je";  break;
                    case IR_NEQ: jop = "jne"; break;
                    case IR_LT:  jop = "jl";  break;
                    case IR_LTE: jop = "jle"; break;
                    case IR_GT:  jop = "jg";  break;
                    case IR_GTE: jop = "jge"; break;
                    default:     jop = "je";  break;
                }
                cg_out(g, "  cmp bx, cx\n");
                cg_out(g, "  %s cmp_t_%d\n", jop, id);
                cg_out(g, "  mov ax, 0\n");
                cg_out(g, "  jmp cmp_e_%d\n", id);
                cg_out(g, "cmp_t_%d:\n", id);
                cg_out(g, "  mov ax, 1\n");
                cg_out(g, "cmp_e_%d:\n", id);
            }
            store_var(g, m, n->dst, "ax");
            break;
        }

        // ── unary ops ─────────────────────────────────────────────────────────
        case IR_NEG: case IR_NOT: {
            if (!var_valid(n->dst)) break;
            load_val(g, m, n->a, "ax");
            cg_out(g, "  %s ax\n", n->op == IR_NEG ? "neg" : "not");
            store_var(g, m, n->dst, "ax");
            break;
        }

        // ── float binary ops ──────────────────────────────────────────────────
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
        case IR_FEQ:  case IR_FNEQ:
        case IR_FLT:  case IR_FLTE:
        case IR_FGT:  case IR_FGTE: {
            if (!var_valid(n->dst)) break;
            load_val_f(g, m, n->a, "fp0");
            load_val_f(g, m, n->b, "fp1");
            int is_fcmp = (n->op >= IR_FEQ);
            if (!is_fcmp) {
                const char *mnem;
                switch (n->op) {
                    case IR_FADD: mnem = "fadd"; break;
                    case IR_FSUB: mnem = "fsub"; break;
                    case IR_FMUL: mnem = "fmul"; break;
                    case IR_FDIV: mnem = "fdiv"; break;
                    default:      mnem = "fadd"; break;
                }
                cg_out(g, "  %s fp0, fp1\n", mnem);
                store_var_f(g, m, n->dst, "fp0");
            } else {
                int id = g_fcmp_ctr++;
                const char *jop;
                switch (n->op) {
                    case IR_FEQ:  jop = "je";  break;
                    case IR_FNEQ: jop = "jne"; break;
                    case IR_FLT:  jop = "jl";  break;
                    case IR_FLTE: jop = "jle"; break;
                    case IR_FGT:  jop = "jg";  break;
                    case IR_FGTE: jop = "jge"; break;
                    default:      jop = "je";  break;
                }
                cg_out(g, "  fcmp fp0, fp1\n");
                cg_out(g, "  %s fcmp_t_%d\n", jop, id);
                cg_out(g, "  mov ax, 0\n");
                cg_out(g, "  jmp fcmp_e_%d\n", id);
                cg_out(g, "fcmp_t_%d:\n", id);
                cg_out(g, "  mov ax, 1\n");
                cg_out(g, "fcmp_e_%d:\n", id);
                store_var(g, m, n->dst, "ax");
            }
            break;
        }

        // ── float conversions ─────────────────────────────────────────────────
        case IR_ITOF: {
            if (!var_valid(n->dst)) break;
            load_val(g, m, n->a, "ax");
            cg_out(g, "  itof fp0, ax\n");
            store_var_f(g, m, n->dst, "fp0");
            break;
        }
        case IR_FTOI: {
            if (!var_valid(n->dst)) break;
            load_val_f(g, m, n->a, "fp0");
            cg_out(g, "  ftoi ax, fp0\n");
            store_var(g, m, n->dst, "ax");
            break;
        }

        // ── control flow ──────────────────────────────────────────────────────
        case IR_LABEL:
            cg_out(g, "L%u:\n", n->label_id);
            break;
        case IR_JMP:
            cg_out(g, "  jmp L%u\n", n->label_id);
            break;
        case IR_JIF:
            load_val(g, m, n->a, "ax");
            cg_out(g, "  mov bx, 0\n");
            cg_out(g, "  cmp ax, bx\n");
            cg_out(g, "  jne L%u\n", n->label_id);
            break;
        case IR_JIFN:
            load_val(g, m, n->a, "ax");
            cg_out(g, "  mov bx, 0\n");
            cg_out(g, "  cmp ax, bx\n");
            cg_out(g, "  je L%u\n", n->label_id);
            break;

        // ── IR_CALL ───────────────────────────────────────────────────────────
        case IR_CALL: {
            // Push args right-to-left.
            for (int k = n->call.nargs - 1; k >= 0; k--) {
                load_val(g, m, n->call.args[k], "ax");
                cg_out(g, "  push ax\n");
            }
            Val callee = n->call.callee;
            if (callee.kind == VAL_VAR) {
                Slot *s = slotmap_find(m, callee.var.name.hash);
                if (s) {
                    // Proc pointer in a local slot → indirect call
                    cg_out(g, "  mov ax, [bp%+d]\n", s->bp_offset);
                    cg_out(g, "  callr ax, mol_err_stub\n");
                } else {
                    // Global symbol → direct call
                    cg_out(g, "  call %s, mol_err_stub\n",
                           callee.var.name.name);
                }
            } else {
                load_val(g, m, callee, "ax");
                cg_out(g, "  callr ax, mol_err_stub\n");
            }
            // Caller pops args.
            if (n->call.nargs > 0)
                cg_out(g, "  mov bx, %d\n  add sp, bx\n",
                       n->call.nargs * 8);
            // First return value arrives in ax.
            if (n->call.nrets > 0)
                store_var(g, m, n->call.rets[0], "ax");
            // Additional return values: future ABI work (out-pointer).
            break;
        }

        // ── IR_RET ────────────────────────────────────────────────────────────
        case IR_RET: {
            if (n->ret.nvals > 0) {
                Val rv = n->ret.vals[0];
                if (rv.kind == VAL_VAR && ty_float(rv.var.type)) {
                    load_val_f(g, m, rv, "fp0");
                    // Spill fp0 to scratch, load into ax for the return ABI.
                    cg_out(g, "  lea ex, [bp%+d]\n", FLOAT_SCRATCH_OFF);
                    cg_out(g, "  fmovs [ex], fp0\n");
                    cg_out(g, "  mov ax, [bp%+d]\n", FLOAT_SCRATCH_OFF);
                } else {
                    load_val(g, m, rv, "ax");
                }
            }
            cg_out(g, "  leave\n  ret\n");
            break;
        }

        default:
            fprintf(stderr, "codegen: unhandled IR op %d\n", n->op);
            abort();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Emit string literals to .cnst section
// ═══════════════════════════════════════════════════════════════════════════════

static void emit_strs(SBuf *g, IrProg *p) {
    if (!p->strs) return;
    // Collect into forward-order array (list is built in reverse).
    int n = 0;
    for (IrStr *s = p->strs; s; s = s->next) n++;
    IrStr **a = malloc(sizeof(IrStr *) * (size_t)n);
    int i = n - 1;
    for (IrStr *s = p->strs; s; s = s->next) a[i--] = s;
    cg_out(g, "\n.cnst\n");
    for (int j = 0; j < n; j++) {
        IrStr *s = a[j];
        cg_out(g, "global %s\n%s:\n", s->label, s->label);
        cg_out(g, "  dq %zu\n", s->len);       // [label+0] = length
        for (size_t k = 0; k < s->len; k++)    // [label+8] = bytes
            cg_out(g, "  db %u\n", (unsigned char)s->data[k]);
        cg_out(g, "  db 0\n");                  // null terminator
    }
    free(a);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Emit a procedure
// ═══════════════════════════════════════════════════════════════════════════════

static void emit_proc(SBuf *g, IrProc *proc) {
    SlotMap m;
    slotmap_init(&m);

    // First pass: allocate all slots (including IR_ALLOCA with full sizes
    // and the float-scratch sentinel).
    alloc_slots(proc, &m);

    cg_out(g, "\nglobal %s\n%s:\n", proc->name, proc->name);

    // Emit ENTER with frame size placeholder; backpatch after body.
    // Format: "  enter NNNN\n"
    //          0123456789   — "  enter " is 8 chars
    size_t enter_pos = g->len;
    cg_out(g, "  enter 0000\n");

    for (int i = 0; i < proc->ninstr; i++)
        emit_instr(g, &m, &proc->instrs[i]);

    // Backpatch the frame size (4 digits, left-justified with spaces).
    char patch[8];
    snprintf(patch, sizeof(patch), "%-4d", m.frame_size);
    memcpy(g->buf + enter_pos + 8, patch, 4);

    free(m.slots);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ir_codegen entry point
// ═══════════════════════════════════════════════════════════════════════════════

const char *ir_codegen(IrProg *p) {
    // Reset global label counters for this compilation unit.
    g_cmp_ctr  = 0;
    g_fcmp_ctr = 0;

    SBuf g;
    sb_init(&g);
    cg_out(&g, "; Mol generated assembly\n.text\n");
    cg_out(&g, "\nglobal mol_err_stub\nmol_err_stub:\n  halt\n");

    for (int i = 0; i < p->nprocs; i++)
        if (p->procs[i].is_extern)
            cg_out(&g, "extern %s\n", p->procs[i].name);

    for (int i = 0; i < p->nprocs; i++)
        if (!p->procs[i].is_extern)
            emit_proc(&g, &p->procs[i]);

    emit_strs(&g, p);
    cg_out(&g, "\n; end\n");
    return g.buf;
}
