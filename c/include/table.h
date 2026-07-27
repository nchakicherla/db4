#ifndef TABLE_H
#define TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "field.h"
#include "index.h"

typedef struct {
    uint32_t offset;
    uint32_t len;
} StringRef;

typedef enum {
    FK_ACTION_NONE = 0,
    FK_ACTION_RESTRICT,
    FK_ACTION_CASCADE,
    FK_ACTION_SET_NULL,
} FkAction;

typedef struct {
    char      **names;
    FieldType  *types;
    void      **columns;
    uint8_t   **col_null;
    char      **fk_table;
    char      **fk_column;
    FkAction   *fk_on_delete;
    FkAction   *fk_on_update;
    bool       *is_pk;
    size_t      n_cols;

    size_t   n_rows;
    size_t   row_cap;

    uint8_t *dead;
    size_t   n_dead;

    Arena  heap;
    char  *heap_data;
    size_t heap_len;
    size_t heap_cap;

    int      pk_col;
    RowIndex pk_index;

    /* The <path>.gen sidecar's value as of this table's last .load (0 if
     * never checkpointed, or not backed by a file at all - see load.c and
     * wal.c's wal_checkpoint/wal_read_generation). interp.c's commit path
     * compares this against the file's *current* value before appending a
     * WAL frame - a mismatch means some other process compacted (and so
     * renumbered) the base CSV via .checkpoint since this table was loaded
     * here, which would otherwise make this process's next commit describe
     * the wrong row to whoever replays the WAL later. */
    uint32_t wal_generation;

    Arena arena;
} Table;

void table_init(Table *t, const char **names, const FieldType *types, size_t n_cols);
void table_term(Table *t);

bool table_failed(const Table *t);
bool table_take_failure(Table *t);

int table_find_column(const Table *t, const char *name);

size_t table_append_row(Table *t);
void   table_delete_row(Table *t, size_t row);
void   table_undelete_row(Table *t, size_t row);
bool   table_row_is_dead(const Table *t, size_t row);

void table_set_null(Table *t, size_t row, size_t col);
bool table_is_null(const Table *t, size_t row, size_t col);

void table_set_int(Table *t, size_t row, size_t col, int64_t v);
void table_set_double(Table *t, size_t row, size_t col, double v);
void table_set_bool(Table *t, size_t row, size_t col, bool v);
void table_set_text(Table *t, size_t row, size_t col, const char *s, size_t len);

int64_t     table_get_int(const Table *t, size_t row, size_t col);
double      table_get_double(const Table *t, size_t row, size_t col);
bool        table_get_bool(const Table *t, size_t row, size_t col);
const char *table_get_text(const Table *t, size_t row, size_t col, size_t *out_len);

StringRef table_get_text_ref(const Table *t, size_t row, size_t col);
void      table_set_text_ref(Table *t, size_t row, size_t col, StringRef ref);

void table_set_foreign_key(Table *t, size_t col, const char *ref_table, const char *ref_column,
                            FkAction on_delete, FkAction on_update);

bool table_get_foreign_key(const Table *t, size_t col, const char **out_table, const char **out_column);

FkAction table_get_on_delete(const Table *t, size_t col);
FkAction table_get_on_update(const Table *t, size_t col);

void table_set_primary_key(Table *t, size_t col);
bool table_is_primary_key(const Table *t, size_t col);

bool table_column_has_duplicate(const Table *t, size_t col, size_t row);

bool table_has_matching_value(const Table *ref_t, size_t ref_col, const Table *t, size_t row, size_t col);

void table_find_matching_rows(const Table *t, size_t col, const Table *val_t, size_t val_row, size_t val_col,
                               void *ctx, bool (*visit)(void *ctx, size_t row));

size_t table_add_column(Table *t, const char *name, FieldType type);

bool table_drop_column(Table *t, size_t col);

void table_compact(Table *t);

void table_compact_heap(Table *t);

#endif
