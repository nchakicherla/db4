#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "db4.h"
#include "linenoise.h"
#include "load.h"
#include "catalog.h"
#include "lock.h"
#include "parser.h"
#include "table.h"
#include "wal.h"

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

/* Folds a table's WAL back into its base CSV (M7) - a brief exclusive
 * lock (see lock.h) excludes concurrent readers' `.load` snapshots and
 * other writers' commits while the base file is rewritten. */
static void cmd_checkpoint(Catalog *catalog, const char *args) {
    char name[MAX_COL_NAME_LEN];
    const char *p = args;
    if (!take_word(&p, name, sizeof name)) {
        printf("usage: .checkpoint <table>\n");
        return;
    }

    int idx = catalog_find(catalog, name);
    if (idx < 0) {
        printf("no such table: %s\n", name);
        return;
    }
    if (!catalog->tables[idx].path) {
        printf("table \"%s\" has no backing file to checkpoint\n", name);
        return;
    }

    const char *path = catalog->tables[idx].path;
    char        wal_path[4160];
    int         n = snprintf(wal_path, sizeof wal_path, "%s.wal", path);
    if (n < 0 || (size_t)n >= sizeof wal_path) {
        printf("path too long: %s\n", path);
        return;
    }

    Db4Lock lock;
    lock.fd = -1;
    bool have_lock = db4_lock_open(&lock, path) && db4_lock_exclusive(&lock);

    bool ok = wal_checkpoint(path, wal_path, &catalog->tables[idx].table);

    if (have_lock) db4_lock_release(&lock);
    db4_lock_close(&lock);

    if (ok) printf("checkpointed %s\n", name);
    else printf("checkpoint failed for %s\n", name);
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

static void print_column_value(Db4Stmt *stmt, int i) {
    switch (db4_column_type(stmt, i)) {
        case DB4_NULL: printf("NULL"); break;
        case DB4_INTEGER: printf("%lld", (long long)db4_column_int64(stmt, i)); break;
        case DB4_FLOAT: printf("%g", db4_column_double(stmt, i)); break;
        case DB4_BOOL: printf("%s", db4_column_bool(stmt, i) ? "true" : "false"); break;
        case DB4_TEXT: {
            size_t len;
            const char *s = db4_column_text(stmt, i, &len);
            printf("%.*s", (int)len, s);
            break;
        }
    }
}

/* A bare (non-dot) line is SQL, run through db4.h (M8) rather than
 * calling parser.c/interp.c directly - main.c is a client of the public
 * API now, same as any other embedder would be. A mutating statement
 * (INSERT/UPDATE/DELETE) with no transaction already open still runs as
 * its own autocommit transaction; that's interp_exec's job underneath,
 * unchanged. */
static void cmd_sql(Db4 *db, const char *line) {
    Db4Stmt *stmt;
    if (!db4_prepare(db, line, strlen(line), &stmt, NULL)) {
        printf("%s\n", db4_errmsg(db));
        return;
    }

    const Stmt *ast = db4_stmt_ast(stmt);
    bool is_select = ast->kind == STMT_SELECT;

    int rc = db4_step(stmt);
    if (rc == DB4_ERROR) {
        printf("%s\n", db4_errmsg(db));
        db4_finalize(stmt);
        return;
    }

    if (is_select) {
        int n_cols = db4_column_count(stmt);
        for (int i = 0; i < n_cols; i++) {
            if (i) printf(" | ");
            printf("%s", db4_column_name(stmt, i));
        }
        printf("\n");

        size_t n_rows = 0;
        while (rc == DB4_ROW) {
            for (int i = 0; i < n_cols; i++) {
                if (i) printf(" | ");
                print_column_value(stmt, i);
            }
            printf("\n");
            n_rows++;
            rc = db4_step(stmt);
        }
        if (rc == DB4_ERROR) {
            printf("%s\n", db4_errmsg(db));
            db4_finalize(stmt);
            return;
        }
        printf("(%zu row%s)\n", n_rows, n_rows == 1 ? "" : "s");
    } else {
        size_t n;
        switch (ast->kind) {
            case STMT_CREATE_TABLE: printf("table \"%s\" created\n", ast->as.create_table.table); break;
            case STMT_BEGIN:        printf("BEGIN\n"); break;
            case STMT_COMMIT:       printf("COMMIT\n"); break;
            case STMT_ROLLBACK:     printf("ROLLBACK\n"); break;
            case STMT_INSERT:
                n = db4_changes(db);
                printf("%zu row%s inserted\n", n, n == 1 ? "" : "s");
                break;
            case STMT_UPDATE:
                n = db4_changes(db);
                printf("%zu row%s updated\n", n, n == 1 ? "" : "s");
                break;
            case STMT_DELETE:
                n = db4_changes(db);
                printf("%zu row%s deleted\n", n, n == 1 ? "" : "s");
                break;
            default: break;
        }
    }

    db4_finalize(stmt);
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
    } else if (strncmp(line, ".checkpoint ", 12) == 0) {
        cmd_checkpoint(catalog, line + 12);
    } else if (strncmp(line, ".parse ", 7) == 0) {
        cmd_parse(line + 7);
    } else {
        printf("unknown command: %s\n", line);
    }
}

int main(void) {
    linenoiseHistorySetMaxLen(100);

    Db4 db;
    db4_open(&db);

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
            dispatch(&db.catalog, line);
        } else {
            cmd_sql(&db, line);
        }
        linenoiseFree(line);
    }

    db4_close(&db);
    return 0;
}
