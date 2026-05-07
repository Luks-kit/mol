// asm.h
#pragma once

#include "vm.h"
#include "dvm_fmt.h"

// ─── Limits ──────────────────────────────────────────────────────────────────

#define ASM_MAX_LABELS  512
#define ASM_MAX_PATCHES 1024
#define ASM_MAX_EQUS    256
#define ASM_LABEL_LEN   64

// ─── Labels ──────────────────────────────────────────────────────────────────

typedef struct {
    char       name[ASM_LABEL_LEN];
    uint8_t    flags;
    DvmSection section;
    int32_t    offset;
} AsmLabel;

// ─── Patches ─────────────────────────────────────────────────────────────────

typedef struct {
    char   name[ASM_LABEL_LEN];
    size_t patch_at;
    int    is64;
    int    line;
} AsmPatch;

// ─── Constants ───────────────────────────────────────────────────────────────

typedef struct {
    char     name[ASM_LABEL_LEN];
    uint64_t value;
} AsmEqu;

// ─── Label table ─────────────────────────────────────────────────────────────

typedef struct {
    AsmLabel  labels[ASM_MAX_LABELS];
    size_t    nlabels;

    AsmPatch  patches[ASM_MAX_PATCHES];
    size_t    npatches;

    AsmEqu    equs[ASM_MAX_EQUS];
    size_t    nequs;
} LabelTable;

// ─── Assembler context ───────────────────────────────────────────────────────

typedef struct {
    Buf        text;
    Buf        data;
    Buf        cnst;

    LabelTable lt;

    DvmRel     rels[DVM_MAX_RELS];
    size_t     nrels;
} AsmCtx;

// ─── API ─────────────────────────────────────────────────────────────────────

int asm_compile(const char *src, AsmCtx *ctx);

void asm_to_prog(const AsmCtx *ctx, DvmProg *prog);

AsmLabel *asm_label_find(LabelTable *lt, const char *name);
