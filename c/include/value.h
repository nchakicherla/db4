#ifndef VALUE_H
#define VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "field.h"
#include "table.h"

/* A single typed, possibly-NULL SQL value - the common currency between
 * interp.c's expression evaluator, the ResultSet a SELECT materializes
 * into, and db4.c's column accessors. TEXT data is borrowed (points into
 * a Table's append-only heap, a parsed Stmt's arena, or a string
 * literal) - Value never owns it. */
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

Value value_int(int64_t v);
Value value_double(double v);
Value value_bool(bool v);
Value value_text(const char *s, size_t len);
Value value_null(FieldType kind);

Value read_column(const Table *t, size_t row, size_t col);

int compare_values(Value a, Value b);

/* Only ever meaningful for a non-null Value - hashing a NULL's contents
 * makes no sense (NULL isn't equal to anything, including another NULL,
 * in SQL's own terms), so every caller is expected to have already
 * checked is_null, same discipline table.c already keeps around
 * indexing (a PK cell is only ever indexed once it's set to a non-null
 * value - see table_set_int/_double/_bool/_text/_text_ref). Shared by
 * table.c's own per-cell hashing (backing the PK index) and interp.c's
 * point-lookup fast path (hashing a WHERE clause's already-evaluated
 * constant side to probe that same index) - one hash algorithm, not two
 * copies of the same mixing constants. */
uint64_t value_hash(Value v);

/* Formats v as text (the same rendering the REPL prints and db4_exec's
 * text-column convenience callback uses) - "NULL" for a null value. */
void print_value(FILE *f, Value v);

/* Renders v as the shortest decimal string that reads back (via strtod) to
 * the exact same double - "3.14" stays "3.14" rather than the noise a fixed
 * %.17g would print for it, but a value that genuinely needs every digit to
 * round-trip still gets them, unlike a fixed %g (6 significant digits by
 * default) which silently drops precision. Shared by print_value (REPL/
 * db4_exec display) and load.c's dump_cell (CSV/WAL persistence) - one
 * formatting rule, not two ways for a DOUBLE to lose precision on its way
 * out. buf must be at least 32 bytes. */
void format_double(char *buf, size_t buf_len, double v);

#endif
