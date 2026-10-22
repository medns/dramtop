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

int main(void) {
    test_names();
    test_format();
    test_mode();
    return check_done("test_output");
}
