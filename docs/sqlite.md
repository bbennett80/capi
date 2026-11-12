# SQLite integration

`capi::sqlite` is an optional, thin adapter around the native SQLite C API.
It adds request lifetimes and explicit transactions; SQL, statement stepping,
column access, result codes, and schema design remain ordinary SQLite.
There is no ORM, SQL interpolation, connection singleton, or hidden worker pool.

## Build

Install SQLite development headers/libraries using your system's package manager,
then enable the adapter. macOS's SDK already includes them.

```sh
cmake -S . -B build-extras -DCMAKE_BUILD_TYPE=Release -DCAPI_WITH_SQLITE=ON
cmake --build build-extras -j
ctest --test-dir build-extras --output-on-failure
./build-extras/capi_notes
```

SQLite 3.8.7 or newer is required. Nothing is downloaded or vendored by CMake.
Default builds do not search for or link SQLite. Vendored applications link
`capi::sqlite`; installed applications request the component:

```cmake
find_package(capi 0.2 CONFIG REQUIRED COMPONENTS sqlite)
target_link_libraries(app PRIVATE capi::sqlite)
```

`pkg-config --cflags --libs capi-sqlite` is available after installation.
The public header is [`capi_sqlite.h`](../include/capi_sqlite.h).

## Connections and setup

```c
sqlite3 *db = NULL;
int rc = capi_sqlite_open("app.db", &db);
if (rc != SQLITE_OK) {
    fprintf(stderr, "%s\n", db ? sqlite3_errmsg(db) : "open failed");
    sqlite3_close(db); /* also close handles returned by a failed open */
    return 1;
}
/* Keep db alive until all requests finish. Close it after the server stops. */
```

Open one connection per event-loop thread. The adapter opens read/write/create
with SQLite's per-connection mutex disabled, enables foreign keys and extended
result codes, and sets the busy timeout to zero. Do not use a connection
concurrently on different threads. Register its pointer or an application
struct as handler data; the framework does not own the connection.

WAL and durability policy are explicit application choices. For example,
`sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL)` can run at startup.
Check the result and choose a policy appropriate for your data. The notes
example enables WAL without changing SQLite's default synchronous setting.

## Statements and bound values

```c
static void create(capi_ctx *ctx) {
    sqlite3 *db = capi_data(ctx);
    sqlite3_stmt *stmt;
    int rc = capi_sqlite_prepare(ctx, db,
        "INSERT INTO messages(body) VALUES(?1)", &stmt);
    if (rc == SQLITE_OK) rc = capi_sqlite_bind_text(stmt, 1, capi_body(ctx));
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) { capi_text(ctx, 500, "Database operation failed\n"); return; }
    capi_text(ctx, 201, "Created\n");
}
```

Every request-prepared statement is finalized after all middleware has unwound,
including short-circuit and failed-response paths. Do not finalize these statements
yourself. Call `sqlite3_reset` if you need to release a cursor early; the eventual
finalize is still automatic. Preparing requires exactly one nonempty statement.
Trailing whitespace is accepted; extra statements and trailing comments are
rejected with `SQLITE_MISUSE`. Startup schema scripts can use native `sqlite3_exec`
with trusted SQL.

Bind positions are 1-based. `capi_sqlite_bind_text` and `capi_sqlite_bind_blob`
copy bytes using `SQLITE_TRANSIENT`. Empty input is an empty text/blob value,
including `{NULL, 0}`; use native `sqlite3_bind_null` for SQL NULL.
Lengths are explicit, embedded NUL bytes are preserved, and values over
`INT_MAX` return `SQLITE_TOOBIG`. Never build SQL by concatenating user input.

Native column views change when the cursor advances or resets. Use
`capi_sqlite_copy_text(ctx, stmt, column, &value)` when building template models
or retaining a value across rows. It copies into the request arena, preserves
lengths, and returns `{NULL, 0}` for SQL NULL. Columns are 0-based; the statement
must currently be on a row. Native integer, floating-point, and blob column
access remain available through SQLite itself.

## Transactions

```c
capi_sqlite_tx *tx;
int rc = capi_sqlite_begin(ctx, db, CAPI_SQLITE_IMMEDIATE, &tx);
if (rc != SQLITE_OK) { /* handle SQLITE_BUSY, etc. */ return; }

/* Prepare, bind, and step each write; check every result. */
if (operation_failed) return; /* cleanup rolls back */

rc = capi_sqlite_commit(tx);
if (rc != SQLITE_OK) return; /* cleanup still rolls back after failed commit */
capi_redirect(ctx, 303, "/");
```

DEFERRED and IMMEDIATE modes map to SQLite's corresponding BEGIN statements.
Transactions cannot nest; an already active transaction returns `SQLITE_MISUSE`.
Managed transactions must not be mixed with native BEGIN/COMMIT/ROLLBACK commands.
The transaction handle is request-owned. Only a successful explicit commit
disables cleanup rollback. `capi_sqlite_rollback` can end one early. A second
commit/rollback on an ended transaction returns `SQLITE_MISUSE`.

LIFO cleanup normally finalizes statements prepared inside a transaction before
rolling it back. Rollback failure marks the HTTP response failed. A response
error after a successful commit does not undo committed data; transactions
follow explicit application decisions, not inferred HTTP status codes.

## Latency and contention

SQLite executes synchronously on the handler's event-loop thread. A query,
disk operation, checkpoint, or commit can stall other connections on that loop.
A zero busy timeout prevents intentional lock-wait sleeping, but does not make
SQLite or disk I/O nonblocking. Keep statements short, bound result sets, and
measure storage latency. The adapter never retries a write automatically.

Handle `(rc & 0xff) == SQLITE_BUSY` or `SQLITE_LOCKED` explicitly; the notes
example returns 503 with `Retry-After: 1`. Do not set a long busy timeout in a
latency-sensitive event loop. For expensive storage work, use a separate service
or a future explicit asynchronous execution facility; this release does not
provide asynchronous handler completion. Multiple independent server processes
may open the same database, subject to SQLite's concurrency rules.

SQLite references: [preparing statements](https://www.sqlite.org/c3ref/prepare.html),
[binding and value lifetimes](https://www.sqlite.org/c3ref/bind_blob.html),
[column lifetimes](https://www.sqlite.org/c3ref/column_blob.html), and
[transaction behavior](https://www.sqlite.org/lang_transaction.html).
