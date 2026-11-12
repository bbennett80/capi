#include "capi_template.h"
#include "support.h"
#include <stdint.h>

typedef struct {
    capi_template *view;
    capi_value data;
    const capi_partial *partials;
    size_t count;
    int render_result;
} fixture;
static void render(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    capi_text(ctx, 202, "old body");
    f->render_result = capi_render_with(ctx, 200, f->view, &f->data, f->partials, f->count);
}
static void run(const char *source, capi_value data, const char *expected) {
    capi_template_error error;
    capi_template *view = capi_template_compile(source, strlen(source), &error);
    CHECK(view); CHECK(error.message[0] == 0);
    fixture f = {view, data, NULL, 0, -1};
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_GET, "/", render, &f) == 0);
    capi_result r = request_text(app, "GET / HTTP/1.1\r\nHost: test\r\n\r\n", 200);
    CHECK(f.render_result == 0);
    CHECK(strstr(r.data, "Content-Type: text/html; charset=utf-8\r\n"));
    body_equals(&r, expected);
    capi_result_free(&r); capi_app_free(app); capi_template_free(view);
}
static void compiler_errors(void) {
    const char *invalid[] = {"{{", "{{{x}}", "{{ }}", "{{/x}}", "{{#x}}", "{{#x}}{{/y}}", "{{#}}",
        "{{a..b}}", "{{.x}}", "{{x.}}", "{{a b}}", "{{= | | =}}", "{{&html}}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        capi_template_error error;
        CHECK(capi_template_compile(invalid[i], strlen(invalid[i]), &error) == NULL);
        CHECK(error.message[0]); CHECK(error.offset <= strlen(invalid[i]));
    }
    capi_template_error error;
    CHECK(capi_template_compile(NULL, 1, &error) == NULL);
    CHECK(capi_template_compile("x", 1024 * 1024 + 1, &error) == NULL);
    CHECK(capi_template_load("/this/template/does/not/exist", &error) == NULL);
    CHECK(error.message[0]);
    char nested[4096] = "";
    for (size_t i = 0; i < 65; ++i) strcat(nested, "{{#x}}");
    for (size_t i = 0; i < 65; ++i) strcat(nested, "{{/x}}");
    CHECK(capi_template_compile(nested, strlen(nested), &error) == NULL);
    char owned[] = "{{.}}";
    capi_template *view = capi_template_compile(owned, strlen(owned), &error); CHECK(view);
    memset(owned, 'x', strlen(owned));
    fixture f = {view, capi_value_text("copied"), NULL, 0, -1};
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_GET, "/", render, &f) == 0);
    capi_result r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    body_equals(&r, "copied"); capi_result_free(&r); capi_app_free(app); capi_template_free(view);
}
static void partials_and_limits(void) {
    capi_template_error error;
    capi_template *view = capi_template_compile("A{{>card}}B", 11, &error); CHECK(view);
    capi_template *card = capi_template_compile("<b>{{.}}</b>", 12, &error); CHECK(card);
    capi_partial partials[] = {{"card", card}};
    fixture f = {view, capi_value_text("<&"), partials, 1, -1};
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_GET, "/", render, &f) == 0);
    capi_result r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    body_equals(&r, "A<b>&lt;&amp;</b>B"); capi_result_free(&r);
    f.count = 0;
    r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 500);
    CHECK(f.render_result == -1); body_equals(&r, "Internal Server Error\n"); capi_result_free(&r);
    capi_template_free(card);
    card = capi_template_compile("{{>card}}", 9, &error); CHECK(card);
    partials[0].view = card; f.count = 1;
    r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 500);
    CHECK(f.render_result == -1); capi_result_free(&r);
    capi_template_free(card); capi_template_free(view);
    f.view = capi_template_compile("{{#items}}{{/items}}", 20, &error); CHECK(f.view);
    capi_value *many = calloc(1000001, sizeof(*many)); CHECK(many);
    capi_field fields[] = {{"items", capi_value_list(many, 1000001)}};
    /* Empty loop bodies must also be subject to the render budget. */
    f.data = capi_value_object(fields, 1); f.count = 0;
    r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 500);
    CHECK(f.render_result == -1); capi_result_free(&r);
    free(many);
    fields[0].value = capi_value_list(NULL, 1);
    r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 500); capi_result_free(&r);
    capi_template_free(f.view);
    f.view = capi_template_compile("{{.}}", 5, &error); CHECK(f.view);
    f.data = capi_value_string((capi_str){NULL, 1});
    r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 500); capi_result_free(&r);
    f.data = capi_value_text("too much output");
    CHECK(capi_app_limits(app, (capi_limits){16384, 1024, 4, 0}) == 0);
    r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 500); capi_result_free(&r);
    capi_template_free(f.view); capi_app_free(app);
}
int main(void) {
    compiler_errors();
    run("", (capi_value){0}, ""); run("literal {", (capi_value){0}, "literal {");
    capi_field person[] = {{"name", capi_value_text("Ada")}};
    capi_field root[] = {
        {"name", capi_value_text("<&>\"'")}, {"html", capi_value_text("<em>trusted</em>")},
        {"yes", capi_value_bool(true)}, {"no", capi_value_bool(false)},
        {"zero", capi_value_int(0)}, {"min", capi_value_int(INT64_MIN)},
        {"person", capi_value_object(person, 1)}, {"empty", capi_value_list(NULL, 0)}
    };
    capi_value data = capi_value_object(root, sizeof(root) / sizeof(*root));
    run("<p title=\"{{name}}\">{{ name }}</p>", data, "<p title=\"&lt;&amp;&gt;&quot;&#39;\">&lt;&amp;&gt;&quot;&#39;</p>");
    run("{{{html}}}{{! comment }}{{missing}}", data, "<em>trusted</em>");
    run("{{#yes}}Y{{/yes}}{{#no}}N{{/no}}{{^no}}I{{/no}}{{^empty}}E{{/empty}}{{#zero}}Z{{/zero}}", data, "YIEZ");
    run("{{person.name}} {{zero}} {{min}} {{yes}} {{no}}", data, "Ada 0 -9223372036854775808 true false");
    run("{{#person}}{{name}}:{{min}}{{/person}}", data, "Ada:-9223372036854775808");
    capi_value items[] = {capi_value_text("one"), capi_value_text("two")};
    capi_field list[] = {{"items", capi_value_list(items, 2)}, {"label", capi_value_text("L")}};
    run("{{#items}}[{{label}}:{{.}}]{{/items}}{{^items}}empty{{/items}}", capi_value_object(list, 2), "[L:one][L:two]");
    partials_and_limits();
    printf("templates: %u checks passed\n", checks); return 0;
}
