#include "field.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

static const char *labels[] = {
    "INT", "DOUBLE", "BOOL", "TEXT", "ERR"
};

const char *field_type_label(FieldType type) {
    if ((size_t)type >= sizeof(labels) / sizeof(labels[0])) return "UNKNOWN";
    return labels[type];
}

bool field_type_from_name(const char *name, FieldType *out) {
    size_t len = strlen(name);
    if (len == 0 || len >= 8) return false;

    char buf[8];
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)tolower((unsigned char)name[i]);
    buf[len] = '\0';

    if (strcmp(buf, "int") == 0)    { *out = FT_INT;    return true; }
    if (strcmp(buf, "double") == 0) { *out = FT_DOUBLE; return true; }
    if (strcmp(buf, "bool") == 0)   { *out = FT_BOOL;   return true; }
    if (strcmp(buf, "text") == 0)   { *out = FT_TEXT;   return true; }
    return false;
}

static bool validate_int(const char *f) {
    if (isspace((unsigned char)f[0])) return false;

    char *end = (char *)f;
    errno = 0;
    strtoll(f, &end, 10);
    if (end == f || *end != '\0') return false;
    if (errno == ERANGE) return false;

    return true;
}

static bool is_decimal_char(char c) {
    return isdigit((unsigned char)c) || c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E';
}

static bool validate_double(const char *f) {
    if (isspace((unsigned char)f[0])) return false;

    for (const char *p = f; *p; p++)
        if (!is_decimal_char(*p)) return false;

    char *end = (char *)f;
    errno = 0;
    double val = strtod(f, &end);
    if (end == f || *end != '\0') return false;
    if (errno == ERANGE && (val == HUGE_VAL || val == -HUGE_VAL)) return false;

    return true;
}

static bool validate_bool(const char *f) {
    size_t len = strlen(f);
    if (len < 4 || len > 5) return false;
    char buf[6] = {0};
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)tolower((unsigned char)f[i]);
    return strcmp(buf, "true") == 0 || strcmp(buf, "false") == 0;
}

bool field_validate(const char *field, FieldType type) {
    switch (type) {
        case FT_INT:    return validate_int(field);
        case FT_DOUBLE: return validate_double(field);
        case FT_BOOL:   return validate_bool(field);
        case FT_TEXT:   return true;
        default:        return false;
    }
}

static bool has_leading_zero(const char *f) {
    if (*f == '+' || *f == '-') f++;
    return f[0] == '0' && isdigit((unsigned char)f[1]);
}

FieldType field_infer_type(const char *field) {
    if (has_leading_zero(field)) return FT_TEXT;
    if (validate_int(field))    return FT_INT;
    if (validate_double(field)) return FT_DOUBLE;
    if (validate_bool(field))   return FT_BOOL;
    return FT_TEXT;
}
