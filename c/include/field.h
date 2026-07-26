#ifndef FIELD_H
#define FIELD_H

#include <stdbool.h>

typedef enum {
    FT_INT = 0,
    FT_DOUBLE,
    FT_BOOL,
    FT_TEXT,
    FT_ERR,
} FieldType;

bool        field_validate(const char *field, FieldType type);
FieldType   field_infer_type(const char *field);
const char *field_type_label(FieldType type);

bool field_type_from_name(const char *name, FieldType *out);

#endif
