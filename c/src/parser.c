#include "parser.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static Expr *parse_or(Parser *p);

static void parser_fail(Parser *p, size_t line, const char *fmt, ...) {
    if (p->failed) return;
    p->failed   = true;
    p->err_line = line;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof p->err, fmt, ap);
    va_end(ap);
}

static void advance(Parser *p) {
    if (p->failed) return;
    p->cur = lexer_next(&p->lexer);
    if (p->cur.type == TOK_ERROR) {
        parser_fail(p, p->cur.line, "unexpected character near \"%.*s\"",
                    (int)(p->cur.len ? p->cur.len : 1), p->cur.start);
    }
}

/* One-token lookahead, e.g. to tell "count(" (a function call) apart from
 * a plain column named "count" - Lexer is a plain POD cursor over the
 * source buffer, so saving/restoring it is enough to "un-advance". */
static Token peek_next(Parser *p) {
    Lexer saved = p->lexer;
    Token t = lexer_next(&p->lexer);
    p->lexer = saved;
    return t;
}

static bool expect(Parser *p, TokenType type, const char *what) {
    if (p->failed) return false;
    if (p->cur.type != type) {
        parser_fail(p, p->cur.line, "expected %s but found %s", what, token_type_name(p->cur.type));
        return false;
    }
    return true;
}

static Expr *new_expr(Parser *p, ExprKind kind) {
    Expr *e = arena_alloc_type(p->arena, Expr);
    if (!e) {
        parser_fail(p, p->cur.line, "out of memory");
        return NULL;
    }
    e->kind = kind;
    return e;
}

static char *copy_token_text(Parser *p, char *buf, size_t buf_len) {
    size_t n = p->cur.len < buf_len - 1 ? p->cur.len : buf_len - 1;
    memcpy(buf, p->cur.start, n);
    buf[n] = '\0';
    return buf;
}

/* Caller must already have confirmed p->cur.type == TOK_IDENT. Consumes
 * either "name" or "table.name". */
static void parse_qualified_name(Parser *p, const char **out_table, const char **out_name) {
    char *first = arena_strndup(p->arena, p->cur.start, p->cur.len);
    advance(p);
    if (p->cur.type != TOK_DOT) {
        *out_table = NULL;
        *out_name  = first;
        return;
    }
    advance(p);
    if (!expect(p, TOK_IDENT, "a column name")) {
        *out_table = NULL;
        *out_name  = NULL;
        return;
    }
    char *second = arena_strndup(p->arena, p->cur.start, p->cur.len);
    advance(p);
    *out_table = first;
    *out_name  = second;
}

static Expr *parse_string_literal(Parser *p) {
    Expr *e = new_expr(p, EXPR_LIT_STRING);
    if (!e) return NULL;

    /* cur.len spans the whole "'...'" run, quotes included. */
    size_t raw_len = p->cur.len >= 2 ? p->cur.len - 2 : 0;
    char *buf = arena_alloc(p->arena, raw_len + 1, _Alignof(char));
    if (!buf) {
        parser_fail(p, p->cur.line, "out of memory");
        return NULL;
    }

    size_t out_i = 0;
    for (size_t i = 0; i < raw_len; i++) {
        char c = p->cur.start[1 + i];
        if (c == '\'' && i + 1 < raw_len && p->cur.start[1 + i + 1] == '\'') i++;
        buf[out_i++] = c;
    }
    buf[out_i] = '\0';

    e->as.string_value.data = buf;
    e->as.string_value.len  = out_i;
    advance(p);
    return e;
}

/* Tries to build a literal from the current token; returns NULL without
 * failing the parser if it isn't one. */
static Expr *make_literal_from_token(Parser *p) {
    switch (p->cur.type) {
        case TOK_INT: {
            Expr *e = new_expr(p, EXPR_LIT_INT);
            if (!e) return NULL;
            char buf[32];
            e->as.int_value = strtoll(copy_token_text(p, buf, sizeof buf), NULL, 10);
            advance(p);
            return e;
        }
        case TOK_FLOAT: {
            Expr *e = new_expr(p, EXPR_LIT_DOUBLE);
            if (!e) return NULL;
            char buf[64];
            e->as.double_value = strtod(copy_token_text(p, buf, sizeof buf), NULL);
            advance(p);
            return e;
        }
        case TOK_STRING:
            return parse_string_literal(p);
        case TOK_TRUE:
        case TOK_FALSE: {
            Expr *e = new_expr(p, EXPR_LIT_BOOL);
            if (!e) return NULL;
            e->as.bool_value = (p->cur.type == TOK_TRUE);
            advance(p);
            return e;
        }
        case TOK_NULL: {
            Expr *e = new_expr(p, EXPR_LIT_NULL);
            if (!e) return NULL;
            advance(p);
            return e;
        }
        case TOK_QUESTION: {
            Expr *e = new_expr(p, EXPR_PARAM);
            if (!e) return NULL;
            e->as.param_index = (int)(++p->param_count);
            advance(p);
            return e;
        }
        default:
            return NULL;
    }
}

static Expr *parse_primary(Parser *p) {
    if (p->failed) return NULL;

    switch (p->cur.type) {
        case TOK_IDENT: {
            Expr *e = new_expr(p, EXPR_COLUMN);
            if (!e) return NULL;
            parse_qualified_name(p, &e->as.column.table, &e->as.column.name);
            if (p->failed) return NULL;
            return e;
        }
        case TOK_LPAREN: {
            advance(p);
            Expr *inner = parse_or(p);
            if (p->failed) return NULL;
            if (!expect(p, TOK_RPAREN, ")")) return NULL;
            advance(p);
            return inner;
        }
        default: {
            Expr *e = make_literal_from_token(p);
            if (e) return e;
            parser_fail(p, p->cur.line, "expected an expression but found %s", token_type_name(p->cur.type));
            return NULL;
        }
    }
}

static Expr *new_binary(Parser *p, BinaryOp op, Expr *left, Expr *right) {
    Expr *e = new_expr(p, EXPR_BINARY);
    if (!e) return NULL;
    e->as.binary.op    = op;
    e->as.binary.left  = left;
    e->as.binary.right = right;
    return e;
}

/* Unary minus binds tighter than * and /, which bind tighter than + and -,
 * which bind tighter than comparisons - the usual arithmetic precedence
 * ladder, with parse_primary (columns/literals/parens) at the bottom. */
static Expr *parse_unary(Parser *p) {
    if (p->failed) return NULL;
    if (p->cur.type == TOK_MINUS) {
        advance(p);
        Expr *operand = parse_unary(p);
        if (p->failed) return NULL;
        Expr *e = new_expr(p, EXPR_NEG);
        if (!e) return NULL;
        e->as.unary_operand = operand;
        return e;
    }
    return parse_primary(p);
}

static Expr *parse_term(Parser *p) {
    Expr *left = parse_unary(p);
    if (p->failed) return NULL;
    while (p->cur.type == TOK_STAR || p->cur.type == TOK_SLASH) {
        BinaryOp op = p->cur.type == TOK_STAR ? OP_MUL : OP_DIV;
        advance(p);
        Expr *right = parse_unary(p);
        if (p->failed) return NULL;
        left = new_binary(p, op, left, right);
        if (p->failed) return NULL;
    }
    return left;
}

static Expr *parse_arith(Parser *p) {
    Expr *left = parse_term(p);
    if (p->failed) return NULL;
    while (p->cur.type == TOK_PLUS || p->cur.type == TOK_MINUS) {
        BinaryOp op = p->cur.type == TOK_PLUS ? OP_ADD : OP_SUB;
        advance(p);
        Expr *right = parse_term(p);
        if (p->failed) return NULL;
        left = new_binary(p, op, left, right);
        if (p->failed) return NULL;
    }
    return left;
}

static bool cmp_op_for(TokenType t, BinaryOp *out) {
    switch (t) {
        case TOK_EQ: *out = OP_EQ; return true;
        case TOK_NE: *out = OP_NE; return true;
        case TOK_LT: *out = OP_LT; return true;
        case TOK_LE: *out = OP_LE; return true;
        case TOK_GT: *out = OP_GT; return true;
        case TOK_GE: *out = OP_GE; return true;
        default: return false;
    }
}

/* Comparisons don't chain (no "a = b = c") - one comparison per level, same
 * as most SQL dialects. */
static Expr *parse_comparison(Parser *p) {
    Expr *left = parse_arith(p);
    if (p->failed) return NULL;

    BinaryOp op;
    if (!cmp_op_for(p->cur.type, &op)) return left;
    advance(p);

    Expr *right = parse_arith(p);
    if (p->failed) return NULL;

    return new_binary(p, op, left, right);
}

static Expr *parse_not(Parser *p) {
    if (p->failed) return NULL;
    if (p->cur.type == TOK_NOT) {
        advance(p);
        Expr *operand = parse_not(p);
        if (p->failed) return NULL;
        Expr *e = new_expr(p, EXPR_NOT);
        if (!e) return NULL;
        e->as.unary_operand = operand;
        return e;
    }
    return parse_comparison(p);
}

static Expr *parse_and(Parser *p) {
    Expr *left = parse_not(p);
    if (p->failed) return NULL;
    while (p->cur.type == TOK_AND) {
        advance(p);
        Expr *right = parse_not(p);
        if (p->failed) return NULL;
        left = new_binary(p, OP_AND, left, right);
        if (p->failed) return NULL;
    }
    return left;
}

static Expr *parse_or(Parser *p) {
    Expr *left = parse_and(p);
    if (p->failed) return NULL;
    while (p->cur.type == TOK_OR) {
        advance(p);
        Expr *right = parse_and(p);
        if (p->failed) return NULL;
        left = new_binary(p, OP_OR, left, right);
        if (p->failed) return NULL;
    }
    return left;
}

static bool agg_func_for(Parser *p, AggFunc *out) {
    char buf[8];
    copy_token_text(p, buf, sizeof buf);
    for (size_t i = 0; buf[i]; i++) buf[i] = (char)tolower((unsigned char)buf[i]);
    if (strcmp(buf, "count") == 0) { *out = AGG_COUNT; return true; }
    if (strcmp(buf, "sum")   == 0) { *out = AGG_SUM;   return true; }
    if (strcmp(buf, "avg")   == 0) { *out = AGG_AVG;   return true; }
    return false;
}

static const char *agg_func_label(AggFunc f) {
    switch (f) {
        case AGG_COUNT: return "COUNT";
        case AGG_SUM:   return "SUM";
        case AGG_AVG:   return "AVG";
    }
    return "?";
}

/* A SELECT item is either "count(...)"/"sum(...)"/"avg(...)" (an
 * aggregate call - determined by one-token lookahead for '(' right after
 * an identifier) or a plain, optionally table-qualified column. */
static bool parse_select_item(Parser *p, SelectItem *out) {
    if (!expect(p, TOK_IDENT, "a column name or aggregate function")) return false;

    if (peek_next(p).type == TOK_LPAREN) {
        AggFunc func;
        if (!agg_func_for(p, &func)) {
            char buf[32];
            copy_token_text(p, buf, sizeof buf);
            parser_fail(p, p->cur.line, "unknown function \"%s\"", buf);
            return false;
        }
        advance(p); /* function name */
        advance(p); /* ( */

        out->is_agg   = true;
        out->agg_func = func;

        if (p->cur.type == TOK_STAR) {
            if (func != AGG_COUNT) {
                parser_fail(p, p->cur.line, "%s(*) is not supported - only COUNT(*)", agg_func_label(func));
                return false;
            }
            advance(p);
            out->agg_arg_is_star = true;
        } else {
            if (!expect(p, TOK_IDENT, "a column name or *")) return false;
            parse_qualified_name(p, &out->agg_arg_table, &out->agg_arg_column);
            if (p->failed) return false;
        }

        if (!expect(p, TOK_RPAREN, ")")) return false;
        advance(p);
        return true;
    }

    out->is_agg = false;
    parse_qualified_name(p, &out->table, &out->column);
    return !p->failed;
}

static bool parse_column_list(Parser *p, ColumnList *out) {
    if (p->cur.type == TOK_STAR) {
        advance(p);
        *out = (ColumnList){ .is_star = true };
        return true;
    }

    size_t cap = 4, count = 0;
    SelectItem *items = arena_alloc(p->arena, cap * sizeof(SelectItem), _Alignof(SelectItem));
    if (!items) {
        parser_fail(p, p->cur.line, "out of memory");
        return false;
    }

    for (;;) {
        SelectItem item = {0};
        if (!parse_select_item(p, &item)) return false;

        if (count == cap) {
            size_t new_cap = cap * 2;
            SelectItem *grown = arena_grow(p->arena, items, cap * sizeof(SelectItem),
                                            new_cap * sizeof(SelectItem), _Alignof(SelectItem));
            if (!grown) {
                parser_fail(p, p->cur.line, "out of memory");
                return false;
            }
            items = grown;
            cap   = new_cap;
        }
        items[count++] = item;

        if (p->cur.type != TOK_COMMA) break;
        advance(p);
    }

    out->is_star = false;
    out->items   = items;
    out->count   = count;
    return true;
}

static bool parse_joins(Parser *p, JoinClause **out_joins, size_t *out_n) {
    JoinClause *joins = NULL;
    size_t cap = 0, count = 0;

    while (p->cur.type == TOK_INNER || p->cur.type == TOK_JOIN) {
        if (p->cur.type == TOK_INNER) {
            advance(p);
            if (!expect(p, TOK_JOIN, "JOIN")) return false;
        }
        advance(p);

        if (!expect(p, TOK_IDENT, "a table name")) return false;
        char *table = arena_strndup(p->arena, p->cur.start, p->cur.len);
        advance(p);

        if (!expect(p, TOK_ON, "ON")) return false;
        advance(p);
        Expr *on = parse_or(p);
        if (p->failed) return false;

        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            JoinClause *grown = arena_grow(p->arena, joins, cap * sizeof(JoinClause),
                                           new_cap * sizeof(JoinClause), _Alignof(JoinClause));
            if (!grown) {
                parser_fail(p, p->cur.line, "out of memory");
                return false;
            }
            joins = grown;
            cap   = new_cap;
        }
        joins[count].table = table;
        joins[count].on    = on;
        count++;
    }

    *out_joins = joins;
    *out_n     = count;
    return true;
}

void parser_init(Parser *p, const char *sql, size_t len, Arena *a) {
    lexer_init(&p->lexer, sql, len);
    p->arena    = a;
    p->failed      = false;
    p->err[0]      = '\0';
    p->err_line    = 0;
    p->param_count = 0;

    p->cur = lexer_next(&p->lexer);
    if (p->cur.type == TOK_ERROR) {
        parser_fail(p, p->cur.line, "unexpected character near \"%.*s\"",
                    (int)(p->cur.len ? p->cur.len : 1), p->cur.start);
    }
}

SelectStmt *parser_parse_select(Parser *p) {
    if (p->failed) return NULL;

    SelectStmt *stmt = arena_zalloc(p->arena, sizeof(SelectStmt), _Alignof(SelectStmt));
    if (!stmt) {
        parser_fail(p, p->cur.line, "out of memory");
        return NULL;
    }

    if (!expect(p, TOK_SELECT, "SELECT")) return NULL;
    advance(p);

    if (!parse_column_list(p, &stmt->columns)) return NULL;

    if (!expect(p, TOK_FROM, "FROM")) return NULL;
    advance(p);

    if (!expect(p, TOK_IDENT, "a table name")) return NULL;
    stmt->table = arena_strndup(p->arena, p->cur.start, p->cur.len);
    advance(p);

    if (!parse_joins(p, &stmt->joins, &stmt->n_joins)) return NULL;

    if (p->cur.type == TOK_WHERE) {
        advance(p);
        stmt->where = parse_or(p);
        if (p->failed) return NULL;
    }

    if (p->cur.type == TOK_GROUP) {
        advance(p);
        if (!expect(p, TOK_BY, "BY")) return NULL;
        advance(p);

        size_t cap = 4, count = 0;
        char **cols = arena_alloc(p->arena, cap * sizeof(char *), _Alignof(char *));
        if (!cols) {
            parser_fail(p, p->cur.line, "out of memory");
            return NULL;
        }
        for (;;) {
            if (!expect(p, TOK_IDENT, "a column name")) return NULL;
            if (count == cap) {
                size_t new_cap = cap * 2;
                char **grown = arena_grow(p->arena, cols, cap * sizeof(char *),
                                           new_cap * sizeof(char *), _Alignof(char *));
                if (!grown) {
                    parser_fail(p, p->cur.line, "out of memory");
                    return NULL;
                }
                cols = grown;
                cap  = new_cap;
            }
            /* GROUP BY is single-table-only for now (see interp.c's grouped/
             * JOIN rejection), so an optional "table." qualifier is accepted
             * syntactically and then discarded rather than left unparsed -
             * a joined query with a qualified GROUP BY column should hit
             * that clear rejection, not a raw parser error here. */
            const char *qual, *name;
            parse_qualified_name(p, &qual, &name);
            if (p->failed) return NULL;
            cols[count++] = (char *)name;
            if (p->cur.type != TOK_COMMA) break;
            advance(p);
        }
        stmt->group_by   = cols;
        stmt->n_group_by = count;
    }

    if (p->cur.type == TOK_ORDER) {
        advance(p);
        if (!expect(p, TOK_BY, "BY")) return NULL;
        advance(p);
        if (!expect(p, TOK_IDENT, "a column name")) return NULL;

        stmt->has_order_by = true;
        parse_qualified_name(p, &stmt->order_by_table, &stmt->order_by_col);
        if (p->failed) return NULL;

        if (p->cur.type == TOK_ASC) {
            advance(p);
        } else if (p->cur.type == TOK_DESC) {
            stmt->order_desc = true;
            advance(p);
        }
    }

    if (p->cur.type == TOK_LIMIT) {
        advance(p);
        if (!expect(p, TOK_INT, "a number")) return NULL;
        char buf[32];
        stmt->has_limit = true;
        stmt->limit     = strtoll(copy_token_text(p, buf, sizeof buf), NULL, 10);
        advance(p);
    }

    if (p->cur.type == TOK_SEMICOLON) advance(p);

    if (!expect(p, TOK_EOF, "end of statement")) return NULL;

    return stmt;
}

static bool parse_value_row(Parser *p, ValueRow *out) {
    if (!expect(p, TOK_LPAREN, "(")) return false;
    advance(p);

    size_t cap = 4, count = 0;
    Expr **values = arena_alloc(p->arena, cap * sizeof(Expr *), _Alignof(Expr *));
    if (!values) {
        parser_fail(p, p->cur.line, "out of memory");
        return false;
    }
    for (;;) {
        Expr *v = parse_or(p);
        if (p->failed) return false;
        if (count == cap) {
            size_t new_cap = cap * 2;
            Expr **grown = arena_grow(p->arena, values, cap * sizeof(Expr *),
                                      new_cap * sizeof(Expr *), _Alignof(Expr *));
            if (!grown) {
                parser_fail(p, p->cur.line, "out of memory");
                return false;
            }
            values = grown;
            cap    = new_cap;
        }
        values[count++] = v;
        if (p->cur.type != TOK_COMMA) break;
        advance(p);
    }
    if (!expect(p, TOK_RPAREN, ")")) return false;
    advance(p);

    out->values   = values;
    out->n_values = count;
    return true;
}

static InsertStmt parse_insert_stmt(Parser *p) {
    InsertStmt stmt = {0};

    if (!expect(p, TOK_INSERT, "INSERT")) return stmt;
    advance(p);
    if (!expect(p, TOK_INTO, "INTO")) return stmt;
    advance(p);
    if (!expect(p, TOK_IDENT, "a table name")) return stmt;
    stmt.table = arena_strndup(p->arena, p->cur.start, p->cur.len);
    advance(p);

    if (p->cur.type == TOK_LPAREN) {
        advance(p);
        size_t cap = 4, count = 0;
        char **names = arena_alloc(p->arena, cap * sizeof(char *), _Alignof(char *));
        if (!names) {
            parser_fail(p, p->cur.line, "out of memory");
            return stmt;
        }
        for (;;) {
            if (!expect(p, TOK_IDENT, "a column name")) return stmt;
            if (count == cap) {
                size_t new_cap = cap * 2;
                char **grown = arena_grow(p->arena, names, cap * sizeof(char *),
                                           new_cap * sizeof(char *), _Alignof(char *));
                if (!grown) {
                    parser_fail(p, p->cur.line, "out of memory");
                    return stmt;
                }
                names = grown;
                cap   = new_cap;
            }
            names[count++] = arena_strndup(p->arena, p->cur.start, p->cur.len);
            advance(p);
            if (p->cur.type != TOK_COMMA) break;
            advance(p);
        }
        if (!expect(p, TOK_RPAREN, ")")) return stmt;
        advance(p);
        stmt.columns   = names;
        stmt.n_columns = count;
    }

    if (!expect(p, TOK_VALUES, "VALUES")) return stmt;
    advance(p);

    size_t cap = 4, count = 0;
    ValueRow *rows = arena_alloc(p->arena, cap * sizeof(ValueRow), _Alignof(ValueRow));
    if (!rows) {
        parser_fail(p, p->cur.line, "out of memory");
        return stmt;
    }
    for (;;) {
        ValueRow row = {0};
        if (!parse_value_row(p, &row)) return stmt;

        if (count == cap) {
            size_t new_cap = cap * 2;
            ValueRow *grown = arena_grow(p->arena, rows, cap * sizeof(ValueRow),
                                          new_cap * sizeof(ValueRow), _Alignof(ValueRow));
            if (!grown) {
                parser_fail(p, p->cur.line, "out of memory");
                return stmt;
            }
            rows = grown;
            cap  = new_cap;
        }
        rows[count++] = row;

        if (p->cur.type != TOK_COMMA) break;
        advance(p);
    }

    stmt.rows   = rows;
    stmt.n_rows = count;
    return stmt;
}

static UpdateStmt parse_update_stmt(Parser *p) {
    UpdateStmt stmt = {0};

    if (!expect(p, TOK_UPDATE, "UPDATE")) return stmt;
    advance(p);
    if (!expect(p, TOK_IDENT, "a table name")) return stmt;
    stmt.table = arena_strndup(p->arena, p->cur.start, p->cur.len);
    advance(p);

    if (!expect(p, TOK_SET, "SET")) return stmt;
    advance(p);

    size_t cap = 4, count = 0;
    Assignment *assigns = arena_alloc(p->arena, cap * sizeof(Assignment), _Alignof(Assignment));
    if (!assigns) {
        parser_fail(p, p->cur.line, "out of memory");
        return stmt;
    }
    for (;;) {
        if (!expect(p, TOK_IDENT, "a column name")) return stmt;
        char *col = arena_strndup(p->arena, p->cur.start, p->cur.len);
        advance(p);
        if (!expect(p, TOK_EQ, "=")) return stmt;
        advance(p);
        Expr *val = parse_or(p);
        if (p->failed) return stmt;

        if (count == cap) {
            size_t new_cap = cap * 2;
            Assignment *grown = arena_grow(p->arena, assigns, cap * sizeof(Assignment),
                                            new_cap * sizeof(Assignment), _Alignof(Assignment));
            if (!grown) {
                parser_fail(p, p->cur.line, "out of memory");
                return stmt;
            }
            assigns = grown;
            cap     = new_cap;
        }
        assigns[count].column = col;
        assigns[count].value  = val;
        count++;

        if (p->cur.type != TOK_COMMA) break;
        advance(p);
    }

    stmt.assignments   = assigns;
    stmt.n_assignments = count;

    if (p->cur.type == TOK_WHERE) {
        advance(p);
        stmt.where = parse_or(p);
        if (p->failed) return stmt;
    }
    return stmt;
}

static DeleteStmt parse_delete_stmt(Parser *p) {
    DeleteStmt stmt = {0};

    if (!expect(p, TOK_DELETE, "DELETE")) return stmt;
    advance(p);
    if (!expect(p, TOK_FROM, "FROM")) return stmt;
    advance(p);
    if (!expect(p, TOK_IDENT, "a table name")) return stmt;
    stmt.table = arena_strndup(p->arena, p->cur.start, p->cur.len);
    advance(p);

    if (p->cur.type == TOK_WHERE) {
        advance(p);
        stmt.where = parse_or(p);
        if (p->failed) return stmt;
    }
    return stmt;
}

static FkAction parse_fk_action(Parser *p) {
    if (p->cur.type == TOK_CASCADE) {
        advance(p);
        return FK_ACTION_CASCADE;
    }
    if (p->cur.type == TOK_RESTRICT) {
        advance(p);
        return FK_ACTION_RESTRICT;
    }
    if (p->cur.type == TOK_SET) {
        advance(p);
        if (!expect(p, TOK_NULL, "NULL")) return FK_ACTION_NONE;
        advance(p);
        return FK_ACTION_SET_NULL;
    }
    parser_fail(p, p->cur.line, "expected CASCADE, RESTRICT, or SET NULL but found %s", token_type_name(p->cur.type));
    return FK_ACTION_NONE;
}

static ColumnDef parse_column_def(Parser *p) {
    ColumnDef c = {0};

    if (!expect(p, TOK_IDENT, "a column name")) return c;
    c.name = arena_strndup(p->arena, p->cur.start, p->cur.len);
    advance(p);

    if (!expect(p, TOK_IDENT, "a column type")) return c;
    char type_buf[16];
    copy_token_text(p, type_buf, sizeof type_buf);
    if (!field_type_from_name(type_buf, &c.type)) {
        parser_fail(p, p->cur.line, "unknown column type \"%s\"", type_buf);
        return c;
    }
    advance(p);

    if (p->cur.type == TOK_PRIMARY) {
        advance(p);
        if (!expect(p, TOK_KEY, "KEY")) return c;
        advance(p);
        c.primary = true;
    }

    if (p->cur.type == TOK_REFERENCES) {
        advance(p);
        if (!expect(p, TOK_IDENT, "a table name")) return c;
        c.fk_table = arena_strndup(p->arena, p->cur.start, p->cur.len);
        advance(p);
        if (!expect(p, TOK_LPAREN, "(")) return c;
        advance(p);
        if (!expect(p, TOK_IDENT, "a column name")) return c;
        c.fk_column = arena_strndup(p->arena, p->cur.start, p->cur.len);
        advance(p);
        if (!expect(p, TOK_RPAREN, ")")) return c;
        advance(p);
        c.has_fk = true;

        while (p->cur.type == TOK_ON) {
            advance(p);
            if (p->cur.type == TOK_DELETE) {
                advance(p);
                c.fk_on_delete = parse_fk_action(p);
                if (p->failed) return c;
            } else if (p->cur.type == TOK_UPDATE) {
                advance(p);
                c.fk_on_update = parse_fk_action(p);
                if (p->failed) return c;
            } else {
                parser_fail(p, p->cur.line, "expected DELETE or UPDATE but found %s", token_type_name(p->cur.type));
                return c;
            }
        }
    }
    return c;
}

static CreateTableStmt parse_create_table_stmt(Parser *p) {
    CreateTableStmt stmt = {0};

    if (!expect(p, TOK_CREATE, "CREATE")) return stmt;
    advance(p);
    if (!expect(p, TOK_TABLE, "TABLE")) return stmt;
    advance(p);
    if (!expect(p, TOK_IDENT, "a table name")) return stmt;
    stmt.table = arena_strndup(p->arena, p->cur.start, p->cur.len);
    advance(p);

    if (!expect(p, TOK_LPAREN, "(")) return stmt;
    advance(p);

    size_t cap = 4, count = 0;
    ColumnDef *cols = arena_alloc(p->arena, cap * sizeof(ColumnDef), _Alignof(ColumnDef));
    if (!cols) {
        parser_fail(p, p->cur.line, "out of memory");
        return stmt;
    }

    for (;;) {
        ColumnDef c = parse_column_def(p);
        if (p->failed) return stmt;

        if (count == cap) {
            size_t new_cap = cap * 2;
            ColumnDef *grown = arena_grow(p->arena, cols, cap * sizeof(ColumnDef),
                                          new_cap * sizeof(ColumnDef), _Alignof(ColumnDef));
            if (!grown) {
                parser_fail(p, p->cur.line, "out of memory");
                return stmt;
            }
            cols = grown;
            cap  = new_cap;
        }
        cols[count++] = c;

        if (p->cur.type != TOK_COMMA) break;
        advance(p);
    }
    if (!expect(p, TOK_RPAREN, ")")) return stmt;
    advance(p);

    stmt.columns   = cols;
    stmt.n_columns = count;
    return stmt;
}

Stmt *parser_parse_statement(Parser *p) {
    if (p->failed) return NULL;

    Stmt *stmt = arena_alloc_type(p->arena, Stmt);
    if (!stmt) {
        parser_fail(p, p->cur.line, "out of memory");
        return NULL;
    }

    switch (p->cur.type) {
        case TOK_SELECT: {
            SelectStmt *sel = parser_parse_select(p);
            if (p->failed) return NULL;
            stmt->kind      = STMT_SELECT;
            stmt->as.select = *sel;
            stmt->n_params  = p->param_count;
            return stmt;
        }
        case TOK_INSERT:
            stmt->kind      = STMT_INSERT;
            stmt->as.insert = parse_insert_stmt(p);
            break;
        case TOK_UPDATE:
            stmt->kind      = STMT_UPDATE;
            stmt->as.update = parse_update_stmt(p);
            break;
        case TOK_DELETE:
            stmt->kind   = STMT_DELETE;
            stmt->as.del = parse_delete_stmt(p);
            break;
        case TOK_CREATE:
            stmt->kind          = STMT_CREATE_TABLE;
            stmt->as.create_table = parse_create_table_stmt(p);
            break;
        case TOK_BEGIN:
            advance(p);
            if (p->cur.type == TOK_TRANSACTION) advance(p);
            stmt->kind = STMT_BEGIN;
            break;
        case TOK_COMMIT:
            advance(p);
            stmt->kind = STMT_COMMIT;
            break;
        case TOK_ROLLBACK:
            advance(p);
            stmt->kind = STMT_ROLLBACK;
            break;
        default:
            parser_fail(p, p->cur.line, "expected a statement but found %s", token_type_name(p->cur.type));
            return NULL;
    }

    if (p->failed) return NULL;
    if (p->cur.type == TOK_SEMICOLON) advance(p);
    if (!expect(p, TOK_EOF, "end of statement")) return NULL;

    stmt->n_params = p->param_count;
    return stmt;
}

bool parser_failed(const Parser *p) { return p->failed; }
const char *parser_error(const Parser *p) { return p->err; }
size_t parser_error_line(const Parser *p) { return p->err_line; }

static const char *fk_action_label(FkAction action) {
    switch (action) {
        case FK_ACTION_CASCADE:  return "cascade";
        case FK_ACTION_SET_NULL: return "set null";
        case FK_ACTION_RESTRICT: return "restrict";
        default:                 return "";
    }
}

static void print_qualified(FILE *f, const char *table, const char *name) {
    if (table) fprintf(f, "%s.%s", table, name);
    else       fprintf(f, "%s", name);
}

static void print_expr(FILE *f, const Expr *e) {
    if (!e) return;

    switch (e->kind) {
        case EXPR_COLUMN:
            print_qualified(f, e->as.column.table, e->as.column.name);
            break;
        case EXPR_LIT_INT:
            fprintf(f, "%lld", (long long)e->as.int_value);
            break;
        case EXPR_LIT_DOUBLE:
            fprintf(f, "%g", e->as.double_value);
            break;
        case EXPR_LIT_STRING:
            fprintf(f, "'%.*s'", (int)e->as.string_value.len, e->as.string_value.data);
            break;
        case EXPR_LIT_BOOL:
            fprintf(f, "%s", e->as.bool_value ? "true" : "false");
            break;
        case EXPR_LIT_NULL:
            fprintf(f, "null");
            break;
        case EXPR_PARAM:
            fprintf(f, "?%d", e->as.param_index);
            break;
        case EXPR_NOT:
            fprintf(f, "(not ");
            print_expr(f, e->as.unary_operand);
            fprintf(f, ")");
            break;
        case EXPR_NEG:
            fprintf(f, "(-");
            print_expr(f, e->as.unary_operand);
            fprintf(f, ")");
            break;
        case EXPR_BINARY: {
            static const char *names[] = {
                [OP_EQ] = "=", [OP_NE] = "!=", [OP_LT] = "<", [OP_LE] = "<=",
                [OP_GT] = ">", [OP_GE] = ">=", [OP_AND] = "and", [OP_OR] = "or",
                [OP_ADD] = "+", [OP_SUB] = "-", [OP_MUL] = "*", [OP_DIV] = "/",
            };
            fprintf(f, "(");
            print_expr(f, e->as.binary.left);
            fprintf(f, " %s ", names[e->as.binary.op]);
            print_expr(f, e->as.binary.right);
            fprintf(f, ")");
            break;
        }
    }
}

static void print_select_item(FILE *f, const SelectItem *item) {
    if (!item->is_agg) {
        print_qualified(f, item->table, item->column);
        return;
    }
    const char *name = item->agg_func == AGG_COUNT ? "count" : item->agg_func == AGG_SUM ? "sum" : "avg";
    fprintf(f, "%s(", name);
    if (item->agg_arg_is_star) fprintf(f, "*");
    else print_qualified(f, item->agg_arg_table, item->agg_arg_column);
    fprintf(f, ")");
}

void select_stmt_print(FILE *f, const SelectStmt *stmt) {
    fprintf(f, "select ");
    if (stmt->columns.is_star) {
        fprintf(f, "*");
    } else {
        for (size_t i = 0; i < stmt->columns.count; i++) {
            if (i) fprintf(f, ", ");
            print_select_item(f, &stmt->columns.items[i]);
        }
    }

    fprintf(f, " from %s", stmt->table);

    for (size_t i = 0; i < stmt->n_joins; i++) {
        fprintf(f, " inner join %s on ", stmt->joins[i].table);
        print_expr(f, stmt->joins[i].on);
    }

    if (stmt->where) {
        fprintf(f, " where ");
        print_expr(f, stmt->where);
    }
    if (stmt->n_group_by) {
        fprintf(f, " group by ");
        for (size_t i = 0; i < stmt->n_group_by; i++) {
            if (i) fprintf(f, ", ");
            fprintf(f, "%s", stmt->group_by[i]);
        }
    }
    if (stmt->has_order_by) {
        fprintf(f, " order by ");
        print_qualified(f, stmt->order_by_table, stmt->order_by_col);
        fprintf(f, " %s", stmt->order_desc ? "desc" : "asc");
    }
    if (stmt->has_limit) {
        fprintf(f, " limit %lld", (long long)stmt->limit);
    }
    fprintf(f, "\n");
}

void stmt_print(FILE *f, const Stmt *stmt) {
    switch (stmt->kind) {
        case STMT_SELECT:
            select_stmt_print(f, &stmt->as.select);
            return;
        case STMT_INSERT: {
            const InsertStmt *s = &stmt->as.insert;
            fprintf(f, "insert into %s", s->table);
            if (s->columns) {
                fprintf(f, " (");
                for (size_t i = 0; i < s->n_columns; i++) {
                    if (i) fprintf(f, ", ");
                    fprintf(f, "%s", s->columns[i]);
                }
                fprintf(f, ")");
            }
            fprintf(f, " values ");
            for (size_t r = 0; r < s->n_rows; r++) {
                if (r) fprintf(f, ", ");
                fprintf(f, "(");
                for (size_t i = 0; i < s->rows[r].n_values; i++) {
                    if (i) fprintf(f, ", ");
                    print_expr(f, s->rows[r].values[i]);
                }
                fprintf(f, ")");
            }
            fprintf(f, "\n");
            return;
        }
        case STMT_UPDATE: {
            const UpdateStmt *s = &stmt->as.update;
            fprintf(f, "update %s set ", s->table);
            for (size_t i = 0; i < s->n_assignments; i++) {
                if (i) fprintf(f, ", ");
                fprintf(f, "%s = ", s->assignments[i].column);
                print_expr(f, s->assignments[i].value);
            }
            if (s->where) {
                fprintf(f, " where ");
                print_expr(f, s->where);
            }
            fprintf(f, "\n");
            return;
        }
        case STMT_DELETE: {
            const DeleteStmt *s = &stmt->as.del;
            fprintf(f, "delete from %s", s->table);
            if (s->where) {
                fprintf(f, " where ");
                print_expr(f, s->where);
            }
            fprintf(f, "\n");
            return;
        }
        case STMT_CREATE_TABLE: {
            const CreateTableStmt *s = &stmt->as.create_table;
            fprintf(f, "create table %s (", s->table);
            for (size_t i = 0; i < s->n_columns; i++) {
                const ColumnDef *c = &s->columns[i];
                if (i) fprintf(f, ", ");
                fprintf(f, "%s %s", c->name, field_type_label(c->type));
                if (c->primary) fprintf(f, " primary key");
                if (c->has_fk) {
                    fprintf(f, " references %s(%s)", c->fk_table, c->fk_column);
                    if (c->fk_on_delete != FK_ACTION_NONE)
                        fprintf(f, " on delete %s", fk_action_label(c->fk_on_delete));
                    if (c->fk_on_update != FK_ACTION_NONE)
                        fprintf(f, " on update %s", fk_action_label(c->fk_on_update));
                }
            }
            fprintf(f, ")\n");
            return;
        }
        case STMT_BEGIN:    fprintf(f, "begin\n");    return;
        case STMT_COMMIT:   fprintf(f, "commit\n");   return;
        case STMT_ROLLBACK: fprintf(f, "rollback\n"); return;
    }
}
