# CLI and resource generators

The `capi` CLI creates projects and editable SQLite CRUD controllers. The CLI
itself is C11 and has no SQLite dependency; generated SQLite applications link
the optional `capi::sqlite` component. Templates are embedded in the executable,
so the installed CLI works independently of the source checkout.

## Install

From the capi repository:

```sh
cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Release \
  -DCAPI_WITH_SQLITE=ON -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-cli -j
cmake --install build-cli
```

The commands below assume `$HOME/.local/bin` is on your PATH. Otherwise use
`"$HOME/.local/bin/capi"` instead of `capi`.

## Start a CRUD API

Run in the directory where you want to create your application:

```sh
capi new my-api --sqlite
cd my-api
capi generate resource posts title:string body:text views:integer published:boolean
capi routes
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build
./build/app
```

Open http://127.0.0.1:8080/ for the hello endpoint. The process creates `app.db`
in its working directory. Stop with Ctrl+C. Optional launch arguments are
`./build/app /path/to/database.db 8081`; port 0 chooses an available port.
The existing `capi new my-service` command still creates a core-only hello app.

Try the generated resource in another terminal:

```sh
curl --data-urlencode 'title=First post' \
  --data-urlencode 'body=A note worth keeping.' \
  --data-urlencode 'views=0' --data-urlencode 'published=false' \
  http://127.0.0.1:8080/posts

curl 'http://127.0.0.1:8080/posts?limit=10&offset=0'
curl http://127.0.0.1:8080/posts/1
curl -X PATCH --data-urlencode 'published=true' http://127.0.0.1:8080/posts/1
curl -X DELETE http://127.0.0.1:8080/posts/1
```

## Commands

```text
capi new <directory> [--sqlite]
capi generate resource <plural_name> <field[:type]>... [--dry-run]
capi g resource <plural_name> <field[:type]>...
capi routes
capi --help
capi generate --help
capi --version
```

Use the exact collection name you want in your URL, such as `posts`, `people`,
or `purchase_orders`. There is no English pluralization heuristic. Resource and
field names must be lowercase ASCII identifiers (`[a-z][a-z0-9_]*`), at most
32 bytes. Up to 16 fields and 64 resources are supported. `id` is an automatic
positive integer primary key; do not declare it. Resource names starting with
SQLite's reserved `sqlite_` prefix are rejected. SQL identifiers are quoted, so
names such as a `select` field work safely.

`--dry-run` validates and prints the planned files and routes without writing
anything. Generating an existing resource fails without overwriting it. The
CLI also refuses existing files or symlinks at either output path. Publication
of the manifest uses an atomic rename, and generators hold a project lock to
prevent concurrent updates. Ordinary generation failures remove only files
created by that invocation. If the process is forcibly killed before manifest
publication, unregistered files may remain; inspect and remove/move those files
before trying again. There is no destructive `--force` option.

## Field types

| Type | Accepted form input | SQLite storage |
| --- | --- | --- |
| `string` (default) | 1–255 UTF-8 bytes | TEXT NOT NULL, byte-length check |
| `text` | 0–4096 UTF-8 bytes | TEXT NOT NULL, byte-length check |
| `integer` | Strict signed decimal in the int64 range | INTEGER NOT NULL, type check |
| `boolean` | `true`, `false`, `1`, or `0` | INTEGER NOT NULL, restricted to 0/1 |

Text rejects embedded NUL, invalid UTF-8, and ASCII control bytes other than
tab/newline/CR. No trimming or other silent normalization occurs. SQL values
always use bound parameters. JSON strings are escaped with `capi_json_string`;
boolean responses use JSON booleans, not strings or integers.

Requests use `application/x-www-form-urlencoded` bodies (charset parameters
are accepted). JSON request parsing is not included in this generator. POST
and PUT require every field. PATCH changes only supplied fields and requires
at least one recognized field. Unknown fields are ignored and the first
duplicate value wins, matching capi's form helper. Edit the controller to adopt
a stricter contract or add domain-specific validation.

## Generated routes

| Method | Path | Result |
| --- | --- | --- |
| GET | `/posts` | 200, JSON array; defaults to 20 rows, descending ID |
| GET | `/posts/:id` | 200, JSON object |
| POST | `/posts` | 201, created object and `Location` header |
| PUT | `/posts/:id` | 200, full replacement object |
| PATCH | `/posts/:id` | 200, updated object; omitted fields remain unchanged |
| DELETE | `/posts/:id` | 204, empty response |

Lists accept `limit` from 1 to 100 and nonnegative int64 `offset`. Invalid IDs
or pagination return 400. Missing rows return 404; missing/invalid fields return
422; unsupported form content types return 415; malformed percent encoding
returns 400. SQLite constraints return 409, lock contention returns 503 with
`Retry-After: 1`, and internal database failures produce a generic 500. Normal
framework HEAD, OPTIONS, and 405 behavior applies.

`capi routes` lists the hello endpoint and generated resource routes. It does
not introspect hand-written routes added elsewhere in your application.

## Files and ownership

```text
my-api/
  main.c                server, SQLite connection, request policy
  resources.h           declaration for resource registration
  CMakeLists.txt        explicit source list built from the manifest
  .capi/project         project format marker
  .capi/resources       one generated resource name per line
  resources/
    posts.h             registration declaration
    posts.c             schema, validation, SQL, six handlers, registration
```

Each controller is emitted once and is yours to edit. No generator runtime,
ORM, reflection, or code-generation step is involved in request handling.
`CMakeLists.txt` reads the manifest and writes only a small registration wrapper
to `build/resources.c`. It calls each controller's registration function during
startup. After generating another resource, `cmake --build build` automatically
reconfigures. The CLI never rewrites `main.c`, `CMakeLists.txt`, existing
controllers, or custom code. Custom registration belongs in `main.c`.

The template sources live in [`tools/templates`](../tools/templates). Tests
generate projects in directories with spaces, compile them with warnings as
errors, build after adding a resource, and exercise the actual generated C
against SQLite through `capi_dispatch`.

## Schema and transaction behavior

Each controller contains explicit `CREATE TABLE IF NOT EXISTS` SQL. Startup
bootstraps missing tables and checks expected column names, declared types,
NOT NULL constraints, and the integer primary key. It does not verify every
CHECK constraint or automatically alter existing tables. This is not a migration
engine: edits to generated fields require an explicit schema migration that you
plan and apply separately. IDs use SQLite AUTOINCREMENT to avoid reusing deleted
IDs. The starter explicitly enables WAL and leaves durability defaults intact.

Writes use request-scoped IMMEDIATE transactions and explicit commits. Response
construction happens before commit for create/update, so validation, allocation,
or serialization-helper failures roll back the write. A failure while sending
or serializing the final HTTP response after commit cannot undo it. Statements
finalize and uncommitted transactions roll back during request cleanup.

SQLite calls run synchronously with zero busy waiting. Keep queries bounded;
use the [worker API](workers.md) when storage latency requires background work.
The starter binds to loopback without authentication. Its middleware rejects
browser writes carrying an Origin different from the printed server origin;
command-line clients can omit Origin. Add your own authentication and browser
access policy when turning the scaffold into a multi-user service.
