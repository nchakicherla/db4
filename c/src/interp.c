#include "interp.h"

#include <stdlib.h>
#include <string.h>

#include "cursor.h"
#include "field.h"
#include "load.h"

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
 * it. FKs can only ever target a primary key column (load.c's loader
 * already requires that), so this only has to look at t's own pk_col. */
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

static bool literal_fits_column(const Expr *lit, FieldType col_type, const char *col_name, char *err, size_t err_len) {
    if (lit->kind == EXPR_LIT_NULL) return true;

    FieldType lit_type;
    switch (lit->kind) {
        case EXPR_LIT_INT:    lit_type = FT_INT; break;
        case EXPR_LIT_DOUBLE: lit_type = FT_DOUBLE; break;
        case EXPR_LIT_BOOL:   lit_type = FT_BOOL; break;
        case EXPR_LIT_STRING: lit_type = FT_TEXT; break;
        default:
            snprintf(err, err_len, "value for column \"%s\" must be a literal", col_name);
            return false;
    }

    if (lit_type == col_type) return true;
    if (lit_type == FT_INT && col_type == FT_DOUBLE) return true; /* widen, like an int literal into a double column */

    snprintf(err, err_len, "column \"%s\" is type %s but value is type %s",
             col_name, field_type_label(col_type), field_type_label(lit_type));
    return false;
}

static void assign_literal(Table *t, size_t row, size_t col, const Expr *lit) {
    if (lit->kind == EXPR_LIT_NULL) {
        table_set_null(t, row, col);
        return;
    }
    switch (t->types[col]) {
        case FT_INT:    table_set_int(t, row, col, lit->as.int_value); break;
        case FT_DOUBLE: table_set_double(t, row, col, lit->kind == EXPR_LIT_INT ? (double)lit->as.int_value : lit->as.double_value); break;
        case FT_BOOL:   table_set_bool(t, row, col, lit->as.bool_value); break;
        case FT_TEXT:   table_set_text(t, row, col, lit->as.string_value.data, lit->as.string_value.len); break;
        default: break;
    }
}

static bool interp_exec_insert(const InsertStmt *stmt, Catalog *catalog, Txn *txn, FILE *out, char *err, size_t err_len) {
    int t_idx = catalog_find(catalog, stmt->table);
    if (t_idx < 0) {
        snprintf(err, err_len, "no such table: %s", stmt->table);
        return false;
    }
    Table *t = &catalog->tables[t_idx].table;

    size_t n_target = stmt->n_columns ? stmt->n_columns : t->n_cols;
    if (stmt->n_values != n_target) {
        snprintf(err, err_len, "%zu value%s provided for %zu column%s",
                 stmt->n_values, stmt->n_values == 1 ? "" : "s", n_target, n_target == 1 ? "" : "s");
        return false;
    }

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

    for (size_t i = 0; i < n_target; i++) {
        if (!literal_fits_column(stmt->values[i], t->types[cols[i]], t->names[cols[i]], err, err_len)) {
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

    for (size_t i = 0; i < n_target; i++)
        assign_literal(t, row, cols[i], stmt->values[i]);

    if (!check_row_constraints(t, row, catalog, err, err_len)) {
        table_delete_row(t, row);
        free(cols);
        return false;
    }

    if (txn) txn_log_insert(txn, t, catalog->tables[t_idx].name, row);

    free(cols);
    fprintf(out, "1 row inserted\n");
    return true;
}

static bool interp_exec_update(const UpdateStmt *stmt, Catalog *catalog, Txn *txn, FILE *out, char *err, size_t err_len) {
    int t_idx = catalog_find(catalog, stmt->table);
    if (t_idx < 0) {
        snprintf(err, err_len, "no such table: %s", stmt->table);
        return false;
    }
    Table *t = &catalog->tables[t_idx].table;

    int *cols = malloc(stmt->n_assignments * sizeof(int));
    if (!cols) {
        snprintf(err, err_len, "out of memory");
        return false;
    }

    bool pk_updated = false;
    for (size_t i = 0; i < stmt->n_assignments; i++) {
        int col = table_find_column(t, stmt->assignments[i].column);
        if (col < 0) {
            snprintf(err, err_len, "no such column \"%s\" in table \"%s\"", stmt->assignments[i].column, stmt->table);
            free(cols);
            return false;
        }
        if (!literal_fits_column(stmt->assignments[i].value, t->types[col], t->names[col], err, err_len)) {
            free(cols);
            return false;
        }
        cols[i] = col;
        if (col == t->pk_col) pk_updated = true;
    }

    if (stmt->where && !validate_where(stmt->where, t, err, err_len)) {
        free(cols);
        return false;
    }

    size_t n_updated = 0;
    Cursor cur;
    cursor_init(&cur, t);
    size_t row;
    while (cursor_next(&cur, &row)) {
        if (stmt->where && eval_tri(stmt->where, t, row) != TRI_TRUE) continue;

        if (pk_updated && !check_pk_not_referenced(catalog, t, stmt->table, row, err, err_len)) {
            free(cols);
            return false;
        }

        for (size_t i = 0; i < stmt->n_assignments; i++) {
            if (txn) txn_log_update(txn, t, catalog->tables[t_idx].name, row, (size_t)cols[i]);
            assign_literal(t, row, cols[i], stmt->assignments[i].value);
        }

        if (!check_row_constraints(t, row, catalog, err, err_len)) {
            free(cols);
            return false;
        }
        n_updated++;
    }

    free(cols);
    fprintf(out, "%zu row%s updated\n", n_updated, n_updated == 1 ? "" : "s");
    return true;
}

static bool interp_exec_delete(const DeleteStmt *stmt, Catalog *catalog, Txn *txn, FILE *out, char *err, size_t err_len) {
    int t_idx = catalog_find(catalog, stmt->table);
    if (t_idx < 0) {
        snprintf(err, err_len, "no such table: %s", stmt->table);
        return false;
    }
    Table *t = &catalog->tables[t_idx].table;

    if (stmt->where && !validate_where(stmt->where, t, err, err_len)) return false;

    size_t n_deleted = 0;
    Cursor cur;
    cursor_init(&cur, t);
    size_t row;
    while (cursor_next(&cur, &row)) {
        if (stmt->where && eval_tri(stmt->where, t, row) != TRI_TRUE) continue;

        if (!check_pk_not_referenced(catalog, t, stmt->table, row, err, err_len)) return false;

        if (txn) txn_log_delete(txn, t, catalog->tables[t_idx].name, row);
        table_delete_row(t, row);
        n_deleted++;
    }

    fprintf(out, "%zu row%s deleted\n", n_deleted, n_deleted == 1 ? "" : "s");
    return true;
}

/* Durability: every table touched since BEGIN gets dump_csv's atomic
 * write-temp/fsync/rename back to the path it was loaded from - "nothing
 * durable until commit" (M5's short-term approach; the WAL follow-on that
 * avoids a whole-table rewrite per commit is still ahead of us). */
static bool interp_exec_commit(Catalog *catalog, Txn *txn, char *err, size_t err_len) {
    if (!txn->active) {
        snprintf(err, err_len, "no transaction is active");
        return false;
    }

    const char **names = malloc(catalog->count * sizeof(char *));
    if (!names && catalog->count) {
        snprintf(err, err_len, "out of memory");
        return false;
    }

    size_t n = txn_commit(txn, names, catalog->count);

    for (size_t i = 0; i < n; i++) {
        int idx = catalog_find(catalog, names[i]);
        if (idx < 0) continue;
        if (!catalog->tables[idx].path) continue;
        if (!dump_csv(&catalog->tables[idx].table, catalog->tables[idx].path)) {
            snprintf(err, err_len, "commit failed: could not durably write \"%s\"", names[i]);
            free(names);
            return false;
        }
    }

    free(names);
    return true;
}

/* Dispatches to the right mutation, wrapping it in an implicit BEGIN/COMMIT
 * (or ROLLBACK on failure) when no explicit transaction is already open -
 * autocommit runs silently (no BEGIN/COMMIT feedback), it's the same
 * durability guarantee either way. */
bool interp_exec(const Stmt *stmt, Catalog *catalog, Txn *txn, FILE *out, char *err, size_t err_len) {
    switch (stmt->kind) {
        case STMT_SELECT:
            return interp_exec_select(&stmt->as.select, catalog, out, err, err_len);
        case STMT_BEGIN:
            if (!txn_begin(txn, err, err_len)) return false;
            fprintf(out, "BEGIN\n");
            return true;
        case STMT_COMMIT:
            if (!interp_exec_commit(catalog, txn, err, err_len)) return false;
            fprintf(out, "COMMIT\n");
            return true;
        case STMT_ROLLBACK:
            if (!txn->active) {
                snprintf(err, err_len, "no transaction is active");
                return false;
            }
            txn_rollback(txn);
            fprintf(out, "ROLLBACK\n");
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

            bool ok = stmt->kind == STMT_INSERT ? interp_exec_insert(&stmt->as.insert, catalog, txn, out, err, err_len)
                    : stmt->kind == STMT_UPDATE ? interp_exec_update(&stmt->as.update, catalog, txn, out, err, err_len)
                                                 : interp_exec_delete(&stmt->as.del, catalog, txn, out, err, err_len);

            if (autocommit) {
                if (ok) {
                    ok = interp_exec_commit(catalog, txn, err, err_len);
                } else {
                    txn_rollback(txn);
                }
            }
            return ok;
        }
    }
    snprintf(err, err_len, "internal error: unknown statement kind");
    return false;
}
