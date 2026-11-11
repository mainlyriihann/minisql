# minisql — A Small SQL RDBMS Engine in C++17

A from-scratch relational database engine: a hand-written SQL lexer/parser,
a query executor, page-free heap-file table storage with binary disk
persistence, and a real B+Tree index on each table's primary key. No
external database libraries are used — only the C++ standard library.

## Features

- **SQL subset**: `CREATE TABLE`, `DROP TABLE`, `INSERT`, `SELECT`, `UPDATE`,
  `DELETE`, `SHOW TABLES`, `DESCRIBE`.
- **Data types**: `INT`, `FLOAT`, `TEXT`, `BOOL`, with `NULL`.
- **Constraints**: `PRIMARY KEY` (uniqueness, enforced via the index),
  `NOT NULL`.
- **WHERE expressions**: `=`, `!=`/`<>`, `<`, `<=`, `>`, `>=`, `AND`, `OR`,
  `NOT`, parentheses, arithmetic (`+ - * /`), proper SQL three-valued logic
  for `NULL` (`NULL = x` is unknown/false, `NOT (NULL = x)` is also
  unknown/false — not true).
- **JOIN**: `INNER JOIN ... ON ...` and `LEFT JOIN ... ON ...`, table aliases
  (`AS` or bare), qualified column references (`table.col`).
- **ORDER BY** (multiple columns, `ASC`/`DESC`), **LIMIT**.
- **Indexing**: a real templated B+Tree (`include/BPlusTree.hpp`) maps each
  table's primary key to its row slot. `SELECT ... WHERE pk = value` (or an
  `AND`-conjunct containing that equality) is automatically served by an
  O(log n) index lookup instead of a full table scan — see
  `Executor::extractPKEquality`. Every other query path does a heap scan.
- **Persistence**: each table is written to `<dbdir>/<table>.tbl` in a
  compact binary format; `<dbdir>/catalog.meta` lists the tables in a
  database. Data survives process restarts; the B+Tree index is rebuilt in
  memory on load.
- **REPL**: multi-line statement entry, `.tables`, `.help`, `.exit`.
- **Script mode**: `minisql <dbdir> -f script.sql` runs a whole file;
  statements are parsed and executed one at a time, so a mistake in one
  statement is reported and does **not** prevent the rest of the script from
  running (like `sqlite3`'s CLI).

## Architecture

```
include/
  Types.hpp       Value (tagged union), Column, Schema, DataType
  Row.hpp         Row = vector<Value>, binary (de)serialization
  BPlusTree.hpp   Templated in-memory B+Tree (insert/find/remove/rangeScan)
  Table.hpp       Heap-file row storage + PK index + disk load/save
  Catalog.hpp     Owns all Tables; database-directory persistence
  Lexer.hpp       Tokenizer
  AST.hpp         Statement / Expr node definitions
  Parser.hpp      Recursive-descent parser (precedence climbing for exprs)
  Expression.hpp  WHERE/SET/ON expression evaluation, multi-table context
  Executor.hpp    Executes AST against the Catalog; produces QueryResult
src/
  main.cpp        REPL + script runner + ASCII table pretty-printer
tests/
  demo.sql        End-to-end feature walkthrough
```

**Query execution model**: `SELECT` builds an ordered list of "sources"
(the `FROM` table plus each `JOIN`), computes a nested-loop join by
extending a list of `JoinRow`s (one slice of matched row-pointers per
source) one join clause at a time, filters with `WHERE`, sorts for
`ORDER BY`, truncates for `LIMIT`, then projects the requested columns.
`LEFT JOIN` unmatched rows are backed by synthetic all-`NULL` rows held in a
`std::deque` (stable references, unlike `std::vector`, across further
`push_back`s during the same query).

**Storage model**: a table is a `vector<Row>` (a "heap file") plus a
parallel `vector<bool>` marking which slots are live. Deletes tombstone a
slot and push it onto a free-list for reuse by future inserts, so B+Tree
row-ids (which point at slot indices) never dangle.

## Building

Requires a C++17 compiler (tested with g++ 13). No external dependencies,
no CMake required — just `make`:

```sh
make            # builds build/minisql
```

## Running

```sh
./build/minisql                      # REPL, persists to ./data
./build/minisql mydb                 # REPL, persists to ./mydb
./build/minisql mydb -f script.sql   # run a script against ./mydb, then exit
```

Example session:

```sql
minisql> CREATE TABLE users (id INT PRIMARY KEY, name TEXT NOT NULL, age INT);
Table 'users' created.
minisql> INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 25);
2 row(s) inserted into 'users'.
minisql> SELECT * FROM users WHERE age > 26;
+----+-------+-----+
| id | name  | age |
+----+-------+-----+
| 1  | Alice | 30  |
+----+-------+-----+
1 row(s) returned.
```

See `tests/demo.sql` for a full walkthrough covering every feature
(joins, aliases, ORDER BY, LIMIT, UPDATE, DELETE, persistence).

## Testing this build

The engine has been:
- Unit-tested standalone (B+Tree: sequential/random insert, duplicate
  rejection, range scans, removal, string keys — 1000+ keys).
- Exercised end-to-end via `tests/demo.sql` (schema, multi-row insert,
  `WHERE`, `ORDER BY`/`LIMIT`, `INNER`/`LEFT JOIN`, aliases, `UPDATE`,
  `DELETE`, and a fresh-process reload proving persistence).
- Checked against edge cases: duplicate primary keys, `NOT NULL`/type
  violations, missing tables/columns, division by zero, `IF [NOT]
  EXISTS`, multiple `PRIMARY KEY` columns (rejected), operator precedence
  (`AND` binds tighter than `OR`), and SQL `NULL` three-valued logic.
- Scale-tested with 100,000 rows: bulk insert plus indexed point lookups,
  and a fresh-process reload with re-verified lookup correctness.
- Run clean under **AddressSanitizer + UndefinedBehaviorSanitizer** across
  all of the above, including a mixed insert/delete/reinsert/update stress
  script exercising B+Tree removal and free-list slot reuse — no memory
  errors, no leaks, no UB.
- Compiles warning-free under `-Wall -Wextra -Wpedantic`.

## Known limitations (by design, for a project of this scope)

- No transactions/rollback across multiple INSERT rows in one statement (a
  later row failing does not undo earlier rows in the same statement).
- The B+Tree does not merge/rebalance nodes on delete (entries are simply
  removed from their leaf); this preserves lookup correctness but can leave
  under-full nodes after heavy deletion.
- Only one `PRIMARY KEY` per table; no secondary indexes, no `FOREIGN KEY`,
  no `GROUP BY`/aggregate functions, no nested subqueries.
- Single-process, single-threaded; no concurrency control or locking.
- The on-disk format is a simple custom binary layout, not a page-based
  buffer pool — fine for the data sizes a project like this targets, not
  meant to scale to real database workloads.
