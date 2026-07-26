#include "csv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    FIELD_OK,
    FIELD_UNTERMINATED,   // quoted field never saw a closing quote
    FIELD_BAD_TRAILING,   // content followed a closing quote before , \r \n or EOF
    FIELD_TOO_LARGE,      // field exceeded CSV_MAX_FIELD_LEN before terminating
    FIELD_NOMEM,          // the scratch buffer couldn't grow
} FieldStatus;

static CsvStatus set_err(CsvReader *r, size_t line, const char *msg) {
    snprintf(r->err, sizeof r->err, "%s", msg);
    r->err_line = line;
    return CSV_ERR;
}

// Advances r->pos past whatever remains of the current malformed row, so a
// subsequent csv_reader_next_row() call starts at a genuine row boundary
// instead of resuming mid-field. Without this, retrying after a malformed-
// field error (bad trailing content, oversized field) would parse the
// leftover tail of the broken row as if it were a fresh one, silently
// loading fabricated data.
//
// `in_quotes` reflects whether r->pos already sits inside an opened quoted
// field (true when a field was too large while still inside its quotes;
// false otherwise, since bad-trailing content is only detected once a
// field's quotes have already closed). Quote characters seen while
// scanning toggle this state, which correctly tracks doubled-quote escapes
// too: every "" pair contributes an even number of toggles, so parity
// always lands back on the real in/out-of-quotes state.
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
            r->line++; // embedded newline inside a still-open quote
        }

        r->pos++;
    }
}

// Returns false if the row buffers or the field copy couldn't be
// allocated. The reader stays usable - just without this field.
static bool push_field(CsvReader *r, Arena *a, size_t idx, const char *data, size_t len, bool quoted) {
    if (idx >= r->field_cap) {
        size_t new_cap = r->field_cap ? r->field_cap * 2 : 8;
        if (new_cap > SIZE_MAX / sizeof(char *)) return false;

        // Each result is committed the moment it succeeds rather than both
        // at the end: a successful realloc has already freed the old block,
        // so holding the pointer back would leave r->field_buf dangling if
        // the second realloc then failed. A field_buf that outgrew
        // field_cap is only unused capacity.
        char **grown_fields = realloc(r->field_buf, new_cap * sizeof(char *));
        if (!grown_fields) return false;
        r->field_buf = grown_fields;

        bool *grown_quoted = realloc(r->quoted_buf, new_cap * sizeof(bool));
        if (!grown_quoted) return false;
        r->quoted_buf = grown_quoted;

        r->field_cap = new_cap;
    }

    // arena_strndup would stop at the first embedded NUL in data; a field's
    // true length is already known from the scratch buffer, so copy exactly
    // that many bytes and terminate manually.
    char *field = arena_alloc(a, len + 1, _Alignof(char));
    if (!field) return false;
    memcpy(field, data, len);
    field[len] = '\0';

    r->field_buf[idx]  = field;
    r->quoted_buf[idx] = quoted;
    return true;
}

// Reads one field into r->scratch, leaving r->pos at the delimiter, line
// terminator, or EOF that follows it. Unquoted fields are copied as-is;
// quoted fields are unescaped ("" -> ") and may contain raw commas and
// line breaks. *out_quoted reports whether the field was "..."-wrapped.
static FieldStatus read_field(CsvReader *r, bool *out_quoted) {
    // csv_reader_init reports a scratch buffer it couldn't allocate, but a
    // caller that ignored that must not end up here dereferencing it.
    if (!r->scratch) return FIELD_NOMEM;
    sdsclear(r->scratch);

    if (r->pos < r->len && r->data[r->pos] == '"') {
        *out_quoted = true;
        r->pos++; // consume opening quote

        for (;;) {
            if (r->pos >= r->len) return FIELD_UNTERMINATED;

            char c = r->data[r->pos];
            if (c == '"') {
                if (r->pos + 1 < r->len && r->data[r->pos + 1] == '"') {
                    // Through a temporary: sdscatlen returns NULL on failure
                    // without freeing, so assigning it straight back would
                    // both lose and leak the buffer built so far.
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
                r->pos++; // consume closing quote
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

    // Everything else is zeroed above first, so a reader that failed here is
    // still safe to hand to csv_reader_free - which is what lets the caller
    // tear it down on the same path as any other failure.
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

        if (r->pos >= r->len) break; // EOF ends the row

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

    // Both into locals and committed together, so a caller that inspects
    // `out` after a failure (cmd_load reads out->count to decide whether a
    // flagged row is still usable) never sees a half-filled CsvRow.
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
