#include "wal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "field.h"
#include "load.h"

#define WAL_MAGIC ((uint32_t)0x314C4157u) /* "WAL1" */

enum {
    WAL_FRAME_ROW       = 0,
    WAL_FRAME_TOMBSTONE = 1,
};

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} ByteBuf;

static bool buf_reserve(ByteBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t new_cap = b->cap ? b->cap * 2 : 256;
    while (new_cap < b->len + extra) new_cap *= 2;
    uint8_t *grown = realloc(b->data, new_cap);
    if (!grown) return false;
    b->data = grown;
    b->cap  = new_cap;
    return true;
}

static bool buf_append(ByteBuf *b, const void *p, size_t n) {
    if (n == 0) return true;
    if (!buf_reserve(b, n)) return false;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}

static uint32_t fnv1a(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static bool encode_row(ByteBuf *payload, const Table *t, size_t row) {
    for (size_t col = 0; col < t->n_cols; col++) {
        uint8_t is_null = table_is_null(t, row, col) ? 1 : 0;
        if (!buf_append(payload, &is_null, 1)) return false;
        if (is_null) continue;

        switch (t->types[col]) {
            case FT_INT: {
                int64_t v = table_get_int(t, row, col);
                if (!buf_append(payload, &v, sizeof v)) return false;
                break;
            }
            case FT_DOUBLE: {
                double v = table_get_double(t, row, col);
                if (!buf_append(payload, &v, sizeof v)) return false;
                break;
            }
            case FT_BOOL: {
                uint8_t v = table_get_bool(t, row, col) ? 1 : 0;
                if (!buf_append(payload, &v, 1)) return false;
                break;
            }
            case FT_TEXT: {
                size_t len;
                const char *s = table_get_text(t, row, col, &len);
                uint32_t len32 = (uint32_t)len;
                if (!buf_append(payload, &len32, sizeof len32)) return false;
                if (!buf_append(payload, s, len)) return false;
                break;
            }
            default: break;
        }
    }
    return true;
}

static bool write_frame(FILE *f, uint32_t row, uint8_t kind, const ByteBuf *payload) {
    uint32_t magic       = WAL_MAGIC;
    uint32_t payload_len = payload ? (uint32_t)payload->len : 0;
    uint32_t checksum    = payload ? fnv1a(payload->data, payload->len) : 0;

    if (fwrite(&magic, sizeof magic, 1, f) != 1) return false;
    if (fwrite(&row, sizeof row, 1, f) != 1) return false;
    if (fwrite(&kind, sizeof kind, 1, f) != 1) return false;
    if (fwrite(&payload_len, sizeof payload_len, 1, f) != 1) return false;
    if (fwrite(&checksum, sizeof checksum, 1, f) != 1) return false;
    if (payload_len > 0 && fwrite(payload->data, 1, payload_len, f) != payload_len) return false;
    return true;
}

bool wal_append(const char *wal_path, const Table *t, const size_t *rows, size_t n_rows) {
    if (n_rows == 0) return true;

    FILE *f = fopen(wal_path, "ab");
    if (!f) return false;

    bool ok = true;
    for (size_t i = 0; i < n_rows && ok; i++) {
        size_t row = rows[i];

        if (table_row_is_dead(t, row)) {
            ok = write_frame(f, (uint32_t)row, WAL_FRAME_TOMBSTONE, NULL);
            continue;
        }

        ByteBuf payload = {0};
        ok = encode_row(&payload, t, row) && write_frame(f, (uint32_t)row, WAL_FRAME_ROW, &payload);
        free(payload.data);
    }

    if (ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) ok = false;
    fclose(f);
    return ok;
}

static void apply_row_payload(Table *t, size_t row, const uint8_t *payload, uint32_t payload_len) {
    table_undelete_row(t, row);

    size_t off = 0;
    for (size_t col = 0; col < t->n_cols; col++) {
        if (off >= payload_len) break; /* schema shrank since this frame was written */

        uint8_t is_null = payload[off++];
        if (is_null) {
            table_set_null(t, row, col);
            continue;
        }

        switch (t->types[col]) {
            case FT_INT: {
                int64_t v;
                memcpy(&v, payload + off, sizeof v);
                off += sizeof v;
                table_set_int(t, row, col, v);
                break;
            }
            case FT_DOUBLE: {
                double v;
                memcpy(&v, payload + off, sizeof v);
                off += sizeof v;
                table_set_double(t, row, col, v);
                break;
            }
            case FT_BOOL: {
                uint8_t v = payload[off++];
                table_set_bool(t, row, col, v != 0);
                break;
            }
            case FT_TEXT: {
                uint32_t len;
                memcpy(&len, payload + off, sizeof len);
                off += sizeof len;
                table_set_text(t, row, col, (const char *)(payload + off), len);
                off += len;
                break;
            }
            default: break;
        }
    }
}

bool wal_replay(const char *wal_path, Table *t) {
    FILE *f = fopen(wal_path, "rb");
    if (!f) return true; /* no WAL yet - nothing to replay */

    for (;;) {
        uint32_t magic, row32, payload_len, checksum;
        uint8_t  kind;

        if (fread(&magic, sizeof magic, 1, f) != 1) break; /* clean EOF between frames */
        if (magic != WAL_MAGIC) break;                      /* torn/corrupt frame - stop here */
        if (fread(&row32, sizeof row32, 1, f) != 1) break;
        if (fread(&kind, sizeof kind, 1, f) != 1) break;
        if (fread(&payload_len, sizeof payload_len, 1, f) != 1) break;
        if (fread(&checksum, sizeof checksum, 1, f) != 1) break;

        uint8_t *payload = NULL;
        if (payload_len > 0) {
            payload = malloc(payload_len);
            if (!payload) break;
            if (fread(payload, 1, payload_len, f) != payload_len) { free(payload); break; }
            if (fnv1a(payload, payload_len) != checksum) { free(payload); break; }
        }

        size_t row = row32;
        bool grow_failed = false;
        while (t->n_rows <= row) {
            if (table_append_row(t) == SIZE_MAX) { grow_failed = true; break; }
        }
        if (grow_failed) { free(payload); break; }

        if (kind == WAL_FRAME_TOMBSTONE) {
            table_delete_row(t, row);
        } else {
            apply_row_payload(t, row, payload, payload_len);
        }
        free(payload);
    }

    fclose(f);
    return true;
}

bool wal_checkpoint(const char *csv_path, const char *wal_path, const Table *t) {
    if (!dump_csv(t, csv_path)) return false;
    if (remove(wal_path) != 0 && errno != ENOENT) return false;
    return true;
}
