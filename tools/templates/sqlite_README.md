# My capi API

Install capi with its SQLite component, then generate a resource here:

```sh
capi generate resource posts title:string body:text published:boolean
capi routes
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build
./build/app
```

The server opens `app.db`, creates missing tables, and listens on
http://127.0.0.1:8080. Stop with Ctrl+C. Optional arguments:
`./build/app /path/to/database.db 8081` (port 0 selects a free port).

```sh
curl --data-urlencode 'title=First post' --data-urlencode 'body=Hello!' \
  --data-urlencode 'published=false' http://127.0.0.1:8080/posts
curl 'http://127.0.0.1:8080/posts?limit=20&offset=0'
curl http://127.0.0.1:8080/posts/1
curl -X PATCH --data-urlencode 'published=true' http://127.0.0.1:8080/posts/1
curl -X DELETE http://127.0.0.1:8080/posts/1
```

Requests use URL-encoded forms; responses are JSON. POST and PUT require all
fields. PATCH changes only supplied fields and requires at least one known
field. Unknown fields are ignored; first duplicate values win. Strings contain
1–255 UTF-8 bytes; text accepts 0–4096. Integers are signed 64-bit decimal;
booleans accept `true`, `false`, `1`, or `0`. Text rejects NUL and control bytes
except tabs/newlines/CR. Lists default to 20 rows, descending by ID, and accept
limits 1–100 with nonnegative offsets. IDs are automatic positive integers.

Edit `resources/*.c` to customize validation, SQL, and handlers. The CLI creates
files once and refuses collisions. `.capi/resources` lists resources; CMake
generates only the registration glue in `build/resources.c`. After generating
another resource, `cmake --build build` automatically reconfigures.

The schema in each controller bootstraps missing tables; it is **not a migration
engine**. Changing generated fields does not alter an existing table. Plan and
apply schema changes explicitly before deploying them. SQLite runs synchronously
on the loop, with no busy waiting. Writes use explicit transactions; failed
validation/response construction rolls back before commit. A transport failure
after commit cannot undo a write.

This starter binds to loopback without authentication. The policy in `main.c`
rejects cross-origin browser writes when an Origin header is present; curl
doesn't need that header. Add your application's authentication and browser
access policy before exposing it to other users. No CORS headers are emitted.
