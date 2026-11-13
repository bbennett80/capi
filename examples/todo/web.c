#include "web.h"
#include <capi_template.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct todo_web {
    sqlite3 *db; /* borrowed; one connection on this event loop */
    capi_template *page;
    char *css;
    size_t css_length;
    char origin[128];
};
static const char *selected_filter(capi_ctx *ctx) {
    capi_str value;
    capi_input_result rc = capi_str_eq(capi_request_method(ctx), "POST") ?
        capi_form(ctx, "filter", &value) : capi_query(ctx, "filter", &value);
    if (rc == CAPI_INPUT_FOUND && capi_str_eq(value, "active")) return "active";
    if (rc == CAPI_INPUT_FOUND && capi_str_eq(value, "completed")) return "completed";
    return "all";
}
static void database_failed(capi_ctx *ctx, todo_web *web, int rc) {
    fprintf(stderr, "todo page: SQLite %d: %s\n", rc, sqlite3_errmsg(web->db));
    capi_fail_with(ctx, CAPI_ERROR_DATABASE, "todo_page");
}
static void render_page(capi_ctx *ctx, int status, const char *error) {
    todo_web *web = capi_state(ctx);
    if (!web) { capi_fail_with(ctx, CAPI_ERROR_ARGUMENT, "todo_web_state"); return; }
    const char *filter = selected_filter(ctx);
    sqlite3_stmt *stmt;
    int rc = capi_sqlite_prepare(ctx, web->db, "SELECT count(*),coalesce(sum(completed),0) FROM todos", &stmt);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) { database_failed(ctx, web, rc); return; }
    int64_t total = (int64_t)sqlite3_column_int64(stmt, 0);
    int64_t done = (int64_t)sqlite3_column_int64(stmt, 1), active = total - done;
    rc = sqlite3_reset(stmt);
    if (rc != SQLITE_OK) { database_failed(ctx, web, rc); return; }
    rc = capi_sqlite_prepare(ctx, web->db,
        "SELECT id,title,completed FROM todos WHERE ?1='all' OR (?1='active' AND completed=0)"
        " OR (?1='completed' AND completed=1) ORDER BY id DESC LIMIT 100", &stmt);
    if (rc == SQLITE_OK) rc = capi_sqlite_bind_text(stmt, 1, (capi_str){filter, strlen(filter)});
    if (rc != SQLITE_OK) { database_failed(ctx, web, rc); return; }
    capi_value *rows = capi_alloc(ctx, 100 * sizeof(*rows));
    capi_field *columns = capi_alloc(ctx, 300 * sizeof(*columns));
    if (!rows || !columns) return;
    size_t count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        capi_str title;
        rc = capi_sqlite_copy_text(ctx, stmt, 1, &title);
        if (rc != SQLITE_OK) { database_failed(ctx, web, rc); return; }
        capi_field *row = columns + count * 3;
        row[0] = (capi_field){"id", capi_value_int((int64_t)sqlite3_column_int64(stmt, 0))};
        row[1] = (capi_field){"title", capi_value_string(title)};
        row[2] = (capi_field){"completed", capi_value_bool(sqlite3_column_int(stmt, 2) != 0)};
        rows[count++] = capi_value_object(row, 3);
    }
    if (rc != SQLITE_DONE) { database_failed(ctx, web, rc); return; }
    int64_t matches = !strcmp(filter, "active") ? active : !strcmp(filter, "completed") ? done : total;
    const char *empty_title = !strcmp(filter, "active") ? "All caught up." :
        !strcmp(filter, "completed") ? "A fresh start." : "A little space to begin.";
    const char *empty_text = !strcmp(filter, "active") ? "No active tasks. Enjoy the breathing room." :
        !strcmp(filter, "completed") ? "Your completed tasks will land here." : "Add your first task above. One small thing is enough.";
    capi_field fields[] = {
        {"todos", capi_value_list(rows, count)}, {"total", capi_value_int(total)},
        {"done", capi_value_int(done)}, {"active", capi_value_int(active)},
        {"progress", capi_value_int(total ? (int64_t)((double)done / (double)total * 100.0) : 0)},
        {"filter", capi_value_text(filter)}, {"all_filter", capi_value_bool(!strcmp(filter, "all"))},
        {"active_filter", capi_value_bool(!strcmp(filter, "active"))},
        {"completed_filter", capi_value_bool(!strcmp(filter, "completed"))},
        {"has_more", capi_value_bool(matches > 100)}, {"empty_title", capi_value_text(empty_title)},
        {"empty_text", capi_value_text(empty_text)}, {"error", capi_value_text(error)}
    };
    capi_value model = capi_value_object(fields, sizeof(fields) / sizeof(fields[0]));
    capi_render(ctx, status, web->page, &model);
}
static void home(capi_ctx *ctx) { render_page(ctx, 200, ""); }
static void stylesheet(capi_ctx *ctx) {
    todo_web *web = capi_data(ctx);
    if (!capi_set_header(ctx, "Content-Type", "text/css; charset=utf-8"))
        capi_write(ctx, web->css, web->css_length);
}
static void browser_policy(capi_ctx *ctx) {
    todo_web *web = capi_data(ctx);
    capi_set_state(ctx, web);
    capi_set_header(ctx, "X-Content-Type-Options", "nosniff");
    capi_set_header(ctx, "Cache-Control", "no-store");
    capi_set_header(ctx, "Referrer-Policy", "same-origin");
    capi_set_header(ctx, "Content-Security-Policy",
        "default-src 'none'; style-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
    capi_str method = capi_request_method(ctx), origin = capi_header(ctx, "Origin"), path = capi_path(ctx);
    bool writing = !capi_str_eq(method, "GET") && !capi_str_eq(method, "HEAD") && !capi_str_eq(method, "OPTIONS");
    bool form_route = path.len >= 4 && !memcmp(path.data, "/ui/", 4);
    if (writing && ((form_route && !origin.data) || (origin.data && !capi_str_eq(origin, web->origin)))) {
        render_page(ctx, 403, "Please submit the form from this app's browser address."); return;
    }
    capi_next(ctx);
}
void todo_web_result(capi_ctx *ctx) {
    if (capi_request_error(ctx).code != CAPI_ERROR_NONE) return;
    int status = capi_response_status(ctx);
    if (status >= 200 && status < 300) {
        const char *filter = selected_filter(ctx);
        const char *location = !strcmp(filter, "active") ? "/?filter=active" :
            !strcmp(filter, "completed") ? "/?filter=completed" : "/";
        capi_redirect(ctx, 303, location);
    } else {
        const char *message = status == 422 ? "Please enter a title between 1 and 255 UTF-8 bytes." :
            status == 404 ? "That task no longer exists. Your list is up to date below." :
            status == 503 ? "The database is busy. Please try again in a moment." :
            status == 400 || status == 415 ? "That form could not be read. Please try again." : "We couldn't save that change. Please try again.";
        render_page(ctx, status, message);
    }
}
int todo_web_register(capi_app *app, todo_web *web) {
    if (!app || !web) return -1;
    return capi_use(app, "/", browser_policy, web) || capi_route(app, CAPI_GET, "/", home, web) ||
        capi_route(app, CAPI_GET, "/style.css", stylesheet, web) ? -1 : 0;
}
int todo_web_origin(todo_web *web, const char *origin) {
    if (!web || !origin || !*origin || strlen(origin) >= sizeof(web->origin)) return -1;
    snprintf(web->origin, sizeof(web->origin), "%s", origin); return 0;
}
void todo_web_free(todo_web *web) {
    if (!web) return;
    capi_template_free(web->page); free(web->css); free(web);
}
todo_web *todo_web_new(sqlite3 *db, const char *views, char *error, size_t capacity) {
    todo_web *web = calloc(1, sizeof(*web));
    if (!web) { snprintf(error, capacity, "out of memory"); return NULL; }
    web->db = db;
    char path[4096]; capi_template_error detail;
    int n = snprintf(path, sizeof(path), "%s/index.html", views);
    if (n < 0 || (size_t)n >= sizeof(path)) { snprintf(error, capacity, "view path too long"); goto fail; }
    web->page = capi_template_load(path, &detail);
    if (!web->page) { snprintf(error, capacity, "index.html at byte %zu: %s", detail.offset, detail.message); goto fail; }
    n = snprintf(path, sizeof(path), "%s/style.css", views);
    if (n < 0 || (size_t)n >= sizeof(path)) { snprintf(error, capacity, "style path too long"); goto fail; }
    FILE *file = fopen(path, "rb");
    if (!file) { snprintf(error, capacity, "cannot open style.css"); goto fail; }
    web->css = malloc(65537);
    if (!web->css) { fclose(file); snprintf(error, capacity, "out of memory"); goto fail; }
    web->css_length = fread(web->css, 1, 65537, file);
    bool failed = ferror(file) || web->css_length > 65536;
    if (fclose(file)) failed = true;
    if (failed) { snprintf(error, capacity, "cannot read style.css (limit 64 KiB)"); goto fail; }
    if (todo_web_origin(web, "http://127.0.0.1:8080")) goto fail;
    return web;
fail:
    todo_web_free(web); return NULL;
}
