#define _POSIX_C_SOURCE 200809L
#include "../include/capi.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char main_source[] =
    "#include <capi.h>\n#include <signal.h>\n#include <stdio.h>\n\n"
    "static capi_server *server;\n"
    "static void stop(int n) { (void)n; capi_server_stop(server); }\n"
    "static void hello(capi_ctx *ctx) { capi_json(ctx, 200, \"{\\\"hello\\\":\\\"world\\\"}\"); }\n\n"
    "int main(void) {\n"
    "    capi_app *app = capi_app_new();\n"
    "    if (!app) return 1;\n"
    "    if (capi_route(app, CAPI_GET, \"/\", hello, NULL)) goto fail;\n"
    "    server = capi_server_listen(app, (capi_server_options){.port = 8080});\n"
    "    if (!server) goto fail;\n"
    "    signal(SIGINT, stop); signal(SIGTERM, stop);\n"
    "    puts(\"Listening on http://127.0.0.1:8080\");\n"
    "    int result = capi_server_run(server);\n"
    "    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);\n"
    "    capi_server_free(server); capi_app_free(app);\n"
    "    return result ? 1 : 0;\n"
    "fail:\n"
    "    fprintf(stderr, \"capi: %s\\n\", capi_app_error(app));\n"
    "    capi_app_free(app); return 1;\n}\n";
static const char cmake_source[] =
    "cmake_minimum_required(VERSION 3.16)\nproject(my_capi_app LANGUAGES C)\n"
    "set(CMAKE_C_STANDARD 11)\nset(CMAKE_C_STANDARD_REQUIRED ON)\n"
    "find_package(capi 0.1 CONFIG REQUIRED)\nadd_executable(app main.c)\n"
    "target_link_libraries(app PRIVATE capi::capi)\n";
static int write_file(const char *directory, const char *name, const char *contents) {
    size_t len = strlen(directory) + strlen(name) + 2;
    char *path = malloc(len);
    if (!path) return -1;
    snprintf(path, len, "%s/%s", directory, name);
    FILE *file = fopen(path, "wx");
    if (!file) { free(path); return -1; }
    int result = fputs(contents, file) == EOF ? -1 : 0;
    if (fclose(file)) result = -1;
    free(path); return result;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts("capi " CAPI_VERSION); return 0; }
    if (argc != 3 || strcmp(argv[1], "new")) {
        fputs("usage: capi new <directory>\n       capi --version\n", stderr); return 1;
    }
    if (mkdir(argv[2], 0755)) { fprintf(stderr, "capi: cannot create %s: %s\n", argv[2], strerror(errno)); return 1; }
    if (write_file(argv[2], "main.c", main_source) || write_file(argv[2], "CMakeLists.txt", cmake_source) ||
        write_file(argv[2], ".gitignore", "/build/\n") ||
        write_file(argv[2], "README.md", "# My capi app\n\nInstall capi, then:\n\n```sh\ncmake -S . -B build\ncmake --build build\n./build/app\n```\n")) {
        fprintf(stderr, "capi: could not write project: %s (partial project left at %s)\n", strerror(errno), argv[2]); return 1;
    }
    printf("Created %s\n\nWith capi installed:\n  cmake -S %s -B %s/build\n  cmake --build %s/build\n  %s/build/app\n",
           argv[2], argv[2], argv[2], argv[2], argv[2]);
    return 0;
}
