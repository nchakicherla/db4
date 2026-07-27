#include "load.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arena.h"
#include "budget.h"
#include "csv.h"
#include "field.h"
#include "lock.h"
#include "schema.h"
#include "value.h"
#include "wal.h"

#define LOAD_ARENA_RESERVE ((size_t)8 * ARENA_BLOCK_SIZE)

static void usage_load(void) {
    printf("usage: .load <name> \"<path>\" [{\"col\":\"type\",...} | \"<schema.json>\"]\n");
}

/* Returns false (leaving the cell NULL, its table_append_row default)
 * without setting anything when field doesn't validate against type -
 * load_row counts these so load_csv can warn instead of reporting a clean
 * load that quietly dropped data (see load_row/load_csv). */
static bool set_cell(Table *t, size_t row, size_t col, const char *field) {
    FieldType type = t->types[col];
    if (!field_validate(field, type)) return false;

    switch (type) {
        case FT_INT:    table_set_int(t, row, col, strtoll(field, NULL, 10)); break;
        case FT_DOUBLE: table_set_double(t, row, col, strtod(field, NULL)); break;
        case FT_BOOL:   table_set_bool(t, row, col, tolower((unsigned char)field[0]) == 't'); break;
        case FT_TEXT:   table_set_text(t, row, col, field, strlen(field)); break;
        default: break;
    }
    return true;
}

static bool check_primary_key(const Table *t, size_t row, size_t col) {
    if (!table_is_primary_key(t, col)) return true;

    if (table_is_null(t, row, col)) {
        printf("row %zu: primary key column \"%s\" cannot be NULL\n", row, t->names[col]);
        return false;
    }
    if (table_column_has_duplicate(t, col, row)) {
        printf("row %zu: duplicate value in primary key column \"%s\"\n", row, t->names[col]);
        return false;
    }
    return true;
}

static bool check_row_constraints(Table *t, size_t row, Table *const *fk_ref_table, const int *fk_ref_col) {
    for (size_t col = 0; col < t->n_cols; col++) {
        if (!check_primary_key(t, row, col)) return false;

        if (fk_ref_table[col]) {
            if (table_is_null(t, row, col)) continue;

            if (!table_has_matching_value(fk_ref_table[col], (size_t)fk_ref_col[col], t, row, col)) {
                const char *ref_table_name, *ref_column_name;
                table_get_foreign_key(t, col, &ref_table_name, &ref_column_name);
                printf("row %zu: value in foreign key column \"%s\" not found in %s.%s\n",
                       row, t->names[col], ref_table_name, ref_column_name);
                return false;
            }
        }
    }
    return true;
}

static size_t load_row(Table *t, const CsvRow *row, size_t *out_coerced) {
    size_t n = row->count < t->n_cols ? row->count : t->n_cols;
    size_t r = table_append_row(t);
    if (r == SIZE_MAX) return SIZE_MAX;

    for (size_t i = 0; i < n; i++) {
        if (row->fields[i][0] == '\0' && !row->quoted[i]) continue;
        if (!set_cell(t, r, i, row->fields[i])) (*out_coerced)++;
    }
    return table_failed(t) ? SIZE_MAX : r;
}

static bool fk_chain_reaches(const Catalog *catalog, const Table *ref_table, const char *target, size_t depth_left) {
    if (depth_left == 0) return true;

    for (size_t col = 0; col < ref_table->n_cols; col++) {
        const char *fk_table, *fk_column;
        if (!table_get_foreign_key(ref_table, col, &fk_table, &fk_column)) continue;
        if (strcmp(fk_table, target) == 0) return true;

        int idx = catalog_find(catalog, fk_table);
        if (idx < 0) continue;
        if (fk_chain_reaches(catalog, &catalog->tables[idx].table, target, depth_left - 1)) return true;
    }
    return false;
}

static bool read_quoted(const char **pp, char *buf, size_t buf_len) {
    const char *p = *pp + 1;
    size_t n = 0;
    for (;;) {
        if (*p == '\0') return false;
        if (*p == '"') {
            if (p[1] == '"') {
                if (n + 1 >= buf_len) return false;
                buf[n++] = '"';
                p += 2;
                continue;
            }
            break;
        }
        if (n + 1 >= buf_len) return false;
        buf[n++] = *p++;
    }
    buf[n] = '\0';
    p++;
    while (*p == ' ') p++;
    *pp = p;
    return true;
}

static bool read_file(const char *path, size_t reserve, char **out_data, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("could not open %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        printf("could not read %s\n", path);
        fclose(f);
        return false;
    }

    if (budget_would_exceed((size_t)size + 1 + reserve)) {
        size_t limit = budget_limit(), used = budget_used();
        double freemb = (limit > used ? limit - used : 0) / (1024.0 * 1024.0);
        printf("%s is %.2f MB, which (with working memory) exceeds the memory budget "
               "(%.2f MB free of a %.2f MB cap)\n",
               path, ((size_t)size) / (1024.0 * 1024.0), freemb, limit / (1024.0 * 1024.0));
        fclose(f);
        return false;
    }

    fseek(f, 0, SEEK_SET);

    char *data = malloc((size_t)size + 1);
    if (!data) {
        printf("out of memory reading %s\n", path);
        fclose(f);
        return false;
    }
    size_t n_read = fread(data, 1, (size_t)size, f);
    data[n_read] = '\0';
    fclose(f);

    *out_data = data;
    *out_len  = n_read;
    return true;
}

bool load_csv(Catalog *catalog, bool txn_active, const char *args) {
    char name[64];
    int  name_consumed = 0;
    if (sscanf(args, "%63s%n", name, &name_consumed) != 1) {
        usage_load();
        return false;
    }
    args += name_consumed;
    while (*args == ' ') args++;

    if (txn_active && catalog_find(catalog, name) >= 0) {
        printf("cannot .load over table \"%s\" while a transaction is open "
               "(COMMIT or ROLLBACK first)\n", name);
        return false;
    }

    char path[4096];
    if (*args != '"' || !read_quoted(&args, path, sizeof path)) {
        usage_load();
        return false;
    }

    const char *inline_schema = NULL;
    char       *schema_file_data = NULL;
    size_t      schema_file_len  = 0;
    char        schema_path[4096];

    if (*args == '{') {
        inline_schema = args;
    } else if (*args == '"') {
        if (!read_quoted(&args, schema_path, sizeof schema_path)) {
            usage_load();
            return false;
        }
        if (!read_file(schema_path, 0, &schema_file_data, &schema_file_len)) return false;
    } else if (*args != '\0') {
        usage_load();
        return false;
    }

    char     *data         = NULL;
    size_t    n_read       = 0;
    size_t    data_charged = 0;

    /* A brief shared (reader) lock around the read + WAL replay below -
     * see lock.h. Concurrent readers can hold this together; it excludes
     * only a writer mid-checkpoint (M7). Unlike a commit or a checkpoint
     * (both writes - see interp.c's commit_wal_for_table and main.c's
     * cmd_checkpoint, which refuse outright without their lock), failing
     * to get this one only risks *this* read racing a concurrent writer's
     * WAL append or checkpoint - reading a torn snapshot, not corrupting
     * anything on disk - so a load proceeds anyway rather than refusing a
     * table entirely over what's still just an advisory, best-effort
     * lock. */
    Db4Lock lock;
    lock.fd = -1;
    bool have_lock = db4_lock_open(&lock, path) && db4_lock_shared(&lock);
    if (!have_lock)
        printf("warning: could not acquire a shared lock on %s; reading without one - "
               "a concurrent writer's commit or checkpoint could be observed mid-write\n", path);

    if (!read_file(path, LOAD_ARENA_RESERVE, &data, &n_read)) {
        if (have_lock) db4_lock_release(&lock);
        db4_lock_close(&lock);
        free(schema_file_data);
        return false;
    }
    budget_charge(n_read);
    data_charged = n_read;

    Arena          schema_arena;
    SchemaOverride overrides = {0};
    arena_init(&schema_arena);

    Arena     csv_arena;
    CsvReader reader;
    bool      csv_arena_live = false;
    bool      reader_live    = false;
    Table     new_table;
    bool      new_table_live = false;

    const char *schema_json = inline_schema ? inline_schema : schema_file_data;
    if (schema_json) {
        char err[128];
        bool ok = schema_parse(schema_json, &schema_arena, &overrides, err, sizeof err);
        free(schema_file_data);
        if (!ok) {
            printf("invalid schema: %s\n", err);
            goto fail;
        }
    }

    arena_init(&csv_arena);
    csv_arena_live = true;
    bool reader_ok = csv_reader_init(&reader, data, n_read);
    reader_live = true;
    if (!reader_ok) goto oom;

    CsvRow header;
    CsvStatus header_status = csv_reader_next_row(&reader, &csv_arena, &header);
    if (header_status == CSV_NOMEM) goto oom;
    if (header_status != CSV_OK) {
        printf("could not read header from %s\n", path);
        goto fail;
    }

    for (size_t i = 0; i < header.count; i++) {
        if (strlen(header.fields[i]) >= MAX_COL_NAME_LEN) {
            printf("column name \"%s\" is too long (max %d characters)\n",
                   header.fields[i], MAX_COL_NAME_LEN - 1);
            goto fail;
        }
        for (size_t j = i + 1; j < header.count; j++) {
            if (strcmp(header.fields[i], header.fields[j]) == 0) {
                printf("duplicate column name \"%s\" in header of %s\n", header.fields[i], path);
                goto fail;
            }
        }
    }

    for (size_t i = 0; i < overrides.count; i++) {
        bool found = false;
        for (size_t j = 0; j < header.count; j++) {
            if (strcmp(overrides.names[i], header.fields[j]) == 0) { found = true; break; }
        }
        if (!found) {
            printf("unknown column \"%s\" in schema override\n", overrides.names[i]);
            goto fail;
        }
    }

    int pk_override_idx = -1;
    for (size_t i = 0; i < overrides.count; i++) {
        if (overrides.primary[i]) {
            if (pk_override_idx >= 0) {
                printf("only one primary key column is supported (both \"%s\" and \"%s\" are marked primary)\n",
                       overrides.names[pk_override_idx], overrides.names[i]);
                goto fail;
            }
            pk_override_idx = (int)i;
        }

        if (!overrides.fk_tables[i]) continue;

        if (strcmp(overrides.fk_tables[i], name) == 0) {
            printf("column \"%s\" cannot reference \"%s\" (self-referencing foreign keys aren't supported)\n",
                   overrides.names[i], name);
            goto fail;
        }

        int ref_table_idx = catalog_find(catalog, overrides.fk_tables[i]);
        if (ref_table_idx < 0) {
            printf("unknown table \"%s\" referenced by column \"%s\"\n", overrides.fk_tables[i], overrides.names[i]);
            goto fail;
        }
        Table *ref_table = &catalog->tables[ref_table_idx].table;

        if (fk_chain_reaches(catalog, ref_table, name, catalog->count)) {
            printf("column \"%s\" cannot reference \"%s\" (it already references \"%s\", directly or "
                   "indirectly - this would create a cycle)\n",
                   overrides.names[i], overrides.fk_tables[i], name);
            goto fail;
        }

        int ref_col = table_find_column(ref_table, overrides.fk_columns[i]);
        if (ref_col < 0) {
            printf("no such column \"%s\" in table \"%s\" referenced by column \"%s\"\n",
                   overrides.fk_columns[i], overrides.fk_tables[i], overrides.names[i]);
            goto fail;
        }

        if (!table_is_primary_key(ref_table, (size_t)ref_col)) {
            printf("column \"%s.%s\" is not a primary key\n", overrides.fk_tables[i], overrides.fk_columns[i]);
            goto fail;
        }

        if (ref_table->types[ref_col] != overrides.types[i]) {
            printf("column \"%s\" is type %s but referenced column \"%s.%s\" is type %s\n",
                   overrides.names[i], field_type_label(overrides.types[i]),
                   overrides.fk_tables[i], overrides.fk_columns[i], field_type_label(ref_table->types[ref_col]));
            goto fail;
        }
    }

    CsvRow first = {0};
    CsvStatus status;
    for (;;) {
        status = csv_reader_next_row(&reader, &csv_arena, &first);
        if (status == CSV_EOF) {
            printf("%s has no data rows\n", path);
            goto fail;
        }
        if (status == CSV_NOMEM) goto oom;
        if (status == CSV_ERR) {
            /* Every CSV_ERR (a bad quote, an oversized field, or - unlike
             * those - a row whose column count doesn't match the header,
             * which can still have first.count > 0) names a row that
             * isn't usable data - always move on to the next one rather
             * than falling through to load a malformed row just because
             * this particular error happened to leave some fields parsed. */
            printf("line %zu: %s\n", csv_reader_error_line(&reader), csv_reader_error(&reader));
            continue;
        }
        break;
    }

    FieldType *types = arena_alloc(&csv_arena, header.count * sizeof(FieldType), _Alignof(FieldType));
    if (!types) goto oom;
    for (size_t i = 0; i < header.count; i++) {
        if (schema_lookup(&overrides, header.fields[i], &types[i])) continue;
        types[i] = i < first.count ? field_infer_type(first.fields[i]) : FT_TEXT;
    }

    table_init(&new_table, (const char **)header.fields, types, header.count);
    new_table_live = true;
    if (table_failed(&new_table)) goto oom;

    Table **fk_ref_table = arena_zalloc(&csv_arena, header.count * sizeof(Table *), _Alignof(Table *));
    int    *fk_ref_col   = arena_alloc(&csv_arena, header.count * sizeof(int), _Alignof(int));
    if (!fk_ref_table || !fk_ref_col) goto oom;
    for (size_t i = 0; i < overrides.count; i++) {
        if (!overrides.fk_tables[i]) continue;
        int col = table_find_column(&new_table, overrides.names[i]);
        table_set_foreign_key(&new_table, (size_t)col, overrides.fk_tables[i], overrides.fk_columns[i],
                              overrides.fk_on_delete[i], overrides.fk_on_update[i]);

        int ref_table_idx = catalog_find(catalog, overrides.fk_tables[i]);
        fk_ref_table[col] = &catalog->tables[ref_table_idx].table;
        fk_ref_col[col]   = table_find_column(fk_ref_table[col], overrides.fk_columns[i]);
    }
    if (pk_override_idx >= 0) {
        int col = table_find_column(&new_table, overrides.names[pk_override_idx]);
        table_set_primary_key(&new_table, (size_t)col);
    }
    if (table_failed(&new_table)) goto oom;
    arena_term(&schema_arena);

    size_t n_coerced = 0;
    size_t r = load_row(&new_table, &first, &n_coerced);
    if (r == SIZE_MAX) goto oom;
    if (!check_row_constraints(&new_table, r, fk_ref_table, fk_ref_col)) goto fail;
    size_t n_loaded = 1;

    for (;;) {
        CsvRow row = {0};
        status = csv_reader_next_row(&reader, &csv_arena, &row);
        if (status == CSV_EOF) break;
        if (status == CSV_NOMEM) goto oom;
        if (status == CSV_ERR) {
            /* See the identical comment on the first-row loop above - a
             * wrong-column-count row can have row.count > 0 despite being
             * unusable, so this always skips rather than falling through
             * to load_row (which would otherwise silently truncate/pad it
             * into the table instead of the row being skipped as reported). */
            printf("line %zu: %s\n", csv_reader_error_line(&reader), csv_reader_error(&reader));
            continue;
        }
        size_t rr = load_row(&new_table, &row, &n_coerced);
        if (rr == SIZE_MAX) goto oom;
        if (!check_row_constraints(&new_table, rr, fk_ref_table, fk_ref_col)) goto fail;
        n_loaded++;
    }

    {
        char wal_path[4160];
        int  wn = snprintf(wal_path, sizeof wal_path, "%s.wal", path);
        if (wn > 0 && (size_t)wn < sizeof wal_path && !wal_replay(wal_path, &new_table))
            printf("warning: WAL replay for %s failed; continuing with base data only\n", wal_path);
    }

    /* Remembers the checkpoint generation this table was loaded at, so a
     * later commit from this same process can tell whether some other
     * process has since .checkpoint'd (and so row-renumbered) this table
     * out from under it - see wal_checkpoint and interp.c's
     * commit_wal_for_table. */
    uint32_t gen;
    if (wal_read_generation(path, &gen)) new_table.wal_generation = gen;

    Table *slot = catalog_put(catalog, name, path);
    if (!slot) goto oom;
    *slot = new_table;

    printf("loaded %zu row%s into %zu column%s from %s as %s\n",
           n_loaded, n_loaded == 1 ? "" : "s",
           header.count, header.count == 1 ? "" : "s", path, name);
    if (n_coerced > 0)
        printf("warning: %zu value%s did not match %s column's type and %s left NULL\n",
               n_coerced, n_coerced == 1 ? "" : "s",
               n_coerced == 1 ? "its" : "their", n_coerced == 1 ? "was" : "were");

    csv_reader_free(&reader);
    arena_term(&csv_arena);
    budget_uncharge(data_charged);
    free(data);
    if (have_lock) db4_lock_release(&lock);
    db4_lock_close(&lock);
    return true;

oom:
    {
        char detail[96];
        budget_describe(detail, sizeof detail);
        printf("out of memory loading %s (%s)\n", path, detail);
    }
    goto fail;

fail:
    arena_term(&schema_arena);
    if (new_table_live) table_term(&new_table);
    if (reader_live) csv_reader_free(&reader);
    if (csv_arena_live) arena_term(&csv_arena);
    budget_uncharge(data_charged);
    free(data);
    if (have_lock) db4_lock_release(&lock);
    db4_lock_close(&lock);
    return false;
}

static bool csv_field_needs_quotes(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (s[i] == ',' || s[i] == '"' || s[i] == '\n' || s[i] == '\r') return true;
    return false;
}

static bool write_csv_field(FILE *f, const char *s, size_t len) {
    if (!csv_field_needs_quotes(s, len)) return fwrite(s, 1, len, f) == len;

    if (fputc('"', f) == EOF) return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"' && fputc('"', f) == EOF) return false;
        if (fputc(s[i], f) == EOF) return false;
    }
    return fputc('"', f) != EOF;
}

static bool dump_cell(FILE *f, const Table *t, size_t row, size_t col) {
    if (table_is_null(t, row, col)) return true;

    char buf[64];
    switch (t->types[col]) {
        case FT_INT:
            snprintf(buf, sizeof buf, "%lld", (long long)table_get_int(t, row, col));
            return write_csv_field(f, buf, strlen(buf));
        case FT_DOUBLE:
            format_double(buf, sizeof buf, table_get_double(t, row, col));
            return write_csv_field(f, buf, strlen(buf));
        case FT_BOOL:
            return write_csv_field(f, table_get_bool(t, row, col) ? "true" : "false",
                                    table_get_bool(t, row, col) ? 4 : 5);
        case FT_TEXT: {
            size_t len;
            const char *s = table_get_text(t, row, col, &len);
            return write_csv_field(f, s, len);
        }
        default: return true;
    }
}

static bool write_csv_body(FILE *f, const Table *t, const char *path) {
    for (size_t col = 0; col < t->n_cols; col++) {
        if (col) fputc(',', f);
        write_csv_field(f, t->names[col], strlen(t->names[col]));
    }
    fputc('\n', f);

    for (size_t row = 0; row < t->n_rows; row++) {
        if (table_row_is_dead(t, row)) continue;
        for (size_t col = 0; col < t->n_cols; col++) {
            if (col) fputc(',', f);
            if (!dump_cell(f, t, row, col)) {
                printf("write error while dumping to %s\n", path);
                return false;
            }
        }
        fputc('\n', f);
    }
    return true;
}

/* Writes to a temp file beside path, fsyncs it, then rename()s over path -
 * the write is atomic from any outside observer's point of view (readers
 * see either the whole old file or the whole new one, never a partial
 * write), which is what M5's "nothing durable until commit" needs and
 * what .dump gets for free by sharing this. */
bool dump_csv(const Table *t, const char *path) {
    char tmp_path[4096];
    int n = snprintf(tmp_path, sizeof tmp_path, "%s.tmp-%d", path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp_path) {
        printf("path too long: %s\n", path);
        return false;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        printf("could not open %s for writing\n", tmp_path);
        return false;
    }

    if (!write_csv_body(f, t, tmp_path)) {
        fclose(f);
        remove(tmp_path);
        return false;
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        printf("write error while dumping to %s\n", tmp_path);
        fclose(f);
        remove(tmp_path);
        return false;
    }
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        printf("could not replace %s with %s\n", path, tmp_path);
        remove(tmp_path);
        return false;
    }
    return true;
}

void print_tables(const Catalog *catalog) {
    if (catalog->count == 0) {
        printf("no tables loaded\n");
        return;
    }

    for (size_t i = 0; i < catalog->count; i++) {
        const Table *t = &catalog->tables[i].table;
        printf("%s (%zu cols, %zu rows)\n",
               catalog->tables[i].name, t->n_cols, t->n_rows - t->n_dead);
    }
}

static const char *fk_action_label(FkAction action) {
    switch (action) {
        case FK_ACTION_CASCADE:  return "cascade";
        case FK_ACTION_SET_NULL: return "set_null";
        case FK_ACTION_RESTRICT: return "restrict";
        default:                 return NULL;
    }
}

void print_schema(const Table *t) {
    for (size_t i = 0; i < t->n_cols; i++) {
        printf("%s%s:%s", t->names[i], table_is_primary_key(t, i) ? "*" : "", field_type_label(t->types[i]));
        const char *ref_table, *ref_column;
        if (table_get_foreign_key(t, i, &ref_table, &ref_column)) {
            printf("->%s.%s", ref_table, ref_column);
            const char *on_delete = fk_action_label(table_get_on_delete(t, i));
            const char *on_update = fk_action_label(table_get_on_update(t, i));
            if (on_delete) printf("[on_delete=%s]", on_delete);
            if (on_update) printf("[on_update=%s]", on_update);
        }
        printf(" ");
    }
    printf("\n");
}
