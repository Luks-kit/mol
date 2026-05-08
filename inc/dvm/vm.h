// vm.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

// ─── Register IDs ────────────────────────────────────────────────────────────

#define REG_AX 0
#define REG_BX 1
#define REG_CX 2
#define REG_DX 3
#define REG_DI 4
#define REG_SI 5
#define REG_EX 6
#define REG_FX 7
#define REG_BP 8
#define REG_SP 9

// ─── Flags ───────────────────────────────────────────────────────────────────

#define FL_ZF (1ULL << 0)
#define FL_SF (1ULL << 1)
#define FL_CF (1ULL << 2)
#define FL_OF (1ULL << 3)

// ─── Opcodes ─────────────────────────────────────────────────────────────────

typedef enum __attribute__((packed)) {
    OP_MOV_RR = 0x00,
    OP_MOV_RI,
    OP_MOV_RM,
    OP_MOV_MR,

    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_IMUL,
    OP_DIV,
    OP_IDIV,
    OP_MOD,
    OP_NEG,

    OP_AND,
    OP_OR,
    OP_XOR,
    OP_NOT,
    OP_SHL,
    OP_SHR,
    OP_SAR,

    OP_CMP,
    OP_TEST,

    OP_JMP,
    OP_JE,
    OP_JNE,
    OP_JL,
    OP_JLE,
    OP_JG,
    OP_JGE,

    OP_PUSH,
    OP_POP,
    OP_ENTER,
    OP_LEAVE,

    OP_CALL,
    OP_RET,
    OP_THROW,

    OP_FADD,
    OP_FSUB,
    OP_FMUL,
    OP_FDIV,
    OP_FCMP,

    OP_FMOV_RR,
    OP_FMOV_RI,
    OP_FMOV_RM,
    OP_FMOV_MR,

    OP_ITOF,
    OP_FTOI,

    OP_SYSCALL,
    OP_HALT,

    OP_MOVZX_RM8,
    OP_MOVZX_RM16,
    OP_MOVZX_RM32,

    OP_MOVSX_RM8,
    OP_MOVSX_RM16,
    OP_MOVSX_RM32,

    OP_MOV_MR8,
    OP_MOV_MR16,
    OP_MOV_MR32,

    OP_MOV_RM_OFF,
    OP_MOV_MR_OFF,

    OP_MOVZX_RM8_OFF,
    OP_MOVZX_RM16_OFF,
    OP_MOVZX_RM32_OFF,

    OP_MOVSX_RM8_OFF,
    OP_MOVSX_RM16_OFF,
    OP_MOVSX_RM32_OFF,

    OP_MOV_MR8_OFF,
    OP_MOV_MR16_OFF,
    OP_MOV_MR32_OFF,

    OP_CALLR,

    OP_ALLOCA,
    OP_LEA,

    OP_TRUNC8,
    OP_TRUNC16,
    OP_TRUNC32,

    OP_COUNT
} Op;

// ─── Register file ───────────────────────────────────────────────────────────

typedef union {
    struct {
        uint64_t ax, bx, cx, dx, di, si, ex, fx, bp, sp;
    };

    uint64_t r[10];
} GPR;

typedef struct {
    GPR      gpr;
    uint64_t ip;
    uint64_t fl;
    double   fp[8];
} RF;

#define RF_G(rf, n)    ((rf)->gpr.r[(n) & 7])
#define RF_S(rf, n, v) ((rf)->gpr.r[(n) & 7] = (v))

// ─── Bytecode buffer ─────────────────────────────────────────────────────────

#define DVM_BUF_MAX (256 * 1024)

typedef struct {
    uint8_t buf[DVM_BUF_MAX];
    size_t  len;
} Buf;

// ─── Thread state ────────────────────────────────────────────────────────────

#define SHADOW_DEPTH 2048

typedef enum {
    VM_THREAD_RUNNING  = 0,
    VM_THREAD_BLOCKED  = 1,
    VM_THREAD_FINISHED = 2,
} VMThreadState;

typedef struct VMThread {
    int             id;
    pthread_t       host_thread;
    VMThreadState   state;

    RF              rf;

    uint64_t        shadow[SHADOW_DEPTH];
    uint64_t       *stop;

    void           *vm_stack;
    size_t          stack_size;

    uint8_t        *bytecode;

    int             exit_code;

    struct VMThread *next;
} VMThread;

// ─── Globals ─────────────────────────────────────────────────────────────────

extern VMThread        *vm_threads;
extern pthread_mutex_t  vm_threads_lock;

// ─── API ─────────────────────────────────────────────────────────────────────

VMThread *vm_thread_create(uint8_t *bytecode, size_t stack_size);

void vm_thread_join(VMThread *t);

void vm_thread_detach(VMThread *t);

void vm_thread_destroy(VMThread *t);

void vm_run_thread(VMThread *t);

void vm_run(uint8_t *bytecode, size_t stack_size);
