#ifndef SCHEMA_H
#define SCHEMA_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "field.h"
#include "table.h"

typedef struct {
    char      **names;
    FieldType  *types;
    bool       *primary;
    char      **fk_tables;
    char      **fk_columns;
    FkAction   *fk_on_delete;
    FkAction   *fk_on_update;
    size_t      count;
} SchemaOverride;

bool schema_parse(const char *json, Arena *a, SchemaOverride *out, char *err, size_t err_len);

bool schema_lookup(const SchemaOverride *s, const char *name, FieldType *out_type);

bool schema_lookup_fk(const SchemaOverride *s, const char *name, const char **out_table, const char **out_column);

#endif
