#pragma once
// ─── parse.h — Mol recursive descent parser ──────────────────────────────────
#include "arena.h"
#include "lex.h"
#include "ast.h"

typedef struct {
    Lexer *lex;
    Arena *arena;
} Parser;

// Initialise parser over an already-initialised lexer.
void parser_init(Parser *p, Arena *arena, Lexer *lex);

// Parse a complete source file — returns a NODE_BLOCK of top-level declarations.
Node *parse_file(Parser *p);

// ─── entry points exposed for testing ────────────────────────────────────────
Node *parse_expr(Parser *p);
Node *parse_stmt(Parser *p);
Node *parse_type(Parser *p);
