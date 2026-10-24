/* Unit tests (Linux): counter deltas under multiplexing, how event counts add up
 * to DRAM lines, and which PMU events are chosen for each CPU model. */

#include "../src/linux.c"

#include <ftw.h>
#include <stdarg.h>
#include <sys/stat.h>

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

static void test_correction(void) {
    g.line = 64;
    CHECK_NEAR(read_scale_of(1170, 64000), 1 / 1.17, 1e-12); /* LLC misses 17% above the lines read */
    CHECK_NEAR(read_scale_of(500, 64000), 1.0, 0.0);          /* implausible either way: left as counted */
    CHECK_NEAR(read_scale_of(3000, 64000), 1.0, 0.0);
    CHECK_NEAR(read_scale_of(100, 0), 1.0, 0.0);

    uint64_t acc[EV_KINDS] = {0};
    acc[EV_TOTAL] = 900;
    acc[EV_DEMAND] = 600;
    acc[EV_CYCLES] = 2000;
    acc[EV_INSNS] = 3000;
    dt_counts_t c;
    g.has_total = true;
    g.read_scale = 0.5;
    to_counts(acc, &c);
    CHECK_NEAR(c.total, 450, 0.0);
    CHECK_NEAR(c.demand, 300, 0.0);
    CHECK_NEAR(c.prefetch, 150, 0.0);
    CHECK_NEAR(c.cycles, 2000, 0.0); /* only DRAM lines are corrected */
    CHECK_NEAR(c.insns, 3000, 0.0);

    /* With the memory controller: reads by the counted lines, writes by the RFOs */
    g.read_scale = 1.0;
    g.write_scale = 0.5;
    acc[EV_RFO] = 300;
    to_counts(acc, &c);
    CHECK_NEAR(c.read, 900, 0.0);
    CHECK_NEAR(c.write, 150, 0.0);
    CHECK_NEAR(c.total, 1050, 0.0);
    g.write_scale = 0.0;
}

static void test_readers(void) {
    g.ref_rate = 1e6; /* counted lines per second of one calibration thread */
    CHECK_NEAR(reader_weight(5e5, 1.0), 0.5, 1e-12);
    CHECK_NEAR(reader_weight(3e6, 1.0), 1.0, 0.0); /* one thread alone reads faster, but is one reader */
    CHECK_NEAR(reader_weight(1e6, 0.0), 0.0, 0.0);

    g.cal_threads = 16;
    g.scale_one = 1 / 1.08;
    g.scale_all = 1 / 1.21;
    CHECK_NEAR(readers_scale(1.0), 1 / 1.08, 1e-12);
    CHECK_NEAR(readers_scale(0.3), 1 / 1.08, 1e-12);
    CHECK_NEAR(readers_scale(8.5), 1 / 1.145, 1e-12); /* halfway */
    CHECK_NEAR(readers_scale(16.0), 1 / 1.21, 1e-12);
    CHECK_NEAR(readers_scale(40.0), 1 / 1.21, 1e-12);
    g.cal_threads = 1;
    CHECK_NEAR(readers_scale(1.0), 1 / 1.21, 1e-12);
}

/* A sysfs tree of event sources, made up for each case. */
static char sysfs[64];

static void put(const char *rel, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", sysfs, rel);
    for (char *p = path + strlen(sysfs) + 1; (p = strchr(p, '/')) != NULL; p++) {
        *p = '\0';
        mkdir(path, 0755);
        *p = '/';
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

DT_PRINTF(2, 3) static void putf(const char *text, const char *fmt, ...) {
    char rel[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rel, sizeof rel, fmt, ap);
    va_end(ap);
    put(rel, text);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st, (void)flag, (void)ftw;
    return remove(path);
}

static bool new_sysfs(void) {
    if (sysfs[0]) nftw(sysfs, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
    snprintf(sysfs, sizeof sysfs, "/tmp/dramtop-test-XXXXXX");
    g.mc_root = sysfs;
    return mkdtemp(sysfs) != NULL;
}

static int count_spec(const mc_spec_t *s, int n, int kind, uint64_t config, int cpu) {
    int found = 0;
    for (int i = 0; i < n; i++) found += s[i].kind == kind && s[i].config == config && s[i].cpu == cpu;
    return found;
}

static void test_memory_controller(void) {
    int *ids = NULL, nids = 0, cap = 0;
    parse_cpu_list("0-3,8,10-11\n", &ids, &nids, &cap);
    CHECK_INT(nids, 7);
    CHECK(nids == 7 && ids[3] == 3 && ids[4] == 8 && ids[6] == 11);
    free(ids);

    mc_spec_t s[MAX_MC];
    bool amd = false;
    int n;

    /* Comet Lake: one PMU, with the cores' share of the traffic */
    CHECK(new_sysfs());
    put("uncore_imc/type", "13\n");
    put("uncore_imc/cpumask", "0\n");
    put("uncore_imc/format/event", "config:0-7\n");
    put("uncore_imc/events/data_reads", "event=0x01\n");
    put("uncore_imc/events/data_reads.scale", "6.103515625e-5\n");
    put("uncore_imc/events/data_reads.unit", "MiB\n");
    put("uncore_imc/events/data_writes", "event=0x02\n");
    put("uncore_imc/events/ia_requests", "event=0x04\n");
    put("cpu/type", "4\n");
    put("cpu/events/cache-misses", "event=0x2e,umask=0x41\n");
    n = mc_find(s, MAX_MC, &amd);
    CHECK_INT(n, 3);
    CHECK(count_spec(s, n, MC_RD, 0x01, 0) == 1 && count_spec(s, n, MC_WR, 0x02, 0) == 1);
    CHECK_INT(count_spec(s, n, MC_IA, 0x04, 0), 1);
    CHECK(!amd && n > 0 && s[0].type == 13);
    CHECK_NEAR(s[0].bytes, 64.0, 1e-9);

    /* Alder Lake: free-running counters per controller; the programmable ones name no events */
    CHECK(new_sysfs());
    for (int i = 0; i < 2; i++) {
        putf(i ? "21\n" : "20\n", "uncore_imc_free_running_%d/type", i);
        putf("0\n", "uncore_imc_free_running_%d/cpumask", i);
        putf("config:0-7\n", "uncore_imc_free_running_%d/format/event", i);
        putf("config:8-15\n", "uncore_imc_free_running_%d/format/umask", i);
        putf("event=0xff,umask=0x20\n", "uncore_imc_free_running_%d/events/data_read", i);
        putf("event=0xff,umask=0x30\n", "uncore_imc_free_running_%d/events/data_write", i);
        putf("18\n", "uncore_imc_%d/type", i);
        putf("config:0-7\n", "uncore_imc_%d/format/event", i);
    }
    n = mc_find(s, MAX_MC, &amd);
    CHECK_INT(n, 4);
    CHECK(count_spec(s, n, MC_RD, 0x20ff, 0) == 2 && count_spec(s, n, MC_WR, 0x30ff, 0) == 2);

    /* A two-socket server: one PMU per channel, opened on one CPU of each socket */
    CHECK(new_sysfs());
    for (int i = 0; i < 2; i++) {
        putf("30\n", "uncore_imc_%d/type", i);
        putf("0,56\n", "uncore_imc_%d/cpumask", i);
        putf("config:0-7\n", "uncore_imc_%d/format/event", i);
        putf("config:8-15\n", "uncore_imc_%d/format/umask", i);
        putf("event=0x05,umask=0xcf\n", "uncore_imc_%d/events/cas_count_read", i);
        putf("event=0x05,umask=0xf0\n", "uncore_imc_%d/events/cas_count_write", i);
    }
    n = mc_find(s, MAX_MC, &amd);
    CHECK_INT(n, 8);
    CHECK(count_spec(s, n, MC_RD, 0xcf05, 0) == 2 && count_spec(s, n, MC_RD, 0xcf05, 56) == 2);
    CHECK(count_spec(s, n, MC_WR, 0xf005, 0) == 2 && count_spec(s, n, MC_WR, 0xf005, 56) == 2);

    /* AMD Zen 4: one PMU per memory controller, which lists no events */
    CHECK(new_sysfs());
    put("amd_umc_0/type", "40\n");
    put("amd_umc_0/cpumask", "0\n");
    put("amd_umc_0/format/event", "config:0-7\n");
    put("amd_umc_0/format/rdwrmask", "config:8-9\n");
    put("amd_umc_1/type", "41\n");
    put("amd_umc_1/cpumask", "16\n");
    put("amd_umc_1/format/event", "config:0-7\n");
    put("amd_umc_1/format/rdwrmask", "config:8-9\n");
    n = mc_find(s, MAX_MC, &amd);
    CHECK_INT(n, 4);
    CHECK(amd && count_spec(s, n, MC_RD, 0x10a, 0) == 1 && count_spec(s, n, MC_WR, 0x20a, 16) == 1);

    /* Event files that cannot be placed */
    uint64_t config = 0;
    CHECK(!mc_field(sysfs, "nonexistent", 1, &config));
    put("format/narrow", "config:0-1\n");
    CHECK(!mc_field(sysfs, "narrow", 4, &config)); /* does not fit in two bits */
    put("format/high", "config1:0-31\n");
    CHECK(!mc_field(sysfs, "high", 1, &config));   /* only config itself is used */
    CHECK(!mc_config(sysfs, "event=0x01", &config));
    nftw(sysfs, rm_entry, 8, FTW_DEPTH | FTW_PHYS);

    double conf;
    CHECK_NEAR(mc_scale(150, 100, &conf), 1.5, 1e-12);
    CHECK_NEAR(conf, 1.0, 0.0);
    CHECK_NEAR(mc_scale(300, 100, &conf), 1.5, 1e-12); /* capped: the rest is left unattributed */
    CHECK_NEAR(conf, 0.5, 1e-12);
    CHECK_NEAR(mc_scale(100, 0, &conf), 0.0, 0.0);
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
    CHECK(use_rfo()); /* with the memory controller, the RFOs replace the demand loads */
    CHECK(has_raw(0xe224, EV_RFO) && !has_raw(0x20d1, EV_DEMAND));
    CHECK(!g.has_split && g.nev == 4);
    cpu_id_t ivb = {.vendor = V_INTEL, .family = 6, .model = 0x3e};
    select_events(&ivb, false);
    CHECK(!use_rfo() && has_raw(0x20d1, EV_DEMAND)); /* encoded otherwise there */
    select_events(&genoa, false);
    CHECK(!use_rfo());

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
    test_correction();
    test_readers();
    test_memory_controller();
    test_events();
    return check_done("test_counter");
}
