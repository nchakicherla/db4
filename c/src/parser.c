#include "parser.h"

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
 * failing the parser if it isn't one - callers decide what "not a
 * literal" means in their context (a general expression vs. INSERT/UPDATE's
 * literal-only grammar want different error messages). */
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
        default:
            return NULL;
    }
}

/* INSERT ... VALUES (...) and UPDATE ... SET col = ... both take a literal
 * only - there's no arithmetic yet to make a general expression meaningful
 * there. */
static Expr *parse_literal(Parser *p) {
    if (p->failed) return NULL;
    Expr *e = make_literal_from_token(p);
    if (e) return e;
    parser_fail(p, p->cur.line, "expected a literal value but found %s", token_type_name(p->cur.type));
    return NULL;
}

static Expr *parse_primary(Parser *p) {
    if (p->failed) return NULL;

    switch (p->cur.type) {
        case TOK_IDENT: {
            Expr *e = new_expr(p, EXPR_COLUMN);
            if (!e) return NULL;
            e->as.column = arena_strndup(p->arena, p->cur.start, p->cur.len);
            advance(p);
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

static Expr *new_binary(Parser *p, BinaryOp op, Expr *left, Expr *right) {
    Expr *e = new_expr(p, EXPR_BINARY);
    if (!e) return NULL;
    e->as.binary.op    = op;
    e->as.binary.left  = left;
    e->as.binary.right = right;
    return e;
}

/* Comparisons don't chain (no "a = b = c") - one comparison per level, same
 * as most SQL dialects. */
static Expr *parse_comparison(Parser *p) {
    Expr *left = parse_primary(p);
    if (p->failed) return NULL;

    BinaryOp op;
    if (!cmp_op_for(p->cur.type, &op)) return left;
    advance(p);

    Expr *right = parse_primary(p);
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
        e->as.not_operand = operand;
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

static bool parse_column_list(Parser *p, ColumnList *out) {
    if (p->cur.type == TOK_STAR) {
        advance(p);
        *out = (ColumnList){ .is_star = true };
        return true;
    }

    size_t cap = 4;
    char **names = arena_alloc(p->arena, cap * sizeof(char *), _Alignof(char *));
    if (!names) {
        parser_fail(p, p->cur.line, "out of memory");
        return false;
    }
    size_t count = 0;

    for (;;) {
        if (!expect(p, TOK_IDENT, "a column name")) return false;

        if (count == cap) {
            size_t new_cap = cap * 2;
            char **grown = arena_grow(p->arena, names, cap * sizeof(char *),
                                       new_cap * sizeof(char *), _Alignof(char *));
            if (!grown) {
                parser_fail(p, p->cur.line, "out of memory");
                return false;
            }
            names = grown;
            cap   = new_cap;
        }

        names[count++] = arena_strndup(p->arena, p->cur.start, p->cur.len);
        advance(p);
        if (p->cur.type != TOK_COMMA) break;
        advance(p);
    }

    out->is_star = false;
    out->names   = names;
    out->count   = count;
    return true;
}

void parser_init(Parser *p, const char *sql, size_t len, Arena *a) {
    lexer_init(&p->lexer, sql, len);
    p->arena    = a;
    p->failed   = false;
    p->err[0]   = '\0';
    p->err_line = 0;

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

    if (p->cur.type == TOK_WHERE) {
        advance(p);
        stmt->where = parse_or(p);
        if (p->failed) return NULL;
    }

    if (p->cur.type == TOK_ORDER) {
        advance(p);
        if (!expect(p, TOK_BY, "BY")) return NULL;
        advance(p);
        if (!expect(p, TOK_IDENT, "a column name")) return NULL;

        stmt->has_order_by = true;
        stmt->order_by_col  = arena_strndup(p->arena, p->cur.start, p->cur.len);
        advance(p);

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
    if (!expect(p, TOK_LPAREN, "(")) return stmt;
    advance(p);

    size_t vcap = 4, vcount = 0;
    Expr **values = arena_alloc(p->arena, vcap * sizeof(Expr *), _Alignof(Expr *));
    if (!values) {
        parser_fail(p, p->cur.line, "out of memory");
        return stmt;
    }
    for (;;) {
        Expr *v = parse_literal(p);
        if (p->failed) return stmt;
        if (vcount == vcap) {
            size_t new_cap = vcap * 2;
            Expr **grown = arena_grow(p->arena, values, vcap * sizeof(Expr *),
                                       new_cap * sizeof(Expr *), _Alignof(Expr *));
            if (!grown) {
                parser_fail(p, p->cur.line, "out of memory");
                return stmt;
            }
            values = grown;
            vcap   = new_cap;
        }
        values[vcount++] = v;
        if (p->cur.type != TOK_COMMA) break;
        advance(p);
    }
    if (!expect(p, TOK_RPAREN, ")")) return stmt;
    advance(p);

    stmt.values   = values;
    stmt.n_values = vcount;
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
        Expr *val = parse_literal(p);
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

    return stmt;
}

bool parser_failed(const Parser *p) { return p->failed; }
const char *parser_error(const Parser *p) { return p->err; }
size_t parser_error_line(const Parser *p) { return p->err_line; }

static void print_expr(FILE *f, const Expr *e) {
    if (!e) return;

    switch (e->kind) {
        case EXPR_COLUMN:
            fprintf(f, "%s", e->as.column);
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
        case EXPR_NOT:
            fprintf(f, "(not ");
            print_expr(f, e->as.not_operand);
            fprintf(f, ")");
            break;
        case EXPR_BINARY: {
            static const char *names[] = {
                [OP_EQ] = "=", [OP_NE] = "!=", [OP_LT] = "<", [OP_LE] = "<=",
                [OP_GT] = ">", [OP_GE] = ">=", [OP_AND] = "and", [OP_OR] = "or",
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

void select_stmt_print(FILE *f, const SelectStmt *stmt) {
    fprintf(f, "select ");
    if (stmt->columns.is_star) {
        fprintf(f, "*");
    } else {
        for (size_t i = 0; i < stmt->columns.count; i++) {
            if (i) fprintf(f, ", ");
            fprintf(f, "%s", stmt->columns.names[i]);
        }
    }

    fprintf(f, " from %s", stmt->table);

    if (stmt->where) {
        fprintf(f, " where ");
        print_expr(f, stmt->where);
    }
    if (stmt->has_order_by) {
        fprintf(f, " order by %s %s", stmt->order_by_col, stmt->order_desc ? "desc" : "asc");
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
            fprintf(f, " values (");
            for (size_t i = 0; i < s->n_values; i++) {
                if (i) fprintf(f, ", ");
                print_expr(f, s->values[i]);
            }
            fprintf(f, ")\n");
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
        case STMT_BEGIN:    fprintf(f, "begin\n");    return;
        case STMT_COMMIT:   fprintf(f, "commit\n");   return;
        case STMT_ROLLBACK: fprintf(f, "rollback\n"); return;
    }
}
