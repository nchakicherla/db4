#ifndef CATALOG_H
#define CATALOG_H

#include <stddef.h>

#include "table.h"

typedef struct {
    char  *name;
    Table  table;
} NamedTable;

typedef struct {
    NamedTable *tables;
    size_t      count;
    size_t      cap;
} Catalog;

int catalog_find(const Catalog *c, const char *name);

Table *catalog_put(Catalog *c, const char *name);

void catalog_term(Catalog *c);

#endif
