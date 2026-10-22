/*
 * Minimal assertions for the unit tests. Every failed check is printed, and
 * check_done() turns the result into the exit status.
 */

#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <string.h>

static int checks_run, checks_failed;

#define CHECK(cond)            check_true((cond) != 0, #cond, __FILE__, __LINE__)
#define CHECK_INT(a, b)        check_int((long long)(a), (long long)(b), #a, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps)  check_near((a), (b), (eps), #a, __FILE__, __LINE__)
#define CHECK_STR(a, b)        check_str((a), (b), #a, __FILE__, __LINE__)

static inline void check_true(int ok, const char *expr, const char *file, int line) {
    checks_run++;
    if (ok) return;
    checks_failed++;
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
}

static inline void check_int(long long a, long long b, const char *expr, const char *file, int line) {
    checks_run++;
    if (a == b) return;
    checks_failed++;
    fprintf(stderr, "%s:%d: %s is %lld, expected %lld\n", file, line, expr, a, b);
}

static inline void check_near(double a, double b, double eps, const char *expr, const char *file, int line) {
    checks_run++;
    double d = a > b ? a - b : b - a;
    if (d <= eps) return;
    checks_failed++;
    fprintf(stderr, "%s:%d: %s is %.9g, expected %.9g\n", file, line, expr, a, b);
}

static inline void check_str(const char *a, const char *b, const char *expr, const char *file, int line) {
    checks_run++;
    if (a && b && !strcmp(a, b)) return;
    checks_failed++;
    fprintf(stderr, "%s:%d: %s is \"%s\", expected \"%s\"\n", file, line, expr, a ? a : "(null)", b ? b : "(null)");
}

static inline int check_done(const char *name) {
    if (checks_failed) {
        fprintf(stderr, "%s: %d of %d checks failed\n", name, checks_failed, checks_run);
        return 1;
    }
    printf("%s: %d checks passed\n", name, checks_run);
    return 0;
}

#endif
