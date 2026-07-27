#ifndef RESULT_H
#define RESULT_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

/* The materialized output of a SELECT - built once (by interp_exec_select,
 * via exec_select_plain/exec_select_grouped) before db4_step starts
 * handing rows back one at a time. Every cell is self-typed (Value
 * carries its own kind/is_null), so no separate column-type array is
 * needed alongside col_names. */
typedef struct {
    char   **col_names; /* n_cols owned strings */
    size_t   n_cols;

    Value  *cells; /* n_rows * n_cols, row-major, owned */
    size_t  n_rows;
    size_t  row_cap;
} ResultSet;

/* Zeroes rs and allocates its n_cols column-name slots (still unset -
 * see result_set_set_col_name). Safe to call on an already-zeroed
 * ResultSet only; never on one already populated. */
bool result_set_init(ResultSet *rs, size_t n_cols);

bool result_set_set_col_name(ResultSet *rs, size_t idx, const char *name);

/* Appends one row - row_values must have exactly rs->n_cols entries. */
bool result_set_add_row(ResultSet *rs, const Value *row_values);

void result_set_free(ResultSet *rs);

#endif
