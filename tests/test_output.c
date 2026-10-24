/* Unit tests: formatting helpers, process name sanitizing, JSON escaping and the
 * choice between the full-screen view, line output and colors. */

#define main dramtop_main
#include "../src/dramtop.c"
#undef main

#include "check.h"

static const char *json_of(const char *s, char *out, size_t n) {
    FILE *f = tmpfile();
    if (!f) return "(tmpfile failed)";
    json_str(f, s);
    rewind(f);
    size_t k = fread(out, 1, n - 1, f);
    out[k] = '\0';
    fclose(f);
    return out;
}

static void test_names(void) {
    char name[] = "ev\"il\\\x1b[31m\x7f\xc3\xa9";
    sanitize(name);
    CHECK_STR(name, "ev\"il\\?[31m???");

    char buf[8];
    copy_str(buf, sizeof buf, "abcdefghij");
    CHECK_STR(buf, "abcdefg");
    copy_str(buf, sizeof buf, "abc");
    CHECK_STR(buf, "abc");

    char out[64];
    CHECK_STR(json_of("plain", out, sizeof out), "\"plain\"");
    CHECK_STR(json_of("a\"b\\c", out, sizeof out), "\"a\\\"b\\\\c\"");
    char hostile[] = "ev\"il\\\x1b[31m";
    sanitize(hostile);
    CHECK_STR(json_of(hostile, out, sizeof out), "\"ev\\\"il\\\\?[31m\"");
}

static void test_format(void) {
    char buf[96];
    g_color = false;
    CHECK_STR(fmt_bw(buf, sizeof buf, true, 12.5, 6), " 12.50 GB/s");
    CHECK_STR(fmt_bw(buf, sizeof buf, false, 12.5, 6), "          -");
    CHECK_STR(fmt_ipc(buf, sizeof buf, 1.234), "1.23");
    CHECK_STR(fmt_ipc(buf, sizeof buf, 0.0), "   -");
    CHECK_STR(bar(buf, sizeof buf, 50.0, 8), "[####----]");
    CHECK_STR(bar(buf, sizeof buf, 150.0, 4), "[####]");
    CHECK_STR(bar(buf, sizeof buf, -5.0, 4), "[----]");
    g_color = true;
    CHECK_STR(bar(buf, sizeof buf, 80.0, 2), "[" RED "##" RST "]");
    g_color = false;
}

static void test_mode(void) {
    opts_t o = {.interval = 1.0};
    outmode_t m = output_mode(&o, NULL, false, true, true, true);
    CHECK(!m.batch && m.color && m.tell && !m.warn_dumb);

    m = output_mode(&o, NULL, true, true, true, true); /* TERM unset or "dumb" */
    CHECK(m.batch && !m.color && !m.tell && m.warn_dumb);
    o.batch = true;
    m = output_mode(&o, NULL, true, true, true, true);
    CHECK(m.batch && !m.warn_dumb);
    o.batch = false;

    m = output_mode(&o, NULL, false, true, false, true); /* stdout is a pipe or file */
    CHECK(m.batch && !m.color);
    m = output_mode(&o, NULL, false, false, true, true); /* no keyboard, but colors still work */
    CHECK(m.batch && m.color);
    m = output_mode(&o, NULL, false, true, true, false); /* stderr redirected */
    CHECK(!m.batch && !m.tell);

    m = output_mode(&o, "1", false, true, true, true);
    CHECK(!m.batch && !m.color);
    m = output_mode(&o, "", false, true, true, true); /* an empty NO_COLOR counts as unset */
    CHECK(m.color);

    o.no_color = true;
    m = output_mode(&o, NULL, false, true, true, true);
    CHECK(!m.batch && !m.color);
    o.no_color = false;

    o.json = o.batch = true;
    m = output_mode(&o, NULL, false, true, true, true);
    CHECK(m.batch && !m.color && !m.tell);
}

static void test_platform_columns(void) {
    g.info = (dt_info_t){.has_split = true, .has_cfs = true}; /* Linux on AMD or Intel */
    const char *const *t = timeline_titles();
    CHECK_STR(t[0], "DEMAND RD");
    CHECK_STR(t[1], "HW PREFETCH");
    CHECK_STR(t[2], "IPC");

    g.info = (dt_info_t){.has_rw = true, .has_gpu = true, .estimated = true}; /* macOS */
    t = timeline_titles();
    CHECK_STR(t[0], "READ");
    CHECK_STR(t[1], "WRITE");
    CHECK_STR(t[2], "GPU%");

    dt_proc_t p = {.m = {.has_bw = true}};
    p.confidence = 0.9;
    CHECK_INT(conf_mark(&p), ' ');
    p.confidence = 0.5;
    CHECK_INT(conf_mark(&p), '~');
    p.confidence = 0.2;
    CHECK_INT(conf_mark(&p), '?');
    p.m.has_bw = false;
    CHECK_INT(conf_mark(&p), ' ');
    g.info.estimated = false;
    p.m.has_bw = true;
    CHECK_INT(conf_mark(&p), ' ');

    g.info = (dt_info_t){.has_rw = true, .rw_system_only = true}; /* Windows with the memory controller counters */
    t = timeline_titles();
    CHECK_STR(t[0], "READ");
    CHECK_STR(t[1], "WRITE");
    CHECK_STR(t[2], "IPC");
    CHECK(!proc_rw());
    g.info.rw_system_only = false;
    CHECK(proc_rw());

    g.info = (dt_info_t){.has_rw = true, .rw_total_only = true, .has_gpu = true}; /* a Mac giving the total only */
    sample_t s = {.m = {.read_gbs = 30.0, .gpu_pct = 40.0}};
    char c3[24], c4[24], c7[16];
    timeline_cells(&s, c3, c4, c7);
    CHECK_STR(c3, "         -");
    CHECK_STR(c4, "          -");
    CHECK_STR(c7, " 40%");
    CHECK(!gpu_traffic_known());
    g.info.nagents = 7;
    g.info.gpu_agent = 2;
    CHECK(gpu_traffic_known());
}

int main(void) {
    test_names();
    test_format();
    test_mode();
    test_platform_columns();
    return check_done("test_output");
}
