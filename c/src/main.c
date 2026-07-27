#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arena.h"
#include "db4.h"
#include "lexer.h"
#include "linenoise.h"
#include "load.h"
#include "catalog.h"
#include "lock.h"
#include "parser.h"
#include "table.h"
#include "wal.h"

/* Every line-processing function below returns one of these instead of a
 * plain bool, so a .quit/.exit reached inside a .read'd file (or a script
 * passed on the command line - see main()) can unwind all the way back
 * to main() and actually exit, not just stop that one nested file - the
 * same behavior sqlite3's own CLI gives .quit wherever it's encountered. */
typedef enum { LINE_OK, LINE_FAIL, LINE_QUIT } LineResult;

static LineResult dispatch(Db4 *db, const char *line);
static LineResult process_line(Db4 *db, const char *line);

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

/* Parses a "..."-quoted argument (no escape sequences). Leaves *pp just
 * past the closing quote on success. */
static bool take_quoted(const char **pp, char *buf, size_t buf_len) {
    const char *p = skip_spaces(*pp);
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        if (n + 1 >= buf_len) return false;
        buf[n++] = *p++;
    }
    if (*p != '"') return false;
    buf[n] = '\0';
    *pp = p + 1;
    return true;
}

static bool cmd_dump(Catalog *catalog, const char *args) {
    char name[MAX_COL_NAME_LEN], path[512];
    const char *p = args;
    if (!take_word(&p, name, sizeof name) || !take_quoted(&p, path, sizeof path)) {
        printf("usage: .dump <table> \"<path>\"\n");
        return false;
    }

    int idx = catalog_find(catalog, name);
    if (idx < 0) {
        printf("no such table: %s\n", name);
        return false;
    }

    if (!dump_csv(&catalog->tables[idx].table, path)) {
        printf("dump failed for %s\n", name);
        return false;
    }
    printf("dumped %s to %s\n", name, path);
    return true;
}

/* Folds a table's WAL back into its base CSV (M7) - a brief exclusive
 * lock (see lock.h) excludes concurrent readers' `.load` snapshots and
 * other writers' commits while the base file is rewritten. */
static bool cmd_checkpoint(Catalog *catalog, const char *args) {
    char name[MAX_COL_NAME_LEN];
    const char *p = args;
    if (!take_word(&p, name, sizeof name)) {
        printf("usage: .checkpoint <table>\n");
        return false;
    }

    int idx = catalog_find(catalog, name);
    if (idx < 0) {
        printf("no such table: %s\n", name);
        return false;
    }
    if (!catalog->tables[idx].path) {
        printf("table \"%s\" has no backing file to checkpoint\n", name);
        return false;
    }

    const char *path = catalog->tables[idx].path;
    char        wal_path[4160];
    int         n = snprintf(wal_path, sizeof wal_path, "%s.wal", path);
    if (n < 0 || (size_t)n >= sizeof wal_path) {
        printf("path too long: %s\n", path);
        return false;
    }

    Db4Lock lock;
    lock.fd = -1;
    bool have_lock = db4_lock_open(&lock, path) && db4_lock_exclusive(&lock);

    bool ok = wal_checkpoint(path, wal_path, &catalog->tables[idx].table);

    if (have_lock) db4_lock_release(&lock);
    db4_lock_close(&lock);

    if (ok) printf("checkpointed %s\n", name);
    else printf("checkpoint failed for %s\n", name);
    return ok;
}

static bool cmd_schema(Catalog *catalog, const char *args) {
    char name[MAX_COL_NAME_LEN];
    const char *p = args;
    if (!take_word(&p, name, sizeof name)) {
        printf("usage: .schema <table>\n");
        return false;
    }

    int idx = catalog_find(catalog, name);
    if (idx < 0) {
        printf("no such table: %s\n", name);
        return false;
    }

    print_schema(&catalog->tables[idx].table);
    return true;
}

/* Basic filesystem navigation, so a REPL session can move around to
 * wherever the CSVs it wants to `.load`/`.dump` actually live without
 * exiting back to a shell. Paths are quoted, same convention as `.dump`'s
 * path argument (not `.schema`'s bare table-name word) - both because a
 * path can contain a space and because it lets tab completion's existing
 * quoted-path handling (complete_filename, below) cover these commands
 * for free instead of needing a second completion scheme. */
static bool cmd_cd(const char *args) {
    char path[1024];
    const char *p = args;
    if (!take_quoted(&p, path, sizeof path)) {
        printf("usage: .cd \"<path>\"\n");
        return false;
    }

    if (chdir(path) != 0) {
        printf("cd: %s: %s\n", path, strerror(errno));
        return false;
    }

    char cwd[1024];
    if (getcwd(cwd, sizeof cwd)) printf("%s\n", cwd);
    return true;
}

/* Lists a directory's entries, alphabetically (scandir's own job, not
 * hand-rolled), one per line with a trailing '/' on subdirectories -
 * matching complete_filename's own convention for the same reason: a
 * consistent "how does this REPL denote a directory" answer. */
static int name_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* opendir/readdir + a manual sort, not scandir/alphasort - those are BSD
 * extensions that glibc hides under -std=c11's strict mode without a
 * feature-test macro (the reason linenoise.c already needs its own
 * -D_GNU_SOURCE build rule in the makefile; not worth another one just
 * for this). opendir/readdir are the same portable POSIX.1 pair
 * complete_filename below already relies on. */
static bool cmd_ls(const char *args) {
    char path[1024] = ".";
    const char *p   = args;
    if (*skip_spaces(p) != '\0' && !take_quoted(&p, path, sizeof path)) {
        printf("usage: .ls [\"<path>\"]\n");
        return false;
    }

    DIR *dp = opendir(path);
    if (!dp) {
        printf("ls: %s: %s\n", path, strerror(errno));
        return false;
    }

    bool    oom  = false;
    char  **names = NULL;
    size_t  n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (n == cap) {
            size_t  new_cap = cap ? cap * 2 : 16;
            char  **grown   = realloc(names, new_cap * sizeof(char *));
            if (!grown) { printf("out of memory\n"); oom = true; break; }
            names = grown;
            cap   = new_cap;
        }

        char full[1536];
        snprintf(full, sizeof full, "%s/%s", path, ent->d_name);
        struct stat st;
        bool is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);

        size_t entry_cap = strlen(ent->d_name) + 2;
        char  *entry     = malloc(entry_cap);
        if (!entry) { printf("out of memory\n"); oom = true; break; }
        snprintf(entry, entry_cap, is_dir ? "%s/" : "%s", ent->d_name);
        names[n++] = entry;
    }
    closedir(dp);

    qsort(names, n, sizeof(char *), name_cmp);
    for (size_t i = 0; i < n; i++) {
        printf("%s\n", names[i]);
        free(names[i]);
    }
    free(names);
    return !oom;
}

/* Lexes and parses any statement and prints the resulting AST without
 * running it - lets the grammar be exercised (including INSERT/UPDATE/
 * DELETE/BEGIN/COMMIT/ROLLBACK as of M5) independently of execution. */
static bool cmd_parse(const char *args) {
    Arena arena;
    arena_init(&arena);

    Parser parser;
    parser_init(&parser, args, strlen(args), &arena);

    Stmt *stmt = parser_parse_statement(&parser);
    bool  ok   = !parser_failed(&parser);
    if (!ok) {
        printf("line %zu: %s\n", parser_error_line(&parser), parser_error(&parser));
    } else {
        stmt_print(stdout, stmt);
    }

    arena_term(&arena);
    return ok;
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
static bool cmd_sql(Db4 *db, const char *line) {
    Db4Stmt *stmt;
    if (!db4_prepare(db, line, strlen(line), &stmt, NULL)) {
        printf("%s\n", db4_errmsg(db));
        return false;
    }

    const Stmt *ast = db4_stmt_ast(stmt);
    bool is_select = ast->kind == STMT_SELECT;

    int rc = db4_step(stmt);
    if (rc == DB4_ERROR) {
        printf("%s\n", db4_errmsg(db));
        db4_finalize(stmt);
        return false;
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
            return false;
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
    return true;
}

/* Reads REPL lines from an already-open file (not stdin/linenoise - a
 * plain fixed-buffer fgets loop, portable ISO C, matching the same
 * "no BSD/GNU extension without the makefile already carving out a
 * feature-test-macro rule for it" stance cmd_ls settled on above) and
 * feeds each one through process_line, same as main()'s own loop or a
 * nested .read. Always stops at the first non-OK result - a script
 * failing partway should be visible, not silently skipped past, and a
 * .quit/.exit inside the file must propagate all the way back to
 * main() rather than just ending this one file's processing. */
static LineResult cmd_read(Db4 *db, const char *args) {
    char path[1024];
    const char *p = args;
    if (!take_quoted(&p, path, sizeof path)) {
        printf("usage: .read \"<path>\"\n");
        return LINE_FAIL;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("read: %s: %s\n", path, strerror(errno));
        return LINE_FAIL;
    }

    LineResult result = LINE_OK;
    char       line[8192];
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (line[0] == '\0') continue;

        result = process_line(db, line);
        if (result != LINE_OK) break;
    }
    fclose(f);
    return result;
}

static LineResult dispatch(Db4 *db, const char *line) {
    Catalog *catalog = &db->catalog;
    if (strncmp(line, ".load ", 6) == 0) {
        return load_csv(catalog, line + 6) ? LINE_OK : LINE_FAIL;
    } else if (strcmp(line, ".tables") == 0) {
        print_tables(catalog);
        return LINE_OK;
    } else if (strncmp(line, ".schema ", 8) == 0) {
        return cmd_schema(catalog, line + 8) ? LINE_OK : LINE_FAIL;
    } else if (strncmp(line, ".dump ", 6) == 0) {
        return cmd_dump(catalog, line + 6) ? LINE_OK : LINE_FAIL;
    } else if (strncmp(line, ".checkpoint ", 12) == 0) {
        return cmd_checkpoint(catalog, line + 12) ? LINE_OK : LINE_FAIL;
    } else if (strncmp(line, ".parse ", 7) == 0) {
        return cmd_parse(line + 7) ? LINE_OK : LINE_FAIL;
    } else if (strncmp(line, ".cd ", 4) == 0) {
        return cmd_cd(line + 4) ? LINE_OK : LINE_FAIL;
    } else if (strcmp(line, ".ls") == 0) {
        return cmd_ls("") ? LINE_OK : LINE_FAIL;
    } else if (strncmp(line, ".ls ", 4) == 0) {
        return cmd_ls(line + 4) ? LINE_OK : LINE_FAIL;
    } else if (strncmp(line, ".read ", 6) == 0) {
        return cmd_read(db, line + 6);
    } else {
        printf("unknown command: %s\n", line);
        return LINE_FAIL;
    }
}

/* The one place .quit/.exit is recognized - shared by main()'s own loop
 * and cmd_read, so a .quit reached inside a .read'd file (or a script
 * passed on the command line) means the same thing it does typed
 * directly: stop everything, not just the current file. */
static LineResult process_line(Db4 *db, const char *line) {
    if (strcmp(line, ".quit") == 0 || strcmp(line, ".exit") == 0) return LINE_QUIT;
    if (line[0] == '.') return dispatch(db, line);
    return cmd_sql(db, line) ? LINE_OK : LINE_FAIL;
}

/* --- Tab completion ---
 *
 * linenoise's completion callback (linenoise.h) takes only the buffer,
 * no user-data pointer, so - same reason interp.c's qsort comparator
 * reaches its sort state through file-scope statics rather than an
 * argument - the one Db4 this REPL ever has is reachable through a
 * file-scope pointer set once in main(), not threaded through. */
static Db4 *g_completion_db;

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* linenoise always completes at the end of the buffer (there's no
 * mid-line completion support here) - this finds where the identifier-
 * shaped word ending there begins, so completion can replace just that
 * trailing word instead of the whole line. */
static size_t word_start(const char *buf, size_t len) {
    size_t i = len;
    while (i > 0 && ident_char(buf[i - 1])) i--;
    return i;
}

static void add_completion_word(linenoiseCompletions *lc, const char *buf, size_t start, const char *word) {
    char full[1024];
    snprintf(full, sizeof full, "%.*s%s", (int)start, buf, word);
    linenoiseAddCompletion(lc, full);
}

static const char *DOT_COMMANDS[] = {
    ".load", ".tables", ".schema", ".dump", ".checkpoint", ".parse", ".cd", ".ls", ".read", ".quit", ".exit",
};

/* Only while the command word itself is still being typed (no space yet)
 * - completing an argument position is a different job, handled below by
 * complete_sql_word (table/column names) and complete_filename (paths). */
static void complete_dot_command(linenoiseCompletions *lc, const char *buf, size_t len) {
    for (size_t i = 0; i < sizeof(DOT_COMMANDS) / sizeof(DOT_COMMANDS[0]); i++)
        if (strncmp(DOT_COMMANDS[i], buf, len) == 0) linenoiseAddCompletion(lc, DOT_COMMANDS[i]);
}

/* SQL keywords (via lexer.c's own table, so the two can't drift) plus
 * every currently-loaded table and column name. Not scoped to whichever
 * table a FROM/JOIN/UPDATE already names - that would need real
 * position-aware parsing of the partial statement; offering the union of
 * every loaded table's columns is a deliberately simpler approximation.
 * This same word-completion also ends up serving dot-command arguments
 * for free (".schema cus<TAB>" completes against table names) since it
 * doesn't care what precedes the word - only what SQL surface exists. */
static void complete_sql_word(linenoiseCompletions *lc, const char *buf, size_t start) {
    const char *word     = buf + start;
    size_t      word_len = strlen(word);
    if (word_len == 0) return;

    for (size_t i = 0; i < lexer_keyword_count(); i++) {
        const char *kw = lexer_keyword_name(i);
        if (strncasecmp(kw, word, word_len) != 0) continue;
        char upper[32];
        size_t n = 0;
        for (; kw[n] && n + 1 < sizeof upper; n++) upper[n] = (char)toupper((unsigned char)kw[n]);
        upper[n] = '\0';
        add_completion_word(lc, buf, start, upper);
    }

    if (!g_completion_db) return;
    const Catalog *catalog = &g_completion_db->catalog;
    for (size_t t = 0; t < catalog->count; t++) {
        const char *tname = catalog->tables[t].name;
        if (strncmp(tname, word, word_len) == 0) add_completion_word(lc, buf, start, tname);

        const Table *table = &catalog->tables[t].table;
        for (size_t c = 0; c < table->n_cols; c++) {
            const char *cname = table->names[c];
            if (strncmp(cname, word, word_len) == 0) add_completion_word(lc, buf, start, cname);
        }
    }
}

/* Filename completion inside a quoted path argument - ".load"/".dump"'s
 * (see cmd_dump/load_csv), ".cd"/".ls"'s (see take_quoted's doc comment
 * for why those two are quoted too), and ".read"'s (cmd_read, below).
 * "Inside a quote" is
 * approximated as "an odd number of '"' characters seen so far" rather
 * than a real tokenizer; good enough for the one quoted argument these
 * commands actually take, and a misfire (e.g. mid-way through .load's
 * optional quoted JSON schema argument) just yields no matches rather
 * than anything harmful. */
static void complete_filename(linenoiseCompletions *lc, const char *buf) {
    static const char *PATH_COMMANDS[] = { ".load ", ".dump ", ".cd ", ".ls ", ".read " };
    bool in_path_command = false;
    for (size_t i = 0; i < sizeof(PATH_COMMANDS) / sizeof(PATH_COMMANDS[0]); i++)
        if (strncmp(buf, PATH_COMMANDS[i], strlen(PATH_COMMANDS[i])) == 0) { in_path_command = true; break; }
    if (!in_path_command) return;

    size_t quotes = 0, last_quote = 0;
    for (size_t i = 0; buf[i]; i++)
        if (buf[i] == '"') { quotes++; last_quote = i; }
    if (quotes % 2 == 0) return; /* not currently inside an open quote */

    const char *partial      = buf + last_quote + 1;
    const char *slash        = strrchr(partial, '/');
    size_t      dir_len      = slash ? (size_t)(slash - partial) + 1 : 0;
    const char *fname_prefix = slash ? slash + 1 : partial;
    size_t      prefix_len   = strlen(fname_prefix);

    char dir_path[768];
    if (dir_len > 0) snprintf(dir_path, sizeof dir_path, "%.*s", (int)dir_len, partial);
    else snprintf(dir_path, sizeof dir_path, ".");

    DIR *dp = opendir(dir_path);
    if (!dp) return;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strncmp(ent->d_name, fname_prefix, prefix_len) != 0) continue;
        if (fname_prefix[0] != '.' && ent->d_name[0] == '.') continue; /* hide dotfiles unless asked for */

        char stat_path[900];
        snprintf(stat_path, sizeof stat_path, "%.*s%s", (int)dir_len, partial, ent->d_name);
        struct stat st;
        bool is_dir = stat(stat_path, &st) == 0 && S_ISDIR(st.st_mode);

        char full[1024];
        snprintf(full, sizeof full, "%.*s%.*s%s%s",
                 (int)(last_quote + 1), buf, (int)dir_len, partial, ent->d_name, is_dir ? "/" : "");
        linenoiseAddCompletion(lc, full);
    }
    closedir(dp);
}

static void repl_completion(const char *buf, linenoiseCompletions *lc) {
    size_t len = strlen(buf);

    if (buf[0] == '.' && strchr(buf, ' ') == NULL) {
        complete_dot_command(lc, buf, len);
        return;
    }

    complete_filename(lc, buf);
    complete_sql_word(lc, buf, word_start(buf, len));
}

/* Given a file to run (bin/main <path>, as opposed to piping into stdin
 * or typing interactively), the simplest correct thing is to make that
 * file *be* stdin: linenoise already falls back to a plain non-editing
 * line reader (no "db4> " prompt, no history) whenever stdin isn't a
 * terminal - proven throughout this codebase's own test suite, which
 * pipes REPL scripts into bin/main this same way. freopen gets a script
 * argument the exact same treatment for free, no separate reading path
 * needed. */
int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "usage: %s [script]\n", argv[0]);
        return 1;
    }
    bool batch_mode = argc == 2;
    if (batch_mode && !freopen(argv[1], "r", stdin)) {
        fprintf(stderr, "%s: %s: %s\n", argv[0], argv[1], strerror(errno));
        return 1;
    }

    linenoiseHistorySetMaxLen(100);
    linenoiseSetCompletionCallback(repl_completion);

    Db4 db;
    db4_open(&db);
    g_completion_db = &db;

    /* A script (batch_mode) stops at its first failure, same as
     * cmd_read and matching sqlite3's own non-interactive default - a
     * script failing partway should be visible, not silently run to
     * completion past the error. Plain stdin (typed or piped, no
     * argument given) keeps the REPL's existing "print the error, keep
     * going" behavior unchanged - several of this project's own tests
     * rely on being able to see state after a rejected statement. */
    bool  had_error = false;
    char *line;
    while ((line = linenoise("db4> ")) != NULL) {
        if (line[0] == '\0') {
            linenoiseFree(line);
            continue;
        }

        linenoiseHistoryAdd(line);

        LineResult result = process_line(&db, line);
        linenoiseFree(line);

        if (result == LINE_QUIT) break;
        if (result == LINE_FAIL) {
            had_error = true;
            if (batch_mode) break;
        }
    }

    db4_close(&db);
    return had_error ? 1 : 0;
}
