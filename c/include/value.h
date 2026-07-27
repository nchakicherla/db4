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

/* Formats v as text (the same rendering the REPL prints and db4_exec's
 * text-column convenience callback uses) - "NULL" for a null value. */
void print_value(FILE *f, Value v);

#endif
