#include "db4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "interp.h"
#include "parser.h"
#include "result.h"
#include "value.h"

struct Db4Stmt {
    Db4       *db;
    Arena      arena; /* owns the parsed AST (stmt) below */
    Stmt      *stmt;

    bool       is_select;
    bool       executed; /* has the first db4_step run the statement yet? */
    bool       failed;

    ResultSet  rs;      /* valid once executed, for a SELECT */
    size_t     row_idx; /* next row to serve; row_idx - 1 is "current" after DB4_ROW */
};

bool db4_open(Db4 *db) {
    db->catalog = (Catalog){0};
    txn_init(&db->txn);
    db->errmsg[0] = '\0';
    db->changes   = 0;
    return true;
}

void db4_close(Db4 *db) {
    txn_term(&db->txn);
    catalog_term(&db->catalog);
}

bool db4_prepare(Db4 *db, const char *sql, size_t sql_len, Db4Stmt **out_stmt, const char **out_tail) {
    Db4Stmt *stmt = malloc(sizeof(Db4Stmt));
    if (!stmt) {
        snprintf(db->errmsg, sizeof db->errmsg, "out of memory");
        return false;
    }

    arena_init(&stmt->arena);

    Parser parser;
    parser_init(&parser, sql, sql_len, &stmt->arena);
    stmt->stmt = parser_parse_statement(&parser);

    if (parser_failed(&parser)) {
        snprintf(db->errmsg, sizeof db->errmsg, "line %zu: %s", parser_error_line(&parser), parser_error(&parser));
        arena_term(&stmt->arena);
        free(stmt);
        return false;
    }

    stmt->db        = db;
    stmt->is_select = stmt->stmt->kind == STMT_SELECT;
    stmt->executed  = false;
    stmt->failed    = false;
    stmt->rs        = (ResultSet){0};
    stmt->row_idx   = 0;

    /* parser.cur is the lookahead token the grammar stopped on (normally
     * TOK_EOF) - its start, not the lexer's own read position (which is
     * already one token further along), is where "unconsumed input"
     * actually begins. */
    if (out_tail) *out_tail = parser.cur.start;
    *out_stmt = stmt;
    return true;
}

const Stmt *db4_stmt_ast(const Db4Stmt *stmt) {
    return stmt->stmt;
}

/* Runs the statement on first use (db4_step and, below, the column-metadata
 * accessors all funnel through here) - a SELECT's ResultSet, column names
 * included, only exists once interp_exec has actually run it, so a caller
 * that asks for column_count/column_name before ever stepping (legal, and
 * expected to work, in sqlite3's prepare/step split - column metadata is
 * known at compile time there) needs execution triggered on its behalf
 * rather than being handed a stale zero-column answer. */
static void ensure_executed(Db4Stmt *stmt) {
    if (stmt->executed) return;
    stmt->executed = true;

    char   err[256];
    size_t changes = 0;
    bool   ok = interp_exec(stmt->stmt, &stmt->db->catalog, &stmt->db->txn, &stmt->rs, &changes, err, sizeof err);
    stmt->db->changes = changes;

    if (!ok) {
        snprintf(stmt->db->errmsg, sizeof stmt->db->errmsg, "%s", err);
        stmt->failed = true;
    }
}

int db4_step(Db4Stmt *stmt) {
    ensure_executed(stmt);

    if (stmt->failed) return DB4_ERROR;
    if (!stmt->is_select) return DB4_DONE;

    if (stmt->row_idx < stmt->rs.n_rows) {
        stmt->row_idx++;
        return DB4_ROW;
    }
    return DB4_DONE;
}

void db4_finalize(Db4Stmt *stmt) {
    if (!stmt) return;
    result_set_free(&stmt->rs);
    arena_term(&stmt->arena);
    free(stmt);
}

int db4_column_count(Db4Stmt *stmt) {
    ensure_executed(stmt);
    return (stmt->is_select && !stmt->failed) ? (int)stmt->rs.n_cols : 0;
}

const char *db4_column_name(Db4Stmt *stmt, int col) {
    ensure_executed(stmt);
    if (!stmt->is_select || stmt->failed || col < 0 || (size_t)col >= stmt->rs.n_cols) return NULL;
    return stmt->rs.col_names[col];
}

static const Value *current_cell(const Db4Stmt *stmt, int col) {
    if (!stmt->is_select || stmt->row_idx == 0 || col < 0 || (size_t)col >= stmt->rs.n_cols) return NULL;
    size_t row = stmt->row_idx - 1;
    return &stmt->rs.cells[row * stmt->rs.n_cols + (size_t)col];
}

int db4_column_type(const Db4Stmt *stmt, int col) {
    const Value *v = current_cell(stmt, col);
    if (!v || v->is_null) return DB4_NULL;
    switch (v->kind) {
        case FT_INT:    return DB4_INTEGER;
        case FT_DOUBLE: return DB4_FLOAT;
        case FT_BOOL:   return DB4_BOOL;
        case FT_TEXT:   return DB4_TEXT;
        default:        return DB4_NULL;
    }
}

int64_t db4_column_int64(const Db4Stmt *stmt, int col) {
    const Value *v = current_cell(stmt, col);
    if (!v || v->is_null) return 0;
    if (v->kind == FT_DOUBLE) return (int64_t)v->as.d;
    if (v->kind == FT_BOOL) return v->as.b ? 1 : 0;
    return v->as.i;
}

double db4_column_double(const Db4Stmt *stmt, int col) {
    const Value *v = current_cell(stmt, col);
    if (!v || v->is_null) return 0.0;
    if (v->kind == FT_INT) return (double)v->as.i;
    if (v->kind == FT_BOOL) return v->as.b ? 1.0 : 0.0;
    return v->as.d;
}

bool db4_column_bool(const Db4Stmt *stmt, int col) {
    const Value *v = current_cell(stmt, col);
    if (!v || v->is_null) return false;
    if (v->kind == FT_INT) return v->as.i != 0;
    if (v->kind == FT_DOUBLE) return v->as.d != 0.0;
    return v->as.b;
}

const char *db4_column_text(const Db4Stmt *stmt, int col, size_t *out_len) {
    const Value *v = current_cell(stmt, col);
    if (!v || v->is_null || v->kind != FT_TEXT) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = v->as.s.len;
    return v->as.s.data;
}

size_t db4_changes(const Db4 *db) {
    return db->changes;
}

const char *db4_errmsg(const Db4 *db) {
    return db->errmsg;
}

/* NUL-terminated, malloc'd text rendering of one cell - db4_exec's
 * callback shape needs every column pre-formatted as text (sqlite3_exec's
 * shape), unlike db4_column_* which hands back typed values. */
static char *format_value_text(Value v) {
    if (v.is_null) return NULL;
    char  *buf = NULL;
    size_t len = 0;
    FILE  *mem = open_memstream(&buf, &len);
    if (!mem) return NULL;
    print_value(mem, v);
    fclose(mem);
    return buf;
}

bool db4_exec(Db4 *db, const char *sql, Db4ExecCallback cb, void *ctx) {
    Db4Stmt    *stmt;
    const char *tail;
    if (!db4_prepare(db, sql, strlen(sql), &stmt, &tail)) return false;

    while (*tail == ' ' || *tail == '\t' || *tail == '\n' || *tail == '\r') tail++;
    if (*tail != '\0') {
        snprintf(db->errmsg, sizeof db->errmsg, "db4_exec only runs one statement at a time");
        db4_finalize(stmt);
        return false;
    }

    bool ok = true;
    for (;;) {
        int rc = db4_step(stmt);
        if (rc == DB4_ERROR) { ok = false; break; }
        if (rc == DB4_DONE) break;
        if (!cb) continue;

        int n_cols = (int)stmt->rs.n_cols;
        char **owned = calloc((size_t)n_cols, sizeof(char *));
        const char **col_text  = malloc((size_t)n_cols * sizeof(char *));
        const char **col_names = malloc((size_t)n_cols * sizeof(char *));
        if (!owned || !col_text || !col_names) {
            free(owned); free(col_text); free(col_names);
            snprintf(db->errmsg, sizeof db->errmsg, "out of memory");
            ok = false;
            break;
        }

        const Value *row = &stmt->rs.cells[(stmt->row_idx - 1) * stmt->rs.n_cols];
        for (int i = 0; i < n_cols; i++) {
            owned[i]     = format_value_text(row[i]);
            col_text[i]  = owned[i];
            col_names[i] = stmt->rs.col_names[i];
        }

        bool cont = cb(ctx, n_cols, col_text, col_names);

        for (int i = 0; i < n_cols; i++) free(owned[i]);
        free(owned);
        free(col_text);
        free(col_names);

        if (!cont) break;
    }

    db4_finalize(stmt);
    return ok;
}
