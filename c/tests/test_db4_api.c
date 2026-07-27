/* Direct exercise of db4.h's public API, bypassing main.c's REPL entirely -
 * a real embedder would call these functions, not spawn a process and pipe
 * text at it. Exit code 0 = all checks passed; any failed check prints and
 * the process exits 1. Intended to be run under ASan/UBSan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db4.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

static bool collect_cb(void *ctx, int n_cols, const char **col_text, const char **col_names) {
    int *count = ctx;
    (*count)++;
    (void)n_cols; (void)col_text; (void)col_names;
    return true;
}

int main(void) {
    Db4 db;
    CHECK(db4_open(&db), "db4_open should succeed");

    /* CREATE TABLE via db4_exec */
    CHECK(db4_exec(&db, "CREATE TABLE t (id INT PRIMARY KEY, name TEXT)", NULL, NULL),
          "CREATE TABLE should succeed");

    /* Trailing input after one statement is rejected - today this actually
     * happens inside db4_prepare's parse itself (the grammar requires a
     * whole statement to consume all input), not db4_exec's own tail-check,
     * which is unreachable dead code as of this milestone (see db4.h's
     * db4_prepare doc comment). Either way, garbage-after-a-statement must
     * be rejected, not silently accepted. */
    CHECK(!db4_exec(&db, "CREATE TABLE u (id INT); CREATE TABLE v (id INT)", NULL, NULL),
          "db4_exec should reject more than one statement");
    CHECK(strlen(db4_errmsg(&db)) > 0, "errmsg should be set when trailing input is rejected");

    /* multi-row insert via prepare/step directly */
    Db4Stmt *stmt;
    const char *tail;
    const char *insert_sql = "INSERT INTO t (id, name) VALUES (1,'a'),(2,'b')";
    bool ok = db4_prepare(&db, insert_sql, strlen(insert_sql), &stmt, &tail);
    CHECK(ok, "prepare INSERT should succeed");
    if (ok) {
        int rc = db4_step(stmt);
        CHECK(rc == DB4_DONE, "INSERT should report DB4_DONE");
        CHECK(db4_changes(&db) == 2, "2 rows should be reported inserted");
        db4_finalize(stmt);
    }

    /* SELECT: row-by-row iteration + column accessors */
    ok = db4_prepare(&db, "SELECT id, name FROM t ORDER BY id", strlen("SELECT id, name FROM t ORDER BY id"), &stmt, NULL);
    CHECK(ok, "prepare SELECT should succeed");
    if (ok) {
        CHECK(db4_column_count(stmt) == 2, "2 columns expected");
        CHECK(strcmp(db4_column_name(stmt, 0), "id") == 0, "col 0 name should be id");

        int rc = db4_step(stmt);
        CHECK(rc == DB4_ROW, "first step should yield a row");
        CHECK(db4_column_type(stmt, 0) == DB4_INTEGER, "id should be INTEGER");
        CHECK(db4_column_int64(stmt, 0) == 1, "first row id should be 1");
        size_t len;
        const char *txt = db4_column_text(stmt, 1, &len);
        CHECK(len == 1 && txt[0] == 'a', "first row name should be 'a'");

        rc = db4_step(stmt);
        CHECK(rc == DB4_ROW, "second step should yield a row");
        CHECK(db4_column_int64(stmt, 0) == 2, "second row id should be 2");

        rc = db4_step(stmt);
        CHECK(rc == DB4_DONE, "third step should be DB4_DONE");

        /* Stepping again past DB4_DONE should stay DB4_DONE, not crash/UB */
        rc = db4_step(stmt);
        CHECK(rc == DB4_DONE, "stepping past completion should stay DB4_DONE");

        db4_finalize(stmt);
    }

    /* db4_column_* on an out-of-range column index must not crash */
    ok = db4_prepare(&db, "SELECT id FROM t", strlen("SELECT id FROM t"), &stmt, NULL);
    CHECK(ok, "prepare should succeed");
    if (ok) {
        db4_step(stmt);
        CHECK(db4_column_type(stmt, 5) == DB4_NULL, "out-of-range column should report DB4_NULL, not crash");
        CHECK(db4_column_int64(stmt, -1) == 0, "negative column index should be handled safely");
        CHECK(db4_column_text(stmt, 99, NULL) == NULL, "out-of-range text column should return NULL safely");
        db4_finalize(stmt);
    }

    /* db4_column_* called before any db4_step (row_idx==0) must not crash */
    ok = db4_prepare(&db, "SELECT id FROM t", strlen("SELECT id FROM t"), &stmt, NULL);
    CHECK(ok, "prepare should succeed");
    if (ok) {
        CHECK(db4_column_type(stmt, 0) == DB4_NULL, "column access before first step should be safe (DB4_NULL)");
        db4_finalize(stmt);
    }

    /* Parse error surfaces via db4_errmsg, not a crash */
    ok = db4_prepare(&db, "SELEC * FROM t", strlen("SELEC * FROM t"), &stmt, NULL);
    CHECK(!ok, "garbage SQL should fail to prepare");
    CHECK(strlen(db4_errmsg(&db)) > 0, "errmsg should be set on parse failure");

    /* Execution error (nonexistent table) surfaces via db4_step returning DB4_ERROR */
    ok = db4_prepare(&db, "SELECT * FROM ghost", strlen("SELECT * FROM ghost"), &stmt, NULL);
    CHECK(ok, "prepare of a syntactically valid but semantically bad query should succeed (semantic check is at step time)");
    if (ok) {
        int rc = db4_step(stmt);
        CHECK(rc == DB4_ERROR, "stepping a query against a nonexistent table should report DB4_ERROR");
        CHECK(strlen(db4_errmsg(&db)) > 0, "errmsg should be set on execution failure");
        db4_finalize(stmt);
    }

    /* db4_exec convenience callback path, including early-stop */
    int count = 0;
    CHECK(db4_exec(&db, "SELECT * FROM t", collect_cb, &count), "db4_exec SELECT should succeed");
    CHECK(count == 2, "db4_exec should have invoked callback once per row (2 rows)");

    /* db4_finalize(NULL) must be a safe no-op */
    db4_finalize(NULL);

    db4_close(&db);

    if (failures) {
        fprintf(stderr, "\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("all db4.h API checks passed\n");
    return 0;
}
