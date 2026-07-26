#ifndef LOAD_H
#define LOAD_H

#include <stdbool.h>

#include "session.h"
#include "table.h"

#define MAX_COL_NAME_LEN 64

void load_csv(Session *session, const char *args);

bool dump_csv(const Table *t, const char *path);

void print_tables(const Session *session);
void print_schema(const Table *t);

#endif
