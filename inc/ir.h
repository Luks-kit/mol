#pragma once
// ─── ir.h — Mol intermediate representation ───────────────────────────────────
//
// Three-address IR between the typed AST and DVM assembly.
//
// Abstraction level (MIR):
//   - Variables are named (Atom) with resolved types
//   - Memory is modelled as explicit address computation + load/store
//   - Control flow is explicit: numeric label IDs + conditional jumps
//   - Calls carry their argument list inline
//   - No physical registers, no stack layout, no DVM opcodes
//
// Memory model — five primitives, everything else composes:
//   IR_ADDR        t = &var              (address of a named variable)
//   IR_FIELD_ADDR  t = &base + offset    (field address; offset resolved)
//   IR_INDEX_ADDR  t = &base[i]          (element address)
//   IR_LOAD        t = *ptr              (word-sized load)
//   IR_STORE       *ptr = val            (word-sized store)
//
// Aggregate copy (records, arrays):
//   IR_MEMCPY      dst_ptr src_ptr nbytes
//
// IR_COPY is scalar word copy only (one register's worth of bits).

#include "arena.h"
#include "ast.h"
#include "check.h"
#include <stdint.h>
#include <stdio.h>

// ─── Label ID ─────────────────────────────────────────────────────────────────

typedef uint32_t LabelId;
#define LABEL_INVALID UINT32_MAX

// ─── Variable ─────────────────────────────────────────────────────────────────
// Named storage with a resolved type. Temporaries get compiler-generated names.

typedef struct {
    Atom  name;
    Type *type;   // NULL = untyped word
} Var;

#define VAR_NULL ((Var){(Atom){0,NULL},NULL})
static inline int var_valid(Var v) { return v.name.hash != 0 || v.name.name != NULL; }

// ─── Value operand ────────────────────────────────────────────────────────────
// An operand is a variable, an integer immediate, a float immediate, or an
// atom hash. Labels are not values — they appear directly on jump instructions.

typedef enum {
    VAL_VAR,    // named variable
    VAL_IMM,    // integer immediate (u64)
    VAL_FIMM,   // float immediate (double)
    VAL_ATOM,   // atom hash (u64) — distinct from IMM for clarity
    VAL_NONE,   // unused slot
} ValKind;

typedef struct {
    ValKind  kind;
    union {
        Var      var;
        uint64_t imm;
        double   fimm;
        uint64_t atom_hash;
    };
} Val;

static inline Val val_var (Var v)        { Val o; o.kind=VAL_VAR;  o.var=v;       return o; }
static inline Val val_imm (uint64_t v)   { Val o; o.kind=VAL_IMM;  o.imm=v;       return o; }
static inline Val val_fimm(double v)     { Val o; o.kind=VAL_FIMM; o.fimm=v;      return o; }
static inline Val val_atom(uint64_t h)   { Val o; o.kind=VAL_ATOM; o.atom_hash=h; return o; }
static inline Val val_none(void)         { Val o; o.kind=VAL_NONE;                return o; }

// ─── IR opcodes ───────────────────────────────────────────────────────────────

typedef enum {

    // ── proc parameters ───────────────────────────────────────────────────────
    // dst = value of parameter at index .param_idx.
    // Codegen maps this to [bp + 8 + param_idx*8].
    IR_PARAM,
    IR_CONST,

    IR_FCONST,      // dst = fimm (double)
    IR_ATOM,        // dst = atom_hash (u64)

    // ── scalar word copy ──────────────────────────────────────────────────────
    // One word (8 bytes). NOT for aggregates — use IR_MEMCPY for those.
    IR_COPY,        // dst = src

    // ── address computation ───────────────────────────────────────────────────
    IR_ADDR,        // dst = &var                  (a = var being addressed)
    IR_FIELD_ADDR,  // dst = base_ptr + field_off  (a = base ptr, .field_off)
    IR_INDEX_ADDR,  // dst = base_ptr + idx*base_sz      (a = base ptr, b = idx)

    // ── memory access ─────────────────────────────────────────────────────────
    IR_LOAD,        // dst = *ptr                  (a = ptr var)
    IR_STORE,       // *ptr = val                  (a = ptr var, b = value)
    
    IR_ALLOCA,   // dst = alloca(nbytes) — reserves stack space, dst holds the address
    // ── aggregate copy ────────────────────────────────────────────────────────
    // Copies nbytes from src_ptr to dst_ptr. Used for record/array assignment.
    // dst_ptr in a, src_ptr in b, byte count in .nbytes.
    IR_MEMCPY,

    // ── string literal initialisation ─────────────────────────────────────────
    // Initialises a string variable (2-field record) from a cnst literal.
    // dst = the string var. .str_label = cnst label.
    // cnst layout: [label+0]=dq length, [label+8]=raw bytes, then db 0.
    // Lowering emits:
    //   t0 = load [str_label + 0]   (length)
    //   store &dst.length, t0
    //   t1 = field_addr str_label, 8  (data pointer)
    //   store &dst.data, t1
    IR_STR_INIT,

    // ── integer arithmetic ────────────────────────────────────────────────────
    IR_ADD,   IR_SUB,
    IR_MUL,   IR_IMUL,   // unsigned / signed
    IR_DIV,   IR_IDIV,
    IR_MOD,
    IR_NEG,
    IR_AND,   IR_OR,   IR_XOR,   IR_NOT,
    IR_SHL,   IR_SHR,  IR_SAR,

    // ── float arithmetic ──────────────────────────────────────────────────────
    IR_FADD,  IR_FSUB,  IR_FMUL,  IR_FDIV,
    IR_ITOF,            // dst(float) = (float)src(int)
    IR_FTOI,            // dst(int)   = (int)src(float)

    // ── comparison → 0 or 1 ──────────────────────────────────────────────────
    IR_EQ,  IR_NEQ,
    IR_LT,  IR_LTE,
    IR_GT,  IR_GTE,
    // float comparisons
    IR_FEQ, IR_FNEQ,
    IR_FLT, IR_FLTE,
    IR_FGT, IR_FGTE,

    // ── control flow ──────────────────────────────────────────────────────────
    IR_LABEL,   // label definition; .label_id = the ID being defined here
    IR_JMP,     // unconditional jump; .label_id = target
    IR_JIF,     // jump if a != 0;    .label_id = target
    IR_JIFN,    // jump if a == 0;    .label_id = target

    // ── calls ─────────────────────────────────────────────────────────────────
    // Arguments live inline in .call.args[]. Callee is a Val:
    //   VAL_VAR with type TY_PROC = named global/extern proc (direct call)
    //   VAL_VAR with type TY_PTR  = proc pointer (indirect call)
    // Return values live in .call.rets[]; nrets=0 for void.
    IR_CALL,

    // ── return ────────────────────────────────────────────────────────────────
    // Returns zero or more values (matching proc out_tuple).
    IR_RET,

} IrOp;

// ─── IR instruction ───────────────────────────────────────────────────────────

#define IR_MAX_ARGS 16
#define IR_MAX_RETS  8

typedef struct {
    IrOp op;

    // destination variable (VAR_NULL if instruction produces no value,
    // or if result is in .call.rets[] for IR_CALL)
    Var dst;

    // source operands
    Val a;
    Val b;

    // op-specific payload
    union {
        // IR_FIELD_ADDR
        int64_t  field_off;

        // IR_INDEX_ADDR: element size in bytes (derived from array element type)
        int      index_scale;

        // IR_PARAM: index into the proc's input parameter list
        int      param_idx;

        // IR_MEMCPY
        size_t   nbytes;

        // IR_STR_INIT
        char     str_label[64];

        // IR_LABEL / IR_JMP / IR_JIF / IR_JIFN
        LabelId  label_id;

        // IR_CALL
        struct {
            Val  callee;
            Val  args[IR_MAX_ARGS];
            int  nargs;
            Var  rets[IR_MAX_RETS];
            int  nrets;
        } call;

        // IR_RET
        struct {
            Val  vals[IR_MAX_RETS];
            int  nvals;
        } ret;
    };
} IrInstr;

// ─── IR procedure ─────────────────────────────────────────────────────────────

typedef struct {
    char      name[64];
    int       is_extern;   // 1 = no body, emit 'extern' in asm
    IrInstr  *instrs;
    int       ninstr;
    int       cap;
} IrProc;

// ─── String literal ───────────────────────────────────────────────────────────

typedef struct IrStr IrStr;
struct IrStr {
    char    label[64];
    char   *data;       // raw interpreted bytes (arena-owned)
    size_t  len;
    IrStr  *next;
};

// ─── IR program ───────────────────────────────────────────────────────────────

typedef struct {
    Arena   *arena;
    IrProc  *procs;
    int      nprocs;
    int      proc_cap;
    int      tmp_counter;    // temp variable name counter
    LabelId  label_counter;  // label ID counter
    IrStr   *strs;
} IrProg;

// ─── Builder API ──────────────────────────────────────────────────────────────

// Initialise an IR program backed by the given arena.
void ir_init(IrProg *p, Arena *arena);

// Allocate a fresh temporary variable with the given type.
Var ir_tmp(IrProg *p, Type *type);

// Allocate a fresh label ID.
LabelId ir_label(IrProg *p);

// Register a string literal; returns its cnst label.
const char *ir_str_lit(IrProg *p, const char *data, size_t len);

// Begin a new procedure. Returns pointer to it (stable — arena-allocated).
IrProc *ir_proc_begin(IrProg *p, const char *name, int is_extern);

// Append an instruction to a procedure.
void ir_emit(IrProc *proc, Arena *arena, IrInstr instr);

// Append an instruction and return dst (convenience for expressions).
Var ir_emit_dst(IrProc *proc, Arena *arena, IrInstr instr);

// ─── Lowering: typed AST → IrProg ────────────────────────────────────────────
void ir_lower(IrProg *p, Checker *c, Node *file);

// ─── Debug dump: print IR to FILE ────────────────────────────────────────────
void ir_dump(IrProg *p, FILE *f);

// ─── Codegen: IrProg → DVM assembly text ─────────────────────────────────────
const char *ir_codegen(IrProg *p);
