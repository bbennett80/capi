#ifndef APP_RESOURCES_H
#define APP_RESOURCES_H
#include <capi_sqlite.h>
CAPI_NODISCARD int app_resources_register(capi_app *app, sqlite3 *db);
#endif
