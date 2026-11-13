#ifndef RESOURCE_todos_H
#define RESOURCE_todos_H
#include <capi_sqlite.h>
CAPI_NODISCARD int resource_todos_register(capi_app *app, sqlite3 *db);
#endif
