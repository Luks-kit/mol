// ─── main.c — Mol compiler driver ────────────────────────────────────────────
//
// Pipeline:
//   source file
//     → lex + parse  → AST
//     → type check   → annotated AST
//     → codegen      → DVM assembly text
//     → asm_compile  → DvmProg (object)
//     → dvm_link     → linked DvmProg executable
//     → vm_run       → execute
//
// Usage:
//   molc <source.mol>           compile and run
//   molc -o <out.dvm> <src.mol> compile to .dvm, don't run
//   molc -S <src.mol>           dump assembly to stdout, don't assemble
//   molc -h                     print help

#include "arena.h"
#include "lex.h"
#include "parse.h"
#include "check.h"
#include "codegen.h"
#include "dvm/asm.h"
#include "dvm/linker.h"
#include "dvm/vm.h"
#include "dvm/dvm_fmt.h"
#include "dvm/dvm_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── read entire file into a malloc'd buffer ──────────────────────────────────

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "molc: cannot open '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fputs("molc: out of memory\n", stderr); exit(1); }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

// ─── usage ────────────────────────────────────────────────────────────────────

static void usage(void) {
    fputs(
        "usage: molc [options] <source.mol>\n"
        "options:\n"
        "  -o <file>   write linked .dvm executable to file (default: run in memory)\n"
        "  -S          dump generated assembly text to stdout, then exit\n"
        "  -h          show this help\n",
        stderr
    );
    exit(1);
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    const char *src_path  = NULL;
    const char *out_path  = NULL;
    int         dump_asm  = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h")) {
            usage();
        } else if (!strcmp(argv[i], "-S")) {
            dump_asm = 1;
        } else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fputs("molc: -o requires argument\n", stderr); exit(1); }
            out_path = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "molc: unknown option '%s'\n", argv[i]);
            usage();
        } else {
            if (src_path) { fputs("molc: multiple source files not supported\n", stderr); exit(1); }
            src_path = argv[i];
        }
    }

    if (!src_path) usage();

    // ── 1. read source ────────────────────────────────────────────────────────

    char *src = read_file(src_path);

    // ── 2. arena for compiler data structures ─────────────────────────────────

    Arena arena;
    arena_init(&arena, 0);

    // ── 3. lex + parse ────────────────────────────────────────────────────────

    Lexer  lex;
    lex_init(&lex, &arena, src_path, src);

    Parser parser;
    parser_init(&parser, &arena, &lex);

    Node *ast = parse_file(&parser);

    // ── 4. type check ─────────────────────────────────────────────────────────

    Checker checker;
    checker_init(&checker, &arena);
    check_file(&checker, ast);

    // ── 5. codegen → assembly text ────────────────────────────────────────────

    CGen cgen;
    cgen_init(&cgen, &arena);
    const char *asm_text = cgen_file(&cgen, &checker, ast);

    if (dump_asm) {
        fputs(asm_text, stdout);
        arena_free(&arena);
        free(src);
        return 0;
    }

    // ── 6. assemble → DvmProg object ──────────────────────────────────────────

    AsmCtx asm_ctx;
    if (!asm_compile(asm_text, &asm_ctx)) {
        fputs("molc: assembly failed\n", stderr);
        exit(1);
    }

    DvmProg obj;
    asm_to_prog(&asm_ctx, &obj);

    // ── 7. link → executable DvmProg ─────────────────────────────────────────

    DvmProg exe;
    if (!dvm_link(&obj, 1, &exe, "main")) {
        fputs("molc: link failed\n", stderr);
        exit(1);
    }

    // ── 8a. write to file if requested ────────────────────────────────────────

    if (out_path) {
        if (!dvm_write_file(out_path, &exe)) {
            fprintf(stderr, "molc: failed to write '%s'\n", out_path);
            exit(1);
        }
        fprintf(stderr, "molc: wrote '%s'\n", out_path);
        dvm_prog_free(&exe);
        arena_free(&arena);
        free(src);
        return 0;
    }

    // ── 8b. run in memory ─────────────────────────────────────────────────────
    //
    // vm_run expects a flat bytecode buffer starting at offset 0.
    // For a linked executable we just run the code section directly.
    // Relocs should have been resolved by the linker.
    //
    // Stack size: 1 MB default.

    #define STACK_SIZE (1024 * 1024)
    vm_run(exe.code, STACK_SIZE);

    dvm_prog_free(&exe);
    arena_free(&arena);
    free(src);
    return 0;
}
