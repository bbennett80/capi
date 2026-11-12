# Fieldnotes

A persistent, server-rendered example combining capi's optional SQLite and
template libraries. All application logic is C11. There is no JavaScript or
frontend build step.

```sh
make extras
./build-extras/capi_notes
```

Open **http://127.0.0.1:8080**. Add and delete notes; restart the process and
saved notes remain. The database defaults to `notes.db` in the working directory.
Stop with Ctrl+C. The executable accepts:

```sh
./build-extras/capi_notes /path/to/notes.db 8081 /path/to/views
```

All arguments are optional, in that order. Port 0 chooses an ephemeral port.
The view directory defaults to the source tree path baked in by CMake; pass
it explicitly when moving the executable. Templates load once at startup.

The app demonstrates:

- URL-encoded form decoding, validation, and POST/redirect/GET.
- Bound SQL parameters and request-scoped statement finalization.
- Explicit commits and automatic rollback on early returns/errors.
- WAL configured at startup, foreign keys, and 503 responses on lock contention.
- Request-owned copies of SQLite columns for a template list model.
- Escaped interpolation, an empty-state section, and a shared card partial.
- Origin checks for POST, restrictive response headers, and graceful shutdown.

Only the latest 100 notes are displayed. Titles are limited to 120 bytes and
bodies to 4000 bytes. Times are UTC. Form limits are enforced on decoded byte
lengths; the browser's character limits are only a convenience.

This is a local example without authentication, bound to loopback. Use the exact
printed origin; opening it as `localhost` instead of `127.0.0.1` will fail POST
origin checks. Command-line clients include the Origin header explicitly:

```sh
curl -i -H 'Origin: http://127.0.0.1:8080' \
  --data-urlencode 'title=A thought worth keeping' \
  --data-urlencode 'body=Ordinary text, safely rendered.' \
  http://127.0.0.1:8080/notes
```

Integration tests use temporary databases and in-memory HTTP dispatch; they
cover persistence across restart, escaping, SQL-looking input, bad forms,
origin rejection, and deletion.
