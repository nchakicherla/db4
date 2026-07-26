#include "session.h"

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

int session_find(const Session *s, const char *name) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->tables[i].name, name) == 0) return (int)i;
    return -1;
}

Table *session_put(Session *s, const char *name) {
    int idx = session_find(s, name);
    if (idx >= 0) {
        table_term(&s->tables[idx].table);
        s->current = (size_t)idx;
        return &s->tables[idx].table;
    }

    if (s->count == s->cap) {
        size_t new_cap = s->cap ? s->cap * 2 : 4;
        if (new_cap > SIZE_MAX / sizeof(NamedTable)) return NULL;
        NamedTable *grown = realloc(s->tables, new_cap * sizeof(NamedTable));
        if (!grown) return NULL;
        s->tables = grown;
        s->cap    = new_cap;
    }

    char *dup = dup_name(name);
    if (!dup) return NULL;

    s->tables[s->count].name = dup;
    s->current = s->count;
    return &s->tables[s->count++].table;
}

Table *session_current(Session *s) {
    return &s->tables[s->current].table;
}

const char *session_current_name(const Session *s) {
    return s->tables[s->current].name;
}

void session_term(Session *s) {
    for (size_t i = 0; i < s->count; i++) {
        table_term(&s->tables[i].table);
        free(s->tables[i].name);
    }
    free(s->tables);
    s->tables  = NULL;
    s->count   = 0;
    s->cap     = 0;
    s->current = 0;
}
