#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "generate.h"
#include <stddef.h>
#include "scaffold_templates.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_FIELDS = 16, MAX_RESOURCES = 64, NAME_SIZE = 33 };
static const char marker[] = "capi-sqlite-project-v1\n";
typedef struct { char name[NAME_SIZE]; const char *kind; size_t maximum; } field;
typedef struct { char data[8192]; size_t len; bool failed; } buffer;
static void append(buffer *b, const char *format, ...) {
    if (b->failed) return;
    va_list ap; va_start(ap, format);
    int n = vsnprintf(b->data + b->len, sizeof(b->data) - b->len, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(b->data) - b->len) b->failed = true;
    else b->len += (size_t)n;
}
static bool identifier(const char *s) {
    size_t len = strlen(s);
    if (!len || len >= NAME_SIZE || s[0] < 'a' || s[0] > 'z') return false;
    for (size_t i = 1; i < len; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return false;
    return true;
}
static bool regular(const char *path) {
    struct stat st; return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}
static bool directory(const char *path) {
    struct stat st; return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static FILE *create(const char *dir, const char *name) {
    char path[4096]; int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) { errno = ENAMETOOLONG; return NULL; }
    return fopen(path, "wx");
}
static int write_text(const char *dir, const char *name, const char *text) {
    FILE *file = create(dir, name); if (!file) return -1;
    int rc = fputs(text, file) < 0 ? -1 : 0;
    if (fclose(file)) rc = -1;
    return rc;
}
static int write_lines(const char *dir, const char *name, const char *const *lines) {
    FILE *file = create(dir, name); if (!file) return -1;
    int rc = 0;
    for (; *lines; ++lines) if (fputs(*lines, file) < 0) { rc = -1; break; }
    if (fclose(file)) rc = -1;
    return rc;
}
int cli_new_sqlite(const char *dir) {
    if (mkdir(dir, 0755)) { fprintf(stderr, "capi: cannot create %s: %s\n", dir, strerror(errno)); return 1; }
    char path[4096];
    const char *folders[] = {".capi", "resources"};
    for (size_t i = 0; i < 2; ++i) {
        int n = snprintf(path, sizeof(path), "%s/%s", dir, folders[i]);
        if (n < 0 || (size_t)n >= sizeof(path) || mkdir(path, 0755)) goto fail;
    }
    if (write_text(dir, ".capi/project", marker) || write_text(dir, ".capi/resources", "") ||
        write_text(dir, "resources.h", "#ifndef APP_RESOURCES_H\n#define APP_RESOURCES_H\n#include <capi_sqlite.h>\nCAPI_NODISCARD int app_resources_register(capi_app *app, sqlite3 *db);\n#endif\n") ||
        write_text(dir, ".gitignore", "/build/\n/app.db\n/app.db-wal\n/app.db-shm\n/.capi/lock\n/.capi/resources.tmp.*\n") ||
        write_lines(dir, "main.c", sqlite_main) || write_lines(dir, "CMakeLists.txt", sqlite_cmake) ||
        write_lines(dir, "README.md", sqlite_readme)) goto fail;
    printf("Created SQLite API at %s\n\n  cd %s\n  capi generate resource posts title:string body:text published:boolean\n  capi routes\n  cmake -S . -B build -DCMAKE_PREFIX_PATH=\"$HOME/.local\"\n  cmake --build build\n  ./build/app\n", dir, dir);
    return 0;
fail:
    fprintf(stderr, "capi: project creation failed: %s (partial project left at %s)\n", strerror(errno), dir);
    return 1;
}
static int load_manifest(char resources[MAX_RESOURCES][NAME_SIZE], size_t *count) {
    if (!directory(".capi") || !directory("resources") || !regular(".capi/project") || !regular(".capi/resources")) goto invalid;
    FILE *file = fopen(".capi/project", "r"); if (!file) goto invalid;
    char line[128]; bool valid = fgets(line, sizeof(line), file) && !strcmp(line, marker) && fgetc(file) == EOF;
    fclose(file); if (!valid) goto invalid;
    file = fopen(".capi/resources", "r"); if (!file) goto invalid;
    *count = 0;
    while (fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);
        if (!len || line[len - 1] != '\n') { fclose(file); goto invalid; }
        line[len - 1] = 0;
        if (*count == MAX_RESOURCES || !identifier(line) || !strncmp(line, "sqlite_", 7)) { fclose(file); goto invalid; }
        for (size_t i = 0; i < *count; ++i) if (!strcmp(resources[i], line)) { fclose(file); goto invalid; }
        strcpy(resources[(*count)++], line);
    }
    valid = !ferror(file); fclose(file);
    if (valid) return 0;
invalid:
    fputs("capi: run inside an intact project created with 'capi new <directory> --sqlite'\n", stderr); return -1;
}
static void print_routes(const char *name) {
    printf("GET     /%s\nPOST    /%s\nGET     /%s/:id\nPUT     /%s/:id\nPATCH   /%s/:id\nDELETE  /%s/:id\n", name, name, name, name, name, name);
}
int cli_routes(void) {
    char resources[MAX_RESOURCES][NAME_SIZE]; size_t count;
    if (load_manifest(resources, &count)) return 1;
    puts("GET     /");
    for (size_t i = 0; i < count; ++i) print_routes(resources[i]);
    puts("\nGenerated resources only; inspect main.c for custom routes. HEAD/OPTIONS are automatic.");
    return 0;
}
static buffer quoted(const buffer *sql) {
    buffer b = {0}; append(&b, "\"");
    for (size_t i = 0; i < sql->len; ++i) {
        if (sql->data[i] == '"' || sql->data[i] == '\\') append(&b, "\\");
        append(&b, "%c", sql->data[i]);
    }
    append(&b, "\""); b.failed = b.failed || sql->failed; return b;
}
static int render(FILE *file, const char *name, field *fields, size_t count) {
    buffer columns = {0}, definitions = {0}, schema = {0}, one = {0}, list = {0}, insert = {0}, update = {0}, delete = {0};
    append(&columns, "\"id\"");
    append(&schema, "CREATE TABLE IF NOT EXISTS \"%s\"(\"id\" INTEGER PRIMARY KEY AUTOINCREMENT CHECK(\"id\">0)", name);
    append(&insert, "INSERT INTO \"%s\"(", name);
    append(&update, "UPDATE \"%s\" SET ", name);
    for (size_t i = 0; i < count; ++i) {
        field *f = &fields[i];
        bool text = !strcmp(f->kind, "STRING") || !strcmp(f->kind, "TEXT");
        append(&definitions, "    {\"%s\", %s, %zu},\n", f->name, f->kind, f->maximum);
        append(&columns, ",\"%s\"", f->name);
        append(&schema, ",\"%s\" %s NOT NULL CHECK(typeof(\"%s\")='%s'", f->name, text ? "TEXT" : "INTEGER", f->name, text ? "text" : "integer");
        if (text) append(&schema, " AND length(CAST(\"%s\" AS BLOB)) BETWEEN %d AND %zu", f->name, !strcmp(f->kind, "STRING") ? 1 : 0, f->maximum);
        if (!strcmp(f->kind, "BOOLEAN")) append(&schema, " AND \"%s\" IN (0,1)", f->name);
        append(&schema, ")");
        append(&insert, "%s\"%s\"", i ? "," : "", f->name);
        append(&update, "%s\"%s\"=CASE WHEN ?%zu THEN ?%zu ELSE \"%s\" END", i ? "," : "", f->name, i * 2 + 1, i * 2 + 2, f->name);
    }
    append(&schema, ")");
    append(&one, "SELECT %s FROM \"%s\" WHERE \"id\"=?1", columns.data, name);
    append(&list, "SELECT %s FROM \"%s\" ORDER BY \"id\" DESC LIMIT ?1 OFFSET ?2", columns.data, name);
    append(&insert, ") VALUES(");
    for (size_t i = 0; i < count; ++i) append(&insert, "%s?%zu", i ? "," : "", i + 1);
    append(&insert, ")");
    append(&update, " WHERE \"id\"=?%zu", count * 2 + 1);
    append(&delete, "DELETE FROM \"%s\" WHERE \"id\"=?1", name);
    buffer sql[] = {quoted(&schema), quoted(&one), quoted(&list), quoted(&insert), quoted(&update), quoted(&delete)};
    if (columns.failed || definitions.failed) { errno = EOVERFLOW; return -1; }
    for (size_t i = 0; i < 6; ++i) if (sql[i].failed) { errno = EOVERFLOW; return -1; }
    const char *tokens[] = {"@RESOURCE@", "@FIELDS@", "@SCHEMA@", "@SELECT_ONE@", "@SELECT_LIST@", "@INSERT@", "@UPDATE@", "@DELETE@"};
    const char *values[] = {name, definitions.data, sql[0].data, sql[1].data, sql[2].data, sql[3].data, sql[4].data, sql[5].data};
    for (const char *const *line = resource_template; *line; ++line) {
        const char *p = *line;
        while (*p) {
            size_t i;
            for (i = 0; i < 8; ++i) if (!strncmp(p, tokens[i], strlen(tokens[i]))) break;
            if (i < 8) { if (fputs(values[i], file) < 0) return -1; p += strlen(tokens[i]); }
            else if (fputc(*p++, file) == EOF) return -1;
        }
    }
    return 0;
}
int cli_generate(int argc, char **argv) {
    if (argc == 1 && (!strcmp(argv[0], "--help") || !strcmp(argv[0], "help"))) {
        puts("usage: capi generate resource <plural_name> <field[:string|text|integer|boolean]>... [--dry-run]\n"
             "Example: capi generate resource posts title:string body:text published:boolean");
        return 0;
    }
    bool dry_run = argc > 0 && !strcmp(argv[argc - 1], "--dry-run");
    if (dry_run) --argc;
    if (argc < 3 || strcmp(argv[0], "resource") || !identifier(argv[1]) || !strncmp(argv[1], "sqlite_", 7) || argc - 2 > MAX_FIELDS) {
        fputs("usage: capi generate resource <plural_name> <field[:string|text|integer|boolean]>... [--dry-run]\nNames: lowercase ASCII identifiers, 1-32 bytes; maximum 16 fields; sqlite_ is reserved.\n", stderr); return 1;
    }
    field fields[MAX_FIELDS] = {0}; size_t count = (size_t)argc - 2;
    for (size_t i = 0; i < count; ++i) {
        const char *spec = argv[i + 2], *colon = strchr(spec, ':');
        size_t len = colon ? (size_t)(colon - spec) : strlen(spec);
        if (!len || len >= NAME_SIZE) goto bad_field;
        memcpy(fields[i].name, spec, len); fields[i].name[len] = 0;
        if (!identifier(fields[i].name) || !strcmp(fields[i].name, "id")) goto bad_field;
        for (size_t j = 0; j < i; ++j) if (!strcmp(fields[i].name, fields[j].name)) goto bad_field;
        const char *type = colon ? colon + 1 : "string";
        if (!strcmp(type, "string")) { fields[i].kind = "STRING"; fields[i].maximum = 255; }
        else if (!strcmp(type, "text")) { fields[i].kind = "TEXT"; fields[i].maximum = 4096; }
        else if (!strcmp(type, "integer")) fields[i].kind = "INTEGER";
        else if (!strcmp(type, "boolean")) fields[i].kind = "BOOLEAN";
        else goto bad_field;
    }
    char resources[MAX_RESOURCES][NAME_SIZE]; size_t nresources;
    if (load_manifest(resources, &nresources)) return 1;
    int lock = -1;
    if (!dry_run) {
        lock = open(".capi/lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
        struct stat lock_stat;
        struct flock guard = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
        if (lock < 0 || fstat(lock, &lock_stat) || !S_ISREG(lock_stat.st_mode) || fcntl(lock, F_SETLK, &guard)) {
            fputs("capi: could not lock project (another generator may be running)\n", stderr);
            if (lock >= 0) close(lock);
            return 1;
        }
    }
    if (load_manifest(resources, &nresources)) { if (lock >= 0) close(lock); return 1; }
    char header[96], source[96];
    snprintf(header, sizeof(header), "resources/%s.h", argv[1]);
    snprintf(source, sizeof(source), "resources/%s.c", argv[1]);
    struct stat st;
    bool exists = false;
    for (size_t i = 0; i < nresources; ++i) if (!strcmp(resources[i], argv[1])) exists = true;
    if (exists || nresources == MAX_RESOURCES || lstat(header, &st) == 0 || errno != ENOENT || lstat(source, &st) == 0 || errno != ENOENT) {
        fputs("capi: resource exists, output path is unavailable, or 64-resource limit reached; nothing overwritten\n", stderr);
        if (lock >= 0) close(lock);
        return 1;
    }
    if (dry_run) { printf("Would create %s and %s; register in .capi/resources\n", source, header); print_routes(argv[1]); return 0; }
    bool made_header = false, made_source = false;
    char temporary[] = ".capi/resources.tmp.XXXXXX";
    int fd = mkstemp(temporary);
    if (fd < 0) goto fail;
    FILE *manifest = fdopen(fd, "w");
    if (!manifest) { close(fd); goto fail; }
    for (size_t i = 0; i < nresources; ++i) fprintf(manifest, "%s\n", resources[i]);
    fprintf(manifest, "%s\n", argv[1]);
    bool failed = ferror(manifest) != 0;
    if (fclose(manifest)) failed = true;
    if (failed) goto fail;
    FILE *file = fopen(header, "wx"); if (!file) goto fail;
    made_header = true;
    int rc = fprintf(file, "#ifndef RESOURCE_%s_H\n#define RESOURCE_%s_H\n#include <capi_sqlite.h>\nCAPI_NODISCARD int resource_%s_register(capi_app *app, sqlite3 *db);\n#endif\n", argv[1], argv[1], argv[1]);
    if (fclose(file)) rc = -1;
    if (rc < 0) goto fail;
    file = fopen(source, "wx"); if (!file) goto fail;
    made_source = true;
    rc = render(file, argv[1], fields, count);
    if (fclose(file)) rc = -1;
    if (rc || rename(temporary, ".capi/resources")) goto fail;
    close(lock);
    printf("Created %s and %s\nRegistered /%s. Build with: cmake --build build\n", source, header, argv[1]);
    return 0;
fail:
    fprintf(stderr, "capi: generation failed: %s; manifest unchanged\n", strerror(errno));
    if (fd >= 0) unlink(temporary);
    if (made_source) unlink(source);
    if (made_header) unlink(header);
    close(lock); return 1;
bad_field:
    fputs("capi: invalid, duplicate, or reserved field; types: string, text, integer, boolean; id is automatic\n", stderr);
    return 1;
}
