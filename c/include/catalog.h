#ifndef CATALOG_H
#define CATALOG_H

#include <stddef.h>

#include "table.h"

typedef struct {
    char  *name;
    char  *path;   /* source CSV path from .load - M5's commit durability
                    * writes back here; NULL is not currently reachable
                    * since every table today comes from .load. */
    Table  table;
} NamedTable;

typedef struct {
    NamedTable *tables;
    size_t      count;
    size_t      cap;
} Catalog;

int catalog_find(const Catalog *c, const char *name);

Table *catalog_put(Catalog *c, const char *name, const char *path);

void catalog_term(Catalog *c);

#endif
