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

    /* Bound "?" parameter values (db4_bind_*), sized to stmt->n_params -
     * NULL/0 if the statement has none. Every slot starts out
     * value_null(FT_TEXT) (unbound behaves as SQL NULL, same as sqlite3)
     * until a matching db4_bind_* call overwrites it. TEXT binds are
     * borrowed, not copied - matching Value's existing "never owns
     * memory" contract (value.h) - so the caller's buffer must stay
     * alive through db4_step. */
    Value     *params;
    size_t     n_params;

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

    stmt->n_params = stmt->stmt->n_params;
    stmt->params   = NULL;
    if (stmt->n_params > 0) {
        stmt->params = malloc(stmt->n_params * sizeof(Value));
        if (!stmt->params) {
            snprintf(db->errmsg, sizeof db->errmsg, "out of memory");
            arena_term(&stmt->arena);
            free(stmt);
            return false;
        }
        for (size_t i = 0; i < stmt->n_params; i++) stmt->params[i] = value_null(FT_TEXT);
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
    bool   ok = interp_exec(stmt->stmt, &stmt->db->catalog, &stmt->db->txn, stmt->params, stmt->n_params,
                             &stmt->rs, &changes, err, sizeof err);
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
    free(stmt->params);
    free(stmt);
}

/* Puts stmt back into its pre-execution state so the next db4_step re-runs
 * it from scratch - sqlite3_reset's shape, minus a returned error code:
 * nothing here can itself fail (freeing an already-empty ResultSet is
 * safe), so there's nothing meaningful to report. Bound parameters are
 * deliberately left alone (matching sqlite3_reset, not sqlite3's separate
 * sqlite3_clear_bindings) - the point is reuse: bind, step, reset, rebind
 * just the values that changed, step again, without re-parsing the SQL
 * each time. Safe to call whether or not the statement has run yet. */
void db4_reset(Db4Stmt *stmt) {
    if (!stmt) return;
    result_set_free(&stmt->rs);
    stmt->rs       = (ResultSet){0};
    stmt->row_idx  = 0;
    stmt->executed = false;
    stmt->failed   = false;
}

int db4_bind_parameter_count(const Db4Stmt *stmt) {
    return (int)stmt->n_params;
}

/* Shared by every db4_bind_* below: bounds-checks idx (1-based, sqlite3's
 * convention) and refuses to bind once the statement has already run -
 * a bind after that point could never take effect, so silently accepting
 * it would just hide a caller bug. */
static bool bind_set(Db4Stmt *stmt, int idx, Value v) {
    if (stmt->executed) {
        snprintf(stmt->db->errmsg, sizeof stmt->db->errmsg, "cannot bind parameter after the statement has executed");
        return false;
    }
    if (idx < 1 || (size_t)idx > stmt->n_params) {
        snprintf(stmt->db->errmsg, sizeof stmt->db->errmsg,
                 "parameter index %d out of range (statement has %zu)", idx, stmt->n_params);
        return false;
    }
    stmt->params[idx - 1] = v;
    return true;
}

bool db4_bind_int64(Db4Stmt *stmt, int idx, int64_t v) { return bind_set(stmt, idx, value_int(v)); }
bool db4_bind_double(Db4Stmt *stmt, int idx, double v) { return bind_set(stmt, idx, value_double(v)); }
bool db4_bind_bool(Db4Stmt *stmt, int idx, bool v)     { return bind_set(stmt, idx, value_bool(v)); }

bool db4_bind_text(Db4Stmt *stmt, int idx, const char *text, size_t len) {
    return bind_set(stmt, idx, value_text(text, len));
}

bool db4_bind_null(Db4Stmt *stmt, int idx) {
    return bind_set(stmt, idx, value_null(FT_TEXT));
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
        bool row_oom = false;
        for (int i = 0; i < n_cols; i++) {
            owned[i] = format_value_text(row[i]);
            /* format_value_text returns NULL for both a genuinely NULL
             * cell and a failed allocation - row[i].is_null is what
             * actually disambiguates them, so a failure here (row[i] not
             * null, yet nothing came back) is real out-of-memory, not a
             * NULL column the callback should be told about as one. */
            if (!owned[i] && !row[i].is_null) { row_oom = true; break; }
            col_text[i]  = owned[i];
            col_names[i] = stmt->rs.col_names[i];
        }
        if (row_oom) {
            for (int i = 0; i < n_cols; i++) free(owned[i]);
            free(owned); free(col_text); free(col_names);
            snprintf(db->errmsg, sizeof db->errmsg, "out of memory");
            ok = false;
            break;
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
