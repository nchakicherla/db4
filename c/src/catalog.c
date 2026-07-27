#include "catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_name(const char *name) {
    size_t len = strlen(name);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, name, len + 1);
    return out;
}

int catalog_find(const Catalog *c, const char *name) {
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->tables[i].name, name) == 0) return (int)i;
    return -1;
}

Table *catalog_put(Catalog *c, const char *name, const char *path) {
    int idx = catalog_find(c, name);
    if (idx >= 0) {
        table_term(&c->tables[idx].table);
        free(c->tables[idx].path);
        c->tables[idx].path = path ? dup_name(path) : NULL;
        return &c->tables[idx].table;
    }

    if (c->count == c->cap) {
        size_t new_cap = c->cap ? c->cap * 2 : 4;
        if (new_cap > SIZE_MAX / sizeof(NamedTable)) return NULL;
        NamedTable *grown = realloc(c->tables, new_cap * sizeof(NamedTable));
        if (!grown) return NULL;
        c->tables = grown;
        c->cap    = new_cap;
    }

    char *dup = dup_name(name);
    if (!dup) return NULL;

    char *path_dup = path ? dup_name(path) : NULL;
    if (path && !path_dup) return NULL;

    c->tables[c->count].name = dup;
    c->tables[c->count].path = path_dup;
    return &c->tables[c->count++].table;
}

void catalog_term(Catalog *c) {
    for (size_t i = 0; i < c->count; i++) {
        table_term(&c->tables[i].table);
        free(c->tables[i].name);
        free(c->tables[i].path);
    }
    free(c->tables);
    c->tables = NULL;
    c->count  = 0;
    c->cap    = 0;
}
