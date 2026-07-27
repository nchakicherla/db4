#include "interp.h"

#include <stdlib.h>
#include <string.h>

#include "cursor.h"
#include "field.h"

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

/* Column types (and literal kinds) are static, so a WHERE clause's shape
 * can be fully type-checked once, before any row is read - no per-row
 * runtime type error is possible after this passes. */
static bool infer_expr_type(const Expr *e, const Table *t, ExprValueType *out, char *err, size_t err_len) {
    switch (e->kind) {
        case EXPR_COLUMN: {
            int col = table_find_column(t, e->as.column);
            if (col < 0) {
                snprintf(err, err_len, "no such column \"%s\"", e->as.column);
                return false;
            }
            *out = field_type_to_expr_type(t->types[col]);
            return true;
        }
        case EXPR_LIT_INT:    *out = ETYPE_INT;    return true;
        case EXPR_LIT_DOUBLE: *out = ETYPE_DOUBLE; return true;
        case EXPR_LIT_BOOL:   *out = ETYPE_BOOL;   return true;
        case EXPR_LIT_STRING: *out = ETYPE_STRING; return true;
        case EXPR_LIT_NULL:   *out = ETYPE_NULL;   return true;
        case EXPR_NOT: {
            ExprValueType operand;
            if (!infer_expr_type(e->as.not_operand, t, &operand, err, err_len)) return false;
            if (operand != ETYPE_BOOL && operand != ETYPE_NULL) {
                snprintf(err, err_len, "NOT requires a boolean expression, found %s", expr_type_label(operand));
                return false;
            }
            *out = ETYPE_BOOL;
            return true;
        }
        case EXPR_BINARY: {
            ExprValueType l, r;
            if (!infer_expr_type(e->as.binary.left, t, &l, err, err_len)) return false;
            if (!infer_expr_type(e->as.binary.right, t, &r, err, err_len)) return false;

            if (e->as.binary.op == OP_AND || e->as.binary.op == OP_OR) {
                if ((l != ETYPE_BOOL && l != ETYPE_NULL) || (r != ETYPE_BOOL && r != ETYPE_NULL)) {
                    snprintf(err, err_len, "%s requires boolean operands, found %s and %s",
                             e->as.binary.op == OP_AND ? "AND" : "OR", expr_type_label(l), expr_type_label(r));
                    return false;
                }
                *out = ETYPE_BOOL;
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

static bool validate_where(const Expr *e, const Table *t, char *err, size_t err_len) {
    ExprValueType ty;
    if (!infer_expr_type(e, t, &ty, err, err_len)) return false;
    if (ty != ETYPE_BOOL && ty != ETYPE_NULL) {
        snprintf(err, err_len, "WHERE clause must be a boolean expression, found %s", expr_type_label(ty));
        return false;
    }
    return true;
}

typedef struct {
    FieldType kind;
    bool      is_null;
    union {
        int64_t i;
        double  d;
        bool    b;
        struct {
            const char *data;
            size_t      len;
        } s;
    } as;
} Value;

static Value value_int(int64_t v)    { Value r; r.kind = FT_INT;    r.is_null = false; r.as.i = v; return r; }
static Value value_double(double v)  { Value r; r.kind = FT_DOUBLE; r.is_null = false; r.as.d = v; return r; }
static Value value_bool(bool v)      { Value r; r.kind = FT_BOOL;   r.is_null = false; r.as.b = v; return r; }
static Value value_text(const char *s, size_t len) {
    Value r;
    r.kind = FT_TEXT;
    r.is_null = false;
    r.as.s.data = s;
    r.as.s.len  = len;
    return r;
}
static Value value_null(FieldType kind) { Value r; r.kind = kind; r.is_null = true; return r; }

static Value read_column(const Table *t, size_t row, size_t col) {
    if (table_is_null(t, row, col)) return value_null(t->types[col]);
    switch (t->types[col]) {
        case FT_INT:    return value_int(table_get_int(t, row, col));
        case FT_DOUBLE: return value_double(table_get_double(t, row, col));
        case FT_BOOL:   return value_bool(table_get_bool(t, row, col));
        case FT_TEXT: {
            size_t len;
            const char *s = table_get_text(t, row, col, &len);
            return value_text(s, len);
        }
        default: return value_null(FT_TEXT);
    }
}

static Value eval_value(const Expr *e, const Table *t, size_t row) {
    switch (e->kind) {
        case EXPR_COLUMN: {
            int col = table_find_column(t, e->as.column);
            return read_column(t, row, (size_t)col);
        }
        case EXPR_LIT_INT:    return value_int(e->as.int_value);
        case EXPR_LIT_DOUBLE: return value_double(e->as.double_value);
        case EXPR_LIT_BOOL:   return value_bool(e->as.bool_value);
        case EXPR_LIT_STRING: return value_text(e->as.string_value.data, e->as.string_value.len);
        case EXPR_LIT_NULL:   return value_null(FT_TEXT);
        default:              return value_null(FT_TEXT); /* unreachable: NOT/AND/OR aren't values */
    }
}

static int compare_values(Value a, Value b) {
    if ((a.kind == FT_INT || a.kind == FT_DOUBLE) && (b.kind == FT_INT || b.kind == FT_DOUBLE)) {
        double x = a.kind == FT_INT ? (double)a.as.i : a.as.d;
        double y = b.kind == FT_INT ? (double)b.as.i : b.as.d;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.kind == FT_BOOL) return (int)a.as.b - (int)b.as.b;

    size_t n = a.as.s.len < b.as.s.len ? a.as.s.len : b.as.s.len;
    int c = n ? memcmp(a.as.s.data, b.as.s.data, n) : 0;
    if (c != 0) return c;
    if (a.as.s.len != b.as.s.len) return a.as.s.len < b.as.s.len ? -1 : 1;
    return 0;
}

/* Three-valued (true/false/unknown) so NULL propagates through AND/OR/NOT
 * the way SQL defines it (e.g. FALSE AND unknown = FALSE, not unknown) -
 * a row matches the WHERE clause only when this comes out TRI_TRUE. */
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

static Tri eval_tri(const Expr *e, const Table *t, size_t row) {
    switch (e->kind) {
        case EXPR_LIT_BOOL: return e->as.bool_value ? TRI_TRUE : TRI_FALSE;
        case EXPR_LIT_NULL: return TRI_UNKNOWN;
        case EXPR_COLUMN:    return value_to_tri(eval_value(e, t, row));
        case EXPR_NOT: {
            Tri v = eval_tri(e->as.not_operand, t, row);
            if (v == TRI_UNKNOWN) return TRI_UNKNOWN;
            return v == TRI_TRUE ? TRI_FALSE : TRI_TRUE;
        }
        case EXPR_BINARY:
            if (e->as.binary.op == OP_AND) {
                Tri l = eval_tri(e->as.binary.left, t, row);
                Tri r = eval_tri(e->as.binary.right, t, row);
                if (l == TRI_FALSE || r == TRI_FALSE) return TRI_FALSE;
                if (l == TRI_UNKNOWN || r == TRI_UNKNOWN) return TRI_UNKNOWN;
                return TRI_TRUE;
            }
            if (e->as.binary.op == OP_OR) {
                Tri l = eval_tri(e->as.binary.left, t, row);
                Tri r = eval_tri(e->as.binary.right, t, row);
                if (l == TRI_TRUE || r == TRI_TRUE) return TRI_TRUE;
                if (l == TRI_UNKNOWN || r == TRI_UNKNOWN) return TRI_UNKNOWN;
                return TRI_FALSE;
            }
            return eval_comparison(e->as.binary.op,
                                    eval_value(e->as.binary.left, t, row),
                                    eval_value(e->as.binary.right, t, row));
        default: return TRI_UNKNOWN; /* unreachable: not a valid predicate node */
    }
}

static void print_value(FILE *f, Value v) {
    if (v.is_null) {
        fprintf(f, "NULL");
        return;
    }
    switch (v.kind) {
        case FT_INT:    fprintf(f, "%lld", (long long)v.as.i); break;
        case FT_DOUBLE: fprintf(f, "%g", v.as.d); break;
        case FT_BOOL:   fprintf(f, "%s", v.as.b ? "true" : "false"); break;
        case FT_TEXT:   fprintf(f, "%.*s", (int)v.as.s.len, v.as.s.data); break;
        default: break;
    }
}

/* qsort has no portable reentrant form in C11, and exec is synchronous and
 * single-threaded within one REPL command, so a file-scope sort context is
 * safe here without needing qsort_r/thread-locals. */
static const Table *g_sort_table;
static int          g_sort_col;
static bool         g_sort_desc;

static int sort_cmp(const void *pa, const void *pb) {
    size_t ra = *(const size_t *)pa;
    size_t rb = *(const size_t *)pb;

    Value va = read_column(g_sort_table, ra, (size_t)g_sort_col);
    Value vb = read_column(g_sort_table, rb, (size_t)g_sort_col);

    int c;
    if (va.is_null && vb.is_null) c = 0;
    else if (va.is_null) c = -1;
    else if (vb.is_null) c = 1;
    else c = compare_values(va, vb);

    return g_sort_desc ? -c : c;
}

bool interp_exec_select(const SelectStmt *stmt, const Catalog *catalog, FILE *out, char *err, size_t err_len) {
    int t_idx = catalog_find(catalog, stmt->table);
    if (t_idx < 0) {
        snprintf(err, err_len, "no such table: %s", stmt->table);
        return false;
    }
    const Table *t = &catalog->tables[t_idx].table;

    size_t n_proj = stmt->columns.is_star ? t->n_cols : stmt->columns.count;
    int *proj_cols = malloc(n_proj * sizeof(int));
    if (!proj_cols) {
        snprintf(err, err_len, "out of memory");
        return false;
    }

    if (stmt->columns.is_star) {
        for (size_t i = 0; i < n_proj; i++) proj_cols[i] = (int)i;
    } else {
        for (size_t i = 0; i < n_proj; i++) {
            int col = table_find_column(t, stmt->columns.names[i]);
            if (col < 0) {
                snprintf(err, err_len, "no such column \"%s\" in table \"%s\"", stmt->columns.names[i], stmt->table);
                free(proj_cols);
                return false;
            }
            proj_cols[i] = col;
        }
    }

    if (stmt->where && !validate_where(stmt->where, t, err, err_len)) {
        free(proj_cols);
        return false;
    }

    int order_col = -1;
    if (stmt->has_order_by) {
        order_col = table_find_column(t, stmt->order_by_col);
        if (order_col < 0) {
            snprintf(err, err_len, "no such column \"%s\" in table \"%s\"", stmt->order_by_col, stmt->table);
            free(proj_cols);
            return false;
        }
    }

    size_t *rows = NULL;
    size_t  n_rows = 0, cap = 0;

    Cursor cur;
    cursor_init(&cur, t);
    size_t row;
    while (cursor_next(&cur, &row)) {
        if (stmt->where && eval_tri(stmt->where, t, row) != TRI_TRUE) continue;

        if (n_rows == cap) {
            size_t new_cap = cap ? cap * 2 : 16;
            size_t *grown = realloc(rows, new_cap * sizeof(size_t));
            if (!grown) {
                snprintf(err, err_len, "out of memory");
                free(rows);
                free(proj_cols);
                return false;
            }
            rows = grown;
            cap  = new_cap;
        }
        rows[n_rows++] = row;
    }

    if (stmt->has_order_by) {
        g_sort_table = t;
        g_sort_col   = order_col;
        g_sort_desc  = stmt->order_desc;
        qsort(rows, n_rows, sizeof(size_t), sort_cmp);
    }

    size_t n_out = n_rows;
    if (stmt->has_limit) {
        size_t lim = stmt->limit < 0 ? 0 : (size_t)stmt->limit;
        if (lim < n_out) n_out = lim;
    }

    for (size_t i = 0; i < n_proj; i++) {
        if (i) fprintf(out, " | ");
        fprintf(out, "%s", t->names[proj_cols[i]]);
    }
    fprintf(out, "\n");

    for (size_t i = 0; i < n_out; i++) {
        size_t r = rows[i];
        for (size_t c = 0; c < n_proj; c++) {
            if (c) fprintf(out, " | ");
            print_value(out, read_column(t, r, (size_t)proj_cols[c]));
        }
        fprintf(out, "\n");
    }
    fprintf(out, "(%zu row%s)\n", n_out, n_out == 1 ? "" : "s");

    free(rows);
    free(proj_cols);
    return true;
}
