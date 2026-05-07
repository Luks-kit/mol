// dvm_fmt.h
#pragma once

#include <stdint.h>
#include <stddef.h>

// ─── File tags ───────────────────────────────────────────────────────────────

#define DVM_MAGIC     "\x64\x76\x6d\x40"
#define DVM_MAGIC_LEN 4

#define DVM_TAG_CNST "CNST"
#define DVM_TAG_DATA "DATA"
#define DVM_TAG_CODE "CODE"
#define DVM_TAG_ENTR "ENTR"
#define DVM_TAG_SYMS "SYMS"
#define DVM_TAG_RELS "RELS"
#define DVM_TAG_END  "END\0"

#define DVM_TAG_LEN 4

// ─── Sections ────────────────────────────────────────────────────────────────

typedef enum __attribute__((packed)) {
    DVM_SEC_CNST = 0,
    DVM_SEC_DATA = 1,
    DVM_SEC_CODE = 2,
    DVM_SEC_NONE = 0xFF,
} DvmSection;

// ─── Symbol flags ────────────────────────────────────────────────────────────

#define DVM_SYM_LOCAL  0x00
#define DVM_SYM_GLOBAL 0x01
#define DVM_SYM_EXTERN 0x02

// ─── Limits ──────────────────────────────────────────────────────────────────

#define DVM_MAX_SYMS  1024
#define DVM_MAX_RELS  2048
#define DVM_SYM_NAME  64

// ─── Symbols ────────────────────────────────────────────────────────────────

typedef struct {
    char       name[DVM_SYM_NAME];
    uint8_t    flags;
    DvmSection section;
    uint32_t   offset;
} DvmSym;

// ─── Relocations ─────────────────────────────────────────────────────────────

#define DVM_REL_DATA 0
#define DVM_REL_CODE 1

typedef struct {
    uint32_t code_offset;
    uint32_t sym_index;
    uint8_t  kind;
} DvmRel;

// ─── Program representation ──────────────────────────────────────────────────

typedef struct {
    uint8_t  *cnst;
    size_t    cnst_len;

    uint8_t  *data;
    size_t    data_len;

    uint8_t  *code;
    size_t    code_len;

    DvmSym    syms[DVM_MAX_SYMS];
    size_t    nsyms;

    DvmRel    rels[DVM_MAX_RELS];
    size_t    nrels;

    uint32_t  entry_offset;
    int       has_entry;
} DvmProg;

// ─── Loaded runtime image ────────────────────────────────────────────────────

typedef struct {
    uint8_t  *cnst;
    size_t    cnst_len;

    uint8_t  *data;
    size_t    data_len;

    uint8_t  *code;
    size_t    code_len;

    uint32_t  entry_offset;
} DvmLoaded;

// ─── Lifetime helpers ────────────────────────────────────────────────────────

void dvm_prog_free(DvmProg *prog);
void dvm_loaded_free(DvmLoaded *img);
