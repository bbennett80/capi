#ifndef CAPI_NOTES_APP_H
#define CAPI_NOTES_APP_H
#include "capi.h"
typedef struct notes_app notes_app;
notes_app *notes_open(const char *database, const char *view_directory, char *error, size_t capacity);
void notes_close(notes_app *notes);
capi_app *notes_http(notes_app *notes);
int notes_origin(notes_app *notes, const char *origin);
#endif
