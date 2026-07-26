#ifndef SESSION_H
#define SESSION_H

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
    size_t      current;
} Session;

int session_find(const Session *s, const char *name);

Table *session_put(Session *s, const char *name);

Table *session_current(Session *s);
const char *session_current_name(const Session *s);

void session_term(Session *s);

#endif
