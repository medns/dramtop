/* Unit tests: command line parsing and the +/- interval steps. */

#define main dramtop_main
#include "../src/dramtop.c"
#undef main

#include "check.h"

static opts_t o;
static char err[512];

static int parse_list(const char **args) {
    int argc = 0;
    while (args[argc]) argc++;
    err[0] = '\0';
    return parse_args(argc, (char **)args, &o, err, sizeof err);
}

#define PARSE(...) parse_list((const char *[]){"dramtop", __VA_ARGS__, NULL})

static void test_accepted(void) {
    CHECK_INT(parse_list((const char *[]){"dramtop", NULL}), ARGS_OK);
    CHECK_NEAR(o.interval, 1.0, 0.0);
    CHECK(!o.filter_pid && !o.filter_name && !o.max_count && !o.ceiling);
    CHECK(!o.batch && !o.json && !o.no_color);

    CHECK_INT(PARSE("-p", "123"), ARGS_OK);
    CHECK_INT(o.filter_pid, 123);
    CHECK_INT(PARSE("-p124"), ARGS_OK);
    CHECK_INT(o.filter_pid, 124);
    CHECK_INT(PARSE("--pid", "77"), ARGS_OK);
    CHECK_INT(o.filter_pid, 77);
    CHECK_INT(PARSE("--pid=78"), ARGS_OK);
    CHECK_INT(o.filter_pid, 78);
    CHECK_INT(PARSE("--inter", "0.5"), ARGS_OK); /* a unique prefix is enough */
    CHECK_NEAR(o.interval, 0.5, 0.0);

    CHECK_INT(PARSE("-bJ"), ARGS_OK);
    CHECK(o.batch && o.json);
    CHECK_INT(PARSE("-bi0.25", "-n", "3"), ARGS_OK);
    CHECK(o.batch && !o.json);
    CHECK_NEAR(o.interval, 0.25, 0.0);
    CHECK_INT(o.max_count, 3);
    CHECK_INT(PARSE("-bn", "4"), ARGS_OK);
    CHECK_INT(o.max_count, 4);

    CHECK_INT(PARSE("-P", "python3", "-c", "120.5"), ARGS_OK);
    CHECK_STR(o.filter_name, "python3");
    CHECK_NEAR(o.ceiling, 120.5, 0.0);
    CHECK_INT(PARSE("-P", ""), ARGS_OK);
    CHECK(o.filter_name == NULL);
    CHECK_INT(PARSE("--no-color", "--json"), ARGS_OK);
    CHECK(o.no_color && o.json && o.batch);
    CHECK_INT(PARSE("-i", "1", "-i", "2"), ARGS_OK);
    CHECK_NEAR(o.interval, 2.0, 0.0);
    CHECK_INT(PARSE("-i", "3600"), ARGS_OK);
    CHECK_INT(PARSE("-b", "--"), ARGS_OK);
}

static void test_rejected(void) {
    CHECK_INT(PARSE("-i", "0.05"), ARGS_ERROR);
    CHECK_STR(err, "dramtop: invalid value for --interval: '0.05'\nTry 'dramtop --help'.\n");
    CHECK_INT(PARSE("-i", "3601"), ARGS_ERROR);
    CHECK_INT(PARSE("-i", "nan"), ARGS_ERROR);
    CHECK_INT(PARSE("-i", "1s"), ARGS_ERROR);
    CHECK_INT(PARSE("-i", "-5"), ARGS_ERROR); /* taken as the value even though it looks like an option */
    CHECK_INT(PARSE("-p", "0"), ARGS_ERROR);
    CHECK_INT(PARSE("-n", "0"), ARGS_ERROR);
    CHECK_INT(PARSE("--ceiling="), ARGS_ERROR);
    CHECK_STR(err, "dramtop: invalid value for --ceiling: ''\nTry 'dramtop --help'.\n");

    CHECK_INT(PARSE("-p"), ARGS_ERROR);
    CHECK_STR(err, "dramtop: option requires an argument -- 'p'\nTry 'dramtop --help'.\n");
    CHECK_INT(PARSE("--pid"), ARGS_ERROR);
    CHECK_STR(err, "dramtop: option '--pid' requires an argument\nTry 'dramtop --help'.\n");
    CHECK_INT(PARSE("-x"), ARGS_ERROR);
    CHECK_STR(err, "dramtop: invalid option -- 'x'\nTry 'dramtop --help'.\n");
    CHECK_INT(PARSE("--bogus"), ARGS_ERROR);
    CHECK_STR(err, "dramtop: unrecognized option '--bogus'\nTry 'dramtop --help'.\n");
    CHECK_INT(PARSE("--p", "5"), ARGS_ERROR); /* --pid or --proc */
    CHECK_STR(err, "dramtop: option '--p' is ambiguous\nTry 'dramtop --help'.\n");
    CHECK_INT(PARSE("--no-color=1"), ARGS_ERROR);
    CHECK_STR(err, "dramtop: option '--no-color' doesn't allow an argument\nTry 'dramtop --help'.\n");

    CHECK_INT(PARSE("foo"), ARGS_ERROR);
    CHECK_STR(err, "dramtop: unexpected argument 'foo'\nTry 'dramtop --help'.\n");
    CHECK_INT(PARSE("-", "-b"), ARGS_ERROR);
    CHECK_INT(PARSE("--", "-b"), ARGS_ERROR);
    CHECK_STR(err, "dramtop: unexpected argument '-b'\nTry 'dramtop --help'.\n");
}

static void test_help_and_version(void) {
    CHECK_INT(PARSE("-h"), ARGS_HELP);
    CHECK_INT(PARSE("--version"), ARGS_VERSION);
    CHECK_INT(PARSE("--ver"), ARGS_VERSION);
    CHECK_INT(PARSE("-bh"), ARGS_HELP);
    CHECK_INT(PARSE("foo", "-h"), ARGS_HELP); /* options are handled before stray operands, as with getopt_long */
    CHECK_INT(PARSE("--help", "--bogus"), ARGS_HELP);
    CHECK_INT(PARSE("--bogus", "--help"), ARGS_ERROR);
}

/* '+' calls step_interval(-1) for a faster refresh, '-' calls step_interval(+1). */
static void test_interval_steps(void) {
    double v = 1.0;
    v = step_interval(v, +1);
    CHECK_NEAR(v, 2.0, 0.0);
    v = step_interval(v, +1);
    CHECK_NEAR(v, 5.0, 0.0);
    v = step_interval(v, +1);
    CHECK_NEAR(v, 5.0, 0.0);
    v = step_interval(step_interval(v, -1), -1);
    CHECK_NEAR(v, 1.0, 0.0);

    v = step_interval(1.0, -1);
    CHECK_NEAR(v, 0.5, 0.0);
    v = step_interval(step_interval(step_interval(v, -1), -1), -1);
    CHECK_NEAR(v, 0.1, 0.0);
    v = step_interval(step_interval(step_interval(v, +1), +1), +1);
    CHECK_NEAR(v, 1.0, 0.0);

    CHECK_NEAR(step_interval(0.7, +1), 1.0, 0.0); /* an -i value off the steps snaps to the nearest first */
    CHECK_NEAR(step_interval(3600.0, -1), 2.0, 0.0);
}

int main(void) {
    test_accepted();
    test_rejected();
    test_help_and_version();
    test_interval_steps();
    return check_done("test_cli");
}
