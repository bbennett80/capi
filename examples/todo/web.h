#ifndef TODO_WEB_H
#define TODO_WEB_H
#include <capi_sqlite.h>

typedef struct todo_web todo_web;
CAPI_NODISCARD todo_web *todo_web_new(sqlite3 *db, const char *views, char *error, size_t capacity);
void todo_web_free(todo_web *web);
CAPI_NODISCARD int todo_web_register(capi_app *app, todo_web *web);
CAPI_NODISCARD int todo_web_origin(todo_web *web, const char *origin);
/* Convert a generated action's result into a browser redirect or HTML error. */
void todo_web_result(capi_ctx *ctx);
#endif
