// ─── main.c — Mol compiler driver ────────────────────────────────────────────
//
// Pipeline:
//   source file
//     → lex + parse  → AST
//     → type check   → annotated AST
//     → codegen      → DVM assembly text
//     → asm_compile  → DvmProg (object)
//     → dvm_link     → linked DvmProg executable   (skipped with -c)
//     → vm_run       → execute                     (skipped with -c or -o)
//
// Usage:
//   molc <source.mol>             compile, link, and run in memory
//   molc -o <out.dvm> <src.mol>   compile, link, write executable, don't run
//   molc -c <src.mol>             compile to object file only (no link)
//   molc -c -o <out.obj> <src>    compile to named object file
//   molc -S <src.mol>             dump generated assembly to stdout, then exit
//   molc -h                       show this help

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

// ─── helpers ──────────────────────────────────────────────────────────────────

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

// replace extension in path with new_ext, return malloc'd string
static char *replace_ext(const char *path, const char *new_ext) {
    const char *dot = strrchr(path, '.');
    const char *sep = strrchr(path, '/');
    // only replace if dot comes after the last separator
    size_t base_len = (dot && (!sep || dot > sep))
                    ? (size_t)(dot - path)
                    : strlen(path);
    size_t ext_len  = strlen(new_ext);
    char *out = malloc(base_len + ext_len + 1);
    memcpy(out, path, base_len);
    memcpy(out + base_len, new_ext, ext_len);
    out[base_len + ext_len] = '\0';
    return out;
}

// ─── usage ────────────────────────────────────────────────────────────────────

static void usage(void) {
    fputs(
        "usage: molc [options] <source.mol>\n"
        "options:\n"
        "  -c          compile to object file only, do not link\n"
        "  -o <file>   output file (object with -c, executable otherwise)\n"
        "  -S          dump generated assembly to stdout, then exit\n"
        "  -h          show this help\n"
        "\n"
        "default output names:\n"
        "  -c          <source>.dvm  (object file)\n"
        "  -o          writes linked executable to given path\n"
        "  (neither)   compile, link, run in memory\n",
        stderr
    );
    exit(1);
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    const char *src_path = NULL;
    const char *out_path = NULL;
    int         dump_asm = 0;
    int         obj_only = 0;   // -c: stop after assembling, write object

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h")) {
            usage();
        } else if (!strcmp(argv[i], "-S")) {
            dump_asm = 1;
        } else if (!strcmp(argv[i], "-c")) {
            obj_only = 1;
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

    // ── 2. arena ──────────────────────────────────────────────────────────────

    Arena arena;
    arena_init(&arena, 0);

    // ── 3. lex + parse ────────────────────────────────────────────────────────

    Lexer lex;
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

    AsmCtx *asm_ctx = arena_alloc(&arena, sizeof(AsmCtx));
    if (!asm_compile(asm_text, asm_ctx)) {
        fputs("molc: assembly failed\n", stderr);
        exit(1);
    }

    DvmProg obj;
    asm_to_prog(asm_ctx, &obj);

    // ── 7. -c: write object file and stop ─────────────────────────────────────

    if (obj_only) {
        // default output name: replace source extension with .dvm
        char *default_out = replace_ext(src_path, ".dvm");
        const char *dest  = out_path ? out_path : default_out;

        if (!dvm_write_file(dest, &obj)) {
            fprintf(stderr, "molc: failed to write '%s'\n", dest);
            free(default_out);
            exit(1);
        }
        fprintf(stderr, "molc: wrote object '%s'\n", dest);
        free(default_out);
        arena_free(&arena);
        free(src);
        return 0;
    }

    // ── 8. collect imports and link ───────────────────────────────────────────

    #define MAX_IMPORTS 64
    DvmProg *import_objs = arena_calloc(&arena, sizeof(DvmProg) * (MAX_IMPORTS));
    int     nimports = 0;

    for (int i = 0; i < ast->block.nstmts; i++) {
        Node *n = ast->block.stmts[i];
        if (n->kind != NODE_IMPORT) continue;
        if (nimports >= MAX_IMPORTS) {
            fputs("molc: too many imports\n", stderr); exit(1);
        }
        if (!dvm_read_file(n->import_path.path, &import_objs[nimports])) {
            fprintf(stderr, "molc: failed to read import '%s'\n",
                    n->import_path.path);
            exit(1);
        }
        nimports++;
    }

    DvmProg *all_objs = arena_calloc(&arena, sizeof(DvmProg)* (MAX_IMPORTS + 1));
    all_objs[0] = obj;
    for (int i = 0; i < nimports; i++) all_objs[i + 1] = import_objs[i];

    DvmProg exe;
    if (!dvm_link(all_objs, (size_t)(nimports + 1), &exe, "main")) {
        fputs("molc: link failed\n", stderr);
        exit(1);
    }

    for (int i = 0; i < nimports; i++) dvm_prog_free(&import_objs[i]);

    // ── 9a. write executable to file ──────────────────────────────────────────

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

    // ── 9b. run in memory ─────────────────────────────────────────────────────

    #define STACK_SIZE (1024 * 1024)
    vm_run(exe.code, STACK_SIZE);

    dvm_prog_free(&exe);
    arena_free(&arena);
    free(src);
    return 0;
}
