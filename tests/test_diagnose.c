/* Unit tests: rates computed from counter totals, and the bottleneck diagnosis. */

#define main dramtop_main
#include "../src/dramtop.c"
#undef main

#include "check.h"

static metrics_t measured(double gbs, double cpu_pct, double ipc, double mpki) {
    return (metrics_t){.has_bw = true, .total_gbs = gbs, .util_pct = util_of(gbs), .cpu_pct = cpu_pct, .ipc = ipc,
                       .mpki = mpki};
}

static void test_rates(void) {
    g.info.line = 64;
    dt_counts_t c = {.total = 1e9, .demand = 6e8, .prefetch = 4e8, .cycles = 2e9, .insns = 3e9};
    metrics_t m = {0};
    compute_rates(&m, &c, 2.0);
    CHECK(m.has_bw);
    CHECK_NEAR(m.total_gbs, 32.0, 1e-9); /* 1e9 lines of 64 bytes in 2 seconds */
    CHECK_NEAR(m.demand_gbs, 19.2, 1e-9);
    CHECK_NEAR(m.prefetch_gbs, 12.8, 1e-9);
    CHECK_NEAR(m.ipc, 1.5, 1e-12);
    CHECK_NEAR(m.mpki, 1e9 * 1000.0 / 3e9, 1e-9);

    dt_counts_t none = {0};
    compute_rates(&m, &none, 1.0);
    CHECK_NEAR(m.ipc, 0.0, 0.0);
    CHECK_NEAR(m.mpki, 0.0, 0.0);

    g.peak = 200.0;
    CHECK_NEAR(util_of(50.0), 25.0, 1e-12);
    CHECK_NEAR(util_of(500.0), 100.0, 0.0);
    g.peak = 0.0;
    CHECK_NEAR(util_of(50.0), 0.0, 0.0);
}

static void test_rules(void) {
    g.peak = 100.0;
    g.info.eff_cpus = 8;

    metrics_t m = {.cpu_pct = 10.0}; /* counters not read yet */
    CHECK_INT(diagnose(&m, 0), ST_SAMPLING);
    m.cpu_pct = 1.0;
    CHECK_INT(diagnose(&m, 0), ST_IDLE);

    m = measured(0.1, 2.0, 1.0, 1.0);
    CHECK_INT(diagnose(&m, 0), ST_IDLE);

    m = measured(50.0, 10.0, 1.0, 1.0);
    CHECK_INT(diagnose(&m, 0), ST_MEM_BW);
    m = measured(42.0, 10.0, 1.0, 1.0);
    CHECK_INT(diagnose(&m, 0), ST_MEM_BW);
    m = measured(41.9, 10.0, 1.0, 1.0);
    CHECK_INT(diagnose(&m, 0), ST_ACTIVE);

    m = measured(10.0, 95.0, 1.5, 1.0);
    CHECK_INT(diagnose(&m, 0.3), ST_CONTENTION); /* CFS throttled in 30% of the periods */
    CHECK_INT(diagnose(&m, 0.2), ST_COMPUTE);

    m = measured(10.0, 95.0, 1.0, 12.0);
    CHECK_INT(diagnose(&m, 0), ST_MEM_LAT);
    m = measured(10.0, 95.0, 0.4, 4.0);
    CHECK_INT(diagnose(&m, 0), ST_MEM_LAT);
    m = measured(10.0, 95.0, 0.8, 4.0);
    CHECK_INT(diagnose(&m, 0), ST_COMPUTE);
    m = measured(10.0, 50.0, 0.3, 20.0); /* DRAM-heavy, but less than one core busy */
    CHECK_INT(diagnose(&m, 0), ST_ACTIVE);

    m = measured(5.0, 760.0, 0.2, 1.0); /* all 8 CPUs busy at a very low IPC without DRAM misses */
    CHECK_INT(diagnose(&m, 0), ST_CONTENTION);
    m = measured(5.0, 700.0, 0.2, 1.0);
    CHECK_INT(diagnose(&m, 0), ST_COMPUTE);

    CHECK_STR(STATE[ST_MEM_BW].label, "MEMORY-BOUND (DRAM BW Saturated)");
    CHECK_STR(STATE[ST_CONTENTION].label, "LOCK-CONTENTION / CFS-THROTTLED");
    CHECK_STR(STATE[ST_NOACCESS].label, "NO PMU ACCESS");
}

int main(void) {
    test_rates();
    test_rules();
    return check_done("test_diagnose");
}
