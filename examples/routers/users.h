#ifndef EXAMPLE_USERS_H
#define EXAMPLE_USERS_H
#include <capi.h>
/* label is borrowed by handlers; it must outlive the application. */
CAPI_NODISCARD capi_router *users_router_new(const char *label);
#endif
