#include "table.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void table_misuse(Table *t, const char *msg) {
#ifndef NDEBUG
    fprintf(stderr, "db4: %s\n", msg);
    assert(0 && "table misuse");
#endif
    (void)msg;
    arena_fail(&t->arena, ARENA_FAIL_MISUSE);
}

bool table_failed(const Table *t) {
    return arena_failed(&t->arena) || arena_failed(&t->heap);
}

bool table_take_failure(Table *t) {
    bool failed = table_failed(t);
    arena_take_failure(&t->arena);
    arena_take_failure(&t->heap);
    return failed;
}

static size_t field_width(FieldType type) {
    switch (type) {
        case FT_INT:    return sizeof(int64_t);
        case FT_DOUBLE: return sizeof(double);
        case FT_BOOL:   return sizeof(bool);
        case FT_TEXT:   return sizeof(StringRef);
        default:        return 0;
    }
}

static size_t field_align(FieldType type) {
    switch (type) {
        case FT_INT:    return _Alignof(int64_t);
        case FT_DOUBLE: return _Alignof(double);
        case FT_BOOL:   return _Alignof(bool);
        case FT_TEXT:   return _Alignof(StringRef);
        default:        return 1;
    }
}

static uint64_t cell_hash(const Table *t, size_t row, size_t col);
static void index_row_if_primary_key(Table *t, size_t row, size_t col);

static bool dead_test(const Table *t, size_t row) {
    return (t->dead[row / 8] >> (row % 8)) & 1;
}

static void dead_set(Table *t, size_t row) {
    t->dead[row / 8] |= (uint8_t)(1u << (row % 8));
}

static void dead_clear(Table *t, size_t row) {
    t->dead[row / 8] &= (uint8_t)~(1u << (row % 8));
}

static bool null_test(const Table *t, size_t col, size_t row) {
    return (t->col_null[col][row / 8] >> (row % 8)) & 1;
}

static void null_set(Table *t, size_t col, size_t row) {
    t->col_null[col][row / 8] |= (uint8_t)(1u << (row % 8));
}

static void null_clear(Table *t, size_t col, size_t row) {
    t->col_null[col][row / 8] &= (uint8_t)~(1u << (row % 8));
}

static size_t buf_append(Arena *a, char **data, size_t *len, size_t *cap,
                          const void *src, size_t src_len) {
    size_t needed = arena_checked_add(a, *len, src_len);
    if (arena_failed(a)) return SIZE_MAX;

    if (needed > *cap) {
        size_t grown = *cap ? arena_checked_mul(a, *cap, 2) : 64;
        while (grown < needed && !arena_failed(a)) grown = arena_checked_mul(a, grown, 2);

        char *buf = arena_grow(a, *data, *cap, grown, _Alignof(char));
        if (!buf) return SIZE_MAX;
        *data = buf;
        *cap  = grown;
    }

    size_t offset = *len;
    if (src_len) memcpy(*data + offset, src, src_len);
    *len = needed;
    return offset;
}

void table_init(Table *t, const char **names, const FieldType *types, size_t n_cols) {
    arena_init(&t->arena);
    arena_init(&t->heap);

    t->n_cols    = 0;
    t->n_rows    = 0;
    t->row_cap   = 0;
    t->dead      = NULL;
    t->n_dead    = 0;
    t->heap_data = NULL;
    t->heap_len  = 0;
    t->heap_cap  = 0;
    t->pk_col    = -1;
    t->pk_index  = (RowIndex){0};

    for (size_t i = 0; i < n_cols; i++)
        if (field_width(types[i]) == 0) {
            table_misuse(t, "table column type must not be FT_ERR");
            return;
        }

    t->names     = arena_alloc(&t->arena, n_cols * sizeof(char *), _Alignof(char *));
    t->types     = arena_alloc(&t->arena, n_cols * sizeof(FieldType), _Alignof(FieldType));
    t->columns   = arena_zalloc(&t->arena, n_cols * sizeof(void *), _Alignof(void *));
    t->col_null  = arena_zalloc(&t->arena, n_cols * sizeof(uint8_t *), _Alignof(uint8_t *));
    t->fk_table     = arena_zalloc(&t->arena, n_cols * sizeof(char *), _Alignof(char *));
    t->fk_column    = arena_zalloc(&t->arena, n_cols * sizeof(char *), _Alignof(char *));
    t->fk_on_delete = arena_zalloc(&t->arena, n_cols * sizeof(FkAction), _Alignof(FkAction));
    t->fk_on_update = arena_zalloc(&t->arena, n_cols * sizeof(FkAction), _Alignof(FkAction));
    t->is_pk        = arena_zalloc(&t->arena, n_cols * sizeof(bool), _Alignof(bool));
    if (arena_failed(&t->arena)) return;

    for (size_t i = 0; i < n_cols; i++) {
        t->names[i] = arena_strdup(&t->arena, names[i]);
        t->types[i] = types[i];
    }
    if (arena_failed(&t->arena)) return;

    t->n_cols = n_cols;
}

void table_term(Table *t) {
    arena_term(&t->arena);
    arena_term(&t->heap);
}

int table_find_column(const Table *t, const char *name) {
    for (size_t i = 0; i < t->n_cols; i++)
        if (strcmp(t->names[i], name) == 0) return (int)i;
    return -1;
}

static bool table_grow(Table *t) {
    size_t new_cap = t->row_cap ? t->row_cap * 2 : 8;

    for (size_t i = 0; i < t->n_cols; i++) {
        size_t width = field_width(t->types[i]);
        void *grown = arena_grow(&t->arena, t->columns[i],
                                 arena_checked_mul(&t->arena, t->row_cap, width),
                                 arena_checked_mul(&t->arena, new_cap, width),
                                 field_align(t->types[i]));
        if (!grown) return false;
        t->columns[i] = grown;
    }

    size_t old_bytes = (t->row_cap + 7) / 8;
    size_t new_bytes = (new_cap + 7) / 8;

    uint8_t *dead = arena_grow(&t->arena, t->dead, old_bytes, new_bytes, _Alignof(uint8_t));
    if (!dead) return false;

    for (size_t i = 0; i < t->n_cols; i++) {
        uint8_t *nulls = arena_grow(&t->arena, t->col_null[i], old_bytes, new_bytes, _Alignof(uint8_t));
        if (!nulls) return false;
        t->col_null[i] = nulls;
    }

    t->dead    = dead;
    t->row_cap = new_cap;
    return true;
}

size_t table_add_column(Table *t, const char *name, FieldType type) {
    size_t width = field_width(type);
    if (width == 0) {
        table_misuse(t, "table column type must not be FT_ERR");
        return SIZE_MAX;
    }

    size_t old_n = t->n_cols;
    size_t new_n = old_n + 1;

    char     **names        = arena_grow(&t->arena, t->names,        old_n * sizeof(char *),  new_n * sizeof(char *),  _Alignof(char *));
    FieldType *types        = arena_grow(&t->arena, t->types,        old_n * sizeof(FieldType), new_n * sizeof(FieldType), _Alignof(FieldType));
    void     **columns      = arena_grow(&t->arena, t->columns,      old_n * sizeof(void *),  new_n * sizeof(void *),  _Alignof(void *));
    uint8_t  **col_null     = arena_grow(&t->arena, t->col_null,     old_n * sizeof(uint8_t *), new_n * sizeof(uint8_t *), _Alignof(uint8_t *));
    char     **fk_table     = arena_grow(&t->arena, t->fk_table,     old_n * sizeof(char *),  new_n * sizeof(char *),  _Alignof(char *));
    char     **fk_column    = arena_grow(&t->arena, t->fk_column,    old_n * sizeof(char *),  new_n * sizeof(char *),  _Alignof(char *));
    FkAction  *fk_on_delete = arena_grow(&t->arena, t->fk_on_delete, old_n * sizeof(FkAction), new_n * sizeof(FkAction), _Alignof(FkAction));
    FkAction  *fk_on_update = arena_grow(&t->arena, t->fk_on_update, old_n * sizeof(FkAction), new_n * sizeof(FkAction), _Alignof(FkAction));
    bool      *is_pk        = arena_grow(&t->arena, t->is_pk,        old_n * sizeof(bool),    new_n * sizeof(bool),    _Alignof(bool));

    char    *col_name   = arena_strdup(&t->arena, name);
    void    *col_data   = arena_zalloc(&t->arena, arena_checked_mul(&t->arena, t->row_cap, width), field_align(type));
    size_t   null_bytes = (t->row_cap + 7) / 8;
    uint8_t *col_nulls  = arena_alloc(&t->arena, null_bytes, _Alignof(uint8_t));

    if (arena_failed(&t->arena)) return SIZE_MAX;

    t->names        = names;
    t->types        = types;
    t->columns      = columns;
    t->col_null     = col_null;
    t->fk_table     = fk_table;
    t->fk_column    = fk_column;
    t->fk_on_delete = fk_on_delete;
    t->fk_on_update = fk_on_update;
    t->is_pk        = is_pk;

    t->names[old_n]   = col_name;
    t->types[old_n]   = type;
    t->columns[old_n] = col_data;

    memset(col_nulls, 0xFF, null_bytes);
    t->col_null[old_n] = col_nulls;

    t->n_cols = new_n;
    return old_n;
}

bool table_drop_column(Table *t, size_t col) {
    if (t->n_cols <= 1) return false;

    size_t tail = t->n_cols - col - 1;
    memmove(&t->names[col],        &t->names[col + 1],        tail * sizeof(char *));
    memmove(&t->types[col],        &t->types[col + 1],        tail * sizeof(FieldType));
    memmove(&t->columns[col],      &t->columns[col + 1],      tail * sizeof(void *));
    memmove(&t->col_null[col],     &t->col_null[col + 1],     tail * sizeof(uint8_t *));
    memmove(&t->fk_table[col],     &t->fk_table[col + 1],     tail * sizeof(char *));
    memmove(&t->fk_column[col],    &t->fk_column[col + 1],    tail * sizeof(char *));
    memmove(&t->fk_on_delete[col], &t->fk_on_delete[col + 1], tail * sizeof(FkAction));
    memmove(&t->fk_on_update[col], &t->fk_on_update[col + 1], tail * sizeof(FkAction));
    memmove(&t->is_pk[col],        &t->is_pk[col + 1],        tail * sizeof(bool));

    if (t->pk_col == (int)col) {
        t->pk_col = -1;
        row_index_clear(&t->pk_index);
    } else if (t->pk_col > (int)col) {
        t->pk_col--;
    }

    t->n_cols--;
    return true;
}

size_t table_append_row(Table *t) {
    if (t->n_rows == t->row_cap && !table_grow(t)) return SIZE_MAX;

    size_t row = t->n_rows++;
    for (size_t i = 0; i < t->n_cols; i++) {
        size_t width = field_width(t->types[i]);
        memset((char *)t->columns[i] + row * width, 0, width);
        null_set(t, i, row);
    }
    dead_clear(t, row);
    return row;
}

void table_delete_row(Table *t, size_t row) {
    if (dead_test(t, row)) return;
    dead_set(t, row);
    t->n_dead++;
}

/* Undoes table_delete_row - used by txn.c to roll back a DELETE, not part
 * of any SQL-visible operation. */
void table_undelete_row(Table *t, size_t row) {
    if (!dead_test(t, row)) return;
    dead_clear(t, row);
    t->n_dead--;
}

bool table_row_is_dead(const Table *t, size_t row) {
    return dead_test(t, row);
}

void table_set_null(Table *t, size_t row, size_t col) {
    null_set(t, col, row);
}

bool table_is_null(const Table *t, size_t row, size_t col) {
    return null_test(t, col, row);
}

void table_set_int(Table *t, size_t row, size_t col, int64_t v) {
    ((int64_t *)t->columns[col])[row] = v;
    null_clear(t, col, row);
    index_row_if_primary_key(t, row, col);
}

void table_set_double(Table *t, size_t row, size_t col, double v) {
    ((double *)t->columns[col])[row] = v;
    null_clear(t, col, row);
    index_row_if_primary_key(t, row, col);
}

void table_set_bool(Table *t, size_t row, size_t col, bool v) {
    ((bool *)t->columns[col])[row] = v;
    null_clear(t, col, row);
    index_row_if_primary_key(t, row, col);
}

void table_set_text(Table *t, size_t row, size_t col, const char *s, size_t len) {
    if (len > UINT32_MAX) {
        arena_fail(&t->heap, ARENA_FAIL_OOM);
        return;
    }

    size_t offset = buf_append(&t->heap, &t->heap_data, &t->heap_len, &t->heap_cap, s, len);
    if (offset == SIZE_MAX) return;
    if (offset > UINT32_MAX) {
        arena_fail(&t->heap, ARENA_FAIL_OOM);
        return;
    }

    ((StringRef *)t->columns[col])[row] = (StringRef){ (uint32_t)offset, (uint32_t)len };
    null_clear(t, col, row);
    index_row_if_primary_key(t, row, col);
}

int64_t table_get_int(const Table *t, size_t row, size_t col) {
    return ((int64_t *)t->columns[col])[row];
}

double table_get_double(const Table *t, size_t row, size_t col) {
    return ((double *)t->columns[col])[row];
}

bool table_get_bool(const Table *t, size_t row, size_t col) {
    return ((bool *)t->columns[col])[row];
}

const char *table_get_text(const Table *t, size_t row, size_t col, size_t *out_len) {
    StringRef ref = ((StringRef *)t->columns[col])[row];
    *out_len = ref.len;
    return t->heap_data ? t->heap_data + ref.offset : "";
}

StringRef table_get_text_ref(const Table *t, size_t row, size_t col) {
    return ((StringRef *)t->columns[col])[row];
}

void table_set_text_ref(Table *t, size_t row, size_t col, StringRef ref) {
    ((StringRef *)t->columns[col])[row] = ref;
    null_clear(t, col, row);
    index_row_if_primary_key(t, row, col);
}

void table_set_foreign_key(Table *t, size_t col, const char *ref_table, const char *ref_column,
                            FkAction on_delete, FkAction on_update) {
    char *ref_t = arena_strdup(&t->arena, ref_table);
    char *ref_c = arena_strdup(&t->arena, ref_column);
    if (!ref_t || !ref_c) return;

    t->fk_table[col]     = ref_t;
    t->fk_column[col]    = ref_c;
    t->fk_on_delete[col] = on_delete;
    t->fk_on_update[col] = on_update;
}

bool table_get_foreign_key(const Table *t, size_t col, const char **out_table, const char **out_column) {
    if (!t->fk_table[col]) return false;
    *out_table  = t->fk_table[col];
    *out_column = t->fk_column[col];
    return true;
}

FkAction table_get_on_delete(const Table *t, size_t col) {
    return t->fk_on_delete[col];
}

FkAction table_get_on_update(const Table *t, size_t col) {
    return t->fk_on_update[col];
}

void table_set_primary_key(Table *t, size_t col) {
    for (size_t i = 0; i < t->n_cols; i++)
        if (i != col && t->is_pk[i]) {
            table_misuse(t, "table already has a primary key column");
            return;
        }
    t->is_pk[col] = true;
    t->pk_col = (int)col;

    for (size_t r = 0; r < t->n_rows; r++) {
        if (dead_test(t, r) || table_is_null(t, r, col)) continue;
        row_index_insert(&t->pk_index, &t->arena, cell_hash(t, r, col), r);
    }
}

bool table_is_primary_key(const Table *t, size_t col) {
    return t->is_pk[col];
}

static bool cell_equal(const Table *ta, size_t row_a, size_t col_a,
                        const Table *tb, size_t row_b, size_t col_b) {
    switch (ta->types[col_a]) {
        case FT_INT:    return table_get_int(ta, row_a, col_a) == table_get_int(tb, row_b, col_b);
        case FT_DOUBLE: return table_get_double(ta, row_a, col_a) == table_get_double(tb, row_b, col_b);
        case FT_BOOL:   return table_get_bool(ta, row_a, col_a) == table_get_bool(tb, row_b, col_b);
        case FT_TEXT: {
            size_t len_a, len_b;
            const char *sa = table_get_text(ta, row_a, col_a, &len_a);
            const char *sb = table_get_text(tb, row_b, col_b, &len_b);
            return len_a == len_b && memcmp(sa, sb, len_a) == 0;
        }
        default: return false;
    }
}

static uint64_t cell_hash(const Table *t, size_t row, size_t col) {
    switch (t->types[col]) {
        case FT_INT: {
            uint64_t x = (uint64_t)table_get_int(t, row, col);
            x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27; x *= 0x94d049bb133111ebULL;
            return x ^ (x >> 31);
        }
        case FT_DOUBLE: {
            double d = table_get_double(t, row, col);
            if (d == 0.0) d = 0.0;
            uint64_t bits;
            memcpy(&bits, &d, sizeof bits);
            bits ^= bits >> 30; bits *= 0xbf58476d1ce4e5b9ULL;
            bits ^= bits >> 27; bits *= 0x94d049bb133111ebULL;
            return bits ^ (bits >> 31);
        }
        case FT_BOOL:
            return table_get_bool(t, row, col) ? 0x9e3779b97f4a7c15ULL : 0xd6e8feb86659fd93ULL;
        case FT_TEXT: {
            size_t len;
            const char *s = table_get_text(t, row, col, &len);
            uint64_t h = 14695981039346656037ULL;
            for (size_t i = 0; i < len; i++) {
                h ^= (unsigned char)s[i];
                h *= 1099511628211ULL;
            }
            return h;
        }
        default: return 0;
    }
}

static void index_row_if_primary_key(Table *t, size_t row, size_t col) {
    if (t->pk_col != (int)col) return;
    row_index_insert(&t->pk_index, &t->arena, cell_hash(t, row, col), row);
}

typedef struct {
    const Table *t;
    size_t       row, col;
    bool         found;
} DupCtx;

static bool dup_visit(void *ctx_, size_t r) {
    DupCtx *ctx = ctx_;
    if (r == ctx->row || dead_test(ctx->t, r) || table_is_null(ctx->t, r, ctx->col)) return true;
    if (cell_equal(ctx->t, ctx->row, ctx->col, ctx->t, r, ctx->col)) { ctx->found = true; return false; }
    return true;
}

bool table_column_has_duplicate(const Table *t, size_t col, size_t row) {
    if (table_is_null(t, row, col)) return false;

    if (t->pk_col == (int)col && row_index_usable(&t->pk_index)) {
        DupCtx ctx = { t, row, col, false };
        row_index_find(&t->pk_index, cell_hash(t, row, col), &ctx, dup_visit);
        return ctx.found;
    }

    for (size_t r = 0; r < t->n_rows; r++) {
        if (r == row || dead_test(t, r) || table_is_null(t, r, col)) continue;
        if (cell_equal(t, row, col, t, r, col)) return true;
    }
    return false;
}

typedef struct {
    const Table *ref_t;
    size_t       ref_col;
    const Table *t;
    size_t       row, col;
    bool         found;
} MatchCtx;

static bool match_visit(void *ctx_, size_t r) {
    MatchCtx *ctx = ctx_;
    if (dead_test(ctx->ref_t, r) || table_is_null(ctx->ref_t, r, ctx->ref_col)) return true;
    if (cell_equal(ctx->ref_t, r, ctx->ref_col, ctx->t, ctx->row, ctx->col)) { ctx->found = true; return false; }
    return true;
}

bool table_has_matching_value(const Table *ref_t, size_t ref_col, const Table *t, size_t row, size_t col) {
    if (ref_t->types[ref_col] != t->types[col]) return false;

    if (ref_t->pk_col == (int)ref_col && row_index_usable(&ref_t->pk_index)) {
        MatchCtx ctx = { ref_t, ref_col, t, row, col, false };
        row_index_find(&ref_t->pk_index, cell_hash(t, row, col), &ctx, match_visit);
        return ctx.found;
    }

    for (size_t r = 0; r < ref_t->n_rows; r++) {
        if (dead_test(ref_t, r) || table_is_null(ref_t, r, ref_col)) continue;
        if (cell_equal(ref_t, r, ref_col, t, row, col)) return true;
    }
    return false;
}

void table_find_matching_rows(const Table *t, size_t col, const Table *val_t, size_t val_row, size_t val_col,
                               void *ctx, bool (*visit)(void *ctx, size_t row)) {
    if (t->types[col] != val_t->types[val_col]) return;
    if (table_is_null(val_t, val_row, val_col)) return;

    for (size_t r = 0; r < t->n_rows; r++) {
        if (dead_test(t, r) || table_is_null(t, r, col)) continue;
        if (cell_equal(t, r, col, val_t, val_row, val_col))
            if (!visit(ctx, r)) return;
    }
}

void table_compact(Table *t) {
    size_t dest = 0;
    for (size_t row = 0; row < t->n_rows; row++) {
        if (dead_test(t, row)) continue;

        if (dest != row) {
            for (size_t i = 0; i < t->n_cols; i++) {
                size_t width = field_width(t->types[i]);
                memcpy((char *)t->columns[i] + dest * width,
                       (char *)t->columns[i] + row * width,
                       width);
                if (null_test(t, i, row)) null_set(t, i, dest);
                else                      null_clear(t, i, dest);
            }
        }
        dest++;
    }

    t->n_rows = dest;
    t->n_dead = 0;
    if (t->dead) memset(t->dead, 0, (t->row_cap + 7) / 8);

    if (t->pk_col >= 0) {
        row_index_clear(&t->pk_index);
        for (size_t row = 0; row < t->n_rows; row++) {
            if (table_is_null(t, row, (size_t)t->pk_col)) continue;
            row_index_insert(&t->pk_index, &t->arena, cell_hash(t, row, (size_t)t->pk_col), row);
        }
    }
}

void table_compact_heap(Table *t) {
    Arena new_heap;
    arena_init(&new_heap);

    size_t total = 0;
    for (size_t col = 0; col < t->n_cols; col++) {
        if (t->types[col] != FT_TEXT) continue;
        StringRef *refs = t->columns[col];
        for (size_t row = 0; row < t->n_rows; row++) {
            if (dead_test(t, row)) continue;
            total = arena_checked_add(&new_heap, total, refs[row].len);
        }
    }

    char *new_data = arena_alloc(&new_heap, total, _Alignof(char));
    if (!new_data) {
        arena_term(&new_heap);
        arena_fail(&t->heap, ARENA_FAIL_OOM);
        return;
    }

    size_t new_len = 0;
    for (size_t col = 0; col < t->n_cols; col++) {
        if (t->types[col] != FT_TEXT) continue;

        StringRef *refs = t->columns[col];
        for (size_t row = 0; row < t->n_rows; row++) {
            if (dead_test(t, row)) continue;

            StringRef old = refs[row];
            const char *src = t->heap_data ? t->heap_data + old.offset : "";
            if (old.len) memcpy(new_data + new_len, src, old.len);
            refs[row] = (StringRef){ (uint32_t)new_len, old.len };
            new_len += old.len;
        }
    }

    ArenaFailure prior = arena_failure(&t->heap);
    arena_term(&t->heap);
    t->heap      = new_heap;
    arena_fail(&t->heap, prior);
    t->heap_data = new_data;
    t->heap_len  = new_len;
    t->heap_cap  = new_len;
}
