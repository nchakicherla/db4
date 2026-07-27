#ifndef LOAD_H
#define LOAD_H

#include <stdbool.h>

#include "catalog.h"
#include "table.h"

#define MAX_COL_NAME_LEN 64

bool load_csv(Catalog *catalog, const char *args);

bool dump_csv(const Table *t, const char *path);

void print_tables(const Catalog *catalog);
void print_schema(const Table *t);

#endif
