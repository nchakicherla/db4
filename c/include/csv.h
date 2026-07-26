#ifndef CSV_H
#define CSV_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "sds.h"

#define CSV_MAX_FIELD_LEN (1 << 20)

typedef enum {
    CSV_OK = 0,
    CSV_EOF,
    CSV_ERR,
    CSV_NOMEM,
} CsvStatus;

typedef struct {
    char  **fields;
    bool   *quoted;
    size_t  count;
} CsvRow;

typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
    size_t      line;

    size_t      n_cols;

    sds         scratch;
    char      **field_buf;
    bool       *quoted_buf;
    size_t      field_cap;

    char        err[128];
    size_t      err_line;
} CsvReader;

bool csv_reader_init(CsvReader *r, const char *data, size_t len);
void csv_reader_free(CsvReader *r);

CsvStatus csv_reader_next_row(CsvReader *r, Arena *a, CsvRow *out);

const char *csv_reader_error(const CsvReader *r);
size_t      csv_reader_error_line(const CsvReader *r);

#endif
