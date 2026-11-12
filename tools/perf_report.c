/* Static, self-contained HTML from the raw CSV. No browser dependencies. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BACKENDS = 2, WORKLOADS = 6, LEVELS = 5, REPEATS = 3 };
static const unsigned connections[] = {1, 8, 32, 64, 256};
static const char *keys[] = {"text", "json", "route", "bulk", "template", "sqlite"};
static const char *names[] = {"Plain text", "JSON response", "Route + query", "16 KiB body", "HTML template", "SQLite read"};
static const char *descriptions[] = {
    "A 17-byte greeting through capi_text.",
    "A small static JSON document through capi_json.",
    "Parameterized route, checked integer, percent-decoded query, middleware header, escaped JSON.",
    "A 16,384-byte binary body copied into the response buffer.",
    "A precompiled template rendering ten notes with integer fields and HTML escaping.",
    "Prepare, bind, read ten indexed rows, copy titles, serialize JSON, finalize. 1,000 seeded rows in memory; no disk or writes."
};
typedef struct { unsigned requests; double seconds, rps, p50, p95, p99; unsigned long long bytes; bool seen; } sample;
static sample data[BACKENDS][WORKLOADS][LEVELS][REPEATS];
static char backends[BACKENDS][16];
static FILE *out;
static void escape(const char *s) {
    for (; *s; ++s) {
        switch (*s) {
        case '&': fputs("&amp;", out); break;
        case '<': fputs("&lt;", out); break;
        case '>': fputs("&gt;", out); break;
        case '"': fputs("&quot;", out); break;
        default: fputc(*s, out);
        }
    }
}
static double median(double a, double b, double c) {
    if (a > b) { double t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}
static double rate(int b, int w, int l) {
    sample *s = data[b][w][l]; return median(s[0].rps, s[1].rps, s[2].rps);
}
static int read_csv(const char *path) {
    FILE *file = fopen(path, "r"); if (!file) { perror(path); return -1; }
    char line[512];
    const char *header = "backend,workload,repeat,connections,requests,seconds,rps,p50_ms,p95_ms,p99_ms,wire_bytes\n";
    if (!fgets(line, sizeof(line), file) || strcmp(line, header)) { fclose(file); return -1; }
    while (fgets(line, sizeof(line), file)) {
        char backend[16], workload[24], extra;
        unsigned repeat, concurrency; sample s = {0};
        int fields = sscanf(line, "%15[^,],%23[^,],%u,%u,%u,%lf,%lf,%lf,%lf,%lf,%llu %c",
            backend, workload, &repeat, &concurrency, &s.requests, &s.seconds, &s.rps, &s.p50, &s.p95, &s.p99, &s.bytes, &extra);
        if (fields != 11) { fprintf(stderr, "Malformed benchmark row: %s", line); fclose(file); return -1; }
        int b = -1, w = -1, l = -1;
        for (int i = 0; i < BACKENDS; ++i) {
            if (!backends[i][0]) { strcpy(backends[i], backend); b = i; break; }
            if (!strcmp(backends[i], backend)) { b = i; break; }
        }
        for (int i = 0; i < WORKLOADS; ++i) if (!strcmp(keys[i], workload)) w = i;
        for (int i = 0; i < LEVELS; ++i) if (connections[i] == concurrency) l = i;
        if (fields != 11 || b < 0 || w < 0 || l < 0 || repeat < 1 || repeat > REPEATS ||
            !s.requests || !isfinite(s.seconds) || !isfinite(s.rps) || !isfinite(s.p50) || !isfinite(s.p95) || !isfinite(s.p99) ||
            s.seconds <= 0 || s.rps <= 0 || s.p50 < 0 || s.p95 < s.p50 || s.p99 < s.p95 ||
            s.bytes < s.requests || data[b][w][l][repeat - 1].seen) {
            fprintf(stderr, "Invalid/duplicate benchmark row: %s", line); fclose(file); return -1;
        }
        s.seen = true; data[b][w][l][repeat - 1] = s;
    }
    int error = ferror(file); fclose(file); return error ? -1 : 0;
}
static void chart(int w) {
    double max = 0;
    for (int b = 0; b < BACKENDS; ++b) for (int l = 0; l < LEVELS; ++l)
        if (rate(b, w, l) > max) max = rate(b, w, l);
    max = (double)((unsigned)(max / 10000) + 1) * 10000;
    fprintf(out, "<article class=chart><div class=eyebrow>0%d / WORKLOAD</div><h3>%s</h3><p>%s</p>", w + 1, names[w], descriptions[w]);
    fprintf(out, "<svg viewBox='0 0 390 205' role=img aria-labelledby='chart%d'><title id='chart%d'>%s: median requests per second at 1, 8, 32, 64, and 256 connections. Full values in the table below.</title>", w, w, names[w]);
    for (int i = 0; i < 3; ++i) {
        double y = 20 + (double)i * 65;
        fprintf(out, "<path class=grid d='M48 %.1fH370'/><text x=40 y='%.1f' text-anchor=end>%.0fk</text>", y, y + 4, max * (2 - i) / 2000);
    }
    for (int l = 0; l < LEVELS; ++l) fprintf(out, "<text x='%d' y=177 text-anchor=middle>%u</text>", 55 + l * 76, connections[l]);
    fputs("<text x=208 y=199 text-anchor=middle>concurrent connections</text>", out);
    for (int b = 0; b < BACKENDS; ++b) {
        fprintf(out, "<polyline class='series s%d' points='", b);
        for (int l = 0; l < LEVELS; ++l) fprintf(out, "%d,%.2f ", 55 + l * 76, 150 - rate(b, w, l) / max * 130);
        fputs("'/>", out);
        for (int l = 0; l < LEVELS; ++l) {
            fprintf(out, "<circle class='dot d%d' cx='%d' cy='%.2f' r=3.5><title>", b, 55 + l * 76, 150 - rate(b, w, l) / max * 130);
            escape(backends[b]); fprintf(out, ": %u connections, %.0f requests/s</title></circle>", connections[l], rate(b, w, l));
        }
    }
    fputs("</svg></article>", out);
}
int main(int argc, char **argv) {
    if (argc != 4) { fputs("usage: capi_perf_report <results.csv> <environment.txt> <report.html>\n", stderr); return 1; }
    if (read_csv(argv[1])) return 1;
    unsigned long long requests = 0;
    double elapsed = 0, peak = 0;
    int peak_b = 0, peak_w = 0, peak_l = 0;
    for (int b = 0; b < BACKENDS; ++b) for (int w = 0; w < WORKLOADS; ++w) for (int l = 0; l < LEVELS; ++l) {
        for (int r = 0; r < REPEATS; ++r) {
            sample *s = &data[b][w][l][r];
            if (!s->seen) { fputs("Incomplete matrix; refusing to publish a partial report\n", stderr); return 1; }
            requests += s->requests; elapsed += s->seconds;
        }
        if (rate(b, w, l) > peak) { peak = rate(b, w, l); peak_b = b; peak_w = w; peak_l = l; }
    }
    FILE *environment = fopen(argv[2], "r"); if (!environment) { perror(argv[2]); return 1; }
    out = fopen(argv[3], "w"); if (!out) { perror(argv[3]); fclose(environment); return 1; }
    fputs("<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta name=color-scheme content=dark><title>capi / Performance notebook</title><style>"
        ":root{color-scheme:dark;--bg:#101612;--panel:#17201a;--line:#334037;--text:#eaf0e5;--muted:#adb9ad;--green:#b8eb7c;--amber:#edba78}"
        "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.6 system-ui,sans-serif}"
        "main{max-width:1100px;margin:auto;padding:32px 28px 70px}a{color:var(--green);text-underline-offset:4px}"
        "nav{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--line);padding:0 0 22px}"
        ".brand{font-size:26px;font-weight:800;letter-spacing:-1px}.tag,.eyebrow{font:11px/1.5 ui-monospace,monospace;letter-spacing:1.4px;color:var(--muted)}"
        ".tag{border:1px solid var(--line);padding:5px 10px;border-radius:20px}header{padding:54px 0 32px}"
        "h1{font-size:clamp(40px,7vw,76px);line-height:1.05;letter-spacing:-3px;margin:17px 0 22px;max-width:850px}"
        "header p{font-size:18px;max-width:720px;color:var(--muted)}h2{font-size:26px;letter-spacing:-.7px;margin:0}"
        "h3{font-size:21px;margin:7px 0}p{margin:10px 0}.accent{color:var(--green)}"
        ".stats{display:grid;grid-template-columns:repeat(3,1fr);border:1px solid var(--line);margin:12px 0 48px}"
        ".stat{padding:24px;border-right:1px solid var(--line)}.stat:last-child{border:0}.number{font-size:38px;line-height:1.2;letter-spacing:-1.5px;font-variant-numeric:tabular-nums}"
        ".stat p{font-size:13px;color:var(--muted);margin-bottom:0}.section-head{display:flex;align-items:center;justify-content:space-between;gap:18px;margin-bottom:20px;flex-wrap:wrap}"
        ".legend{font:12px ui-monospace,monospace;display:flex;gap:20px}.legend span:before{content:'';display:inline-block;width:20px;height:3px;vertical-align:middle;margin-right:7px;background:var(--green)}"
        ".legend span+span:before{background:var(--amber)}.charts{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}", out);
    fputs(
        ".chart{background:var(--panel);border:1px solid var(--line);padding:22px}.chart p{min-height:66px;color:var(--muted);font-size:13px}"
        "svg{display:block;width:100%;height:auto;margin-top:20px}svg text{fill:var(--muted);font:10px ui-monospace,monospace}.grid{stroke:var(--line);stroke-width:1}"
        ".series{fill:none;stroke-width:2.5}.s0{stroke:var(--green)}.s1{stroke:var(--amber);stroke-dasharray:5 3}.d0{fill:var(--green)}.d1{fill:var(--amber)}"
        ".note{font-size:13px;color:var(--muted);margin:16px 0 42px}.method{display:grid;grid-template-columns:1fr 1fr;gap:28px;margin:42px 0}"
        ".method p,.method li{color:var(--muted);font-size:14px}.method ul{padding-left:20px}code,pre{font-family:ui-monospace,monospace;font-size:12px}"
        "pre{white-space:pre-wrap;overflow-wrap:anywhere;padding:20px;background:var(--panel);border:1px solid var(--line);color:var(--muted)}"
        "details{border-top:1px solid var(--line);border-bottom:1px solid var(--line);padding:20px 0;margin:30px 0}summary{cursor:pointer;font-weight:650}"
        ".table-scroll{overflow:auto;max-height:620px;margin-top:20px}table{width:100%;border-collapse:collapse;font-size:12px;font-variant-numeric:tabular-nums;white-space:nowrap}"
        "th,td{padding:10px;text-align:right;border-bottom:1px solid var(--line)}th{position:sticky;top:0;background:var(--bg);color:var(--muted)}th:first-child,td:first-child{text-align:left}"
        "footer{display:flex;justify-content:space-between;gap:15px;flex-wrap:wrap;border-top:1px solid var(--line);padding-top:22px;margin-top:40px;color:var(--muted);font-size:12px}"
        "@media(max-width:650px){main{padding:22px 16px 45px}.stats,.charts,.method{grid-template-columns:1fr}.stat{border-right:0;border-bottom:1px solid var(--line)}"
        ".chart p{min-height:0}header{padding-top:36px}h1{letter-spacing:-1.8px}.number{font-size:34px}}"
        "@media print{details .table-scroll{max-height:none}body{background:white;color:black}main{max-width:none}svg text{fill:#333}.chart,.stat{break-inside:avoid}}"
        "</style><main><nav><div class=brand>capi<span class=accent>.</span></div><div class=tag>PERFORMANCE NOTEBOOK / 0.4</div></nav>"
        "<header><div class=eyebrow>LOCAL MEASUREMENTS · REPEATED RUNS</div><h1>Small framework.<br><span class=accent>Measured in requests.</span></h1>"
        "<p>Six real HTTP workloads. Five concurrency levels. Two reactor backends. Repeated measurements, with the raw data attached.</p></header><section class=stats aria-label='Measurement summary'>", out);
    fprintf(out, "<div class=stat><div class=eyebrow>BEST MEDIAN THROUGHPUT</div><div class='number accent'>%.1fk <small>req/s</small></div><p>%s · ", peak / 1000, names[peak_w]);
    escape(backends[peak_b]); fprintf(out, " · %u connections</p></div>", connections[peak_l]);
    fprintf(out, "<div class=stat><div class=eyebrow>MEASURED REQUESTS</div><div class=number>%.1fM</div><p>180 measured runs · %.1f minutes under measurement</p></div>", (double)requests / 1000000, elapsed / 60);
    fputs("<div class=stat><div class=eyebrow>FAILED MEASURED RUNS</div><div class=number>0 / 180</div><p>Every request checked for HTTP 200 and complete response length</p></div></section>"
        "<section aria-labelledby=throughput><div class=section-head><h2 id=throughput>Throughput across concurrency</h2><div class=legend><span>", out);
    escape(backends[0]); fputs("</span><span>", out); escape(backends[1]); fputs("</span></div></div><div class=charts>", out);
    for (int w = 0; w < WORKLOADS; ++w) chart(w);
    fputs("</div><p class=note>Each point is the median of three runs. Y-axes scale independently per workload; connection settings are equally spaced on the X-axis. These are same-host loopback measurements, not a ranking against other frameworks.</p></section>"
        "<details><summary>All 60 configurations · throughput ranges and latency</summary><p class=note>Latency values are medians of the three per-run percentiles, not pooled percentiles. Range shows the minimum and maximum throughput across those runs. Bytes include response headers.</p>"
        "<div class=table-scroll><table><thead><tr><th scope=col>Workload / backend</th><th scope=col>Connections</th><th scope=col>Median req/s</th><th scope=col>Min–max req/s</th><th scope=col>p50 ms</th><th scope=col>p95 ms</th><th scope=col>p99 ms</th><th scope=col>Wire B/req</th></tr></thead><tbody>", out);
    for (int w = 0; w < WORKLOADS; ++w) for (int b = 0; b < BACKENDS; ++b) for (int l = 0; l < LEVELS; ++l) {
        sample *s = data[b][w][l]; double min = s[0].rps, max = min;
        for (int r = 1; r < REPEATS; ++r) { if (s[r].rps < min) min = s[r].rps; if (s[r].rps > max) max = s[r].rps; }
        fprintf(out, "<tr><td>%s / ", names[w]); escape(backends[b]);
        fprintf(out, "</td><td>%u</td><td>%.0f</td><td>%.0f–%.0f</td><td>%.3f</td><td>%.3f</td><td>%.3f</td><td>%.0f</td></tr>",
            connections[l], rate(b, w, l), min, max, median(s[0].p50, s[1].p50, s[2].p50),
            median(s[0].p95, s[1].p95, s[2].p95), median(s[0].p99, s[1].p99, s[2].p99), (double)s[0].bytes / s[0].requests);
    }
    fputs("</tbody></table></div></details><section class=method><div><h2>How this was measured</h2>"
        "<ul><li>CMake Release builds: <code>-O3 -DNDEBUG</code>. One server event loop and a separate C load-generator process. No TLS, access logging, workers, or pipelining.</li>"
        "<li>Persistent HTTP/1.1, one in-flight request per connection. Connections open before timing. Request/response I/O and client scheduling are timed; connect time and latency sorting are excluded.</li>"
        "<li>5,000 warmup requests before every measured run. Three rounds; each configuration sends the request count recorded in the CSV. Backends run sequentially; the first round runs forward, the second reverses workload/concurrency order.</li>"
        "<li>Any HTTP status, framing, connection, or stall failure aborts collection. Counts exclude warmup requests. The generator checks status and length, not body content.</li></ul></div>"
        "<div><h2>How to read the results</h2><p>More connections can increase throughput while increasing per-request latency. The fastest point is a property of this workload and machine, not a universal tuning recommendation.</p>"
        "<p>The client and server share CPU, memory, and the loopback stack. The poll-based client can become the bottleneck. No CPU pinning, machine isolation, or randomized backend order was used; differences between backends are descriptive, not causal.</p>"
        "<p>SQLite here is an in-memory, read-only fixture. It measures adapter and query/serialization cost, not disk persistence, contention, write durability, or the complete Notes app. Tail latency and production capacity need longer tests from another host.</p></div></section>"
        "<section><div class=section-head><h2>The measurement environment</h2><a href=environment.txt>Plain text ↗</a></div><pre>", out);
    char line[512]; while (fgets(line, sizeof(line), environment)) escape(line);
    bool failed = ferror(environment) != 0; fclose(environment);
    fputs("</pre></section><footer><span>capi · C11 · dependency-free core</span><span><a href=results.csv download>Raw CSV</a> · <a href=../../performance.md>Methodology &amp; reproduction</a></span></footer></main></html>\n", out);
    failed = failed || ferror(out); if (fclose(out)) failed = true;
    return failed ? 1 : 0;
}
