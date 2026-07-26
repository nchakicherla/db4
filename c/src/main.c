#include <stdio.h>
#include <string.h>

#include "linenoise.h"
#include "load.h"
#include "session.h"
#include "table.h"

static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static bool take_word(const char **pp, char *buf, size_t buf_len) {
    const char *p = skip_spaces(*pp);
    size_t n = 0;
    while (*p && *p != ' ') {
        if (n + 1 >= buf_len) return false;
        buf[n++] = *p++;
    }
    if (n == 0) return false;
    buf[n] = '\0';
    *pp = p;
    return true;
}

static void cmd_dump(Session *session, const char *args) {
    char name[MAX_COL_NAME_LEN], path[512];
    const char *p = args;
    if (!take_word(&p, name, sizeof name)) {
        printf("usage: .dump <table> \"<path>\"\n");
        return;
    }
    p = skip_spaces(p);
    if (*p != '"') {
        printf("usage: .dump <table> \"<path>\"\n");
        return;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        if (n + 1 >= sizeof path) { printf("path too long\n"); return; }
        path[n++] = *p++;
    }
    if (*p != '"') {
        printf("usage: .dump <table> \"<path>\"\n");
        return;
    }
    path[n] = '\0';

    int idx = session_find(session, name);
    if (idx < 0) {
        printf("no such table: %s\n", name);
        return;
    }

    if (dump_csv(&session->tables[idx].table, path))
        printf("dumped %s to %s\n", name, path);
}

static void cmd_schema(Session *session, const char *args) {
    char name[MAX_COL_NAME_LEN];
    const char *p = args;
    if (!take_word(&p, name, sizeof name)) {
        printf("usage: .schema <table>\n");
        return;
    }

    int idx = session_find(session, name);
    if (idx < 0) {
        printf("no such table: %s\n", name);
        return;
    }

    print_schema(&session->tables[idx].table);
}

static void dispatch(Session *session, const char *line) {
    if (strncmp(line, ".load ", 6) == 0) {
        load_csv(session, line + 6);
    } else if (strcmp(line, ".tables") == 0) {
        print_tables(session);
    } else if (strncmp(line, ".schema ", 8) == 0) {
        cmd_schema(session, line + 8);
    } else if (strncmp(line, ".dump ", 6) == 0) {
        cmd_dump(session, line + 6);
    } else {
        printf("unknown command: %s\n", line);
    }
}

int main(void) {
    linenoiseHistorySetMaxLen(100);

    Session session = {0};

    char *line;
    while ((line = linenoise("db4> ")) != NULL) {
        if (line[0] == '\0') {
            linenoiseFree(line);
            continue;
        }

        linenoiseHistoryAdd(line);

        if (strcmp(line, ".quit") == 0 || strcmp(line, ".exit") == 0) {
            linenoiseFree(line);
            break;
        }

        if (line[0] == '.') {
            dispatch(&session, line);
        } else {
            printf("%s\n", line);
        }
        linenoiseFree(line);
    }

    session_term(&session);
    return 0;
}
