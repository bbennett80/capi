# Build a todo app with capi

Start with an empty folder and finish with a todo API that saves its data in
SQLite. You'll install capi, create your own project, generate six CRUD routes,
and use them to create, complete, rename, and delete a todo. Then you'll add
one route by hand to see how the generated application fits together.

All application code is C. We'll use `curl` as the client, so you don't need
a browser UI, JavaScript, or a frontend build step. You don't need to write SQL
or C code to reach the first working todo.

## 1. Get the tools

You need Git, a C11 compiler, CMake 3.16 or newer, curl, and SQLite development
headers. Choose the instructions for your system.

**macOS**

If you haven't installed Apple's command-line tools, run this and finish the
installer before continuing:

```sh
xcode-select --install
```

See [Apple's installation guide](https://developer.apple.com/documentation/xcode/installing-the-command-line-tools)
if needed. The macOS SDK supplies SQLite headers and libraries. Install CMake
with [Homebrew](https://brew.sh/) if you don't already have it:

```sh
brew install cmake
```

[Homebrew's CMake package](https://formulae.brew.sh/formula/cmake) provides the
`cmake` command. Git and curl should also be available in your terminal.

**Ubuntu**

```sh
sudo apt update
sudo apt install build-essential cmake git curl libsqlite3-dev
```

The [build-essential package](https://packages.ubuntu.com/noble/build-essential)
provides the compiler/build tools, and
[libsqlite3-dev](https://packages.ubuntu.com/noble/libsqlite3-dev) provides SQLite's
development files. On other Linux distributions, install the equivalent packages.

Check that the commands are available:

```sh
git --version
cc --version
cmake --version
curl --version
```

## 2. Install capi

We'll keep the framework and your application next to each other inside
`$HOME/capi-tutorial`. Use a fresh folder for your first trip through the guide.

```sh
mkdir -p "$HOME/capi-tutorial"
cd "$HOME/capi-tutorial"
git clone https://github.com/bbennett80/capi.git
cd capi
```

Build capi with its SQLite component and install it into your user directory:

```sh
cmake -S . -B build-tutorial \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAPI_WITH_SQLITE=ON \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-tutorial --parallel 2
cmake --install build-tutorial
```

`CAPI_WITH_SQLITE=ON` includes the database adapter our app will use. Installing
to `$HOME/.local` doesn't need `sudo`. Nothing in capi's build downloads packages.

Check the installed CLI:

```sh
"$HOME/.local/bin/capi" --version
```

You'll see `capi` followed by its version. Throughout this guide we use the
full CLI path so you don't need to change your shell's PATH. If `capi` is already
on your PATH, you can type `capi` instead.

## 3. Create your project

Move back to the tutorial folder and create a SQLite application:

```sh
cd "$HOME/capi-tutorial"
"$HOME/.local/bin/capi" new todo-app --sqlite
cd todo-app
```

Your folders now look like this:

```text
capi-tutorial/
  capi/       the framework you installed
  todo-app/   your application; work here from now on
```

The `--sqlite` starter includes a server, a database connection, and the build
setup for generated resources. Your application is a separate project; you
won't edit the framework to add features.

## 4. Generate the todo routes

A todo needs a title and a flag saying whether it's completed. Preview what
the generator will create:

```sh
"$HOME/.local/bin/capi" generate resource todos title:string completed:boolean --dry-run
```

The preview prints the planned files and routes without writing anything.
Now run the same command without `--dry-run`:

```sh
"$HOME/.local/bin/capi" generate resource todos title:string completed:boolean
"$HOME/.local/bin/capi" routes
```

The route listing includes:

```text
GET     /
GET     /todos
POST    /todos
GET     /todos/:id
PUT     /todos/:id
PATCH   /todos/:id
DELETE  /todos/:id
```

`todos` is the collection name and URL. Each row gets an automatic `id`.
`title:string` accepts a nonempty UTF-8 title of up to 255 bytes, and
`completed:boolean` accepts `true` or `false` (also `1` or `0`).

The generator creates `resources/todos.c` and `resources/todos.h`, then adds
`todos` to `.capi/resources`. CMake uses that manifest to build and register
your new routes automatically.

## 5. Build and start the app — Terminal 1

Stay in `todo-app`:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build --parallel 2
./build/app
```

The prefix tells CMake where you installed capi. The server prints:

```text
Listening on http://127.0.0.1:8080
```

Leave this terminal running. On first startup, the app creates `app.db` in your
`todo-app` directory and creates its `todos` table. You don't need a separate
database server or a separate migration command for this initial schema.

## 6. Create and read a todo — Terminal 2

Open a second terminal for the following requests. Keep Terminal 1 running.
The curl commands work from any directory.

First check the server:

```sh
curl http://127.0.0.1:8080/
```

```json
{"hello":"world"}
```

List your todos. A new database starts empty:

```sh
curl http://127.0.0.1:8080/todos
```

```json
[]
```

Create the first todo:

```sh
curl --data-urlencode 'title=Learn capi' \
  --data-urlencode 'completed=false' \
  http://127.0.0.1:8080/todos
```

```json
{"id":1,"title":"Learn capi","completed":false}
```

curl selects POST when you supply `--data-urlencode`. This sends a URL-encoded
form, which is what the generated controller accepts. Responses are JSON. Both
fields are required when creating a todo; there is no implicit boolean default.

The examples use ID `1` because this is a fresh database. If you're repeating
the tutorial, use the ID returned by your create request. Deleted IDs are not
reused. curl's output may run into your next prompt; that's normal for JSON
responses without a trailing newline. Add `-i` to curl to also see HTTP headers.

Read just that todo:

```sh
curl http://127.0.0.1:8080/todos/1
```

List the collection again:

```sh
curl 'http://127.0.0.1:8080/todos?limit=10&offset=0'
```

```json
[{"id":1,"title":"Learn capi","completed":false}]
```

Lists default to 20 items, newest first. You can request 1–100 items with
`limit`, and skip items with `offset`. Quote URLs containing `&` so your shell
doesn't interpret it as a background command.

## 7. Complete and rename it

Use PATCH to change one field without resending the title:

```sh
curl -X PATCH --data-urlencode 'completed=true' \
  http://127.0.0.1:8080/todos/1
```

```json
{"id":1,"title":"Learn capi","completed":true}
```

Use PUT when you want to replace all editable fields:

```sh
curl -X PUT --data-urlencode 'title=Build my first capi app' \
  --data-urlencode 'completed=true' \
  http://127.0.0.1:8080/todos/1
```

```json
{"id":1,"title":"Build my first capi app","completed":true}
```

PATCH only requires the fields you're changing. PUT requires both `title` and
`completed`; omitting either returns a validation error.

Try an empty title to see validation work:

```sh
curl -i --data-urlencode 'title=' --data-urlencode 'completed=false' \
  http://127.0.0.1:8080/todos
```

You'll get HTTP **422** and this JSON body:

```json
{"error":"Invalid or missing field","field":"title"}
```

The invalid todo isn't saved. A successful create returns 201, reads and updates
return 200, and deleting an existing todo returns 204.

## 8. See persistence across a restart

In **Terminal 1**, press **Ctrl+C** to stop the server, then start it again from
the same `todo-app` directory:

```sh
./build/app
```

In **Terminal 2**, fetch the todo again:

```sh
curl http://127.0.0.1:8080/todos/1
```

The renamed, completed todo is still there. SQLite saved it in `app.db`; it
wasn't just stored in server memory. The starter's `.gitignore` excludes the
default database and its temporary SQLite files.

Now delete it:

```sh
curl -i -X DELETE http://127.0.0.1:8080/todos/1
```

HTTP **204 No Content** has an empty body. Fetching `/todos/1` now returns 404.
Listing the collection shows it's empty again:

```sh
curl http://127.0.0.1:8080/todos
```

```json
[]
```

## 9. Make it your own with one C route

Look at the application files:

| File | What you'll find |
| --- | --- |
| `main.c` | Server startup, the SQLite connection, and common request policy |
| `resources/todos.c` | Schema SQL, validation, JSON output, handlers, and route registration |
| `resources/todos.h` | The controller's registration declaration |
| `.capi/resources` | Generated resource names used by the build |
| `build/resources.c` | Small generated wrapper that registers the resources |

The controller is ordinary C source you can read and edit. In `todos.c`,
`index_action` lists todos, `show_action` reads one, `create_action` inserts one,
`update_action` handles PUT/PATCH, and `destroy_action` deletes one. SQL values
are bound parameters, and request cleanup finalizes statements and rolls back
uncommitted transactions.

Let's add a custom health endpoint. In `main.c`, add this function above `main`:

```c
static void health(capi_ctx *ctx) {
    capi_json(ctx, 200, "{\"status\":\"ok\"}");
}
```

Find the setup `if` that calls `capi_use`, `capi_route`, and
`app_resources_register`. Add a route to that chain so the block reads:

```c
if (capi_use(app, "/", policy, origin) ||
    capi_route(app, CAPI_GET, "/", hello, NULL) ||
    capi_route(app, CAPI_GET, "/health", health, NULL) ||
    app_resources_register(app, db)) goto fail;
```

Stop the app in **Terminal 1**, rebuild, and restart it:

```sh
cmake --build build --parallel 2
./build/app
```

Then try it in **Terminal 2**:

```sh
curl http://127.0.0.1:8080/health
```

```json
{"status":"ok"}
```

You now have both CLI-generated routes and a hand-written route in the same
app. `capi routes` lists generated resources and the starter's `/` endpoint;
custom routes such as `/health` are kept in your C code and aren't shown there.

The generator never overwrites existing controllers. Edit generated C files to
change behavior; don't rerun generation over the same resource. The schema
creates missing tables but doesn't alter existing ones. Adding fields to an
existing database needs an explicit schema migration.

## If something gets stuck

| Symptom | What to check |
| --- | --- |
| `cmake` or `cc` not found | Complete the prerequisites for your OS in step 1. |
| `Could NOT find SQLite3` | Install SQLite development headers, then configure capi again with `-DCAPI_WITH_SQLITE=ON`. |
| CMake can't find `capiConfig.cmake` or the `sqlite` component | Repeat capi's build/install, and pass `-DCMAKE_PREFIX_PATH="$HOME/.local"` when configuring the app. |
| The CLI doesn't recognize `--sqlite` or `generate` | Install the current checkout and use `"$HOME/.local/bin/capi"`; an older CLI may be earlier on PATH. |
| Generator says to run inside a project | `cd` into `todo-app`, where `.capi/project` lives. |
| Port 8080 is busy | Run `./build/app app.db 8081` and use port 8081 in every curl URL. |
| A request returns 415 | Use `--data-urlencode`; the generated controller expects a form rather than a JSON request body. |
| A request for ID 1 returns 404 | Use the ID from your create response; IDs aren't reused after deletion. |

This starter is a local API without authentication. For browser-based HTML,
continue with the [Fieldnotes example](../notes/README.md) and the
[template guide](../../docs/templates.md). For more fields, generator options,
and customization, see the [CLI guide](../../docs/cli.md) and
[API guide](../../docs/api.md).
