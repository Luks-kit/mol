// ─── parse.c — Mol recursive descent parser ──────────────────────────────────
#include "parse.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

// ─── helpers ──────────────────────────────────────────────────────────────────

static Tok  peek  (Parser *p)            { return lex_peek(p->lex); }
static Tok  next  (Parser *p)            { return lex_next(p->lex); }
static Tok  expect(Parser *p, TokKind k) { return lex_expect(p->lex, k); }
static int  match (Parser *p, TokKind k) { return lex_match(p->lex, k); }

static Loc loc(Parser *p) {
    Tok t = peek(p);
    return (Loc){ p->lex->filename, t.line, t.col };
}

static void perror_(Parser *p, const char *fmt, ...) {
    Tok t = peek(p);
    fprintf(stderr, "%s:%d:%d: error: ", p->lex->filename, t.line, t.col);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int is_wildcard(Tok t) {
    return t.kind == TOK_IDENT
        && t.strval.len == 1
        && t.strval.data[0] == '_';
}

// ─── growable node list (stack-allocated, arena-finalised) ────────────────────

#define NLIST_MAX 256
typedef struct { Node *buf[NLIST_MAX]; int n; } NList;

static void nl_push(Parser *p, NList *nl, Node *node) {
    if (nl->n >= NLIST_MAX) perror_(p, "too many items in list");
    nl->buf[nl->n++] = node;
}

static Node **nl_finish(NList *nl, Arena *a, int *out_n) {
    *out_n = nl->n;
    if (!nl->n) return NULL;
    Node **arr = arena_alloc(a, sizeof(Node *) * (size_t)nl->n);
    memcpy(arr, nl->buf, sizeof(Node *) * (size_t)nl->n);
    return arr;
}

// ─── forward declarations ─────────────────────────────────────────────────────

static Node *parse_named_decl(Parser *p, int is_const);
static Node *parse_field_decl(Parser *p);
static Node *parse_case_block(Parser *p);
static Node *parse_type_tuple(Parser *p);
static Node *parse_block     (Parser *p);
static Node *parse_stmt_inner(Parser *p);
static Node *parse_expr_prec (Parser *p, int min_prec);
static Node *parse_unary     (Parser *p);
static Node *parse_postfix   (Parser *p, Node *lhs);
static Node *parse_primary   (Parser *p);
static Node *parse_case_arms (Parser *p, Node *subject, Loc l);

// ─── init ─────────────────────────────────────────────────────────────────────

void parser_init(Parser *p, Arena *arena, Lexer *lex) {
    p->arena = arena;
    p->lex   = lex;
}

// ─── file ─────────────────────────────────────────────────────────────────────

Node *parse_file(Parser *p) {
    Loc l = loc(p);
    NList nl = {0};
    while (peek(p).kind != TOK_EOF) {
        Tok t = peek(p);
        Node *node;
        if (t.kind == TOK_IMPORT) {
            next(p);
            Tok path_tok = expect(p, TOK_STRING);
            node = ast_import(p->arena, (Loc){p->lex->filename, t.line, t.col},
                              path_tok.strval.data, path_tok.strval.len);
        } else if (t.kind == TOK_CONST) {
            next(p);
            node = parse_named_decl(p, 1);
        } else if (t.kind == TOK_VAR || t.kind == TOK_LET) {
            node = parse_stmt_inner(p);
        } else if (t.kind == TOK_IDENT) {
            node = parse_named_decl(p, 0);
        } else {
            perror_(p, "expected top-level declaration, got %s",
                    tok_kind_name(t.kind));
            node = NULL;
        }
        nl_push(p, &nl, node);
    }
    int n; Node **arr = nl_finish(&nl, p->arena, &n);
    return ast_block(p->arena, l, arr, n);
}

// ─── named declaration: record or proc ───────────────────────────────────────
//
// Disambiguation: scan past the first balanced '(' ')' and check for '->'.
//   proc:   name ':' '(' in ')' '->' '(' out ')' do body end
//   record: name [+] ':' '(' fields [case block] ')'

static int sniff_is_proc(Lexer *lex) {
    const char *c = lex->cur;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (*c != '(') return 0;
    int depth = 0;
    while (*c) {
        if      (*c == '(') depth++;
        else if (*c == ')') { if (--depth == 0) { c++; break; } }
        c++;
    }
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    return c[0] == '-' && c[1] == '>';
}

static Node *parse_named_decl(Parser *p, int is_const) {
    Tok name_tok = expect(p, TOK_IDENT);
    const char *name = name_tok.strval.data;
    size_t      nlen = name_tok.strval.len;
    Loc l = (Loc){ p->lex->filename, name_tok.line, name_tok.col };

    int extensible = match(p, TOK_PLUS);
    expect(p, TOK_COLON);

    if (sniff_is_proc(p->lex)) {
        if (extensible)
            perror_(p, "proc '%s' cannot be marked extensible", name);
        Node *in_tuple  = parse_type_tuple(p);
        expect(p, TOK_ARROW);
        Node *out_tuple = parse_type_tuple(p);
        if (!match(p, TOK_DO))
            return ast_proc_decl(p->arena, l, name, nlen,
                                 in_tuple, out_tuple, NULL, is_const);
        Node *body = parse_block(p);
        expect(p, TOK_END);
        return ast_proc_decl(p->arena, l, name, nlen,
                             in_tuple, out_tuple, body, is_const);
    }

    // record
    if (is_const)
        perror_(p, "record '%s' cannot be marked const", name);
    NList fields = {0};
    Node *case_block = NULL;
    expect(p, TOK_LPAREN);
    while (peek(p).kind != TOK_RPAREN && peek(p).kind != TOK_EOF) {
        if (peek(p).kind == TOK_CASE) {
            case_block = parse_case_block(p);
            break;
        }
        nl_push(p, &fields, parse_field_decl(p));
        match(p, TOK_COMMA);
    }
    expect(p, TOK_RPAREN);
    int n; Node **arr = nl_finish(&fields, p->arena, &n);
    return ast_record_decl(p->arena, l, name, nlen, arr, n, extensible, case_block);
}

// ─── field declaration: name [type] ──────────────────────────────────────────

static Node *parse_field_decl(Parser *p) {
    Tok name_tok = expect(p, TOK_IDENT);
    Loc l = (Loc){ p->lex->filename, name_tok.line, name_tok.col };
    Node *type = NULL;
    TokKind k = peek(p).kind;
    if (k != TOK_COMMA && k != TOK_RPAREN && k != TOK_CASE && k != TOK_EOF)
        type = parse_type(p);
    return ast_field_decl(p->arena, l,
                          name_tok.strval.data, name_tok.strval.len, type);
}

// ─── case block inside a record ───────────────────────────────────────────────

static Node *parse_case_block(Parser *p) {
    Tok ct = expect(p, TOK_CASE);
    Loc l  = (Loc){ p->lex->filename, ct.line, ct.col };
    Node *subject = parse_expr(p);
    expect(p, TOK_DO);
    NList arms = {0};
    while (peek(p).kind != TOK_END && peek(p).kind != TOK_EOF) {
        Loc al = loc(p);
        Node *pattern = NULL;
        Tok pt = peek(p);
        if (pt.kind == TOK_ATOM) {
            next(p);
            pattern = ast_atom(p->arena, al, pt.strval.data, pt.strval.len);
        } else if (is_wildcard(pt)) {
            next(p);
        } else {
            perror_(p, "expected atom pattern in case arm, got %s",
                    tok_kind_name(pt.kind));
        }
        expect(p, TOK_FAT_ARROW);
        Node *body = parse_type_tuple(p);
        match(p, TOK_COMMA);
        nl_push(p, &arms, ast_case_arm(p->arena, al, pattern, body));
    }
    expect(p, TOK_END);
    int n; Node **arr = nl_finish(&arms, p->arena, &n);
    return ast_case(p->arena, l, subject, arr, n);
}

// ─── type tuple: '(' field_decl* ')' ─────────────────────────────────────────

static Node *parse_type_tuple(Parser *p) {
    Loc l = loc(p);
    expect(p, TOK_LPAREN);
    NList fields = {0};
    while (peek(p).kind != TOK_RPAREN && peek(p).kind != TOK_EOF) {
        nl_push(p, &fields, parse_field_decl(p));
        match(p, TOK_COMMA);
    }
    expect(p, TOK_RPAREN);
    int n; Node **arr = nl_finish(&fields, p->arena, &n);
    return ast_type_tuple(p->arena, l, arr, n);
}

// ─── type expressions ─────────────────────────────────────────────────────────

Node *parse_type(Parser *p) {
    Loc l = loc(p);
    Tok t = peek(p);

    if (t.kind == TOK_CONST) {
        next(p);
        return ast_type_const(p->arena, l, parse_type(p));
    }

    if (t.kind == TOK_LBRACKET) {
        next(p);
        Node *size = (peek(p).kind != TOK_RBRACKET) ? parse_expr(p) : NULL;
        expect(p, TOK_RBRACKET);
        return ast_type_array(p->arena, l, size, parse_type(p));
    }

    if (t.kind == TOK_LPAREN)
        return parse_type_tuple(p);

    if (t.kind == TOK_IDENT) {
        next(p);
        Node *base = ast_type_name(p->arena, l, t.strval.data, t.strval.len);
        if (peek(p).kind == TOK_BANG) {
            next(p);
            return ast_type_ptr(p->arena, l, base);
        }
        return base;
    }

    perror_(p, "expected type, got %s", tok_kind_name(t.kind));
    return NULL;
}

// ─── block ────────────────────────────────────────────────────────────────────

static Node *parse_block(Parser *p) {
    Loc l = loc(p);
    NList stmts = {0};
    for (;;) {
        TokKind k = peek(p).kind;
        if (k == TOK_END || k == TOK_ELSE || k == TOK_EOF) break;
        nl_push(p, &stmts, parse_stmt(p));
    }
    int n; Node **arr = nl_finish(&stmts, p->arena, &n);
    return ast_block(p->arena, l, arr, n);
}

// ─── statements ───────────────────────────────────────────────────────────────

Node *parse_stmt(Parser *p) {
    return parse_stmt_inner(p);
}

static Node *parse_stmt_inner(Parser *p) {
    Loc l = loc(p);
    Tok t = peek(p);

    if (t.kind == TOK_VAR || t.kind == TOK_LET) {
        int is_let = (t.kind == TOK_LET);
        next(p);
        Tok name_tok = expect(p, TOK_IDENT);
        Node *type = NULL;
        TokKind k = peek(p).kind;
        if (k != TOK_ASSIGN && k != TOK_END && k != TOK_ELSE
         && k != TOK_EOF    && k != TOK_COMMA)
            type = parse_type(p);
        Node *init = match(p, TOK_ASSIGN) ? parse_expr(p) : NULL;
        return ast_var_decl(p->arena, l,
                            name_tok.strval.data, name_tok.strval.len,
                            type, init, is_let);
    }

    if (t.kind == TOK_IF) {
        next(p);
        Node *cond = parse_expr(p);
        expect(p, TOK_DO);
        Node *then = parse_block(p);
        Node *els  = NULL;
        if (peek(p).kind == TOK_ELSE) {
            next(p);
            expect(p, TOK_DO);
            els = parse_block(p);
        }
        expect(p, TOK_END);
        return ast_if(p->arena, l, cond, then, els);
    }

    if (t.kind == TOK_WHILE) {
        next(p);
        Node *cond = parse_expr(p);
        expect(p, TOK_DO);
        Node *body = parse_block(p);
        expect(p, TOK_END);
        return ast_while(p->arena, l, cond, body);
    }

    if (t.kind == TOK_LOOP) {
        next(p);
        expect(p, TOK_DO);
        Node *body = parse_block(p);
        expect(p, TOK_END);
        return ast_loop(p->arena, l, body);
    }

    if (t.kind == TOK_FOR) {
        next(p);
        Tok first = expect(p, TOK_IDENT);
        Node *key = NULL, *val = NULL;
        if (match(p, TOK_COMMA)) {
            key = ast_ident(p->arena, l, first.strval.data, first.strval.len);
            Tok second = expect(p, TOK_IDENT);
            val = ast_ident(p->arena, l, second.strval.data, second.strval.len);
        } else {
            val = ast_ident(p->arena, l, first.strval.data, first.strval.len);
        }
        expect(p, TOK_IN);
        Node *iter = parse_expr(p);
        expect(p, TOK_DO);
        Node *body = parse_block(p);
        expect(p, TOK_END);
        return ast_for(p->arena, l, key, val, iter, body);
    }

    if (t.kind == TOK_CASE) {
        next(p);
        Node *subject = parse_expr(p);
        return parse_case_arms(p, subject, l);
    }

    if (t.kind == TOK_EXIT) {
        next(p);
        return ast_exit(p->arena, l);
    }

    return parse_expr(p);
}

// ─── case arms ────────────────────────────────────────────────────────────────

static Node *parse_case_arms(Parser *p, Node *subject, Loc l) {
    expect(p, TOK_DO);
    NList arms = {0};
    while (peek(p).kind != TOK_END && peek(p).kind != TOK_EOF) {
        Loc al = loc(p);
        Node *pattern = NULL;
        Tok pt = peek(p);
        if (pt.kind == TOK_ATOM) {
            next(p);
            pattern = ast_atom(p->arena, al, pt.strval.data, pt.strval.len);
        } else if (pt.kind == TOK_LBRACE) {
            pattern = parse_primary(p);
        } else if (is_wildcard(pt)) {
            next(p);
        } else {
            perror_(p, "expected pattern in case arm, got %s",
                    tok_kind_name(pt.kind));
        }
        expect(p, TOK_FAT_ARROW);
        Node *body;
        if (peek(p).kind == TOK_DO) {
            next(p);
            body = parse_block(p);
            expect(p, TOK_END);
        } else {
            body = parse_stmt(p);
        }
        nl_push(p, &arms, ast_case_arm(p->arena, al, pattern, body));
    }
    expect(p, TOK_END);
    int n; Node **arr = nl_finish(&arms, p->arena, &n);
    return ast_case(p->arena, l, subject, arr, n);
}

// ─── expressions — Pratt parser ───────────────────────────────────────────────

typedef struct { int prec; int right_assoc; BinOp op; } OpInfo;

static OpInfo binop_info(TokKind k) {
    switch (k) {
        case TOK_PIPE:    return (OpInfo){2, 0, BOP_OR};
        case TOK_CARET:   return (OpInfo){3, 0, BOP_XOR};
        case TOK_AMP:     return (OpInfo){4, 0, BOP_AND};
        case TOK_EQ:      return (OpInfo){5, 0, BOP_EQ};
        case TOK_NEQ:     return (OpInfo){5, 0, BOP_NEQ};
        case TOK_LT:      return (OpInfo){6, 0, BOP_LT};
        case TOK_LTE:     return (OpInfo){6, 0, BOP_LTE};
        case TOK_GT:      return (OpInfo){6, 0, BOP_GT};
        case TOK_GTE:     return (OpInfo){6, 0, BOP_GTE};
        case TOK_SHL:     return (OpInfo){7, 0, BOP_SHL};
        case TOK_SHR:     return (OpInfo){7, 0, BOP_SHR};
        case TOK_SAR:     return (OpInfo){7, 0, BOP_SAR};
        case TOK_PLUS:    return (OpInfo){8, 0, BOP_ADD};
        case TOK_MINUS:   return (OpInfo){8, 0, BOP_SUB};
        case TOK_STAR:    return (OpInfo){9, 0, BOP_MUL};
        case TOK_SLASH:   return (OpInfo){9, 0, BOP_DIV};
        case TOK_PERCENT: return (OpInfo){9, 0, BOP_MOD};
        default:          return (OpInfo){0, 0, 0};
    }
}

Node *parse_expr(Parser *p) {
    Loc l = loc(p);
    Node *lhs = parse_expr_prec(p, 1);
    if (peek(p).kind == TOK_ASSIGN) {
        next(p);
        return ast_assign(p->arena, l, lhs, parse_expr(p));
    }
    return lhs;
}

static Node *parse_expr_prec(Parser *p, int min_prec) {
    Node *lhs = parse_postfix(p, parse_unary(p));
    while (1) {
        OpInfo info = binop_info(peek(p).kind);
        if (info.prec < min_prec) break;
        Loc l = loc(p);
        next(p);
        Node *rhs = parse_postfix(p,
            parse_expr_prec(p, info.right_assoc ? info.prec : info.prec + 1));
        lhs = ast_binop(p->arena, l, info.op, lhs, rhs);
    }
    return lhs;
}

static Node *parse_unary(Parser *p) {
    Loc l = loc(p);
    if (peek(p).kind == TOK_MINUS) { next(p); return ast_unop  (p->arena, l, UOP_NEG, parse_unary(p)); }
    if (peek(p).kind == TOK_TILDE) { next(p); return ast_unop  (p->arena, l, UOP_NOT, parse_unary(p)); }
    if (peek(p).kind == TOK_AT)    { next(p); return ast_addrof(p->arena, l,           parse_unary(p)); }
    return parse_primary(p);
}

static Node *parse_postfix(Parser *p, Node *lhs) {
    for (;;) {
        Loc l = loc(p);
        Tok t = peek(p);
        if (t.kind == TOK_BANG) {
            next(p);
            lhs = ast_deref(p->arena, l, lhs);
        } else if (t.kind == TOK_LBRACKET) {
            next(p);
            Node *idx = parse_expr(p);
            expect(p, TOK_RBRACKET);
            lhs = ast_index(p->arena, l, lhs, idx);
        } else if (t.kind == TOK_ATOM) {
            // p:x — lexer emits [IDENT "p"] [ATOM "x"]
            next(p);
            lhs = ast_field(p->arena, l, lhs, t.strval.data, t.strval.len);
        } else if (t.kind == TOK_LPAREN) {
            next(p);
            NList args = {0};
            while (peek(p).kind != TOK_RPAREN && peek(p).kind != TOK_EOF) {
                nl_push(p, &args, parse_expr(p));
                match(p, TOK_COMMA);
            }
            expect(p, TOK_RPAREN);
            int n; Node **arr = nl_finish(&args, p->arena, &n);
            lhs = ast_call(p->arena, l, lhs, arr, n);
        } else if (t.kind == TOK_DOT) {
            next(p);
            Tok ft = expect(p, TOK_IDENT);
            lhs = ast_field(p->arena, l, lhs, ft.strval.data, ft.strval.len);
        } else {
            break;
        }
    }
    return lhs;
}

// ─── primary expressions ──────────────────────────────────────────────────────

static Node *parse_primary(Parser *p) {
    Loc l = loc(p);
    Tok t = next(p);

    switch (t.kind) {
        case TOK_INT:    return ast_intlit  (p->arena, l, t.intval.value, t.intval.suffix);
        case TOK_FLOAT:  return ast_floatlit(p->arena, l, t.floatval);
        case TOK_STRING: return ast_strlit  (p->arena, l, t.strval.data, t.strval.len);
        case TOK_ATOM:   return ast_atom    (p->arena, l, t.strval.data, t.strval.len);
        case TOK_IDENT:  return ast_ident   (p->arena, l, t.strval.data, t.strval.len);

        case TOK_LPAREN: {
            NList exprs = {0};
            while (peek(p).kind != TOK_RPAREN && peek(p).kind != TOK_EOF) {
                nl_push(p, &exprs, parse_expr(p));
                match(p, TOK_COMMA);
            }
            expect(p, TOK_RPAREN);
            if (exprs.n == 1) return exprs.buf[0];
            int n; Node **arr = nl_finish(&exprs, p->arena, &n);
            return ast_mollit(p->arena, l, arr, n);
        }

        case TOK_LBRACKET: {
            NList elems = {0};
            while (peek(p).kind != TOK_RBRACKET && peek(p).kind != TOK_EOF) {
                nl_push(p, &elems, parse_expr(p));
                match(p, TOK_COMMA);
            }
            expect(p, TOK_RBRACKET);
            int n; Node **arr = nl_finish(&elems, p->arena, &n);
            return ast_arraylit(p->arena, l, arr, n);
        }

        case TOK_LBRACE: {
            NList fields = {0};
            while (peek(p).kind != TOK_RBRACE && peek(p).kind != TOK_EOF) {
                Loc fl = loc(p);
                if (peek(p).kind == TOK_ATOM) {
                    Tok fname = next(p);
                    Node *val = parse_expr(p);
                    nl_push(p, &fields,
                            ast_field(p->arena, fl, val,
                                      fname.strval.data, fname.strval.len));
                } else {
                    nl_push(p, &fields, parse_expr(p));
                }
                match(p, TOK_COMMA);
            }
            expect(p, TOK_RBRACE);
            int n; Node **arr = nl_finish(&fields, p->arena, &n);
            return ast_mollit(p->arena, l, arr, n);
        }

        default:
            perror_(p, "unexpected token %s in expression", tok_kind_name(t.kind));
            return NULL;
    }
}
