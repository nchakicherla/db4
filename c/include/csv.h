#ifndef CSV_H
#define CSV_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "sds.h"

// RFC 4180 row reader over an in-memory buffer. Field strings are copied
// into the caller's arena and outlive the reader; csv_reader_free only
// releases the reader's own scratch space.

// Guards against unbounded memory growth while scanning a single field
// (e.g. an unterminated quote that would otherwise consume the rest of
// the buffer before failing).
#define CSV_MAX_FIELD_LEN (1 << 20)

typedef enum {
    CSV_OK = 0,  // out holds a parsed row
    CSV_EOF,     // no more input; out is untouched
    CSV_ERR,     // malformed row; see csv_reader_error()/csv_reader_error_line()
    CSV_NOMEM,   // the row couldn't be allocated; out is untouched
} CsvStatus;

typedef struct {
    char  **fields;
    bool   *quoted; // whether fields[i] was wrapped in "" in the source
    size_t  count;
} CsvRow;

typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
    size_t      line;

    size_t      n_cols;     // pinned from the first row parsed; 0 = unpinned

    sds         scratch;    // reusable buffer for the field currently being read
    char      **field_buf;  // reusable buffer for the current row's field pointers
    bool       *quoted_buf; // reusable buffer for the current row's quoted flags
    size_t      field_cap;  // shared capacity of field_buf and quoted_buf

    char        err[128];
    size_t      err_line;
} CsvReader;

// Returns false if the reader's scratch buffer - its one allocation -
// couldn't be had. The reader is fully initialized either way, so
// csv_reader_free is safe (and still required) on a failed one; it just
// can't parse anything, and every csv_reader_next_row returns CSV_NOMEM.
bool csv_reader_init(CsvReader *r, const char *data, size_t len);
void csv_reader_free(CsvReader *r);

// On CSV_ERR from a ragged column count, out is still populated (the row
// is usable, just flagged) so the caller can choose to continue past it.
// On CSV_ERR from a malformed field (bad quoting, oversized field), out
// is left untouched and the reader is resynced past the rest of the
// broken row, so a caller that retries with another call parses the next
// real row rather than resuming mid-field.
//
// CSV_NOMEM is separate from CSV_ERR because it isn't a property of the
// input: retrying the next row won't help, and unlike a malformed row it
// says nothing about the file. Callers should abandon the whole read.
CsvStatus csv_reader_next_row(CsvReader *r, Arena *a, CsvRow *out);

const char *csv_reader_error(const CsvReader *r);
size_t      csv_reader_error_line(const CsvReader *r);

#endif
