/* The repository's capi_todo target uses this wrapper. The standalone project
 * generates the same wrapper from .capi/resources in its build directory. */
#include "resources.h"
#include "resources/todos.h"
int app_resources_register(capi_app *app, sqlite3 *db) {
    return resource_todos_register(app, db);
}
