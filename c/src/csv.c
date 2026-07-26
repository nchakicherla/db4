#include "csv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    FIELD_OK,
    FIELD_UNTERMINATED,
    FIELD_BAD_TRAILING,
    FIELD_TOO_LARGE,
    FIELD_NOMEM,
} FieldStatus;

static CsvStatus set_err(CsvReader *r, size_t line, const char *msg) {
    snprintf(r->err, sizeof r->err, "%s", msg);
    r->err_line = line;
    return CSV_ERR;
}

static void resync_to_next_row(CsvReader *r, bool in_quotes) {
    while (r->pos < r->len) {
        char c = r->data[r->pos];

        if (c == '"') {
            in_quotes = !in_quotes;
        } else if (!in_quotes && (c == '\r' || c == '\n')) {
            r->pos++;
            if (c == '\r' && r->pos < r->len && r->data[r->pos] == '\n') r->pos++;
            r->line++;
            return;
        } else if (c == '\n') {
            r->line++;
        }

        r->pos++;
    }
}

static bool push_field(CsvReader *r, Arena *a, size_t idx, const char *data, size_t len, bool quoted) {
    if (idx >= r->field_cap) {
        size_t new_cap = r->field_cap ? r->field_cap * 2 : 8;
        if (new_cap > SIZE_MAX / sizeof(char *)) return false;

        char **grown_fields = realloc(r->field_buf, new_cap * sizeof(char *));
        if (!grown_fields) return false;
        r->field_buf = grown_fields;

        bool *grown_quoted = realloc(r->quoted_buf, new_cap * sizeof(bool));
        if (!grown_quoted) return false;
        r->quoted_buf = grown_quoted;

        r->field_cap = new_cap;
    }

    char *field = arena_alloc(a, len + 1, _Alignof(char));
    if (!field) return false;
    memcpy(field, data, len);
    field[len] = '\0';

    r->field_buf[idx]  = field;
    r->quoted_buf[idx] = quoted;
    return true;
}

static FieldStatus read_field(CsvReader *r, bool *out_quoted) {
    if (!r->scratch) return FIELD_NOMEM;
    sdsclear(r->scratch);

    if (r->pos < r->len && r->data[r->pos] == '"') {
        *out_quoted = true;
        r->pos++;

        for (;;) {
            if (r->pos >= r->len) return FIELD_UNTERMINATED;

            char c = r->data[r->pos];
            if (c == '"') {
                if (r->pos + 1 < r->len && r->data[r->pos + 1] == '"') {
                    sds grown = sdscatlen(r->scratch, "\"", 1);
                    if (!grown) return FIELD_NOMEM;
                    r->scratch = grown;
                    r->pos += 2;
                    if (sdslen(r->scratch) > CSV_MAX_FIELD_LEN) {
                        resync_to_next_row(r, true);
                        return FIELD_TOO_LARGE;
                    }
                    continue;
                }
                r->pos++;
                break;
            }
            if (c == '\n') r->line++;
            sds grown = sdscatlen(r->scratch, &c, 1);
            if (!grown) return FIELD_NOMEM;
            r->scratch = grown;
            r->pos++;
            if (sdslen(r->scratch) > CSV_MAX_FIELD_LEN) {
                resync_to_next_row(r, true);
                return FIELD_TOO_LARGE;
            }
        }

        if (r->pos < r->len) {
            char c = r->data[r->pos];
            if (c != ',' && c != '\r' && c != '\n') {
                resync_to_next_row(r, false);
                return FIELD_BAD_TRAILING;
            }
        }
        return FIELD_OK;
    }

    *out_quoted = false;
    while (r->pos < r->len) {
        char c = r->data[r->pos];
        if (c == ',' || c == '\r' || c == '\n') break;
        sds grown = sdscatlen(r->scratch, &c, 1);
        if (!grown) return FIELD_NOMEM;
        r->scratch = grown;
        r->pos++;
        if (sdslen(r->scratch) > CSV_MAX_FIELD_LEN) {
            resync_to_next_row(r, false);
            return FIELD_TOO_LARGE;
        }
    }
    return FIELD_OK;
}

static int has_utf8_bom(const char *data, size_t len) {
    return len >= 3 &&
           (unsigned char)data[0] == 0xEF &&
           (unsigned char)data[1] == 0xBB &&
           (unsigned char)data[2] == 0xBF;
}

bool csv_reader_init(CsvReader *r, const char *data, size_t len) {
    r->data       = data;
    r->len        = len;
    r->pos        = has_utf8_bom(data, len) ? 3 : 0;
    r->line       = 1;
    r->n_cols     = 0;
    r->scratch    = sdsempty();
    r->field_buf  = NULL;
    r->quoted_buf = NULL;
    r->field_cap  = 0;
    r->err[0]     = '\0';
    r->err_line   = 0;

    return r->scratch != NULL;
}

void csv_reader_free(CsvReader *r) {
    sdsfree(r->scratch);
    free(r->field_buf);
    free(r->quoted_buf);
    r->scratch    = NULL;
    r->field_buf  = NULL;
    r->quoted_buf = NULL;
}

CsvStatus csv_reader_next_row(CsvReader *r, Arena *a, CsvRow *out) {
    if (r->pos >= r->len) return CSV_EOF;

    size_t start_line = r->line;
    size_t count = 0;

    for (;;) {
        bool quoted = false;
        FieldStatus fs = read_field(r, &quoted);
        if (fs == FIELD_UNTERMINATED)
            return set_err(r, start_line, "unterminated quoted field");
        if (fs == FIELD_BAD_TRAILING)
            return set_err(r, start_line, "unexpected character after closing quote");
        if (fs == FIELD_TOO_LARGE)
            return set_err(r, start_line, "field exceeds maximum length");
        if (fs == FIELD_NOMEM)
            return CSV_NOMEM;

        if (!push_field(r, a, count++, r->scratch, sdslen(r->scratch), quoted))
            return CSV_NOMEM;

        if (r->pos >= r->len) break;

        char c = r->data[r->pos];
        if (c == ',') {
            r->pos++;
            continue;
        }

        r->pos++;
        if (c == '\r' && r->pos < r->len && r->data[r->pos] == '\n') r->pos++;
        r->line++;
        break;
    }

    char **fields = arena_memdup(a, r->field_buf, count * sizeof(char *), _Alignof(char *));
    bool  *quoted = arena_memdup(a, r->quoted_buf, count * sizeof(bool), _Alignof(bool));
    if (!fields || !quoted) return CSV_NOMEM;

    out->fields = fields;
    out->quoted = quoted;
    out->count  = count;

    if (r->n_cols == 0) {
        r->n_cols = count;
        return CSV_OK;
    }
    if (count != r->n_cols) {
        char msg[96];
        snprintf(msg, sizeof msg, "row has %zu column%s, expected %zu",
                 count, count == 1 ? "" : "s", r->n_cols);
        return set_err(r, start_line, msg);
    }
    return CSV_OK;
}

const char *csv_reader_error(const CsvReader *r) {
    return r->err;
}

size_t csv_reader_error_line(const CsvReader *r) {
    return r->err_line;
}
