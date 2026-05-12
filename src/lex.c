// ─── lex.c — Mol lexer ────────────────────────────────────────────────────────
#include "lex.h"
#include "ast.h"   // IntSuffix
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

// ─── error ────────────────────────────────────────────────────────────────────

void lex_error(Lexer *l, int line, int col, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: error: ", l->filename, line, col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

// ─── keyword table ────────────────────────────────────────────────────────────

typedef struct { const char *word; TokKind kind; } KW;

static const KW keywords[] = {
    { "var",   TOK_VAR   },
    { "let",   TOK_LET   },
    { "const", TOK_CONST },
    { "import",TOK_IMPORT},
    { "do",    TOK_DO    },
    { "end",   TOK_END   },
    { "if",    TOK_IF    },
    { "else",  TOK_ELSE  },
    { "while", TOK_WHILE },
    { "loop",  TOK_LOOP  },
    { "for",   TOK_FOR   },
    { "in",    TOK_IN    },
    { "case",  TOK_CASE  },
    { "exit",  TOK_EXIT  },
    { "proc",  TOK_PROC  },  // reserved — gives better errors
    { NULL,    TOK_EOF   },
};

static TokKind keyword_lookup(const char *s, size_t len) {
    for (int i = 0; keywords[i].word; i++) {
        if (strlen(keywords[i].word) == len &&
            memcmp(keywords[i].word, s, len) == 0)
            return keywords[i].kind;
    }
    return TOK_IDENT;
}

// ─── helpers ──────────────────────────────────────────────────────────────────

static inline char peek_c(Lexer *l)          { return *l->cur; }
static inline char peek2_c(Lexer *l)         { return l->cur[0] ? l->cur[1] : '\0'; }
static inline char adv(Lexer *l)             { return *l->cur++; }
static inline int  at_end(Lexer *l)          { return *l->cur == '\0'; }

static void newline(Lexer *l) {
    l->line++;
    l->line_start = l->cur;
}

static inline int cur_col(Lexer *l) {
    return (int)(l->cur - l->line_start) + 1;
}

static Tok make_tok(Lexer *l, TokKind kind, int line, int col) {
    Tok t = {0};
    t.kind = kind;
    t.line = line;
    t.col  = col;
    return t;
}

// ─── skip whitespace and (* ... *) comments ───────────────────────────────────

static void skip_ws(Lexer *l) {
again:
    while (!at_end(l) && isspace((unsigned char)peek_c(l))) {
        if (peek_c(l) == '\n') {
            adv(l);
            newline(l);
        } else {
            adv(l);
        }
    }
    // nested (* ... *) comments
    if (peek_c(l) == '(' && peek2_c(l) == '*') {
        int start_line = l->line, start_col = cur_col(l);
        adv(l); adv(l);  // consume (*
        int depth = 1;
        while (!at_end(l) && depth > 0) {
            if (peek_c(l) == '(' && peek2_c(l) == '*') {
                adv(l); adv(l); depth++;
            } else if (peek_c(l) == '*' && peek2_c(l) == ')') {
                adv(l); adv(l); depth--;
            } else {
                if (peek_c(l) == '\n') { adv(l); newline(l); }
                else adv(l);
            }
        }
        if (depth != 0)
            lex_error(l, start_line, start_col, "unterminated comment");
        goto again;
    }
}

// ─── integer suffix parsing ───────────────────────────────────────────────────

static int parse_int_suffix(Lexer *l, int line, int col) {
    if (!isalpha((unsigned char)peek_c(l))) return INT_UNSUFFIXED;

    const char *start = l->cur;
    while (isalnum((unsigned char)peek_c(l))) adv(l);
    size_t len = (size_t)(l->cur - start);

#define SFX(s, v) if (len == strlen(s) && memcmp(start, s, len) == 0) return (v)
    SFX("u",   INT_U);
    SFX("i8",  INT_I8);  SFX("u8",  INT_U8);
    SFX("i16", INT_I16); SFX("u16", INT_U16);
    SFX("i32", INT_I32); SFX("u32", INT_U32);
    SFX("i64", INT_I64); SFX("u64", INT_U64);
#undef SFX
    lex_error(l, line, col, "unknown integer suffix '%.*s'", (int)len, start);
    return INT_UNSUFFIXED; // unreachable
}

// ─── lex_scan — produce the next token from raw source ────────────────────────

static Tok lex_scan(Lexer *l) {
    skip_ws(l);

    if (at_end(l))
        return make_tok(l, TOK_EOF, l->line, cur_col(l));

    int  line = l->line;
    int  col  = cur_col(l);
    char c    = adv(l);

    // ── string literal ────────────────────────────────────────────────────────
    if (c == '"') {
        // process escape sequences into a temporary buffer
        char *buf = arena_alloc(l->arena, 4096);
        size_t len = 0;
        while (!at_end(l) && peek_c(l) != '"') {
            if (peek_c(l) == '\\') {
                adv(l);  // consume backslash
                char esc = adv(l);
                switch (esc) {
                    case 'n':  buf[len++] = '\n'; break;
                    case 't':  buf[len++] = '\t'; break;
                    case 'r':  buf[len++] = '\r'; break;
                    case '0':  buf[len++] = '\0'; break;
                    case '\\': buf[len++] = '\\'; break;
                    case '"':  buf[len++] = '"';  break;
                    default:   buf[len++] = esc;  break;
                }
            } else {
                char ch = adv(l);
                if (ch == '\n') newline(l);
                buf[len++] = ch;
            }
            if (len >= 4095)
                lex_error(l, line, col, "string literal too long");
        }
        if (at_end(l))
            lex_error(l, line, col, "unterminated string literal");
        adv(l);  // consume closing "
        buf[len] = '\0';
        Tok t = make_tok(l, TOK_STRING, line, col);
        t.strval.data = buf;
        t.strval.len  = len;
        return t;
    }

    // ── atom literal :foo ─────────────────────────────────────────────────────
    // A colon followed immediately (no space) by an identifier character is an
    // atom literal. A colon followed by = is TOK_ASSIGN, by > is TOK_ARROW.
    // Anything else is TOK_COLON.
    if (c == ':') {
        if (peek_c(l) == '=') { adv(l); return make_tok(l, TOK_ASSIGN, line, col); }
        if (peek_c(l) == ':') { adv(l); return make_tok(l, TOK_COLONCOLON, line, col); }
        if (isalpha((unsigned char)peek_c(l)) || peek_c(l) == '_') {
            const char *start = l->cur;
            while (isalnum((unsigned char)peek_c(l)) || peek_c(l) == '_') adv(l);
            size_t len = (size_t)(l->cur - start);
            Tok t = make_tok(l, TOK_ATOM, line, col);
            t.strval.data = arena_strndup(l->arena, start, len);
            t.strval.len  = len;
            return t;
        }
        return make_tok(l, TOK_COLON, line, col);
    }

    // ── identifier or keyword ─────────────────────────────────────────────────
    if (isalpha((unsigned char)c) || c == '_') {
        const char *start = l->cur - 1;
        while (isalnum((unsigned char)peek_c(l)) || peek_c(l) == '_') adv(l);
        size_t len = (size_t)(l->cur - start);
        TokKind kw = keyword_lookup(start, len);
        Tok t = make_tok(l, kw, line, col);
        if (kw == TOK_IDENT) {
            t.strval.data = arena_strndup(l->arena, start, len);
            t.strval.len  = len;
        }
        return t;
    }

    // ── numeric literal ───────────────────────────────────────────────────────
    if (isdigit((unsigned char)c)) {
        // Collect digits (and possible 0x hex prefix)
        const char *start = l->cur - 1;
        int is_hex = 0;
        if (c == '0' && (peek_c(l) == 'x' || peek_c(l) == 'X')) {
            adv(l); is_hex = 1;
            while (isxdigit((unsigned char)peek_c(l))) adv(l);
        } else {
            while (isdigit((unsigned char)peek_c(l))) adv(l);
        }
        // float?
        int is_float = 0;
        if (!is_hex && peek_c(l) == '.' && isdigit((unsigned char)peek2_c(l))) {
            is_float = 1;
            adv(l);
            while (isdigit((unsigned char)peek_c(l))) adv(l);
        }
        if (!is_hex && (peek_c(l) == 'e' || peek_c(l) == 'E')) {
            is_float = 1;
            adv(l);
            if (peek_c(l) == '+' || peek_c(l) == '-') adv(l);
            while (isdigit((unsigned char)peek_c(l))) adv(l);
        }
        if (is_float) {
            Tok t = make_tok(l, TOK_FLOAT, line, col);
            t.floatval = strtod(start, NULL);
            return t;
        }
        // integer
        uint64_t val = is_hex
            ? (uint64_t)strtoull(start + 2, NULL, 16)
            : (uint64_t)strtoull(start,     NULL, 10);
        int suffix = parse_int_suffix(l, line, col);
        Tok t = make_tok(l, TOK_INT, line, col);
        t.intval.value  = val;
        t.intval.suffix = suffix;
        return t;
    }

    // ── two-character operators ───────────────────────────────────────────────
    char n = peek_c(l);
    if (c == '-' && n == '>') { adv(l); return make_tok(l, TOK_ARROW,     line, col); }
    if (c == '=' && n == '>') { adv(l); return make_tok(l, TOK_FAT_ARROW, line, col); }
    if (c == '<' && n == '<') { adv(l); return make_tok(l, TOK_SHL,       line, col); }
    if (c == '>' && n == '>') {
        adv(l);
        if (peek_c(l) == '>') { adv(l); return make_tok(l, TOK_SAR, line, col); }
        return make_tok(l, TOK_SHR, line, col);
    }
    if (c == '<' && n == '=') { adv(l); return make_tok(l, TOK_LTE, line, col); }
    if (c == '>' && n == '=') { adv(l); return make_tok(l, TOK_GTE, line, col); }
    if (c == '!' && n == '=') { adv(l); return make_tok(l, TOK_NEQ, line, col); }

    // ── single-character tokens ───────────────────────────────────────────────
    switch (c) {
        case '+': return make_tok(l, TOK_PLUS,     line, col);
        case '-': return make_tok(l, TOK_MINUS,    line, col);
        case '*': return make_tok(l, TOK_STAR,     line, col);
        case '/': return make_tok(l, TOK_SLASH,    line, col);
        case '%': return make_tok(l, TOK_PERCENT,  line, col);
        case '&': return make_tok(l, TOK_AMP,      line, col);
        case '|': return make_tok(l, TOK_PIPE,     line, col);
        case '^': return make_tok(l, TOK_CARET,    line, col);
        case '~': return make_tok(l, TOK_TILDE,    line, col);
        case '=': return make_tok(l, TOK_EQ,       line, col);
        case '<': return make_tok(l, TOK_LT,       line, col);
        case '>': return make_tok(l, TOK_GT,       line, col);
        case '!': return make_tok(l, TOK_BANG,     line, col);
        case '@': return make_tok(l, TOK_AT,       line, col);
        case '(': return make_tok(l, TOK_LPAREN,   line, col);
        case ')': return make_tok(l, TOK_RPAREN,   line, col);
        case '[': return make_tok(l, TOK_LBRACKET, line, col);
        case ']': return make_tok(l, TOK_RBRACKET, line, col);
        case '{': return make_tok(l, TOK_LBRACE,   line, col);
        case '}': return make_tok(l, TOK_RBRACE,   line, col);
        case ',': return make_tok(l, TOK_COMMA,    line, col);
        case '_': return make_tok(l, TOK_UNDERSCORE, line, col);
        case '.': return make_tok(l, TOK_DOT,      line, col);
        default:  break;
    }

    // ── unknown character ─────────────────────────────────────────────────────
    Tok t = make_tok(l, TOK_ERROR, line, col);
    char msg[32];
    snprintf(msg, sizeof(msg), "unexpected character '%c'", c);
    t.strval.data = arena_strdup(l->arena, msg);
    t.strval.len  = strlen(t.strval.data);
    lex_error(l, line, col, "%s", t.strval.data);
    return t;  // unreachable
}

// ─── public API ───────────────────────────────────────────────────────────────

void lex_init(Lexer *l, Arena *arena, const char *filename, const char *src) {
    memset(l, 0, sizeof(*l));
    l->arena      = arena;
    l->filename   = filename;
    l->src        = src;
    l->cur        = src;
    l->line_start = src;
    l->line       = 1;
    l->has_peek   = 0;
}

Tok lex_next(Lexer *l) {
    if (l->has_peek) {
        l->has_peek = 0;
        return l->peek;
    }
    return lex_scan(l);
}

Tok lex_peek(Lexer *l) {
    if (!l->has_peek) {
        l->peek     = lex_scan(l);
        l->has_peek = 1;
    }
    return l->peek;
}

Tok lex_expect(Lexer *l, TokKind kind) {
    Tok t = lex_next(l);
    if (t.kind != kind)
        lex_error(l, t.line, t.col,
                  "expected %s, got %s",
                  tok_kind_name(kind), tok_kind_name(t.kind));
    return t;
}

int lex_match(Lexer *l, TokKind kind) {
    if (lex_peek(l).kind == kind) {
        lex_next(l);
        return 1;
    }
    return 0;
}

const char *tok_kind_name(TokKind kind) {
    switch (kind) {
        case TOK_INT:        return "integer literal";
        case TOK_FLOAT:      return "float literal";
        case TOK_STRING:     return "string literal";
        case TOK_ATOM:       return "atom";
        case TOK_IDENT:      return "identifier";
        case TOK_VAR:        return "'var'";
        case TOK_LET:        return "'let'";
        case TOK_CONST:      return "'const'";
        case TOK_IMPORT:     return "'import'";
        case TOK_PROC:       return "'proc'";
        case TOK_DO:         return "'do'";
        case TOK_END:        return "'end'";
        case TOK_IF:         return "'if'";
        case TOK_ELSE:       return "'else'";
        case TOK_WHILE:      return "'while'";
        case TOK_LOOP:       return "'loop'";
        case TOK_FOR:        return "'for'";
        case TOK_IN:         return "'in'";
        case TOK_CASE:       return "'case'";
        case TOK_EXIT:       return "'exit'";
        case TOK_COLON:      return "':'";
        case TOK_COLONCOLON: return "'::'";
        case TOK_ARROW:      return "'->'";
        case TOK_ASSIGN:     return "':='";
        case TOK_FAT_ARROW:  return "'=>'";
        case TOK_PLUS:       return "'+'";
        case TOK_MINUS:      return "'-'";
        case TOK_STAR:       return "'*'";
        case TOK_SLASH:      return "'/'";
        case TOK_PERCENT:    return "'%'";
        case TOK_AMP:        return "'&'";
        case TOK_PIPE:       return "'|'";
        case TOK_CARET:      return "'^'";
        case TOK_TILDE:      return "'~'";
        case TOK_SHL:        return "'<<'";
        case TOK_SHR:        return "'>>'";
        case TOK_SAR:        return "'>>>'";
        case TOK_EQ:         return "'='";
        case TOK_NEQ:        return "'!='";
        case TOK_LT:         return "'<'";
        case TOK_LTE:        return "'<='";
        case TOK_GT:         return "'>'";
        case TOK_GTE:        return "'>='";
        case TOK_BANG:       return "'!'";
        case TOK_AT:         return "'@'";
        case TOK_LPAREN:     return "'('";
        case TOK_RPAREN:     return "')'";
        case TOK_LBRACKET:   return "'['";
        case TOK_RBRACKET:   return "']'";
        case TOK_LBRACE:     return "'{'";
        case TOK_RBRACE:     return "'}'";
        case TOK_COMMA:      return "','";
        case TOK_UNDERSCORE: return "'_'";
        case TOK_DOT:        return "'.'";
        case TOK_PLUS_EXT:   return "'+'";
        case TOK_EOF:        return "end of file";
        case TOK_ERROR:      return "error";
        default:             return "unknown";
    }
}
