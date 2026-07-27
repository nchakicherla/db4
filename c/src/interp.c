#include "interp.h"

#include <stdlib.h>
#include <string.h>

#include "cursor.h"
#include "field.h"
#include "load.h"
#include "lock.h"
#include "result.h"
#include "value.h"
#include "wal.h"

typedef enum {
    ETYPE_INT,
    ETYPE_DOUBLE,
    ETYPE_BOOL,
    ETYPE_STRING,
    ETYPE_NULL,
} ExprValueType;

static ExprValueType field_type_to_expr_type(FieldType t) {
    switch (t) {
        case FT_INT:    return ETYPE_INT;
        case FT_DOUBLE: return ETYPE_DOUBLE;
        case FT_BOOL:   return ETYPE_BOOL;
        case FT_TEXT:   return ETYPE_STRING;
        default:        return ETYPE_NULL;
    }
}

static const char *expr_type_label(ExprValueType t) {
    switch (t) {
        case ETYPE_INT:    return "int";
        case ETYPE_DOUBLE: return "double";
        case ETYPE_BOOL:   return "bool";
        case ETYPE_STRING: return "text";
        case ETYPE_NULL:   return "null";
    }
    return "?";
}

static bool is_numeric(ExprValueType t) { return t == ETYPE_INT || t == ETYPE_DOUBLE; }
static bool is_arith_op(BinaryOp op) { return op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV; }

/* A query touches one or more tables (JOIN adds more); every column
 * reference resolves against this set by table-qualifier (if given) or by
 * unique unqualified name. UPDATE/DELETE/INSERT and unjoined SELECTs just
 * use a single-element RowCtx - the same evaluator serves every caller. */
typedef struct {
    const char  *alias; /* the table's own catalog name - no AS aliasing yet */
    const Table *table;
    size_t       row;
} RowSource;

typedef struct {
    const RowSource *sources;
    size_t           n_sources;

    /* Bound "?" parameter values for this execution (db4_bind_*, via
     * db4.c's Db4Stmt) - empty/NULL if the statement has none. A
     * parameter's value is fixed for the whole statement (like a literal),
     * so resolving EXPR_PARAM's type from the actual bound Value here,
     * once, is exactly as sound as the existing "check every literal's
     * type up front" pass - no per-row runtime type check needed. */
    const Value *params;
    size_t       n_params;
} RowCtx;

static bool resolve_column(const RowCtx *ctx, const char *qual_table, const char *col_name,
                            size_t *out_src, int *out_col, char *err, size_t err_len) {
    if (qual_table) {
        for (size_t i = 0; i < ctx->n_sources; i++) {
            if (strcmp(ctx->sources[i].alias, qual_table) != 0) continue;
            int col = table_find_column(ctx->sources[i].table, col_name);
            if (col < 0) {
                snprintf(err, err_len, "no such column \"%s\" in table \"%s\"", col_name, qual_table);
                return false;
            }
            *out_src = i;
            *out_col = col;
            return true;
        }
        snprintf(err, err_len, "no such table \"%s\" in query", qual_table);
        return false;
    }

    int found_src = -1, found_col = -1;
    for (size_t i = 0; i < ctx->n_sources; i++) {
        int col = table_find_column(ctx->sources[i].table, col_name);
        if (col < 0) continue;
        if (found_src >= 0) {
            snprintf(err, err_len, "column \"%s\" is ambiguous (present in both \"%s\" and \"%s\")",
                     col_name, ctx->sources[found_src].alias, ctx->sources[i].alias);
            return false;
        }
        found_src = (int)i;
        found_col = col;
    }
    if (found_src < 0) {
        snprintf(err, err_len, "no such column \"%s\"", col_name);
        return false;
    }
    *out_src = (size_t)found_src;
    *out_col = found_col;
    return true;
}

/* Column types (and literal kinds) are static, so a WHERE/ON/SET clause's
 * shape can be fully type-checked once, before any row is read - no
 * per-row runtime type error is possible after this passes. */
static bool infer_expr_type(const Expr *e, const RowCtx *ctx, ExprValueType *out, char *err, size_t err_len) {
    switch (e->kind) {
        case EXPR_COLUMN: {
            size_t src;
            int col;
            if (!resolve_column(ctx, e->as.column.table, e->as.column.name, &src, &col, err, err_len)) return false;
            *out = field_type_to_expr_type(ctx->sources[src].table->types[col]);
            return true;
        }
        case EXPR_LIT_INT:    *out = ETYPE_INT;    return true;
        case EXPR_LIT_DOUBLE: *out = ETYPE_DOUBLE; return true;
        case EXPR_LIT_BOOL:   *out = ETYPE_BOOL;   return true;
        case EXPR_LIT_STRING: *out = ETYPE_STRING; return true;
        case EXPR_LIT_NULL:   *out = ETYPE_NULL;   return true;
        case EXPR_PARAM: {
            size_t idx = (size_t)e->as.param_index - 1;
            if (idx >= ctx->n_params) {
                snprintf(err, err_len, "parameter ?%d is not bound", e->as.param_index);
                return false;
            }
            Value v = ctx->params[idx];
            *out = v.is_null ? ETYPE_NULL : field_type_to_expr_type(v.kind);
            return true;
        }
        case EXPR_NOT: {
            ExprValueType operand;
            if (!infer_expr_type(e->as.unary_operand, ctx, &operand, err, err_len)) return false;
            if (operand != ETYPE_BOOL && operand != ETYPE_NULL) {
                snprintf(err, err_len, "NOT requires a boolean expression, found %s", expr_type_label(operand));
                return false;
            }
            *out = ETYPE_BOOL;
            return true;
        }
        case EXPR_NEG: {
            ExprValueType operand;
            if (!infer_expr_type(e->as.unary_operand, ctx, &operand, err, err_len)) return false;
            if (!is_numeric(operand) && operand != ETYPE_NULL) {
                snprintf(err, err_len, "unary - requires a numeric expression, found %s", expr_type_label(operand));
                return false;
            }
            *out = operand;
            return true;
        }
        case EXPR_BINARY: {
            ExprValueType l, r;
            if (!infer_expr_type(e->as.binary.left, ctx, &l, err, err_len)) return false;
            if (!infer_expr_type(e->as.binary.right, ctx, &r, err, err_len)) return false;

            if (e->as.binary.op == OP_AND || e->as.binary.op == OP_OR) {
                if ((l != ETYPE_BOOL && l != ETYPE_NULL) || (r != ETYPE_BOOL && r != ETYPE_NULL)) {
                    snprintf(err, err_len, "%s requires boolean operands, found %s and %s",
                             e->as.binary.op == OP_AND ? "AND" : "OR", expr_type_label(l), expr_type_label(r));
                    return false;
                }
                *out = ETYPE_BOOL;
                return true;
            }

            if (is_arith_op(e->as.binary.op)) {
                bool l_ok = is_numeric(l) || l == ETYPE_NULL;
                bool r_ok = is_numeric(r) || r == ETYPE_NULL;
                if (!l_ok || !r_ok) {
                    static const char *labels[] = { [OP_ADD] = "+", [OP_SUB] = "-", [OP_MUL] = "*", [OP_DIV] = "/" };
                    snprintf(err, err_len, "%s requires numeric operands, found %s and %s",
                             labels[e->as.binary.op], expr_type_label(l), expr_type_label(r));
                    return false;
                }
                if (l == ETYPE_NULL || r == ETYPE_NULL) *out = ETYPE_NULL;
                else if (e->as.binary.op == OP_DIV) *out = ETYPE_DOUBLE; /* always widen - avoids integer division-by-zero UB */
                else *out = (l == ETYPE_DOUBLE || r == ETYPE_DOUBLE) ? ETYPE_DOUBLE : ETYPE_INT;
                return true;
            }

            bool compatible = l == ETYPE_NULL || r == ETYPE_NULL || (is_numeric(l) && is_numeric(r)) || l == r;
            if (!compatible) {
                snprintf(err, err_len, "cannot compare %s and %s", expr_type_label(l), expr_type_label(r));
                return false;
            }
            *out = ETYPE_BOOL;
            return true;
        }
    }
    snprintf(err, err_len, "internal error: unknown expression kind");
    return false;
}

static bool validate_where(const Expr *e, const RowCtx *ctx, char *err, size_t err_len) {
    ExprValueType ty;
    if (!infer_expr_type(e, ctx, &ty, err, err_len)) return false;
    if (ty != ETYPE_BOOL && ty != ETYPE_NULL) {
        snprintf(err, err_len, "WHERE clause must be a boolean expression, found %s", expr_type_label(ty));
        return false;
    }
    return true;
}

/* Value/read_column/compare_values now live in value.h/value.c - shared
 * with db4.c's column accessors and the ResultSet a SELECT materializes
 * into (result.h). */

/* Three-valued (true/false/unknown) so NULL propagates through AND/OR/NOT
 * the way SQL defines it (e.g. FALSE AND unknown = FALSE, not unknown) -
 * a row matches a predicate only when this comes out TRI_TRUE. */
typedef enum { TRI_FALSE, TRI_TRUE, TRI_UNKNOWN } Tri;

static Tri value_to_tri(Value v) {
    if (v.is_null) return TRI_UNKNOWN;
    return v.as.b ? TRI_TRUE : TRI_FALSE;
}

static Tri eval_comparison(BinaryOp op, Value l, Value r) {
    if (l.is_null || r.is_null) return TRI_UNKNOWN;
    int c = compare_values(l, r);
    bool result;
    switch (op) {
        case OP_EQ: result = c == 0; break;
        case OP_NE: result = c != 0; break;
        case OP_LT: result = c < 0; break;
        case OP_LE: result = c <= 0; break;
        case OP_GT: result = c > 0; break;
        case OP_GE: result = c >= 0; break;
        default: result = false; break;
    }
    return result ? TRI_TRUE : TRI_FALSE;
}

static Value eval_value(const Expr *e, const RowCtx *ctx);

static Tri eval_tri(const Expr *e, const RowCtx *ctx) {
    switch (e->kind) {
        case EXPR_LIT_BOOL: return e->as.bool_value ? TRI_TRUE : TRI_FALSE;
        case EXPR_LIT_NULL: return TRI_UNKNOWN;
        case EXPR_COLUMN:   return value_to_tri(eval_value(e, ctx));
        case EXPR_PARAM:    return value_to_tri(eval_value(e, ctx));
        case EXPR_NEG:      return value_to_tri(eval_value(e, ctx)); /* not a valid predicate post-validation, kept harmless */
        case EXPR_NOT: {
            Tri v = eval_tri(e->as.unary_operand, ctx);
            if (v == TRI_UNKNOWN) return TRI_UNKNOWN;
            return v == TRI_TRUE ? TRI_FALSE : TRI_TRUE;
        }
        case EXPR_BINARY:
            if (e->as.binary.op == OP_AND) {
                Tri l = eval_tri(e->as.binary.left, ctx);
                Tri r = eval_tri(e->as.binary.right, ctx);
                if (l == TRI_FALSE || r == TRI_FALSE) return TRI_FALSE;
                if (l == TRI_UNKNOWN || r == TRI_UNKNOWN) return TRI_UNKNOWN;
                return TRI_TRUE;
            }
            if (e->as.binary.op == OP_OR) {
                Tri l = eval_tri(e->as.binary.left, ctx);
                Tri r = eval_tri(e->as.binary.right, ctx);
                if (l == TRI_TRUE || r == TRI_TRUE) return TRI_TRUE;
                if (l == TRI_UNKNOWN || r == TRI_UNKNOWN) return TRI_UNKNOWN;
                return TRI_FALSE;
            }
            if (is_arith_op(e->as.binary.op)) return value_to_tri(eval_value(e, ctx));
            return eval_comparison(e->as.binary.op, eval_value(e->as.binary.left, ctx), eval_value(e->as.binary.right, ctx));
        default: return TRI_UNKNOWN;
    }
}

static Value eval_value(const Expr *e, const RowCtx *ctx) {
    switch (e->kind) {
        case EXPR_COLUMN: {
            size_t src;
            int col;
            char discard[64];
            /* infer_expr_type already proved this resolves, for the same ctx shape - the
             * only way it could fail here is a bug, not bad input, so the error is thrown away. */
            resolve_column(ctx, e->as.column.table, e->as.column.name, &src, &col, discard, sizeof discard);
            return read_column(ctx->sources[src].table, ctx->sources[src].row, (size_t)col);
        }
        case EXPR_LIT_INT:    return value_int(e->as.int_value);
        case EXPR_LIT_DOUBLE: return value_double(e->as.double_value);
        case EXPR_LIT_BOOL:   return value_bool(e->as.bool_value);
        case EXPR_LIT_STRING: return value_text(e->as.string_value.data, e->as.string_value.len);
        case EXPR_LIT_NULL:   return value_null(FT_TEXT);
        case EXPR_PARAM:      return ctx->params[e->as.param_index - 1];
        case EXPR_NEG: {
            Value v = eval_value(e->as.unary_operand, ctx);
            if (v.is_null) return v;
            return v.kind == FT_INT ? value_int(-v.as.i) : value_double(-v.as.d);
        }
        case EXPR_NOT: {
            Tri t = eval_tri(e, ctx);
            if (t == TRI_UNKNOWN) return value_null(FT_BOOL);
            return value_bool(t == TRI_TRUE);
        }
        case EXPR_BINARY: {
            BinaryOp op = e->as.binary.op;
            if (op == OP_AND || op == OP_OR || !is_arith_op(op)) {
                Tri t = eval_tri(e, ctx);
                if (t == TRI_UNKNOWN) return value_null(FT_BOOL);
                return value_bool(t == TRI_TRUE);
            }

            Value l = eval_value(e->as.binary.left, ctx);
            Value r = eval_value(e->as.binary.right, ctx);
            if (l.is_null || r.is_null) return value_null(FT_DOUBLE);

            if (op == OP_DIV) {
                double x = l.kind == FT_INT ? (double)l.as.i : l.as.d;
                double y = r.kind == FT_INT ? (double)r.as.i : r.as.d;
                return value_double(x / y);
            }
            if (l.kind == FT_DOUBLE || r.kind == FT_DOUBLE) {
                double x = l.kind == FT_INT ? (double)l.as.i : l.as.d;
                double y = r.kind == FT_INT ? (double)r.as.i : r.as.d;
                switch (op) {
                    case OP_ADD: return value_double(x + y);
                    case OP_SUB: return value_double(x - y);
                    default:     return value_double(x * y);
                }
            }
            switch (op) {
                case OP_ADD: return value_int(l.as.i + r.as.i);
                case OP_SUB: return value_int(l.as.i - r.as.i);
                default:     return value_int(l.as.i * r.as.i);
            }
        }
        default: return value_null(FT_TEXT); /* unreachable */
    }
}

/* --- Shared by INSERT/UPDATE: constraint checks and value assignment --- */

/* Shared by INSERT (validate the freshly appended row) and UPDATE
 * (validate the just-modified row) - the same checks load.c's loader runs
 * per row, just against the catalog's current state instead of a
 * scratch table being built up. */
static bool check_row_constraints(const Table *t, size_t row, const Catalog *catalog, char *err, size_t err_len) {
    for (size_t col = 0; col < t->n_cols; col++) {
        if (table_is_primary_key(t, col)) {
            if (table_is_null(t, row, col)) {
                snprintf(err, err_len, "primary key column \"%s\" cannot be NULL", t->names[col]);
                return false;
            }
            if (table_column_has_duplicate(t, col, row)) {
                snprintf(err, err_len, "duplicate value in primary key column \"%s\"", t->names[col]);
                return false;
            }
        }

        const char *ref_table_name, *ref_col_name;
        if (!table_get_foreign_key(t, col, &ref_table_name, &ref_col_name)) continue;
        if (table_is_null(t, row, col)) continue;

        int ref_idx = catalog_find(catalog, ref_table_name);
        if (ref_idx < 0) continue;
        const Table *ref_table = &catalog->tables[ref_idx].table;
        int ref_col = table_find_column(ref_table, ref_col_name);
        if (ref_col < 0) continue;

        if (!table_has_matching_value(ref_table, (size_t)ref_col, t, row, col)) {
            snprintf(err, err_len, "value in foreign key column \"%s\" not found in %s.%s",
                     t->names[col], ref_table_name, ref_col_name);
            return false;
        }
    }
    return true;
}

/* No FK action (CASCADE/SET_NULL) is implemented yet - table_set_foreign_key
 * still records which one was declared, but the only thing enforced here is
 * the safety floor every action needs anyway: never let a DELETE or an
 * UPDATE of the primary key silently orphan a row elsewhere that points at
 * it. FKs can only ever target a primary key column (load.c's loader and
 * CREATE TABLE both require that), so this only has to look at t's own
 * pk_col. */
static bool check_pk_not_referenced(const Catalog *catalog, const Table *t, const char *table_name,
                                     size_t row, char *err, size_t err_len) {
    if (t->pk_col < 0) return true;
    size_t col = (size_t)t->pk_col;
    if (table_is_null(t, row, col)) return true;

    for (size_t i = 0; i < catalog->count; i++) {
        const Table *other = &catalog->tables[i].table;
        for (size_t c = 0; c < other->n_cols; c++) {
            const char *ref_table, *ref_col;
            if (!table_get_foreign_key(other, c, &ref_table, &ref_col)) continue;
            if (strcmp(ref_table, table_name) != 0) continue;
            if (strcmp(ref_col, t->names[col]) != 0) continue;

            if (table_has_matching_value(other, c, t, row, col)) {
                snprintf(err, err_len, "row is referenced by %s.%s (foreign key)", catalog->tables[i].name, other->names[c]);
                return false;
            }
        }
    }
    return true;
}

static bool expr_has_column_ref(const Expr *e) {
    switch (e->kind) {
        case EXPR_COLUMN: return true;
        case EXPR_NOT:
        case EXPR_NEG: return expr_has_column_ref(e->as.unary_operand);
        case EXPR_BINARY: return expr_has_column_ref(e->as.binary.left) || expr_has_column_ref(e->as.binary.right);
        default: return false;
    }
}

static bool expr_fits_column(const Expr *e, const RowCtx *ctx, FieldType col_type, const char *col_name,
                              char *err, size_t err_len) {
    ExprValueType ty;
    if (!infer_expr_type(e, ctx, &ty, err, err_len)) return false;
    if (ty == ETYPE_NULL) return true;

    FieldType et = ty == ETYPE_INT ? FT_INT : ty == ETYPE_DOUBLE ? FT_DOUBLE : ty == ETYPE_BOOL ? FT_BOOL : FT_TEXT;
    if (et == col_type) return true;
    if (et == FT_INT && col_type == FT_DOUBLE) return true; /* widen, like an int expression into a double column */

    snprintf(err, err_len, "column \"%s\" is type %s but value is type %s",
             col_name, field_type_label(col_type), field_type_label(et));
    return false;
}

static void assign_value(Table *t, size_t row, size_t col, Value v) {
    if (v.is_null) {
        table_set_null(t, row, col);
        return;
    }
    switch (t->types[col]) {
        case FT_INT:    table_set_int(t, row, col, v.as.i); break;
        case FT_DOUBLE: table_set_double(t, row, col, v.kind == FT_INT ? (double)v.as.i : v.as.d); break;
        case FT_BOOL:   table_set_bool(t, row, col, v.as.b); break;
        case FT_TEXT:   table_set_text(t, row, col, v.as.s.data, v.as.s.len); break;
        default: break;
    }
}

/* --- SELECT: plain projection, joins, ORDER BY, LIMIT --- */

#define MAX_JOIN_SOURCES 8

/* qsort has no portable reentrant form in C11, and exec is synchronous and
 * single-threaded within one REPL command, so file-scope sort context is
 * safe here without needing qsort_r/thread-locals. */
static size_t       g_sort_src;
static int          g_sort_col;
static const Table *g_sort_table;
static bool         g_sort_desc;

static int join_sort_cmp(const void *pa, const void *pb) {
    const size_t *ta = pa;
    const size_t *tb = pb;
    Value va = read_column(g_sort_table, ta[g_sort_src], (size_t)g_sort_col);
    Value vb = read_column(g_sort_table, tb[g_sort_src], (size_t)g_sort_col);

    int c;
    if (va.is_null && vb.is_null) c = 0;
    else if (va.is_null) c = -1;
    else if (vb.is_null) c = 1;
    else c = compare_values(va, vb);

    return g_sort_desc ? -c : c;
}

static bool exec_select_plain(const SelectStmt *stmt, const Catalog *catalog, const Value *params, size_t n_params,
                               ResultSet *out_rs, char *err, size_t err_len) {
    size_t n_sources = 1 + stmt->n_joins;
    if (n_sources > MAX_JOIN_SOURCES) {
        snprintf(err, err_len, "too many joined tables (max %d)", MAX_JOIN_SOURCES);
        return false;
    }

    const char  *names[MAX_JOIN_SOURCES];
    const Table *tables[MAX_JOIN_SOURCES];

    names[0] = stmt->table;
    int idx0 = catalog_find(catalog, stmt->table);
    if (idx0 < 0) {
        snprintf(err, err_len, "no such table: %s", stmt->table);
        return false;
    }
    tables[0] = &catalog->tables[idx0].table;

    for (size_t j = 0; j < stmt->n_joins; j++) {
        names[1 + j] = stmt->joins[j].table;
        int idx = catalog_find(catalog, stmt->joins[j].table);
        if (idx < 0) {
            snprintf(err, err_len, "no such table: %s", stmt->joins[j].table);
            return false;
        }
        tables[1 + j] = &catalog->tables[idx].table;
    }

    for (size_t i = 0; i < n_sources; i++)
        for (size_t j = i + 1; j < n_sources; j++)
            if (strcmp(names[i], names[j]) == 0) {
                snprintf(err, err_len, "cannot join \"%s\" to itself (table aliasing is not yet supported)", names[i]);
                return false;
            }

    RowSource static_sources[MAX_JOIN_SOURCES];
    for (size_t i = 0; i < n_sources; i++) static_sources[i] = (RowSource){ names[i], tables[i], 0 };
    RowCtx static_ctx = { static_sources, n_sources, params, n_params };

    for (size_t j = 0; j < stmt->n_joins; j++) {
        ExprValueType ty;
        if (!infer_expr_type(stmt->joins[j].on, &static_ctx, &ty, err, err_len)) return false;
        if (ty != ETYPE_BOOL && ty != ETYPE_NULL) {
            snprintf(err, err_len, "JOIN ... ON must be a boolean expression, found %s", expr_type_label(ty));
            return false;
        }
    }
    if (stmt->where && !validate_where(stmt->where, &static_ctx, err, err_len)) return false;

    size_t n_proj = stmt->columns.is_star ? 0 : stmt->columns.count;
    size_t *proj_src = NULL;
    int    *proj_col = NULL;
    if (!stmt->columns.is_star) {
        proj_src = malloc(n_proj * sizeof(size_t));
        proj_col = malloc(n_proj * sizeof(int));
        if (!proj_src || !proj_col) {
            snprintf(err, err_len, "out of memory");
            free(proj_src);
            free(proj_col);
            return false;
        }
        for (size_t i = 0; i < n_proj; i++) {
            const SelectItem *item = &stmt->columns.items[i];
            if (!resolve_column(&static_ctx, item->table, item->column, &proj_src[i], &proj_col[i], err, err_len)) {
                free(proj_src);
                free(proj_col);
                return false;
            }
        }
    }

    int order_src = -1, order_col = -1;
    if (stmt->has_order_by) {
        size_t s;
        int c;
        if (!resolve_column(&static_ctx, stmt->order_by_table, stmt->order_by_col, &s, &c, err, err_len)) {
            free(proj_src);
            free(proj_col);
            return false;
        }
        order_src = (int)s;
        order_col = c;
    }

    size_t width = n_sources;
    size_t *cur = NULL;
    size_t  cur_count = 0, cur_cap = 0;

    Cursor c0;
    cursor_init(&c0, tables[0]);
    size_t row0;
    while (cursor_next(&c0, &row0)) {
        if (cur_count == cur_cap) {
            size_t new_cap = cur_cap ? cur_cap * 2 : 16;
            size_t *grown = realloc(cur, new_cap * width * sizeof(size_t));
            if (!grown) {
                snprintf(err, err_len, "out of memory");
                free(cur);
                free(proj_src);
                free(proj_col);
                return false;
            }
            cur     = grown;
            cur_cap = new_cap;
        }
        cur[cur_count * width + 0] = row0;
        cur_count++;
    }

    for (size_t j = 0; j < stmt->n_joins; j++) {
        size_t filled = 1 + j;
        size_t *next = NULL;
        size_t  next_count = 0, next_cap = 0;

        for (size_t i = 0; i < cur_count; i++) {
            Cursor cj;
            cursor_init(&cj, tables[filled]);
            size_t jrow;
            while (cursor_next(&cj, &jrow)) {
                RowSource probe[MAX_JOIN_SOURCES];
                for (size_t s = 0; s < filled; s++) probe[s] = (RowSource){ names[s], tables[s], cur[i * width + s] };
                probe[filled] = (RowSource){ names[filled], tables[filled], jrow };
                RowCtx probe_ctx = { probe, filled + 1, params, n_params };

                if (eval_tri(stmt->joins[j].on, &probe_ctx) != TRI_TRUE) continue;

                if (next_count == next_cap) {
                    size_t new_cap = next_cap ? next_cap * 2 : 16;
                    size_t *grown = realloc(next, new_cap * width * sizeof(size_t));
                    if (!grown) {
                        snprintf(err, err_len, "out of memory");
                        free(next);
                        free(cur);
                        free(proj_src);
                        free(proj_col);
                        return false;
                    }
                    next     = grown;
                    next_cap = new_cap;
                }
                for (size_t s = 0; s < filled; s++) next[next_count * width + s] = cur[i * width + s];
                next[next_count * width + filled] = jrow;
                next_count++;
            }
        }
        free(cur);
        cur       = next;
        cur_count = next_count;
    }

    size_t *matched = NULL;
    size_t  n_matched = 0, mcap = 0;
    for (size_t i = 0; i < cur_count; i++) {
        if (stmt->where) {
            RowSource srcs[MAX_JOIN_SOURCES];
            for (size_t s = 0; s < n_sources; s++) srcs[s] = (RowSource){ names[s], tables[s], cur[i * width + s] };
            RowCtx ctx = { srcs, n_sources, params, n_params };
            if (eval_tri(stmt->where, &ctx) != TRI_TRUE) continue;
        }
        if (n_matched == mcap) {
            size_t new_cap = mcap ? mcap * 2 : 16;
            size_t *grown = realloc(matched, new_cap * width * sizeof(size_t));
            if (!grown) {
                snprintf(err, err_len, "out of memory");
                free(matched);
                free(cur);
                free(proj_src);
                free(proj_col);
                return false;
            }
            matched = grown;
            mcap    = new_cap;
        }
        memcpy(&matched[n_matched * width], &cur[i * width], width * sizeof(size_t));
        n_matched++;
    }
    free(cur);

    if (stmt->has_order_by) {
        g_sort_src   = (size_t)order_src;
        g_sort_col   = order_col;
        g_sort_table = tables[order_src];
        g_sort_desc  = stmt->order_desc;
        qsort(matched, n_matched, width * sizeof(size_t), join_sort_cmp);
    }

    size_t n_out = n_matched;
    if (stmt->has_limit) {
        size_t lim = stmt->limit < 0 ? 0 : (size_t)stmt->limit;
        if (lim < n_out) n_out = lim;
    }

    bool qualify_header = n_sources > 1;

    size_t n_cols = n_proj;
    if (stmt->columns.is_star) {
        n_cols = 0;
        for (size_t s = 0; s < n_sources; s++) n_cols += tables[s]->n_cols;
    }

    Value *row_buf = NULL;

    if (!result_set_init(out_rs, n_cols)) goto oom;

    {
        size_t k = 0;
        char   namebuf[192];
        if (stmt->columns.is_star) {
            for (size_t s = 0; s < n_sources; s++) {
                for (size_t c = 0; c < tables[s]->n_cols; c++) {
                    if (qualify_header) snprintf(namebuf, sizeof namebuf, "%s.%s", names[s], tables[s]->names[c]);
                    else snprintf(namebuf, sizeof namebuf, "%s", tables[s]->names[c]);
                    if (!result_set_set_col_name(out_rs, k++, namebuf)) goto oom;
                }
            }
        } else {
            for (size_t i = 0; i < n_proj; i++)
                if (!result_set_set_col_name(out_rs, k++, stmt->columns.items[i].column)) goto oom;
        }
    }

    row_buf = malloc(n_cols * sizeof(Value));
    if (n_cols > 0 && !row_buf) goto oom;

    for (size_t i = 0; i < n_out; i++) {
        const size_t *tuple = &matched[i * width];
        size_t k = 0;
        if (stmt->columns.is_star) {
            for (size_t s = 0; s < n_sources; s++)
                for (size_t c = 0; c < tables[s]->n_cols; c++)
                    row_buf[k++] = read_column(tables[s], tuple[s], c);
        } else {
            for (size_t c = 0; c < n_proj; c++)
                row_buf[k++] = read_column(tables[proj_src[c]], tuple[proj_src[c]], (size_t)proj_col[c]);
        }
        if (!result_set_add_row(out_rs, row_buf)) goto oom;
    }

    free(row_buf);
    free(matched);
    free(proj_src);
    free(proj_col);
    return true;

oom:
    snprintf(err, err_len, "out of memory");
    free(row_buf);
    result_set_free(out_rs);
    free(matched);
    free(proj_src);
    free(proj_col);
    return false;
}

/* --- SELECT: GROUP BY / aggregates (single table only) --- */

static const Table *g_group_table;
static const int   *g_group_cols;
static size_t       g_group_n;

static int group_key_cmp(const void *pa, const void *pb) {
    size_t ra = *(const size_t *)pa;
    size_t rb = *(const size_t *)pb;
    for (size_t i = 0; i < g_group_n; i++) {
        Value va = read_column(g_group_table, ra, (size_t)g_group_cols[i]);
        Value vb = read_column(g_group_table, rb, (size_t)g_group_cols[i]);
        int c;
        if (va.is_null && vb.is_null) c = 0;
        else if (va.is_null) c = -1;
        else if (vb.is_null) c = 1;
        else c = compare_values(va, vb);
        if (c != 0) return c;
    }
    return 0;
}

static bool group_key_equal(const Table *t, const int *cols, size_t n_cols, size_t ra, size_t rb) {
    for (size_t i = 0; i < n_cols; i++) {
        Value va = read_column(t, ra, (size_t)cols[i]);
        Value vb = read_column(t, rb, (size_t)cols[i]);
        if (va.is_null != vb.is_null) return false;
        if (va.is_null) continue;
        if (compare_values(va, vb) != 0) return false;
    }
    return true;
}

static bool build_group_col_names(ResultSet *rs, const SelectStmt *stmt) {
    char namebuf[192];
    for (size_t i = 0; i < stmt->columns.count; i++) {
        const SelectItem *item = &stmt->columns.items[i];
        if (!item->is_agg) {
            snprintf(namebuf, sizeof namebuf, "%s", item->column);
        } else {
            const char *fname = item->agg_func == AGG_COUNT ? "count" : item->agg_func == AGG_SUM ? "sum" : "avg";
            if (item->agg_arg_is_star) snprintf(namebuf, sizeof namebuf, "%s(*)", fname);
            else snprintf(namebuf, sizeof namebuf, "%s(%s)", fname, item->agg_arg_column);
        }
        if (!result_set_set_col_name(rs, i, namebuf)) return false;
    }
    return true;
}

static bool build_group_row(ResultSet *rs, const SelectStmt *stmt, const Table *t, const int *item_col,
                             const bool *item_is_group, const size_t *rows, size_t count) {
    size_t n_items = stmt->columns.count;
    Value *row_buf = malloc(n_items * sizeof(Value));
    if (!row_buf) return false;

    for (size_t i = 0; i < n_items; i++) {
        const SelectItem *item = &stmt->columns.items[i];

        if (item_is_group[i]) {
            row_buf[i] = count ? read_column(t, rows[0], (size_t)item_col[i]) : value_null(FT_TEXT);
            continue;
        }

        switch (item->agg_func) {
            case AGG_COUNT: {
                int64_t n = 0;
                if (item->agg_arg_is_star) {
                    n = (int64_t)count;
                } else {
                    for (size_t k = 0; k < count; k++)
                        if (!table_is_null(t, rows[k], (size_t)item_col[i])) n++;
                }
                row_buf[i] = value_int(n);
                break;
            }
            case AGG_SUM: {
                bool is_double = t->types[item_col[i]] == FT_DOUBLE;
                bool any = false;
                double dsum = 0;
                int64_t isum = 0;
                for (size_t k = 0; k < count; k++) {
                    if (table_is_null(t, rows[k], (size_t)item_col[i])) continue;
                    any = true;
                    if (is_double) dsum += table_get_double(t, rows[k], (size_t)item_col[i]);
                    else            isum += table_get_int(t, rows[k], (size_t)item_col[i]);
                }
                row_buf[i] = !any ? value_null(is_double ? FT_DOUBLE : FT_INT)
                                  : (is_double ? value_double(dsum) : value_int(isum));
                break;
            }
            case AGG_AVG: {
                double sum = 0;
                size_t n = 0;
                for (size_t k = 0; k < count; k++) {
                    if (table_is_null(t, rows[k], (size_t)item_col[i])) continue;
                    n++;
                    sum += t->types[item_col[i]] == FT_DOUBLE ? table_get_double(t, rows[k], (size_t)item_col[i])
                                                               : (double)table_get_int(t, rows[k], (size_t)item_col[i]);
                }
                row_buf[i] = n == 0 ? value_null(FT_DOUBLE) : value_double(sum / (double)n);
                break;
            }
        }
    }

    bool ok = result_set_add_row(rs, row_buf);
    free(row_buf);
    return ok;
}

static bool exec_select_grouped(const SelectStmt *stmt, const Catalog *catalog, const Value *params, size_t n_params,
                                 ResultSet *out_rs, char *err, size_t err_len) {
    int t_idx = catalog_find(catalog, stmt->table);
    if (t_idx < 0) {
        snprintf(err, err_len, "no such table: %s", stmt->table);
        return false;
    }
    const Table *t = &catalog->tables[t_idx].table;

    RowSource src0       = { stmt->table, t, 0 };
    RowCtx    static_ctx = { &src0, 1, params, n_params };

    if (stmt->columns.is_star) {
        snprintf(err, err_len, "SELECT * is not supported with GROUP BY/aggregates - list columns explicitly");
        return false;
    }

    int *gb_cols = NULL;
    if (stmt->n_group_by) {
        gb_cols = malloc(stmt->n_group_by * sizeof(int));
        if (!gb_cols) {
            snprintf(err, err_len, "out of memory");
            return false;
        }
        for (size_t i = 0; i < stmt->n_group_by; i++) {
            int col = table_find_column(t, stmt->group_by[i]);
            if (col < 0) {
                snprintf(err, err_len, "no such column \"%s\" in table \"%s\"", stmt->group_by[i], stmt->table);
                free(gb_cols);
                return false;
            }
            gb_cols[i] = col;
        }
    }

    size_t n_items       = stmt->columns.count;
    int   *item_col      = malloc(n_items * sizeof(int));
    bool  *item_is_group  = malloc(n_items * sizeof(bool));
    if (!item_col || !item_is_group) {
        snprintf(err, err_len, "out of memory");
        free(gb_cols);
        free(item_col);
        free(item_is_group);
        return false;
    }

    for (size_t i = 0; i < n_items; i++) {
        const SelectItem *item = &stmt->columns.items[i];
        if (!item->is_agg) {
            int col = table_find_column(t, item->column);
            if (col < 0) {
                snprintf(err, err_len, "no such column \"%s\" in table \"%s\"", item->column, stmt->table);
                goto fail;
            }
            bool in_group = false;
            for (size_t g = 0; g < stmt->n_group_by; g++)
                if (gb_cols[g] == col) { in_group = true; break; }
            if (!in_group) {
                snprintf(err, err_len, "column \"%s\" must appear in GROUP BY or be used in an aggregate", item->column);
                goto fail;
            }
            item_col[i]      = col;
            item_is_group[i] = true;
        } else {
            item_is_group[i] = false;
            if (item->agg_arg_is_star) {
                item_col[i] = -1;
                continue;
            }
            int col = table_find_column(t, item->agg_arg_column);
            if (col < 0) {
                snprintf(err, err_len, "no such column \"%s\" in table \"%s\"", item->agg_arg_column, stmt->table);
                goto fail;
            }
            if (item->agg_func != AGG_COUNT && t->types[col] != FT_INT && t->types[col] != FT_DOUBLE) {
                snprintf(err, err_len, "%s requires a numeric column, found %s",
                         item->agg_func == AGG_SUM ? "SUM" : "AVG", field_type_label(t->types[col]));
                goto fail;
            }
            item_col[i] = col;
        }
    }

    if (stmt->where && !validate_where(stmt->where, &static_ctx, err, err_len)) goto fail;

    size_t *rows = NULL;
    size_t  n_rows = 0, cap = 0;
    Cursor cur;
    cursor_init(&cur, t);
    size_t row;
    while (cursor_next(&cur, &row)) {
        if (stmt->where) {
            RowSource s   = { stmt->table, t, row };
            RowCtx    ctx = { &s, 1, params, n_params };
            if (eval_tri(stmt->where, &ctx) != TRI_TRUE) continue;
        }
        if (n_rows == cap) {
            size_t new_cap = cap ? cap * 2 : 16;
            size_t *grown = realloc(rows, new_cap * sizeof(size_t));
            if (!grown) {
                snprintf(err, err_len, "out of memory");
                free(rows);
                goto fail;
            }
            rows = grown;
            cap  = new_cap;
        }
        rows[n_rows++] = row;
    }

    if (stmt->n_group_by) {
        g_group_table = t;
        g_group_cols  = gb_cols;
        g_group_n     = stmt->n_group_by;
        qsort(rows, n_rows, sizeof(size_t), group_key_cmp);
    }

    if (!result_set_init(out_rs, n_items) || !build_group_col_names(out_rs, stmt)) {
        snprintf(err, err_len, "out of memory");
        free(rows);
        goto fail;
    }

    bool built_ok;
    if (stmt->n_group_by == 0) {
        built_ok = build_group_row(out_rs, stmt, t, item_col, item_is_group, rows, n_rows);
    } else {
        built_ok = true;
        size_t i = 0;
        while (built_ok && i < n_rows) {
            size_t j = i + 1;
            while (j < n_rows && group_key_equal(t, gb_cols, stmt->n_group_by, rows[i], rows[j])) j++;
            built_ok = build_group_row(out_rs, stmt, t, item_col, item_is_group, rows + i, j - i);
            i = j;
        }
    }
    if (!built_ok) {
        snprintf(err, err_len, "out of memory");
        result_set_free(out_rs);
        free(rows);
        goto fail;
    }

    free(rows);
    free(gb_cols);
    free(item_col);
    free(item_is_group);
    return true;

fail:
    free(gb_cols);
    free(item_col);
    free(item_is_group);
    return false;
}

bool interp_exec_select(const SelectStmt *stmt, const Catalog *catalog, const Value *params, size_t n_params,
                         ResultSet *out_rs, char *err, size_t err_len) {
    bool has_agg = false;
    if (!stmt->columns.is_star)
        for (size_t i = 0; i < stmt->columns.count; i++)
            if (stmt->columns.items[i].is_agg) { has_agg = true; break; }
    bool grouped = has_agg || stmt->n_group_by > 0;

    if (grouped && stmt->n_joins > 0) {
        snprintf(err, err_len, "GROUP BY/aggregates combined with JOIN are not yet supported");
        return false;
    }
    if (grouped && (stmt->has_order_by || stmt->has_limit)) {
        snprintf(err, err_len, "ORDER BY/LIMIT with GROUP BY/aggregates are not yet supported");
        return false;
    }

    if (grouped) return exec_select_grouped(stmt, catalog, params, n_params, out_rs, err, err_len);
    return exec_select_plain(stmt, catalog, params, n_params, out_rs, err, err_len);
}

/* --- INSERT / UPDATE / DELETE / CREATE TABLE --- */

static bool interp_exec_insert(const InsertStmt *stmt, Catalog *catalog, Txn *txn, const Value *params, size_t n_params,
                                size_t *out_changes, char *err, size_t err_len) {
    int t_idx = catalog_find(catalog, stmt->table);
    if (t_idx < 0) {
        snprintf(err, err_len, "no such table: %s", stmt->table);
        return false;
    }
    Table *t = &catalog->tables[t_idx].table;

    size_t n_target = stmt->n_columns ? stmt->n_columns : t->n_cols;
    int *cols = malloc(n_target * sizeof(int));
    if (!cols) {
        snprintf(err, err_len, "out of memory");
        return false;
    }

    if (stmt->n_columns) {
        for (size_t i = 0; i < stmt->n_columns; i++) {
            int col = table_find_column(t, stmt->columns[i]);
            if (col < 0) {
                snprintf(err, err_len, "no such column \"%s\" in table \"%s\"", stmt->columns[i], stmt->table);
                free(cols);
                return false;
            }
            cols[i] = col;
        }
        for (size_t i = 0; i < stmt->n_columns; i++)
            for (size_t j = i + 1; j < stmt->n_columns; j++)
                if (cols[i] == cols[j]) {
                    snprintf(err, err_len, "column \"%s\" specified more than once", stmt->columns[i]);
                    free(cols);
                    return false;
                }
    } else {
        for (size_t i = 0; i < n_target; i++) cols[i] = (int)i;
    }

    RowCtx empty_ctx = { NULL, 0, params, n_params }; /* VALUES can't reference columns - see expr_has_column_ref below */

    size_t n_inserted = 0;
    for (size_t r = 0; r < stmt->n_rows; r++) {
        const ValueRow *vr = &stmt->rows[r];
        if (vr->n_values != n_target) {
            snprintf(err, err_len, "row %zu: %zu value%s provided for %zu column%s",
                     r + 1, vr->n_values, vr->n_values == 1 ? "" : "s", n_target, n_target == 1 ? "" : "s");
            free(cols);
            return false;
        }
        for (size_t i = 0; i < vr->n_values; i++) {
            if (expr_has_column_ref(vr->values[i])) {
                snprintf(err, err_len, "row %zu: column references are not allowed in VALUES", r + 1);
                free(cols);
                return false;
            }
            if (!expr_fits_column(vr->values[i], &empty_ctx, t->types[cols[i]], t->names[cols[i]], err, err_len)) {
                free(cols);
                return false;
            }
        }

        size_t row = table_append_row(t);
        if (row == SIZE_MAX || table_failed(t)) {
            snprintf(err, err_len, "out of memory inserting into %s", stmt->table);
            free(cols);
            return false;
        }
        for (size_t i = 0; i < vr->n_values; i++)
            assign_value(t, row, cols[i], eval_value(vr->values[i], &empty_ctx));

        if (!check_row_constraints(t, row, catalog, err, err_len)) {
            table_delete_row(t, row);
            free(cols);
            return false;
        }
        if (txn) txn_log_insert(txn, t, catalog->tables[t_idx].name, row);
        n_inserted++;
    }

    free(cols);
    *out_changes = n_inserted;
    return true;
}

static bool interp_exec_update(const UpdateStmt *stmt, Catalog *catalog, Txn *txn, const Value *params, size_t n_params,
                                size_t *out_changes, char *err, size_t err_len) {
    int t_idx = catalog_find(catalog, stmt->table);
    if (t_idx < 0) {
        snprintf(err, err_len, "no such table: %s", stmt->table);
        return false;
    }
    Table *t = &catalog->tables[t_idx].table;

    RowSource src0       = { stmt->table, t, 0 };
    RowCtx    static_ctx = { &src0, 1, params, n_params };

    int *cols = malloc(stmt->n_assignments * sizeof(int));
    Value *new_vals = malloc(stmt->n_assignments * sizeof(Value));
    if (!cols || !new_vals) {
        snprintf(err, err_len, "out of memory");
        free(cols);
        free(new_vals);
        return false;
    }

    bool pk_updated = false;
    for (size_t i = 0; i < stmt->n_assignments; i++) {
        int col = table_find_column(t, stmt->assignments[i].column);
        if (col < 0) {
            snprintf(err, err_len, "no such column \"%s\" in table \"%s\"", stmt->assignments[i].column, stmt->table);
            free(cols);
            free(new_vals);
            return false;
        }
        if (!expr_fits_column(stmt->assignments[i].value, &static_ctx, t->types[col], t->names[col], err, err_len)) {
            free(cols);
            free(new_vals);
            return false;
        }
        cols[i] = col;
        if (col == t->pk_col) pk_updated = true;
    }

    if (stmt->where && !validate_where(stmt->where, &static_ctx, err, err_len)) {
        free(cols);
        free(new_vals);
        return false;
    }

    size_t n_updated = 0;
    Cursor cur;
    cursor_init(&cur, t);
    size_t row;
    while (cursor_next(&cur, &row)) {
        RowSource src = { stmt->table, t, row };
        RowCtx    ctx = { &src, 1, params, n_params };

        if (stmt->where && eval_tri(stmt->where, &ctx) != TRI_TRUE) continue;

        if (pk_updated && !check_pk_not_referenced(catalog, t, stmt->table, row, err, err_len)) {
            free(cols);
            free(new_vals);
            return false;
        }

        /* Evaluate every assignment's RHS against the row's state before any of
         * this row's assignments are applied - "SET a = b, b = a" swaps rather
         * than making both equal, matching standard SQL semantics. */
        for (size_t i = 0; i < stmt->n_assignments; i++)
            new_vals[i] = eval_value(stmt->assignments[i].value, &ctx);
        for (size_t i = 0; i < stmt->n_assignments; i++) {
            if (txn) txn_log_update(txn, t, catalog->tables[t_idx].name, row, (size_t)cols[i]);
            assign_value(t, row, cols[i], new_vals[i]);
        }

        if (!check_row_constraints(t, row, catalog, err, err_len)) {
            free(cols);
            free(new_vals);
            return false;
        }
        n_updated++;
    }

    free(cols);
    free(new_vals);
    *out_changes = n_updated;
    return true;
}

static bool interp_exec_delete(const DeleteStmt *stmt, Catalog *catalog, Txn *txn, const Value *params, size_t n_params,
                                size_t *out_changes, char *err, size_t err_len) {
    int t_idx = catalog_find(catalog, stmt->table);
    if (t_idx < 0) {
        snprintf(err, err_len, "no such table: %s", stmt->table);
        return false;
    }
    Table *t = &catalog->tables[t_idx].table;

    RowSource src0       = { stmt->table, t, 0 };
    RowCtx    static_ctx = { &src0, 1, params, n_params };
    if (stmt->where && !validate_where(stmt->where, &static_ctx, err, err_len)) return false;

    size_t n_deleted = 0;
    Cursor cur;
    cursor_init(&cur, t);
    size_t row;
    while (cursor_next(&cur, &row)) {
        RowSource src = { stmt->table, t, row };
        RowCtx    ctx = { &src, 1, params, n_params };
        if (stmt->where && eval_tri(stmt->where, &ctx) != TRI_TRUE) continue;

        if (!check_pk_not_referenced(catalog, t, stmt->table, row, err, err_len)) return false;

        if (txn) txn_log_delete(txn, t, catalog->tables[t_idx].name, row);
        table_delete_row(t, row);
        n_deleted++;
    }

    *out_changes = n_deleted;
    return true;
}

/* DDL - takes effect immediately and isn't undo-logged (there's no "undo a
 * CREATE TABLE" bookkeeping in txn.c; like most real engines, DDL sits
 * outside the row-level transaction machinery). Built in a scratch Table
 * first, same as load.c's loader - only installed into the catalog once
 * everything (duplicate columns, single PK, FK targets) has validated. */
static bool interp_exec_create_table(const CreateTableStmt *stmt, Catalog *catalog, char *err, size_t err_len) {
    if (catalog_find(catalog, stmt->table) >= 0) {
        snprintf(err, err_len, "table \"%s\" already exists", stmt->table);
        return false;
    }
    if (stmt->n_columns == 0) {
        snprintf(err, err_len, "table must have at least one column");
        return false;
    }

    for (size_t i = 0; i < stmt->n_columns; i++)
        for (size_t j = i + 1; j < stmt->n_columns; j++)
            if (strcmp(stmt->columns[i].name, stmt->columns[j].name) == 0) {
                snprintf(err, err_len, "duplicate column name \"%s\"", stmt->columns[i].name);
                return false;
            }

    int pk_idx = -1;
    for (size_t i = 0; i < stmt->n_columns; i++) {
        if (!stmt->columns[i].primary) continue;
        if (pk_idx >= 0) {
            snprintf(err, err_len, "only one primary key column is supported (both \"%s\" and \"%s\" are marked primary)",
                     stmt->columns[pk_idx].name, stmt->columns[i].name);
            return false;
        }
        pk_idx = (int)i;
    }

    for (size_t i = 0; i < stmt->n_columns; i++) {
        const ColumnDef *c = &stmt->columns[i];
        if (!c->has_fk) continue;
        if (strcmp(c->fk_table, stmt->table) == 0) {
            snprintf(err, err_len, "column \"%s\" cannot reference \"%s\" (self-referencing foreign keys aren't supported)",
                     c->name, stmt->table);
            return false;
        }
        int ref_idx = catalog_find(catalog, c->fk_table);
        if (ref_idx < 0) {
            snprintf(err, err_len, "unknown table \"%s\" referenced by column \"%s\"", c->fk_table, c->name);
            return false;
        }
        const Table *ref_table = &catalog->tables[ref_idx].table;
        int ref_col = table_find_column(ref_table, c->fk_column);
        if (ref_col < 0) {
            snprintf(err, err_len, "no such column \"%s\" in table \"%s\" referenced by column \"%s\"",
                     c->fk_column, c->fk_table, c->name);
            return false;
        }
        if (!table_is_primary_key(ref_table, (size_t)ref_col)) {
            snprintf(err, err_len, "column \"%s.%s\" is not a primary key", c->fk_table, c->fk_column);
            return false;
        }
        if (ref_table->types[ref_col] != c->type) {
            snprintf(err, err_len, "column \"%s\" is type %s but referenced column \"%s.%s\" is type %s",
                     c->name, field_type_label(c->type), c->fk_table, c->fk_column, field_type_label(ref_table->types[ref_col]));
            return false;
        }
    }

    char      **names = malloc(stmt->n_columns * sizeof(char *));
    FieldType  *types = malloc(stmt->n_columns * sizeof(FieldType));
    if (!names || !types) {
        snprintf(err, err_len, "out of memory");
        free(names);
        free(types);
        return false;
    }
    for (size_t i = 0; i < stmt->n_columns; i++) {
        names[i] = stmt->columns[i].name;
        types[i] = stmt->columns[i].type;
    }

    Table new_table;
    table_init(&new_table, (const char **)names, types, stmt->n_columns);
    free(names);
    free(types);
    if (table_failed(&new_table)) {
        table_term(&new_table);
        snprintf(err, err_len, "out of memory creating table \"%s\"", stmt->table);
        return false;
    }

    for (size_t i = 0; i < stmt->n_columns; i++) {
        const ColumnDef *c = &stmt->columns[i];
        if (c->has_fk)
            table_set_foreign_key(&new_table, i, c->fk_table, c->fk_column, c->fk_on_delete, c->fk_on_update);
    }
    if (pk_idx >= 0) table_set_primary_key(&new_table, (size_t)pk_idx);
    if (table_failed(&new_table)) {
        table_term(&new_table);
        snprintf(err, err_len, "out of memory creating table \"%s\"", stmt->table);
        return false;
    }

    Table *slot = catalog_put(catalog, stmt->table, NULL);
    if (!slot) {
        table_term(&new_table);
        snprintf(err, err_len, "out of memory");
        return false;
    }
    *slot = new_table;
    return true;
}

/* WAL-appends one table's share of a commit: every distinct row touched
 * since BEGIN, at its current in-memory state (see wal.h - a live row's
 * full columns, or a tombstone). Briefly holds an exclusive lock around
 * the append (M7) so a concurrent reader's `.load` snapshot (a shared
 * lock, see load.c) never observes a torn WAL write. */
static bool commit_wal_for_table(Catalog *catalog, Table *table, const char *table_name,
                                  const size_t *rows, size_t n_rows, char *err, size_t err_len) {
    int idx = catalog_find(catalog, table_name);
    if (idx < 0 || !catalog->tables[idx].path) return true; /* no backing file - nothing to persist */

    const char *path = catalog->tables[idx].path;
    char        wal_path[4160];
    int         n = snprintf(wal_path, sizeof wal_path, "%s.wal", path);
    if (n < 0 || (size_t)n >= sizeof wal_path) {
        snprintf(err, err_len, "path too long for WAL: %s", path);
        return false;
    }

    Db4Lock lock;
    lock.fd = -1;
    bool have_lock = db4_lock_open(&lock, path) && db4_lock_exclusive(&lock);

    bool ok = wal_append(wal_path, table, rows, n_rows);

    if (have_lock) db4_lock_release(&lock);
    db4_lock_close(&lock);

    if (!ok) snprintf(err, err_len, "commit failed: could not append to WAL for \"%s\"", table_name);
    return ok;
}

/* Durability: every table touched since BEGIN gets its changed rows
 * WAL-appended (fsync'd, bounded) rather than a whole-table dump_csv
 * rewrite - "nothing durable until commit" at a fraction of M5's
 * short-term cost. `.checkpoint <table>` folds the WAL back into the
 * base CSV when it's grown enough to be worth compacting. */
static bool interp_exec_commit(Catalog *catalog, Txn *txn, char *err, size_t err_len) {
    if (!txn->active) {
        snprintf(err, err_len, "no transaction is active");
        return false;
    }

    for (size_t i = 0; i < txn->count; i++) {
        Table      *table      = txn->log[i].table;
        const char *table_name = txn->log[i].table_name;

        bool table_seen_before = false;
        for (size_t j = 0; j < i; j++)
            if (txn->log[j].table == table) { table_seen_before = true; break; }
        if (table_seen_before) continue;

        size_t *rows = malloc(txn->count * sizeof(size_t));
        if (!rows) {
            snprintf(err, err_len, "out of memory");
            return false;
        }

        size_t n_rows = 0;
        for (size_t j = i; j < txn->count; j++) {
            if (txn->log[j].table != table) continue;
            size_t row = txn->log[j].row;
            bool   dup = false;
            for (size_t k = 0; k < n_rows; k++)
                if (rows[k] == row) { dup = true; break; }
            if (!dup) rows[n_rows++] = row;
        }

        bool ok = commit_wal_for_table(catalog, table, table_name, rows, n_rows, err, err_len);
        free(rows);
        if (!ok) return false;
    }

    txn->count  = 0;
    txn->active = false;
    return true;
}

/* Dispatches to the right execution path, wrapping a mutation in an
 * implicit BEGIN/COMMIT (or ROLLBACK on failure) when no explicit
 * transaction is already open - autocommit runs silently, same durability
 * guarantee either way. CREATE TABLE is DDL and runs outside the
 * transaction system entirely (see interp_exec_create_table).
 *
 * Purely an engine call now (M8): no printing happens in here - out_rs
 * receives a SELECT's rows (untouched for every other statement kind),
 * out_changes receives INSERT/UPDATE/DELETE's affected-row count (0 for
 * every other kind). Both are optional (pass NULL to ignore). Turning
 * this into human-readable output is db4.c/main.c's job now, not the
 * engine's - see db4.h. */
bool interp_exec(const Stmt *stmt, Catalog *catalog, Txn *txn, const Value *params, size_t n_params,
                  ResultSet *out_rs, size_t *out_changes, char *err, size_t err_len) {
    if (out_changes) *out_changes = 0;

    switch (stmt->kind) {
        case STMT_SELECT:
            return interp_exec_select(&stmt->as.select, catalog, params, n_params, out_rs, err, err_len);
        case STMT_CREATE_TABLE:
            return interp_exec_create_table(&stmt->as.create_table, catalog, err, err_len);
        case STMT_BEGIN:
            return txn_begin(txn, err, err_len);
        case STMT_COMMIT:
            return interp_exec_commit(catalog, txn, err, err_len);
        case STMT_ROLLBACK:
            if (!txn->active) {
                snprintf(err, err_len, "no transaction is active");
                return false;
            }
            txn_rollback(txn);
            return true;
        case STMT_INSERT:
        case STMT_UPDATE:
        case STMT_DELETE: {
            bool autocommit = !txn->active;
            if (autocommit) {
                char begin_err[128];
                if (!txn_begin(txn, begin_err, sizeof begin_err)) {
                    snprintf(err, err_len, "%s", begin_err);
                    return false;
                }
            }

            size_t changes = 0;
            bool ok = stmt->kind == STMT_INSERT ? interp_exec_insert(&stmt->as.insert, catalog, txn, params, n_params, &changes, err, err_len)
                    : stmt->kind == STMT_UPDATE ? interp_exec_update(&stmt->as.update, catalog, txn, params, n_params, &changes, err, err_len)
                                                 : interp_exec_delete(&stmt->as.del, catalog, txn, params, n_params, &changes, err, err_len);
            if (out_changes) *out_changes = changes;

            if (autocommit) {
                if (ok) ok = interp_exec_commit(catalog, txn, err, err_len);
                else    txn_rollback(txn);
            }
            return ok;
        }
    }
    snprintf(err, err_len, "internal error: unknown statement kind");
    return false;
}
