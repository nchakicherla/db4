#include "value.h"

#include <string.h>

Value value_int(int64_t v)    { Value r; r.kind = FT_INT;    r.is_null = false; r.as.i = v; return r; }
Value value_double(double v)  { Value r; r.kind = FT_DOUBLE; r.is_null = false; r.as.d = v; return r; }
Value value_bool(bool v)      { Value r; r.kind = FT_BOOL;   r.is_null = false; r.as.b = v; return r; }

Value value_text(const char *s, size_t len) {
    Value r;
    r.kind = FT_TEXT;
    r.is_null = false;
    r.as.s.data = s;
    r.as.s.len  = len;
    return r;
}

Value value_null(FieldType kind) { Value r; r.kind = kind; r.is_null = true; return r; }

Value read_column(const Table *t, size_t row, size_t col) {
    if (table_is_null(t, row, col)) return value_null(t->types[col]);
    switch (t->types[col]) {
        case FT_INT:    return value_int(table_get_int(t, row, col));
        case FT_DOUBLE: return value_double(table_get_double(t, row, col));
        case FT_BOOL:   return value_bool(table_get_bool(t, row, col));
        case FT_TEXT: {
            size_t len;
            const char *s = table_get_text(t, row, col, &len);
            return value_text(s, len);
        }
        default: return value_null(FT_TEXT);
    }
}

int compare_values(Value a, Value b) {
    if ((a.kind == FT_INT || a.kind == FT_DOUBLE) && (b.kind == FT_INT || b.kind == FT_DOUBLE)) {
        double x = a.kind == FT_INT ? (double)a.as.i : a.as.d;
        double y = b.kind == FT_INT ? (double)b.as.i : b.as.d;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.kind == FT_BOOL) return (int)a.as.b - (int)b.as.b;

    size_t n = a.as.s.len < b.as.s.len ? a.as.s.len : b.as.s.len;
    int c = n ? memcmp(a.as.s.data, b.as.s.data, n) : 0;
    if (c != 0) return c;
    if (a.as.s.len != b.as.s.len) return a.as.s.len < b.as.s.len ? -1 : 1;
    return 0;
}

uint64_t value_hash(Value v) {
    switch (v.kind) {
        case FT_INT: {
            uint64_t x = (uint64_t)v.as.i;
            x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27; x *= 0x94d049bb133111ebULL;
            return x ^ (x >> 31);
        }
        case FT_DOUBLE: {
            double d = v.as.d;
            if (d == 0.0) d = 0.0; /* normalize -0.0 to hash the same as 0.0 */
            uint64_t bits;
            memcpy(&bits, &d, sizeof bits);
            bits ^= bits >> 30; bits *= 0xbf58476d1ce4e5b9ULL;
            bits ^= bits >> 27; bits *= 0x94d049bb133111ebULL;
            return bits ^ (bits >> 31);
        }
        case FT_BOOL:
            return v.as.b ? 0x9e3779b97f4a7c15ULL : 0xd6e8feb86659fd93ULL;
        case FT_TEXT: {
            uint64_t h = 14695981039346656037ULL;
            for (size_t i = 0; i < v.as.s.len; i++) {
                h ^= (unsigned char)v.as.s.data[i];
                h *= 1099511628211ULL;
            }
            return h;
        }
        default: return 0;
    }
}

void print_value(FILE *f, Value v) {
    if (v.is_null) {
        fprintf(f, "NULL");
        return;
    }
    switch (v.kind) {
        case FT_INT:    fprintf(f, "%lld", (long long)v.as.i); break;
        case FT_DOUBLE: fprintf(f, "%g", v.as.d); break;
        case FT_BOOL:   fprintf(f, "%s", v.as.b ? "true" : "false"); break;
        case FT_TEXT:   fprintf(f, "%.*s", (int)v.as.s.len, v.as.s.data); break;
        default: break;
    }
}
