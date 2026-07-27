#include "wal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "field.h"
#include "load.h"

#define WAL_MAGIC        ((uint32_t)0x314C4157u) /* "WAL1" */
#define WAL_HEADER_MAGIC ((uint32_t)0x48344C57u) /* "WL4H" - distinct from a row frame's magic */

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

/* Folds column count plus every column's type and name into one hash, so
 * wal_replay can tell whether the table it's about to replay onto still
 * has the same column layout the frames were encoded against. Column
 * types are inferred from a CSV's first data row (field.c), so editing
 * the base CSV between sessions can silently change a column's type out
 * from under an old WAL - without this check, apply_row_payload would
 * decode a frame's bytes according to the *new* type, reading a TEXT
 * length as raw INT bytes or vice versa (see wal_replay's use of this). */
static uint32_t schema_fingerprint(const Table *t) {
    ByteBuf buf = {0};
    uint32_t n_cols32 = (uint32_t)t->n_cols;
    bool ok = buf_append(&buf, &n_cols32, sizeof n_cols32);
    for (size_t col = 0; ok && col < t->n_cols; col++) {
        uint8_t type = (uint8_t)t->types[col];
        ok = buf_append(&buf, &type, 1) && buf_append(&buf, t->names[col], strlen(t->names[col]) + 1);
    }
    uint32_t h = ok ? fnv1a(buf.data, buf.len) : 0;
    free(buf.data);
    return h;
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

    /* A freshly created (or still-empty) WAL gets a one-time header - magic
     * plus this table's current schema fingerprint - ahead of any row
     * frames, so a later wal_replay can tell whether the table it's about
     * to replay onto still matches. fseek+ftell (not "ab"'s own initial
     * position, unspecified until a write happens) is the same pattern
     * load.c's read_file already uses to size a file. start_size also
     * doubles as the truncation point below if this append fails partway
     * through. */
    fseek(f, 0, SEEK_END);
    long start_size = ftell(f);
    if (start_size < 0) start_size = 0;

    bool ok = true;
    if (start_size == 0) {
        uint32_t header_magic = WAL_HEADER_MAGIC;
        uint32_t fp           = schema_fingerprint(t);
        ok = fwrite(&header_magic, sizeof header_magic, 1, f) == 1
          && fwrite(&fp, sizeof fp, 1, f) == 1;
    }

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

    /* A failure above (disk full mid-write, etc.) can leave a partial frame
     * - or just the header with nothing after it - sitting at the end of
     * the file. Left in place, the next successful append would write
     * valid frames right after that garbage, and wal_replay stops at the
     * first bad frame it finds (by design - see its doc comment), so
     * every frame appended after the tear would be silently lost even
     * once whatever caused this failure has cleared up. Truncating back to
     * this call's starting size makes a failed append leave the file
     * exactly as it found it, matching what the caller (a failed commit)
     * already tells its own caller happened - best effort: if truncation
     * itself fails there's nothing more to do here, and ok is already
     * false either way. */
    if (!ok) {
        fflush(f);
        (void)ftruncate(fileno(f), start_size);
    }

    fclose(f);
    return ok;
}

/* Every multi-byte read below is bounds-checked against payload_len before
 * it happens, even though wal_replay now refuses to enter this loop at all
 * unless the table's schema_fingerprint matches what these frames were
 * encoded against (see wal_replay) - that check rules out the type-drift
 * case that used to make these reads run past the buffer, but this stays
 * as a second, independent line of defense against a shorter-than-expected
 * frame (a corrupt-but-checksum-matching payload, or any mismatch this
 * file's fingerprint doesn't happen to catch) rather than trusting the
 * fingerprint alone. Returns false the moment a read wouldn't fit, so the
 * caller can treat this frame like a torn one - stop replay here, keep
 * whatever was already applied. */
static bool apply_row_payload(Table *t, size_t row, const uint8_t *payload, uint32_t payload_len) {
    table_undelete_row(t, row);

    size_t off = 0;
    for (size_t col = 0; col < t->n_cols; col++) {
        if (off >= payload_len) break; /* current schema has trailing columns this frame never encoded */

        uint8_t is_null = payload[off++];
        if (is_null) {
            table_set_null(t, row, col);
            continue;
        }

        switch (t->types[col]) {
            case FT_INT: {
                if (off + sizeof(int64_t) > payload_len) return false;
                int64_t v;
                memcpy(&v, payload + off, sizeof v);
                off += sizeof v;
                table_set_int(t, row, col, v);
                break;
            }
            case FT_DOUBLE: {
                if (off + sizeof(double) > payload_len) return false;
                double v;
                memcpy(&v, payload + off, sizeof v);
                off += sizeof v;
                table_set_double(t, row, col, v);
                break;
            }
            case FT_BOOL: {
                if (off + 1 > payload_len) return false;
                uint8_t v = payload[off++];
                table_set_bool(t, row, col, v != 0);
                break;
            }
            case FT_TEXT: {
                if (off + sizeof(uint32_t) > payload_len) return false;
                uint32_t len;
                memcpy(&len, payload + off, sizeof len);
                off += sizeof len;
                if (off + len > payload_len) return false;
                table_set_text(t, row, col, (const char *)(payload + off), len);
                off += len;
                break;
            }
            default: break;
        }
    }
    return true;
}

bool wal_replay(const char *wal_path, Table *t) {
    FILE *f = fopen(wal_path, "rb");
    if (!f) return true; /* no WAL yet - nothing to replay */

    uint32_t header_magic;
    if (fread(&header_magic, sizeof header_magic, 1, f) != 1) {
        fclose(f);
        return true; /* empty file - nothing to replay */
    }
    /* Refuse outright rather than guess-decode: either this isn't a WAL
     * this build wrote (no header at all, e.g. corrupt), or the fingerprint
     * that follows doesn't match this table's current column layout - the
     * table's schema (types inferred from a CSV's first data row - see
     * field.c) may have drifted since these frames were encoded, which
     * would otherwise make apply_row_payload decode a column's bytes under
     * the wrong type. */
    if (header_magic != WAL_HEADER_MAGIC) {
        fclose(f);
        return false;
    }
    uint32_t stored_fingerprint;
    if (fread(&stored_fingerprint, sizeof stored_fingerprint, 1, f) != 1) {
        fclose(f);
        return false; /* truncated header */
    }
    if (stored_fingerprint != schema_fingerprint(t)) {
        fclose(f);
        return false;
    }

    /* oom distinguishes a genuine resource failure (malloc/table_append_row
     * failing) from every other reason this loop stops early - a torn/
     * corrupt frame (bad magic, a short read, a bad checksum, or a payload
     * too short for the current schema) is documented, tolerated behavior:
     * "whatever was applied before it stands", and this function keeps
     * returning true for that case exactly as it always has. OOM is a
     * different situation entirely - replay didn't stop because it found
     * the natural end of valid data, it stopped because it couldn't
     * allocate, and the caller (load.c) should be able to tell the two
     * apart and warn accordingly rather than reporting a clean load. */
    bool oom = false;
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
            if (!payload) { oom = true; break; }
            if (fread(payload, 1, payload_len, f) != payload_len) { free(payload); break; }
            if (fnv1a(payload, payload_len) != checksum) { free(payload); break; }
        }

        size_t row = row32;
        bool grow_failed = false;
        while (t->n_rows <= row) {
            if (table_append_row(t) == SIZE_MAX) { grow_failed = true; break; }
        }
        if (grow_failed) { free(payload); oom = true; break; }

        bool applied = true;
        if (kind == WAL_FRAME_TOMBSTONE) {
            table_delete_row(t, row);
        } else {
            applied = apply_row_payload(t, row, payload, payload_len);
        }
        free(payload);
        if (!applied) break; /* short payload for this schema - treat like a torn frame */
    }

    fclose(f);
    return !oom;
}

static bool generation_path(const char *csv_path, char *buf, size_t buf_len) {
    int n = snprintf(buf, buf_len, "%s.gen", csv_path);
    return n > 0 && (size_t)n < buf_len;
}

bool wal_read_generation(const char *csv_path, uint32_t *out_gen) {
    char path[4160];
    if (!generation_path(csv_path, path, sizeof path)) return false;

    FILE *f = fopen(path, "rb");
    if (!f) { *out_gen = 0; return true; } /* never checkpointed */

    uint32_t gen;
    bool     ok = fread(&gen, sizeof gen, 1, f) == 1;
    fclose(f);
    *out_gen = ok ? gen : 0; /* short/corrupt sidecar - treat like "never checkpointed" */
    return true;
}

bool wal_write_generation(const char *csv_path, uint32_t gen) {
    char path[4160];
    if (!generation_path(csv_path, path, sizeof path)) return false;

    char tmp_path[4176];
    int  n = snprintf(tmp_path, sizeof tmp_path, "%s.tmp-%d", path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp_path) return false;

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;

    bool ok = fwrite(&gen, sizeof gen, 1, f) == 1;
    if (ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) ok = false;
    fclose(f);

    if (!ok) { remove(tmp_path); return false; }
    if (rename(tmp_path, path) != 0) { remove(tmp_path); return false; }
    return true;
}

bool wal_checkpoint(const char *csv_path, const char *wal_path, Table *t) {
    /* dump_csv already skips tombstoned rows when it writes, so compacting
     * first changes nothing about the CSV's contents - only what's left in
     * memory afterward. It's the natural place to do it: table.c's PK
     * index (index_row_if_primary_key) never removes an old hash's slot
     * when a PK cell is overwritten or a row is deleted, so a long session
     * with heavy PK churn grows it without bound; table_compact rebuilds
     * it from scratch alongside reclaiming dead rows. Safe here
     * specifically because cmd_checkpoint already refuses to run this
     * while a transaction is open (see main.c) - nothing holds a row
     * number across the renumbering this performs. */
    table_compact(t);

    if (!dump_csv(t, csv_path)) return false;
    if (remove(wal_path) != 0 && errno != ENOENT) return false;

    uint32_t gen;
    if (!wal_read_generation(csv_path, &gen)) gen = t->wal_generation;
    gen++;
    if (!wal_write_generation(csv_path, gen)) return false;
    t->wal_generation = gen;
    return true;
}
