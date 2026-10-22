/* Unit tests (Linux): counter deltas under multiplexing, how event counts add up
 * to DRAM lines, and which PMU events are chosen for each CPU model. */

#include "../src/linux.c"

#include "check.h"

static void test_delta(void) {
    snap_t s = {0};
    CHECK_INT(snap_delta(&s, (const uint64_t[3]){1000, 100, 100}), 1000); /* counted all the time */
    CHECK_INT(snap_delta(&s, (const uint64_t[3]){1500, 200, 150}), 1000); /* counted half the time: 500 scaled up */
    CHECK_INT(snap_delta(&s, (const uint64_t[3]){1500, 300, 150}), 0);    /* never scheduled on the PMU */
    CHECK_INT(snap_delta(&s, (const uint64_t[3]){10, 5, 5}), 0);          /* went backwards: new baseline */
    CHECK_INT(snap_delta(&s, (const uint64_t[3]){30, 15, 15}), 20);
    CHECK_INT(ctr_delta(-1, &s), 0);
}

static void test_lines(void) {
    uint64_t acc[EV_KINDS] = {0};
    acc[EV_DEMAND] = 600;
    acc[EV_PREFETCH] = 400;
    acc[EV_CYCLES] = 2000;
    acc[EV_INSNS] = 3000;
    dt_counts_t c;

    g.has_total = false; /* AMD: demand and prefetch fills are separate events */
    CHECK_INT(total_lines(acc), 1000);
    to_counts(acc, &c);
    CHECK_NEAR(c.total, 1000, 0.0);
    CHECK_NEAR(c.demand, 600, 0.0);
    CHECK_NEAR(c.prefetch, 400, 0.0);
    CHECK_NEAR(c.cycles, 2000, 0.0);
    CHECK_NEAR(c.insns, 3000, 0.0);

    g.has_total = true; /* Intel: one event counts every LLC miss and demand loads are a subset */
    acc[EV_TOTAL] = 900;
    acc[EV_PREFETCH] = 0;
    CHECK_INT(total_lines(acc), 900);
    to_counts(acc, &c);
    CHECK_NEAR(c.prefetch, 300, 0.0);
    acc[EV_TOTAL] = 500; /* multiplexed estimates can put the subset above the total */
    CHECK_INT(total_lines(acc), 600);
}

static bool has_raw(uint64_t config, enum ev_kind kind) {
    for (int i = 0; i < g.nev; i++)
        if (g.ev[i].type == PERF_TYPE_RAW && g.ev[i].config == config && g.ev[i].kind == kind) return true;
    return false;
}

static void test_events(void) {
    cpu_id_t genoa = {.vendor = V_AMD, .family = 0x19, .model = 0x11};
    select_events(&genoa, false);
    CHECK_INT(g.nev, 5);
    CHECK(has_raw(0x4843, EV_DEMAND) && has_raw(0x485a, EV_PREFETCH) && has_raw(0x1f72, EV_PREFETCH));
    CHECK(!has_raw(0xff72, EV_PREFETCH)); /* would count L1 prefetches a second time */
    CHECK(g.has_split && !g.has_total);

    cpu_id_t bergamo = {.vendor = V_AMD, .family = 0x19, .model = 0xa0};
    select_events(&bergamo, false);
    CHECK(has_raw(0x1f72, EV_PREFETCH));
    cpu_id_t zen5 = {.vendor = V_AMD, .family = 0x1a, .model = 0x02};
    select_events(&zen5, false);
    CHECK(has_raw(0x1f72, EV_PREFETCH));
    cpu_id_t milan = {.vendor = V_AMD, .family = 0x19, .model = 0x01};
    select_events(&milan, false);
    CHECK(has_raw(0xff72, EV_PREFETCH) && !has_raw(0x1f72, EV_PREFETCH));

    cpu_id_t intel = {.vendor = V_INTEL, .family = 6, .model = 0x8f};
    select_events(&intel, false);
    CHECK_INT(g.ev[0].type, PERF_TYPE_HARDWARE);
    CHECK_INT(g.ev[0].config, PERF_COUNT_HW_CACHE_MISSES);
    CHECK(has_raw(0x20d1, EV_DEMAND));
    CHECK(g.has_split && g.has_total);

    select_events(&genoa, true); /* fallback when the model-specific events cannot be opened */
    CHECK_INT(g.nev, 3);
    CHECK_STR(g.backend, "Generic PMU (LLC misses)");
    CHECK(!g.has_split && g.has_total);
    CHECK_INT(g.ev[1].config, PERF_COUNT_HW_CPU_CYCLES);
    CHECK_INT(g.ev[2].config, PERF_COUNT_HW_INSTRUCTIONS);
}

int main(void) {
    test_delta();
    test_lines();
    test_events();
    return check_done("test_counter");
}
