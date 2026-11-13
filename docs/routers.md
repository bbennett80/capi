# Reusable routers

Routers let a module collect endpoints and shared middleware, then include the
group in an app under a prefix. They use the dependency-free core's existing
segment trie, handlers, and request APIs.

The organization is inspired by [FastAPI's APIRouter guide](https://fastapi.tiangolo.com/tutorial/bigger-applications/)
and [router API](https://fastapi.tiangolo.com/reference/apirouter/). In capi,
inclusion copies a **snapshot** at setup time. Free the builder once the app has
included it. Configure the app completely before dispatching or starting servers.

## Define a group

```c
#include <capi.h>

static void list_users(capi_ctx *ctx) { capi_json(ctx, 200, "[]"); }
static void show_user(capi_ctx *ctx) {
    capi_str id = capi_param(ctx, "id");
    capi_json(ctx, 200, "{\"id\":");
    capi_json_string(ctx, id);
    capi_write(ctx, "}", 1);
}

/* During application setup: */
capi_router *users = capi_router_new();
if (!users) { /* allocation failure */ }
else {
    int failed = capi_router_route(users, CAPI_GET, "", list_users, NULL) ||
                 capi_router_route(users, CAPI_GET, "/:id", show_user, NULL);
    if (failed) {
        /* Inspect capi_router_error(users), then abort setup. */
    } else if (capi_include_router(app, "/users", users)) {
        /* Inspect capi_app_error(app), then abort setup. */
    }
    capi_router_free(users);
}
```

This adds `GET /users` and `GET /users/:id`. Setup functions return 0 on success
and -1 on failure. Handle results marked `CAPI_NODISCARD` before serving.

## Prefixes and paths

| Local path | Include prefix | Effective path |
| --- | --- | --- |
| `""` | `/users` | `/users` |
| `/` | `/users` | `/users/` |
| `/:id` | `/users` | `/users/:id` |
| `/files/*rest` | `/api` | `/api/files/*rest` |
| `/:id` | `/org/:org/users` | `/org/:org/users/:id` |
| `""` | `""` or `/` | `/` |

An empty router path means the collection's base URL. `/` explicitly adds a
trailing slash below a prefix. capi keeps these URLs distinct. A router defining
both `""` and `/` cannot be included at the app root, where both would resolve
to `/`; that include fails without adding either route.

Prefixes must be empty, `/`, or absolute paths without trailing slashes.
They can contain `:parameters`, but not catch-all segments. Local routes support
terminal `*catch_all` segments. Names must be unique across the joined path;
the normal 16-parameter, 128-segment, and 4096-byte limits apply after joining.

`capi_path` remains the full request path. Parameter helpers see captures from
both prefix and local route. Requests use one trie lookup, with no path stripping
or nested HTTP dispatch.

## Shared middleware

```c
static void authorize(capi_ctx *ctx) {
    const char *token = capi_data(ctx);
    if (!capi_str_eq(capi_header(ctx, "Authorization"), token)) {
        capi_json(ctx, 401, "{\"error\":\"Unauthorized\"}");
        return;
    }
    capi_next(ctx);
}

/* Before including users in the app: */
if (capi_router_use(users, authorize, token)) { /* handle setup failure */ }
```

`capi_router_use(router, handler, data)` applies to all endpoints in the group,
including ones already declared in that builder. Call `capi_next` once to
continue or return to short-circuit. Each callback sees its own registration
data; `capi_data` is restored when `capi_next` returns. Request state and arena
allocations work throughout the pipeline.

Execution order is app middleware, outer router middleware, inner router
middleware, then the endpoint. Middleware within one group follows registration
order; synchronous code after `capi_next` unwinds in reverse. That unwind still
precedes async completion. Use `capi_on_finish` for final logging.

Router middleware belongs to matched endpoints. An unrelated route sharing
the same URL prefix doesn't inherit it. Neither do 404s, 405s, or automatic
OPTIONS responses, where no endpoint was selected. HEAD fallback uses the GET
endpoint's chain; explicitly registered OPTIONS/ANY handlers use their own.
Use app-level `capi_use` for policies that must also cover unmatched requests
or every request under a URL prefix.

## Nest and reuse

```c
/* Check each result before the next dependent setup step. */
int failed = capi_router_include(api, "/users", users);
if (!failed) failed = capi_include_router(app, "/api/v1", api);
if (!failed) failed = capi_include_router(app, "/api/latest", api);
if (failed) { /* abort setup; inspect the failing destination's error */ }
```

Both `/api/v1/users` and `/api/latest/users` expose the module. Each include
owns copies of joined paths, parameter names, and middleware bindings. A builder
can also be included in several apps.

Nested inclusion snapshots the child when it enters the parent. Later child
changes don't affect that parent. Including the parent in an app takes another
snapshot. Finish constructing nested groups before including them; additions
don't appear retroactively. Re-including all the same routes at an existing
prefix reports duplicates rather than replacing the previous snapshot.

Handler and middleware `void *data` pointers are **borrowed**, not deep-copied.
Their resources must live through requests and async cleanup. App limits,
error/finish hooks, and not-found handling remain app settings.

## Atomic setup and costs

`capi_include_router` and `capi_router_include` add the whole group or nothing.
Duplicate shapes/methods, malformed joined paths, parameter conflicts, and
allocation failures leave the destination's registered routes and middleware
unchanged. Parameter names don't distinguish shapes: `/:id` and `/:name` for
the same method conflict.

App inclusion clones the existing trie, adds the group to that staging tree,
and swaps trees only on success. Setup costs therefore scale with the existing
tree and new group. The request path keeps one lookup and flattened middleware
chains, with no router-specific request allocation.

A router supports 4096 routes and 128 middleware bindings per effective router
chain, in addition to the app's middleware limit. Failed includes consume
neither limit. Configure builders/apps on one setup thread, then freeze app
registration while dispatch or servers use it.

## A complete module example

See [`examples/routers`](../examples/routers/README.md): `users.c` builds the
group, `users.h` exports its constructor, and `main.c` includes it twice.
It needs only the core:

```sh
make
./build/capi_routers
```

Existing `capi_route`, generated resource registration functions, and optional
SQLite/template/worker libraries compose with included endpoints. No extra
package or component is needed.
