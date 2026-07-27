#include "result.h"

#include <stdlib.h>
#include <string.h>

static char *dup_str(const char *s) {
    size_t len = strlen(s);
    char  *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len + 1);
    return out;
}

bool result_set_init(ResultSet *rs, size_t n_cols) {
    rs->n_cols    = n_cols;
    rs->col_names = n_cols ? calloc(n_cols, sizeof(char *)) : NULL;
    rs->cells     = NULL;
    rs->n_rows    = 0;
    rs->row_cap   = 0;
    return n_cols == 0 || rs->col_names != NULL;
}

bool result_set_set_col_name(ResultSet *rs, size_t idx, const char *name) {
    char *dup = dup_str(name);
    if (!dup) return false;
    free(rs->col_names[idx]);
    rs->col_names[idx] = dup;
    return true;
}

bool result_set_add_row(ResultSet *rs, const Value *row_values) {
    if (rs->n_rows == rs->row_cap) {
        size_t new_cap = rs->row_cap ? rs->row_cap * 2 : 16;
        Value *grown = realloc(rs->cells, new_cap * rs->n_cols * sizeof(Value));
        if (!grown) return false;
        rs->cells   = grown;
        rs->row_cap = new_cap;
    }
    memcpy(&rs->cells[rs->n_rows * rs->n_cols], row_values, rs->n_cols * sizeof(Value));
    rs->n_rows++;
    return true;
}

void result_set_free(ResultSet *rs) {
    if (!rs) return;
    for (size_t i = 0; i < rs->n_cols; i++) free(rs->col_names[i]);
    free(rs->col_names);
    free(rs->cells);
    rs->col_names = NULL;
    rs->cells     = NULL;
    rs->n_cols = rs->n_rows = rs->row_cap = 0;
}
