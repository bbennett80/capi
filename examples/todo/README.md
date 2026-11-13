# Build a todo app for the web

Let's build **Little list**: a small todo app you use in your browser. You can
add tasks, mark them complete, change their titles, delete them, and filter
the list. SQLite saves your changes across restarts.

The application is C, the page is an HTML template, and the styling is a plain
CSS file you can edit. Ordinary HTML forms make everything work without
JavaScript, a frontend framework, or a CDN connection.

## Try the finished app first

From the capi repository:

```sh
make extras
./build-extras/capi_todo
```

Open **http://127.0.0.1:8080/** in your browser. Leave the server running in the
terminal; from here, use the page:

1. Type **Make time for a walk** and click **Add task**.
2. Click the square beside it to mark it complete. Click again to reopen it.
3. Open **Edit title**, change the text, and click **Save**.
4. Try **All**, **Active**, and **Completed**. The progress card updates too.
5. Use the **×** button to delete a task.

Press **Ctrl+C** in the terminal when you're finished. Run the command again
and your remaining tasks will still be there. The default database is `app.db`
in the directory from which you start the app. If port 8080 is busy, use
`./build-extras/capi_todo app.db 8081` and open port 8081 instead.

Ready to build your own? Follow the steps below in a fresh project. They use
`todo-web` so you can keep an earlier `todo-app` tutorial project untouched.

## 1. Install capi

You need Git, a C11 compiler, CMake 3.16+, curl, and SQLite development headers.

<details>
<summary>Install the prerequisites on macOS or Ubuntu</summary>

On macOS, install [Apple's command-line tools](https://developer.apple.com/documentation/xcode/installing-the-command-line-tools)
if needed, and install [CMake](https://formulae.brew.sh/formula/cmake) with
[Homebrew](https://brew.sh/). The macOS SDK supplies SQLite:

```sh
xcode-select --install
brew install cmake
```

Finish Apple's installer before continuing. Skip either command if that tool
is already installed.

On Ubuntu:

```sh
sudo apt update
sudo apt install build-essential cmake git curl libsqlite3-dev
```

Other Linux distributions provide equivalent compiler, CMake, and SQLite
development packages.

</details>

Create a tutorial workspace and get the framework:

```sh
mkdir -p "$HOME/capi-tutorial"
cd "$HOME/capi-tutorial"
git clone https://github.com/bbennett80/capi.git
cd capi
```

If you've already cloned capi, use your updated checkout instead. Enable both
SQLite and templates, then install into your user directory:

```sh
cmake -S . -B build-tutorial -DCMAKE_BUILD_TYPE=Release \
  -DCAPI_WITH_SQLITE=ON -DCAPI_WITH_TEMPLATES=ON \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-tutorial --parallel 2
cmake --install build-tutorial
```

No `sudo` is needed for this prefix. We'll use the full CLI path below so you
don't need to change your PATH.

## 2. Generate the todo resource

Create your own application next to the framework checkout:

```sh
cd ..
"$HOME/.local/bin/capi" new todo-web --sqlite
cd todo-web
"$HOME/.local/bin/capi" generate resource todos title:string completed:boolean
"$HOME/.local/bin/capi" routes
```

One command creates the SQLite schema, input validation, and six CRUD routes.
The title is required; `completed` is a boolean. IDs are automatic.

```text
GET     /todos
POST    /todos
GET     /todos/:id
PUT     /todos/:id
PATCH   /todos/:id
DELETE  /todos/:id
```

Those routes return JSON. Now we'll put an HTML page in front of the same
actions rather than making a separate database implementation.

## 3. Add the web layer

The finished files in this example save you from typing a whole controller
and stylesheet. While you're still inside the **fresh `todo-web` project**,
copy them across:

```sh
cp ../capi/examples/todo/main.c .
cp ../capi/examples/todo/web.c ../capi/examples/todo/web.h .
cp ../capi/examples/todo/CMakeLists.txt .
cp ../capi/examples/todo/resources/todos.c resources/todos.c
cp -R ../capi/examples/todo/views .
```

If your framework folder has another name, adjust `../capi`. These commands
replace the starter `main.c`, build file, and the controller you just generated;
use a new project so they don't overwrite your custom work. Your generated
header, resource manifest, and registration setup stay in place.

The todo controller differs from the CLI output in three small places:

- It includes `web.h` for the browser response helper.
- It lets the browser's POST update change only the supplied fields, like PATCH.
  The JSON PUT route still requires a full replacement.
- It adds three POST form routes that call the generated create/update/delete
  actions, then turn their results into a redirect or an HTML error page.

The completed `CMakeLists.txt` adds `web.c` and the `capi::templates` library.
It still reads `.capi/resources`, so resources you generate later are picked
up by the build.

Build and launch your app:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build --parallel 2
./build/app
```

Open the printed **http://127.0.0.1:8080/** address. You should see **Little list**,
an add-task form, an empty list, and a progress card. The remaining exercises
happen in your browser and editor.

## 4. Use your app in the browser

Add **Learn capi templates**, then add **Make this page my own**. Complete one
of them and switch between the filters. Open **Edit title** to rename a task;
its completion state stays unchanged. Delete whichever task you don't need.

The browser checks required titles before submission. The C controller also
validates input, so bypassing the browser's validation doesn't bypass the server.
Invalid requests return an HTML page with an error above the list.

Try refreshing after adding a task. It won't create a duplicate: a successful
form submission responds with **303 See Other**, and the browser follows it
with a GET for the updated page. Refresh repeats that GET, not the write.

Stop the server with **Ctrl+C**, run `./build/app` again from the same project
directory, and refresh the browser. Your tasks remain in `app.db`. No separate
database process is needed.

## 5. Understand the HTML template

Open [`views/index.html`](views/index.html). It is ordinary HTML with a few
placeholders. This is the add-task form:

```html
<form method="post" action="/ui/todos">
  <label for="new-title">What needs doing?</label>
  <input id="new-title" name="title" required maxlength="255">
  <input type="hidden" name="completed" value="false">
  <button type="submit">Add task</button>
</form>
```

The `name` attributes match the fields you generated. The hidden boolean starts
new tasks as active. The browser sends a normal URL-encoded form; there is no
fetch handler to write.

Find the list section:

```html
{{#todos}}
  <li class="task {{#completed}}is-complete{{/completed}}">
    <p class="task-title">{{title}}</p>
  </li>
{{/todos}}

{{^todos}}
  <p>Add your first task above.</p>
{{/todos}}
```

`{{#todos}}` repeats its contents for each row. `{{title}}` inserts escaped
text; a title containing `<b>` stays visible text rather than becoming HTML.
`{{#completed}}` adds a CSS class only for a completed task. `{{^todos}}` shows
an empty state when there are no rows in the current filter.

The full template wraps each completion, edit, and delete control in its own
form. For example, a completion button posts to `/ui/todos/{{id}}/update` with
only the new `completed` value. Editing a title posts only `title`, so the
two operations don't overwrite one another's fields.

In [`web.c`](web.c), `render_page` reads SQLite rows and builds the template
model using named C fields:

```c
row[0] = (capi_field){"id", capi_value_int(id)};
row[1] = (capi_field){"title", capi_value_string(title)};
row[2] = (capi_field){"completed", capi_value_bool(completed)};
```

It puts those row objects into a `todos` list and calls `capi_render`. The
template is compiled once at startup; each request supplies fresh data.
Lists show at most 100 matching tasks and say when more exist.

**Make one change:** edit the `<h1>` text to **A little progress, every day.**
Stop and restart the app, then refresh your browser. You changed your page
without changing its C controller.

## 6. Make the CSS yours

Open [`views/style.css`](views/style.css). The HTML loads it with:

```html
<link rel="stylesheet" href="/style.css">
```

`web.c` serves that exact route with `Content-Type: text/css`. It doesn't expose
a directory of arbitrary files. The stylesheet is loaded once at startup,
just like the HTML template; after edits, restart the server and refresh.
You don't need to recompile C for HTML or CSS changes.

Try changing these variables near the top of the stylesheet:

```css
:root {
  --paper: #f5f8fa;
  --green: #36566a;
  --green-soft: #e7eff5;
}
```

The background, primary button, selected filters, and completion controls now
use your new palette. Change `.task-title` to make task text larger, or adjust
the card's border radius. The media queries at the bottom stack the layout on
smaller screens. Try narrowing your browser window.

### Optional: a CSS framework or CDN

You can use any stylesheet with capi. There is no framework-specific frontend
API to learn. Vanilla CSS keeps this example offline and easy to inspect.

If you prefer Tailwind, follow the [official Tailwind CLI guide](https://tailwindcss.com/docs/installation/tailwind-cli)
to compile your utility classes into a local CSS file, then serve that file as
`views/style.css`. Add the corresponding utility classes to your template.
Keep the output within this example's 64 KiB CSS limit (or raise the explicit
limit in `todo_web_new`). This is an optional styling build step; application
request handling stays in C.

For a prebuilt CDN stylesheet, [Bootstrap's CSS-only option](https://getbootstrap.com/docs/5.3/getting-started/introduction/)
is another approach. Add its link **before** your own stylesheet so your
custom rules load afterward:

```html
<link
  href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.8/dist/css/bootstrap.min.css"
  rel="stylesheet"
  integrity="sha384-sRIl4kxILFvY47J16cr9ZwB07vP4J8+LH7qKQnuqkuIAvNWLzeN8tE5YBujZqJLB"
  crossorigin="anonymous">
<link rel="stylesheet" href="/style.css">
```

The app's Content Security Policy currently permits only local CSS. For this
CDN choice, change `style-src 'self'` in `browser_policy` in `web.c` to
`style-src 'self' https://cdn.jsdelivr.net`, keeping the other directives.
Rebuild C and restart the app after that policy change. Only the stylesheet
is needed; the forms don't require Bootstrap JavaScript. The CDN option needs
an internet connection; the default local stylesheet doesn't.

## How the pieces fit

| File | Responsibility |
| --- | --- |
| `main.c` | Open SQLite, load the web layer, register resources, start the server |
| `resources/todos.c` | Generated validation and CRUD actions, plus three browser adapters |
| `web.c` | Read the page model, render HTML, serve CSS, and handle browser responses |
| `views/index.html` | Page structure, forms, loops, and escaped values |
| `views/style.css` | Colors, spacing, controls, and responsive layout |

The form adapters are short. For example:

```c
static void browser_create(capi_ctx *ctx) {
    create_action(ctx);     /* the generated SQLite action */
    todo_web_result(ctx);  /* redirect on success, HTML on validation failure */
}
```

The original JSON endpoints are still available at `/todos` and `/todos/:id`.
Your web UI and API share validation, prepared statements, transactions, and
the same database. `capi routes` lists generated JSON routes; inspect the
controller's registration function for the three custom `/ui/` routes.

Use the exact printed `127.0.0.1` address: browser form writes must carry a
matching Origin. JSON clients can omit Origin, but a supplied foreign Origin is
rejected. This is a local app without authentication. Schema creation only
bootstraps missing tables; it doesn't migrate existing databases when fields change.

For more, see the [template guide](../../docs/templates.md),
[CLI guide](../../docs/cli.md), and [Fieldnotes example](../notes/README.md).
