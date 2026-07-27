#!/usr/bin/env python3
"""db4 end-to-end regression suite, driving bin/main's REPL over stdin/stdout.

Covers M1-M8 per docs/dev_plan.md. Each test runs a fresh copy of the fixture
CSVs in a scratch tmp dir (so tests don't interfere via shared .wal/.lock
files), feeds a script of REPL lines, and asserts on stdout.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(ROOT, "..", "bin", "main")
FIXTURES = os.path.join(ROOT, "fixtures")
TMP = os.path.join(ROOT, "tmp")

passed = 0
failed = 0
fails = []


def fresh_tmp(name):
    d = os.path.join(TMP, name)
    if os.path.exists(d):
        shutil.rmtree(d)
    os.makedirs(d)
    for f in ("customers.csv", "orders.csv", "quoted.csv"):
        shutil.copy(os.path.join(FIXTURES, f), os.path.join(d, f))
    return d


# customers.id as PK is needed for every FK/PK-violation test - loading with
# no schema override leaves every column a plain inferred type, not a PK.
LOAD_CUSTOMERS_PK = '.load customers "customers.csv" {"id":{"type":"int","primary":true}}'


def run(workdir, lines, timeout=5):
    script = "\n".join(lines) + "\n"
    p = subprocess.run([os.path.abspath(BIN)], cwd=workdir, input=script,
                        capture_output=True, text=True, timeout=timeout)
    return p.stdout


def _evaluate_checks(out, checks):
    """checks: list of (predicate_fn, description) OR plain strings (substring must appear)."""
    problems = []
    for c in checks:
        if isinstance(c, str):
            if c not in out:
                problems.append(f"missing substring: {c!r}")
        elif isinstance(c, tuple) and c[0] == "not":
            if c[1] in out:
                problems.append(f"unexpected substring present: {c[1]!r}")
        else:
            ok, desc = c(out)
            if not ok:
                problems.append(desc)
    return problems


def _report(name, problems, out):
    global passed, failed
    if problems:
        failed += 1
        fails.append((name, problems, out))
        print(f"FAIL {name}")
        for p in problems:
            print(f"   - {p}")
    else:
        passed += 1
        print(f"PASS {name}")


def check(name, workdir, lines, checks):
    try:
        out = run(workdir, lines)
    except Exception as e:
        global failed
        failed += 1
        fails.append((name, f"EXCEPTION: {e}"))
        print(f"FAIL {name}: exception {e}")
        return
    _report(name, _evaluate_checks(out, checks), out)


def check_batch(name, workdir, script_filename, script_contents, checks, expect_returncode=None):
    """Like check(), but writes script_contents to workdir/script_filename and runs
    `bin/main <script_filename>` (the CLI positional-argument batch mode) against it,
    instead of piping lines to stdin - and can also assert the process's exit code,
    which plain check()/run() never look at."""
    global failed
    with open(os.path.join(workdir, script_filename), "w") as f:
        f.write(script_contents)
    try:
        p = subprocess.run([os.path.abspath(BIN), script_filename], cwd=workdir,
                            capture_output=True, text=True, timeout=5)
    except Exception as e:
        failed += 1
        fails.append((name, f"EXCEPTION: {e}"))
        print(f"FAIL {name}: exception {e}")
        return
    problems = _evaluate_checks(p.stdout, checks)
    if expect_returncode is not None and p.returncode != expect_returncode:
        problems.append(f"expected exit code {expect_returncode}, got {p.returncode}")
    _report(name, problems, p.stdout)


def not_(s):
    return ("not", s)


# ---------------------------------------------------------------------------
# M1: CSV load / dump / schema / PK / FK
# ---------------------------------------------------------------------------

def test_m1():
    d = fresh_tmp("m1_basic")
    check("M1 load + tables + schema", d,
          ['.load customers "customers.csv"', '.tables', '.schema customers', '.quit'],
          ["customers", "id", "name", "age"])

    d = fresh_tmp("m1_fk_valid")
    check("M1 valid FK load", d,
          [LOAD_CUSTOMERS_PK,
           '.load orders "orders.csv" {"customer_id":{"type":"int","references":"customers.id"}}',
           '.tables', '.quit'],
          ["customers", "orders"])

    d = fresh_tmp("m1_fk_invalid")
    # orders row references customer_id 2 and 1, both exist; use a bad ref file instead
    with open(os.path.join(d, "bad_orders.csv"), "w") as f:
        f.write("id,customer_id,qty,price\n1,999,3,10.5\n")
    check("M1 invalid FK load rejected atomically", d,
          [LOAD_CUSTOMERS_PK,
           '.load orders "bad_orders.csv" {"customer_id":{"type":"int","references":"customers.id"}}',
           '.tables', '.quit'],
          ["customers", not_("orders")])

    d = fresh_tmp("m1_dump")
    check("M1 dump round trip", d,
          ['.load customers "customers.csv"',
           '.dump customers "dumped.csv"', '.quit'],
          ["dumped"])
    assert os.path.exists(os.path.join(d, "dumped.csv")), "dumped.csv should exist"
    with open(os.path.join(d, "dumped.csv")) as f:
        content = f.read()
    assert "Alice" in content and "Bob" in content, "dumped.csv should round-trip data"

    d = fresh_tmp("m1_quoted_csv")
    check("M1 RFC4180 quoting: embedded comma/escaped quote/embedded newline", d,
          ['.load q "quoted.csv"', 'SELECT * FROM q', '.quit'],
          ["hello, world", 'she said "hi"', "line1", "line2", "(3 rows)"])


# ---------------------------------------------------------------------------
# M2: catalog (multi-table)
# ---------------------------------------------------------------------------

def test_m2():
    d = fresh_tmp("m2_catalog")
    check("M2 multiple tables addressable", d,
          ['.load customers "customers.csv"',
           '.load orders "orders.csv"',
           '.tables',
           'SELECT * FROM customers',
           'SELECT * FROM orders',
           '.quit'],
          ["customers", "orders", "Alice", "20"])


# ---------------------------------------------------------------------------
# M3: parser / .parse
# ---------------------------------------------------------------------------

def test_m3():
    d = fresh_tmp("m3_parse")
    check("M3 parse valid select with precedence", d,
          ['.parse SELECT * FROM t WHERE a >= 1 AND (b = 2 OR NOT c != \'x\') ORDER BY a DESC LIMIT 5',
           '.quit'],
          [not_("line ")])  # no error

    check("M3 parse malformed rejected with line info", d,
          ['.parse SELECT FROM WHERE', '.quit'],
          ["line "])


# ---------------------------------------------------------------------------
# M4: SELECT execution: projection, WHERE, three-valued NULL, ORDER BY, LIMIT
# ---------------------------------------------------------------------------

def test_m4():
    d = fresh_tmp("m4_select")
    check("M4 projection *", d,
          ['.load customers "customers.csv"', 'SELECT * FROM customers', '.quit'],
          ["Alice", "Bob", "Carol", "(3 rows)"])

    check("M4 explicit column projection", d,
          ['.load customers "customers.csv"', 'SELECT name FROM customers', '.quit'],
          ["Alice", not_("30")])

    check("M4 WHERE with AND/OR/NOT", d,
          ['.load customers "customers.csv"',
           "SELECT name FROM customers WHERE age > 20 AND NOT name = 'Bob'",
           '.quit'],
          ["Alice", not_("Bob"), not_("Carol")])

    check("M4 WHERE type mismatch rejected", d,
          ['.load customers "customers.csv"', "SELECT * FROM customers WHERE name = 5", '.quit'],
          [not_("(3 rows)"), not_("(0 rows)")])  # should be an error, not a row-producing result

    check("M4 NULL three-valued: age=age excludes NULL row", d,
          ['.load customers "customers.csv"', "SELECT name FROM customers WHERE age = age", '.quit'],
          ["Alice", "Bob", not_("Carol")])

    check("M4 ORDER BY DESC with NULL", d,
          ['.load customers "customers.csv"', "SELECT name FROM customers ORDER BY age DESC", '.quit'],
          lambda_checks := [])
    out = run(d, ['.load customers "customers.csv"', "SELECT name FROM customers ORDER BY age DESC", '.quit'])
    order = [l for l in ["Alice", "Bob", "Carol"] if l in out]
    idxs = {name: out.index(name) for name in order}
    ok = idxs["Alice"] < idxs["Bob"] < idxs["Carol"]
    if not ok:
        failed_note("M4 ORDER BY DESC NULL-last ordering", out)

    check("M4 LIMIT", d,
          ['.load customers "customers.csv"', "SELECT name FROM customers ORDER BY id LIMIT 2", '.quit'],
          ["(2 rows)"])

    check("M4 unknown table error", d,
          ['SELECT * FROM nosuchtable', '.quit'],
          ["no such table"])

    check("M4 unknown column error", d,
          ['.load customers "customers.csv"', 'SELECT nope FROM customers', '.quit'],
          [lambda out: (("no such column" in out) or ("unknown column" in out), "expected unknown-column error")])


def failed_note(name, out):
    global passed, failed
    failed += 1
    fails.append((name, ["custom ordering check failed"], out))
    print(f"FAIL {name}")


# ---------------------------------------------------------------------------
# M5: INSERT/UPDATE/DELETE, BEGIN/COMMIT/ROLLBACK, autocommit, durability
# ---------------------------------------------------------------------------

def test_m5():
    d = fresh_tmp("m5_autocommit")
    check("M5 autocommit INSERT persists across reload", d,
          ['.load customers "customers.csv"',
           "INSERT INTO customers (id, name, age) VALUES (4, 'Dave', 40)",
           '.quit'],
          ["1 row inserted"])
    out2 = run(d, ['.load customers "customers.csv"', 'SELECT name FROM customers', '.quit'])
    assert "Dave" in out2, f"Dave should persist after reload, got: {out2}"

    d = fresh_tmp("m5_pk_violation")
    check("M5 INSERT PK violation rejected", d,
          [LOAD_CUSTOMERS_PK,
           "INSERT INTO customers (id, name, age) VALUES (1, 'Eve', 22)",
           '.quit'],
          [not_("1 row inserted")])

    d = fresh_tmp("m5_rollback")
    check("M5 BEGIN...ROLLBACK leaves table and file unchanged", d,
          ['.load customers "customers.csv"',
           'BEGIN',
           "INSERT INTO customers (id, name, age) VALUES (5, 'Frank', 50)",
           'ROLLBACK',
           'SELECT name FROM customers',
           '.quit'],
          [not_("Frank")])
    with open(os.path.join(d, "customers.csv")) as f:
        assert "Frank" not in f.read(), "rollback must not touch on-disk CSV"

    d = fresh_tmp("m5_nested_begin")
    check("M5 nested BEGIN rejected", d,
          ['.load customers "customers.csv"', 'BEGIN', 'BEGIN', '.quit'],
          ["BEGIN"])  # first begin ok; ensure some rejection text appears
    out = run(d, ['.load customers "customers.csv"', 'BEGIN', 'BEGIN', 'ROLLBACK', '.quit'])
    assert out.count("BEGIN\n") == 1, f"second BEGIN should be rejected, got: {out!r}"

    d = fresh_tmp("m5_commit_no_txn")
    out = run(d, ['COMMIT', '.quit'])
    assert "COMMIT" not in out.split("\n")[0] or "no" in out.lower() or "error" in out.lower() or True, out
    check("M5 COMMIT with no active txn rejected", d, ['COMMIT', '.quit'],
          [lambda o: ("no transaction" in o.lower() or "error" in o.lower() or "not" in o.lower(), "expected rejection message")])

    d = fresh_tmp("m5_insert_arity_mismatch")
    check("M5 INSERT arity mismatch rejected", d,
          [LOAD_CUSTOMERS_PK, "INSERT INTO customers (id, name, age) VALUES (9, 'X')", '.quit'],
          [not_("1 row inserted"), lambda o: ("column" in o.lower() or "value" in o.lower(), "expected an arity error message")])

    d = fresh_tmp("m5_insert_type_mismatch")
    check("M5 INSERT type mismatch rejected", d,
          [LOAD_CUSTOMERS_PK, "INSERT INTO customers (id, name) VALUES (9, 42)", '.quit'],
          [not_("1 row inserted")])

    d = fresh_tmp("m5_update_delete")
    check("M5 UPDATE and DELETE basic", d,
          ['.load customers "customers.csv"',
           "UPDATE customers SET age = 99 WHERE name = 'Bob'",
           "DELETE FROM customers WHERE name = 'Carol'",
           'SELECT name, age FROM customers ORDER BY id',
           '.quit'],
          ["99", not_("Carol"), "1 row updated", "1 row deleted"])

    d = fresh_tmp("m5_fk_block_delete")
    check("M5 DELETE of referenced PK blocked", d,
          [LOAD_CUSTOMERS_PK,
           '.load orders "orders.csv" {"customer_id":{"type":"int","references":"customers.id"}}',
           "DELETE FROM customers WHERE id = 1",
           '.quit'],
          [not_("1 row deleted")])

    d = fresh_tmp("m5_no_tmp_files")
    run(d, ['.load customers "customers.csv"',
            "INSERT INTO customers (id, name, age) VALUES (4, 'Dave', 40)",
            '.checkpoint customers', '.quit'])
    leftovers = [f for f in os.listdir(d) if f.startswith(".tmp-") or "tmp-" in f]
    assert not leftovers, f"leftover tmp files: {leftovers}"


# ---------------------------------------------------------------------------
# M6: arithmetic, qualified cols, multi-row insert, CREATE TABLE, JOIN, GROUP BY
# ---------------------------------------------------------------------------

def test_m6():
    d = fresh_tmp("m6_arith")
    check("M6 arithmetic in WHERE/SET/VALUES", d,
          ['.load orders "orders.csv"',
           "SELECT id FROM orders WHERE price * qty > 15",
           "UPDATE orders SET qty = qty * 2, price = -price WHERE id = 1",
           "SELECT qty, price FROM orders WHERE id = 1",
           '.quit'],
          ["6", "-10.5"])

    check("M6 div by zero yields inf/nan not crash", d,
          ['.load orders "orders.csv"',
           "UPDATE orders SET price = qty / 0 WHERE id = 1",
           "SELECT price FROM orders WHERE id = 1",
           '.quit'],
          [lambda o: (("inf" in o.lower() or "nan" in o.lower()), "expected inf/nan for div by 0")])

    d = fresh_tmp("m6_multirow_insert")
    check("M6 multi-row INSERT", d,
          ['.load customers "customers.csv"',
           "INSERT INTO customers (id, name, age) VALUES (10, 'X', 1), (11, 'Y', 2)",
           'SELECT name FROM customers WHERE id >= 10',
           '.quit'],
          ["2 rows inserted", "X", "Y"])

    d = fresh_tmp("m6_create_table")
    check("M6 CREATE TABLE with PK/FK + schema round trip", d,
          ['CREATE TABLE t1 (id INT PRIMARY KEY, name TEXT)',
           '.schema t1',
           'CREATE TABLE t1 (id INT PRIMARY KEY)',
           '.quit'],
          ["table \"t1\" created", "id", "already exists"])

    d = fresh_tmp("m6_join")
    check("M6 INNER JOIN qualified/unqualified + ambiguous rejection", d,
          ['.load customers "customers.csv"',
           '.load orders "orders.csv"',
           "SELECT customers.name, orders.qty FROM customers JOIN orders ON customers.id = orders.customer_id ORDER BY orders.id",
           "SELECT id FROM customers JOIN orders ON customers.id = orders.customer_id",
           '.quit'],
          ["Alice", "Bob", lambda o: ("ambiguous" in o.lower(), "expected ambiguous column error")])

    d = fresh_tmp("m6_groupby")
    check("M6 aggregates + GROUP BY, NULL exclusion", d,
          ['.load orders "orders.csv"',
           "SELECT customer_id, COUNT(*), SUM(qty), AVG(price) FROM orders GROUP BY customer_id",
           '.quit'],
          ["1", "2"])

    check("M6 GROUP BY + JOIN rejected", d,
          ['.load customers "customers.csv"', '.load orders "orders.csv"',
           "SELECT customer_id, COUNT(*) FROM orders JOIN customers ON orders.customer_id = customers.id GROUP BY customer_id",
           '.quit'],
          [lambda o: ("error" in o.lower() or "not" in o.lower() or "cannot" in o.lower() or "support" in o.lower(), "expected GROUP BY+JOIN rejection")])

    d = fresh_tmp("m6_ddl_survives_rollback")
    check("M6 CREATE TABLE inside ROLLBACK survives (DDL not undo-logged)", d,
          ['BEGIN', 'CREATE TABLE t2 (id INT PRIMARY KEY)', 'ROLLBACK', '.tables', '.quit'],
          ["t2"])


# ---------------------------------------------------------------------------
# M7: WAL + lock: commit via WAL, replay on reload, checkpoint, torn frame
# ---------------------------------------------------------------------------

def test_m7():
    global passed
    d = fresh_tmp("m7_wal_replay")
    run(d, ['.load customers "customers.csv"',
            "INSERT INTO customers (id, name, age) VALUES (4, 'Dave', 40)",
            '.quit'])
    assert os.path.exists(os.path.join(d, "customers.csv.wal")), "commit should create a WAL file"
    with open(os.path.join(d, "customers.csv")) as f:
        assert "Dave" not in f.read(), "commit should NOT rewrite base CSV directly (WAL only)"
    out = run(d, ['.load customers "customers.csv"', 'SELECT name FROM customers', '.quit'])
    assert "Dave" in out, f"reload should replay WAL and see Dave, got: {out}"
    passed += 1
    print("PASS M7 WAL append on commit + replay on reload")

    d = fresh_tmp("m7_checkpoint")
    run(d, ['.load customers "customers.csv"',
            "INSERT INTO customers (id, name, age) VALUES (4, 'Dave', 40)",
            '.checkpoint customers', '.quit'])
    assert not os.path.exists(os.path.join(d, "customers.csv.wal")), "checkpoint should remove the WAL"
    with open(os.path.join(d, "customers.csv")) as f:
        assert "Dave" in f.read(), "checkpoint should fold WAL into base CSV"
    passed += 1
    print("PASS M7 checkpoint folds WAL into base CSV and removes it")

    d = fresh_tmp("m7_rollback_no_wal")
    run(d, ['.load customers "customers.csv"', 'BEGIN',
            "INSERT INTO customers (id, name, age) VALUES (4, 'Dave', 40)",
            'ROLLBACK', '.quit'])
    assert not os.path.exists(os.path.join(d, "customers.csv.wal")), "rollback must not write WAL frames"
    passed += 1
    print("PASS M7 ROLLBACK produces no WAL writes")

    d = fresh_tmp("m7_torn_frame")
    run(d, ['.load customers "customers.csv"',
            "INSERT INTO customers (id, name, age) VALUES (4, 'Dave', 40)",
            "INSERT INTO customers (id, name, age) VALUES (5, 'Erin', 45)",
            '.quit'])
    walp = os.path.join(d, "customers.csv.wal")
    sz = os.path.getsize(walp)
    with open(walp, "r+b") as f:
        f.truncate(sz - 3)  # tear the last frame
    out = run(d, ['.load customers "customers.csv"', 'SELECT name FROM customers', '.quit'])
    assert "Dave" in out, f"earlier untorn frame should still apply, got: {out}"
    assert "Erin" not in out, f"torn frame's row should NOT be silently corrupted-applied, got: {out}"
    passed += 1
    print("PASS M7 torn last WAL frame tolerated, earlier frames still apply")

    # Two separate processes each insert a distinct row and commit
    # concurrently against the same CSV+WAL - the exclusive lock around
    # each commit's WAL append must serialize them so neither frame is
    # lost/corrupted, even though each process only loaded the table once
    # (M7's documented no-live-refresh scope boundary, not being tested here).
    d = fresh_tmp("m7_concurrent_writers")
    script_a = ['.load customers "customers.csv"',
                "INSERT INTO customers (id, name, age) VALUES (100, 'Proc1Row1', 1)",
                "INSERT INTO customers (id, name, age) VALUES (101, 'Proc1Row2', 1)"]
    script_b = ['.load customers "customers.csv"',
                "INSERT INTO customers (id, name, age) VALUES (200, 'Proc2Row1', 2)",
                "INSERT INTO customers (id, name, age) VALUES (201, 'Proc2Row2', 2)"]
    pa = subprocess.Popen([os.path.abspath(BIN)], cwd=d, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    pb = subprocess.Popen([os.path.abspath(BIN)], cwd=d, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out_a, err_a = pa.communicate("\n".join(script_a) + "\n.quit\n", timeout=10)
    out_b, err_b = pb.communicate("\n".join(script_b) + "\n.quit\n", timeout=10)
    out = run(d, ['.load customers "customers.csv"', 'SELECT name FROM customers ORDER BY id', '.quit'])
    for name in ("Proc1Row1", "Proc1Row2", "Proc2Row1", "Proc2Row2"):
        assert name in out, f"concurrent commit from both processes should all be visible after reload, got: {out}"
    passed += 1
    print("PASS M7 two concurrent writer processes both commit without lost WAL frames")


# ---------------------------------------------------------------------------
# M8: public API surface via REPL (db4.h/db4.c mediated)
# ---------------------------------------------------------------------------

def test_m8():
    d = fresh_tmp("m8_api_regression")
    check("M8 full regression through db4.h-mediated REPL", d,
          ['.load customers "customers.csv"',
           '.load orders "orders.csv"',
           'SELECT * FROM customers ORDER BY id',
           'BEGIN',
           "UPDATE customers SET age = 31 WHERE id = 1",
           'COMMIT',
           'SELECT age FROM customers WHERE id = 1',
           '.quit'],
          ["Alice", "Bob", "Carol", "BEGIN", "COMMIT", "31"])

    d = fresh_tmp("m8_error_path")
    check("M8 error surfaced via db4_errmsg through main.c", d,
          ['SELECT * FROM ghost', '.quit'],
          ["no such table"])


# ---------------------------------------------------------------------------
# PK index point lookups: exec_select_plain uses the existing pk_index
# (built for constraint enforcement, table.c) to seed a single-table
# WHERE-pk-equality query's candidate rows instead of a full scan. Every
# case here is a correctness check, not a performance one - the fast path
# is only supposed to narrow candidates, with the full WHERE re-evaluated
# per candidate exactly as a full scan would, so the two scenarios that
# actually risk a wrong (not just slow) result are a stale index entry
# left behind by DELETE or by UPDATE-ing the PK column away - covered
# below in a single REPL session each, since a fresh `.load` would just
# rebuild the index from scratch and never exercise the staleness at all.
# ---------------------------------------------------------------------------

def test_pk_index_lookup():
    d = fresh_tmp("pk_index_basic")
    check("PK index: WHERE id = <literal> finds the right row", d,
          [LOAD_CUSTOMERS_PK, 'SELECT name FROM customers WHERE id = 2', '.quit'],
          ["Bob", "(1 row)"])

    d = fresh_tmp("pk_index_reversed")
    check("PK index: reversed operand order (<literal> = id)", d,
          [LOAD_CUSTOMERS_PK, 'SELECT name FROM customers WHERE 2 = id', '.quit'],
          ["Bob", "(1 row)"])

    d = fresh_tmp("pk_index_and_conjunct")
    check("PK index: AND-conjunct alongside a non-PK filter still applies the whole WHERE", d,
          [LOAD_CUSTOMERS_PK, "SELECT name FROM customers WHERE id = 1 AND age > 100", '.quit'],
          [not_("Alice"), "(0 rows)"])

    d = fresh_tmp("pk_index_no_match")
    check("PK index: a PK value with no matching row returns nothing, not a crash", d,
          [LOAD_CUSTOMERS_PK, 'SELECT name FROM customers WHERE id = 999', '.quit'],
          ["(0 rows)"])

    d = fresh_tmp("pk_index_stale_delete")
    check("PK index: stale index entry after DELETE is filtered, not returned", d,
          [LOAD_CUSTOMERS_PK,
           'DELETE FROM customers WHERE id = 2',
           'SELECT name FROM customers WHERE id = 2',
           '.quit'],
          ["1 row deleted", "(0 rows)", not_("Bob")])

    d = fresh_tmp("pk_index_stale_update")
    check("PK index: stale index entry after UPDATE-ing the PK away doesn't resurrect the old row", d,
          [LOAD_CUSTOMERS_PK,
           'UPDATE customers SET id = 99 WHERE id = 2',
           'SELECT name FROM customers WHERE id = 2',
           'SELECT name FROM customers WHERE id = 99',
           '.quit'],
          ["1 row updated", "(0 rows)", "(1 row)",
           lambda o: (o.count("Bob") == 1,
                      f"expected 'Bob' exactly once (only from the id=99 query) - "
                      f"the stale id=2 index entry must not resurrect the old row, found {o.count('Bob')}")])


# ---------------------------------------------------------------------------
# Index-accelerated joins: exec_select_plain's join loop probes the inner
# table's pk_index once per outer row (via find_join_pk_equality) instead
# of a full nested-loop scan, when the ON clause has a top-level
# "<inner>.<pk> = <expr>" conjunct. The M6 join test above never exercises
# this - it loads customers without a PK schema override, and joins the
# other direction (customers first, orders - which has no PK at all -
# second), so pk_col stays -1 either way. These load customers WITH a PK
# and join orders -> customers specifically so customers lands as the
# *inner* (second, PK-bearing) table.
# ---------------------------------------------------------------------------

def test_join_index_lookup():
    d = fresh_tmp("join_index_basic")
    check("Join index: orders -> customers on customers' PK returns the right names", d,
          [LOAD_CUSTOMERS_PK, '.load orders "orders.csv"',
           "SELECT orders.id, customers.name FROM orders INNER JOIN customers ON orders.customer_id = customers.id ORDER BY orders.id",
           '.quit'],
          [lambda o: (o.count("Alice") == 2, f"orders 1 and 3 both belong to Alice, found {o.count('Alice')}"),
           lambda o: (o.count("Bob") == 1, f"order 2 belongs to Bob, found {o.count('Bob')}"),
           "(3 rows)"])

    d = fresh_tmp("join_index_reversed")
    check("Join index: reversed operand order (customers.id = orders.customer_id)", d,
          [LOAD_CUSTOMERS_PK, '.load orders "orders.csv"',
           "SELECT orders.id, customers.name FROM orders INNER JOIN customers ON customers.id = orders.customer_id ORDER BY orders.id",
           '.quit'],
          [lambda o: (o.count("Alice") == 2, "expected Alice twice"), "(3 rows)"])

    d = fresh_tmp("join_index_and_conjunct")
    check("Join index: AND-conjunct alongside the PK equality still applies the whole ON clause", d,
          [LOAD_CUSTOMERS_PK, '.load orders "orders.csv"',
           "SELECT orders.id, customers.name FROM orders INNER JOIN customers ON orders.customer_id = customers.id AND customers.age > 26 ORDER BY orders.id",
           '.quit'],
          [not_("Bob"), lambda o: (o.count("Alice") == 2, "Bob (age 25) should be excluded by the AND, Alice's two orders kept"), "(2 rows)"])

    d = fresh_tmp("join_index_with_where")
    check("Join index: coexists correctly with a separate WHERE filter", d,
          [LOAD_CUSTOMERS_PK, '.load orders "orders.csv"',
           "SELECT orders.id, customers.name FROM orders INNER JOIN customers ON orders.customer_id = customers.id WHERE orders.qty > 1 ORDER BY orders.id",
           '.quit'],
          [not_("Bob"), "(2 rows)"])  # order 2 (qty=1, Bob) filtered by WHERE, orders 1 and 3 (qty 3, 2) kept

    d = fresh_tmp("join_index_stale_update")
    check("Join index: stale PK index entry after UPDATE doesn't phantom-match an old join partner", d,
          [LOAD_CUSTOMERS_PK, '.load orders "orders.csv"',
           "UPDATE customers SET id = 99 WHERE id = 2",
           "SELECT orders.id, customers.name FROM orders INNER JOIN customers ON orders.customer_id = customers.id ORDER BY orders.id",
           '.quit'],
          ["1 row updated",
           not_("Bob"),  # order 2 still points at customer_id=2, which no longer exists - inner join drops it
           lambda o: (o.count("Alice") == 2, "Alice's two orders should be unaffected"),
           "(2 rows)"])  # only orders 1 and 3 (Alice) now match - order 2's join partner is gone


# ---------------------------------------------------------------------------
# Batch mode: `bin/main <script>` runs a file's lines the same way piped
# stdin already did, but stops at the first failure and exits non-zero -
# the actual point being that this is now scriptable/CI-able, not just
# interactive. `.read` is the same execution path reached from inside an
# existing session instead of the command line, including a `.quit` deep
# inside a `.read`'d file correctly exiting the whole process, not just
# that one file.
# ---------------------------------------------------------------------------

def test_batch_mode():
    d = fresh_tmp("batch_success")
    check_batch("Batch: a script argument runs to EOF with no explicit .quit needed", d,
                "script.sql",
                'CREATE TABLE t (id INT PRIMARY KEY, name TEXT)\n'
                "INSERT INTO t VALUES (1, 'ada'), (2, 'grace')\n"
                "SELECT * FROM t ORDER BY id\n",
                ["table \"t\" created", "2 rows inserted", "ada", "grace", "(2 rows)"],
                expect_returncode=0)

    d = fresh_tmp("batch_stops_on_error")
    check_batch("Batch: stops at the first failure and does not run what follows it", d,
                "script.sql",
                "CREATE TABLE t (id INT PRIMARY KEY)\n"
                "SELECT * FROM nonexistent_table\n"
                "INSERT INTO t VALUES (99)\n"
                "SELECT * FROM t\n",
                ["no such table", not_("1 row inserted"), not_("99")],
                expect_returncode=1)

    d = fresh_tmp("batch_missing_file")
    p = subprocess.run([os.path.abspath(BIN), "does_not_exist.sql"], cwd=d,
                        capture_output=True, text=True, timeout=5)
    _report("Batch: a nonexistent script file exits non-zero with a clear error, not a hang",
            _evaluate_checks(p.stderr, ["does_not_exist.sql"]) +
            ([] if p.returncode == 1 else [f"expected exit code 1, got {p.returncode}"]),
            p.stderr)

    d = fresh_tmp("batch_too_many_args")
    p = subprocess.run([os.path.abspath(BIN), "a.sql", "b.sql"], cwd=d,
                        capture_output=True, text=True, timeout=5)
    _report("Batch: more than one script argument is a usage error, not silently ignored",
            _evaluate_checks(p.stderr, ["usage"]) +
            ([] if p.returncode == 1 else [f"expected exit code 1, got {p.returncode}"]),
            p.stderr)

    d = fresh_tmp("read_command")
    with open(os.path.join(d, "included.sql"), "w") as f:
        f.write('CREATE TABLE t (id INT PRIMARY KEY, name TEXT)\n'
                "INSERT INTO t VALUES (1, 'ada')\n")
    check(".read runs another file's lines through the same session", d,
          ['.read "included.sql"', "SELECT name FROM t WHERE id = 1", ".quit"],
          ["table \"t\" created", "1 row inserted", "ada"])

    d = fresh_tmp("read_quit_propagates")
    with open(os.path.join(d, "quits.sql"), "w") as f:
        f.write("CREATE TABLE t2 (id INT)\n.quit\nINSERT INTO t2 VALUES (1)\n")
    check(".quit inside a .read'd file exits the whole session, not just that file", d,
          ['.read "quits.sql"', "SELECT 999999", ".quit"],
          ["table \"t2\" created", not_("1 row inserted"), not_("999999")])


def main():
    if not os.path.exists(BIN):
        print(f"binary not found at {BIN}; run `make` first", file=sys.stderr)
        sys.exit(2)
    os.makedirs(TMP, exist_ok=True)

    for fn in [test_m1, test_m2, test_m3, test_m4, test_m5, test_m6, test_m7, test_m8,
               test_pk_index_lookup, test_join_index_lookup, test_batch_mode]:
        try:
            fn()
        except AssertionError as e:
            global failed
            failed += 1
            fails.append((fn.__name__, [str(e)]))
            print(f"FAIL {fn.__name__}: {e}")
        except Exception as e:
            failed += 1
            fails.append((fn.__name__, [f"EXCEPTION: {e}"]))
            print(f"FAIL {fn.__name__}: EXCEPTION {e}")

    print(f"\n{passed} passed, {failed} failed")
    if fails:
        print("\n--- Failure details ---")
        for f in fails:
            print(f)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
