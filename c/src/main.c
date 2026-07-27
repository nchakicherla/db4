#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "linenoise.h"
#include "load.h"
#include "catalog.h"
#include "interp.h"
#include "parser.h"
#include "table.h"
#include "txn.h"

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

static void cmd_dump(Catalog *catalog, const char *args) {
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

    int idx = catalog_find(catalog, name);
    if (idx < 0) {
        printf("no such table: %s\n", name);
        return;
    }

    if (dump_csv(&catalog->tables[idx].table, path))
        printf("dumped %s to %s\n", name, path);
}

static void cmd_schema(Catalog *catalog, const char *args) {
    char name[MAX_COL_NAME_LEN];
    const char *p = args;
    if (!take_word(&p, name, sizeof name)) {
        printf("usage: .schema <table>\n");
        return;
    }

    int idx = catalog_find(catalog, name);
    if (idx < 0) {
        printf("no such table: %s\n", name);
        return;
    }

    print_schema(&catalog->tables[idx].table);
}

/* Lexes and parses any statement and prints the resulting AST without
 * running it - lets the grammar be exercised (including INSERT/UPDATE/
 * DELETE/BEGIN/COMMIT/ROLLBACK as of M5) independently of execution. */
static void cmd_parse(const char *args) {
    Arena arena;
    arena_init(&arena);

    Parser parser;
    parser_init(&parser, args, strlen(args), &arena);

    Stmt *stmt = parser_parse_statement(&parser);
    if (parser_failed(&parser)) {
        printf("line %zu: %s\n", parser_error_line(&parser), parser_error(&parser));
    } else {
        stmt_print(stdout, stmt);
    }

    arena_term(&arena);
}

/* A bare (non-dot) line is SQL, run against catalog. A mutating statement
 * (INSERT/UPDATE/DELETE) with no transaction already open runs as its own
 * autocommit transaction - see interp_exec. */
static void cmd_sql(Catalog *catalog, Txn *txn, const char *line) {
    Arena arena;
    arena_init(&arena);

    Parser parser;
    parser_init(&parser, line, strlen(line), &arena);

    Stmt *stmt = parser_parse_statement(&parser);
    if (parser_failed(&parser)) {
        printf("line %zu: %s\n", parser_error_line(&parser), parser_error(&parser));
        arena_term(&arena);
        return;
    }

    char err[128];
    if (!interp_exec(stmt, catalog, txn, stdout, err, sizeof err))
        printf("%s\n", err);

    arena_term(&arena);
}

static void dispatch(Catalog *catalog, const char *line) {
    if (strncmp(line, ".load ", 6) == 0) {
        load_csv(catalog, line + 6);
    } else if (strcmp(line, ".tables") == 0) {
        print_tables(catalog);
    } else if (strncmp(line, ".schema ", 8) == 0) {
        cmd_schema(catalog, line + 8);
    } else if (strncmp(line, ".dump ", 6) == 0) {
        cmd_dump(catalog, line + 6);
    } else if (strncmp(line, ".parse ", 7) == 0) {
        cmd_parse(line + 7);
    } else {
        printf("unknown command: %s\n", line);
    }
}

int main(void) {
    linenoiseHistorySetMaxLen(100);

    Catalog catalog = {0};
    Txn     txn;
    txn_init(&txn);

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
            dispatch(&catalog, line);
        } else {
            cmd_sql(&catalog, &txn, line);
        }
        linenoiseFree(line);
    }

    txn_term(&txn);
    catalog_term(&catalog);
    return 0;
}
