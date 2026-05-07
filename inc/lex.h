#pragma once
// ─── lex.h — Mol lexer ────────────────────────────────────────────────────────
#include "arena.h"
#include <stdint.h>
#include <stddef.h>

// ─── Token kinds ──────────────────────────────────────────────────────────────
typedef enum {
    // literals
    TOK_INT,        // 42  5u  3i8
    TOK_FLOAT,      // 3.14
    TOK_STRING,     // "hello"
    TOK_ATOM,       // :foo
    TOK_IDENT,      // foo

    // keywords
    TOK_VAR,        // var
    TOK_LET,        // let
    TOK_CONST,      // const
    TOK_PROC,       // (no longer a keyword — proc decls use name:(  syntax)
                    // kept as reserved to give a good error message
    TOK_DO,         // do
    TOK_END,        // end
    TOK_IF,         // if
    TOK_ELSE,       // else
    TOK_WHILE,      // while
    TOK_LOOP,       // loop
    TOK_FOR,        // for
    TOK_IN,         // in
    TOK_CASE,       // case
    TOK_EXIT,       // exit

    // punctuation
    TOK_COLON,      // :   (also prefix of atom literal — lexer disambiguates)
    TOK_COLONCOLON, // ::  (reserved, may be used later)
    TOK_ARROW,      // ->
    TOK_ASSIGN,     // :=
    TOK_FAT_ARROW,  // =>
    TOK_PLUS,       // +
    TOK_MINUS,      // -
    TOK_STAR,       // *
    TOK_SLASH,      // /
    TOK_PERCENT,    // %
    TOK_AMP,        // &
    TOK_PIPE,       // |
    TOK_CARET,      // ^
    TOK_TILDE,      // ~
    TOK_SHL,        // <<
    TOK_SHR,        // >>
    TOK_SAR,        // >>> (arithmetic right shift)
    TOK_EQ,         // =
    TOK_NEQ,        // !=
    TOK_LT,         // <
    TOK_LTE,        // <=
    TOK_GT,         // >
    TOK_GTE,        // >=
    TOK_BANG,       // !   (pointer deref postfix / type suffix)
    TOK_AT,         // @   (address-of)
    TOK_LPAREN,     // (
    TOK_RPAREN,     // )
    TOK_LBRACKET,   // [
    TOK_RBRACKET,   // ]
    TOK_LBRACE,     // {
    TOK_RBRACE,     // }
    TOK_COMMA,      // ,
    TOK_UNDERSCORE, // _   (wildcard in case arms)
    TOK_DOT,        // .   (module qualified access)
    TOK_PLUS_EXT,   // +   following a name — extensible marker (name+)
                    // NOTE: the lexer emits TOK_PLUS; the parser handles name+

    // special
    TOK_EOF,
    TOK_ERROR,      // lexer error — message in tok.strval
} TokKind;

// ─── Token ────────────────────────────────────────────────────────────────────
typedef struct {
    TokKind     kind;
    int         line;
    int         col;

    union {
        // TOK_INT
        struct {
            uint64_t value;
            int      suffix;   // IntSuffix from ast.h
        } intval;

        // TOK_FLOAT
        double floatval;

        // TOK_STRING, TOK_ATOM, TOK_IDENT, TOK_ERROR
        struct {
            const char *data;  // interned in arena; NUL-terminated
            size_t      len;
        } strval;
    };
} Tok;

// ─── Lexer ────────────────────────────────────────────────────────────────────
typedef struct {
    const char *src;      // full source text (NUL-terminated)
    const char *cur;      // current position
    const char *line_start; // start of current line (for col calculation)
    int         line;
    const char *filename;
    Arena      *arena;    // for interning strings/atoms/idents

    // one-token lookahead buffer
    Tok         peek;
    int         has_peek;
} Lexer;

// Initialise lexer over src. src must remain valid for the lexer's lifetime.
void lex_init(Lexer *l, Arena *arena, const char *filename, const char *src);

// Return the next token, consuming it.
Tok  lex_next(Lexer *l);

// Return the next token without consuming it.
Tok  lex_peek(Lexer *l);

// Consume the next token and assert it has the expected kind.
// Calls lex_error on mismatch — does not return on failure.
Tok  lex_expect(Lexer *l, TokKind kind);

// Check if the next token has the given kind; consume and return 1 if so.
int  lex_match(Lexer *l, TokKind kind);

// Human-readable token kind name (for error messages).
const char *tok_kind_name(TokKind kind);

// Emit a fatal parse error and exit.
void lex_error(Lexer *l, int line, int col, const char *fmt, ...);
