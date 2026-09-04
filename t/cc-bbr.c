/*
 * Tests for the BBR telemetry and counters (lib/cc-bbr-telemetry.c).
 *
 * BBR itself is not wired yet; what is exercised here is the decoding of `picoquic_bbr_observe()` output and the edge detection
 * built on top of it, driven by synthetic state words. That is the point of building telemetry before the adapter: the counters
 * have to be trustworthy before they are used to judge whether BBR is alive.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "quicly/cc.h"
#include "../deps/picotest/picotest.h"
#include "test.h"

/**
 * Builds the word `picoquic_bbr_observe()` would return.
 */
static uint64_t observed(unsigned state, unsigned ack_phase, unsigned filled_pipe, unsigned in_recovery, unsigned pto_recovery)
{
    return (uint64_t)state | ((uint64_t)ack_phase << 8) | ((uint64_t)filled_pipe << 16) | ((uint64_t)in_recovery << 17) |
           ((uint64_t)pto_recovery << 18);
}

static void test_state_decode(void)
{
    uint64_t w = observed(QUICLY_CC_BBR_PROBE_BW_UP, 3, 1, 0, 1);

    ok(QUICLY_CC_BBR_OBSERVED_STATE(w) == QUICLY_CC_BBR_PROBE_BW_UP);
    ok(QUICLY_CC_BBR_OBSERVED_ACK_PHASE(w) == 3);
    ok(QUICLY_CC_BBR_OBSERVED_FILLED_PIPE(w) == 1);
    ok(QUICLY_CC_BBR_OBSERVED_IN_RECOVERY(w) == 0);
    ok(QUICLY_CC_BBR_OBSERVED_PTO_RECOVERY(w) == 1);

    ok(strcmp(quicly_cc_bbr_state_name(QUICLY_CC_BBR_STARTUP), "startup") == 0);
    ok(strcmp(quicly_cc_bbr_state_name(QUICLY_CC_BBR_PROBE_RTT), "probe_rtt") == 0);
    ok(strcmp(quicly_cc_bbr_state_name(QUICLY_CC_BBR_NUM_STATES), "invalid") == 0);
    ok(strcmp(quicly_cc_bbr_notification_name(QUICLY_CC_BBR_NOTIFY_SPURIOUS_REPEAT), "spurious_repeat") == 0);
    ok(strcmp(quicly_cc_bbr_notification_name(QUICLY_CC_BBR_NUM_NOTIFICATIONS), "invalid") == 0);
}

static void test_state_entries(void)
{
    quicly_cc_t cc = {NULL, 12000};
    quicly_cc_bbr_counters_init(&cc);

    /* three ACKs in startup count as one entry, not three */
    for (int i = 0; i < 3; ++i)
        quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_STARTUP, 0, 0, 0, 0), 0, 0, 100);
    ok(cc.state.bbr.num_state_entries[QUICLY_CC_BBR_STARTUP] == 1);
    ok(cc.state.bbr.num_acks == 3);

    /* advancing through the state machine */
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_DRAIN, 0, 1, 0, 0), 0, 0, 200);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_DOWN, 0, 1, 0, 0), 0, 0, 300);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_CRUISE, 0, 1, 0, 0), 0, 0, 400);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_DOWN, 0, 1, 0, 0), 0, 0, 500);
    ok(cc.state.bbr.num_state_entries[QUICLY_CC_BBR_DRAIN] == 1);
    ok(cc.state.bbr.num_state_entries[QUICLY_CC_BBR_PROBE_BW_DOWN] == 2); /* re-entered, i.e. ProbeBW is cycling */
    ok(cc.state.bbr.num_state_entries[QUICLY_CC_BBR_PROBE_BW_CRUISE] == 1);
    ok(cc.state.bbr.num_state_entries[QUICLY_CC_BBR_PROBE_RTT] == 0);

    /* an out-of-range state must not scribble past the array */
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(200, 0, 1, 0, 0), 0, 0, 600);
    ok(cc.state.bbr.num_state_entries[QUICLY_CC_BBR_STARTUP] == 1);
}

static void test_ack_phase_and_notify(void)
{
    quicly_cc_t cc = {NULL, 12000};
    quicly_cc_bbr_counters_init(&cc);

    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_UP, 0, 1, 0, 0), 0, 0, 100);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_UP, 0, 1, 0, 0), 0, 0, 200);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_UP, 1, 1, 0, 0), 0, 0, 300);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_UP, 2, 1, 0, 0), 0, 0, 400);
    ok(cc.state.bbr.num_ack_phase_changes == 2);

    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_REPEAT, observed(QUICLY_CC_BBR_PROBE_BW_UP, 2, 1, 1, 0), 0, 0, 500);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_TIMEOUT, observed(QUICLY_CC_BBR_PROBE_BW_UP, 2, 1, 1, 1), 0, 0, 600);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_RESTART_FROM_IDLE, observed(QUICLY_CC_BBR_PROBE_BW_UP, 2, 1, 1, 1), 0,
                                  0, 700);
    ok(cc.state.bbr.num_notify[QUICLY_CC_BBR_NOTIFY_ACK] == 4);
    ok(cc.state.bbr.num_notify[QUICLY_CC_BBR_NOTIFY_REPEAT] == 1);
    ok(cc.state.bbr.num_notify[QUICLY_CC_BBR_NOTIFY_TIMEOUT] == 1);
    ok(cc.state.bbr.num_notify[QUICLY_CC_BBR_NOTIFY_RESTART_FROM_IDLE] == 1);
    ok(cc.state.bbr.num_idle_restarts == 1);
    ok(cc.state.bbr.num_pto_recovery == 1);
}

static void test_app_limited(void)
{
    quicly_cc_t cc = {NULL, 12000};
    quicly_cc_bbr_counters_init(&cc);

    for (int i = 0; i < 10; ++i)
        quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_CRUISE, 0, 1, 0, 0), 1000,
                                      i % 5 == 0, 100 + i);
    ok(cc.state.bbr.num_acks == 10);
    ok(cc.state.bbr.num_acks_app_limited == 2);

    /* is_app_limited is only meaningful for ACKs */
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_REPEAT, observed(QUICLY_CC_BBR_PROBE_BW_CRUISE, 0, 1, 0, 0), 1000, 1,
                                  200);
    ok(cc.state.bbr.num_acks_app_limited == 2);
}

static void test_reused_counters(void)
{
    quicly_cc_t cc = {NULL, 12000};
    quicly_cc_bbr_counters_init(&cc);

    ok(cc.cwnd_initial == 12000);
    ok(cc.cwnd_maximum == 12000);
    ok(cc.cwnd_minimum == UINT32_MAX);
    ok(cc.exit_slow_start_at == INT64_MAX);

    /* in startup: no exit recorded, and cwnd extremes track */
    cc.cwnd = 24000;
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_STARTUP, 0, 0, 0, 0), 0, 0, 100);
    ok(cc.cwnd_exiting_slow_start == 0);
    ok(cc.exit_slow_start_at == INT64_MAX);
    ok(cc.cwnd_maximum == 24000);
    ok(cc.cwnd_minimum == 24000);

    /* filled_pipe is BBR's exit from slow start; recorded once, at the cwnd of that moment */
    cc.cwnd = 48000;
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_DRAIN, 0, 1, 0, 0), 0, 0, 200);
    ok(cc.cwnd_exiting_slow_start == 48000);
    ok(cc.exit_slow_start_at == 200);
    cc.cwnd = 60000;
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_DOWN, 0, 1, 0, 0), 0, 0, 300);
    ok(cc.cwnd_exiting_slow_start == 48000);
    ok(cc.exit_slow_start_at == 200);
    ok(cc.cwnd_maximum == 60000);
    ok(cc.cwnd_minimum == 24000); /* the low-water mark stands */

    /* entering recovery is a loss episode, and staying in it is not a second one */
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_REPEAT, observed(QUICLY_CC_BBR_PROBE_BW_DOWN, 0, 1, 1, 0), 0, 0, 400);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_DOWN, 0, 1, 1, 0), 0, 0, 500);
    ok(cc.num_loss_episodes == 1);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_DOWN, 0, 1, 0, 0), 0, 0, 600);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_REPEAT, observed(QUICLY_CC_BBR_PROBE_BW_DOWN, 0, 1, 1, 0), 0, 0, 700);
    ok(cc.num_loss_episodes == 2);

    /* a spurious loss retracts the episode without un-doing the exit from startup */
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_SPURIOUS_REPEAT, observed(QUICLY_CC_BBR_PROBE_BW_DOWN, 0, 1, 0, 0), 0,
                                  0, 800);
    ok(cc.num_loss_episodes == 1);
    ok(cc.num_loss_episodes_undone == 1);
    ok(cc.num_loss_episodes_undone_in_startup == 0);
    ok(cc.cwnd_exiting_slow_start == 48000);
}

static void test_undo_in_startup(void)
{
    quicly_cc_t cc = {NULL, 12000};
    quicly_cc_bbr_counters_init(&cc);

    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_REPEAT, observed(QUICLY_CC_BBR_STARTUP, 0, 0, 1, 0), 0, 0, 100);
    ok(cc.num_loss_episodes == 1);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_SPURIOUS_REPEAT, observed(QUICLY_CC_BBR_STARTUP, 0, 0, 0, 0), 0, 0, 200);
    ok(cc.num_loss_episodes == 0);
    ok(cc.num_loss_episodes_undone == 1);
    ok(cc.num_loss_episodes_undone_in_startup == 1);

    /* must not underflow when more undos arrive than episodes */
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_SPURIOUS_REPEAT, observed(QUICLY_CC_BBR_STARTUP, 0, 0, 0, 0), 0, 0, 300);
    ok(cc.num_loss_episodes == 0);
    ok(cc.num_loss_episodes_undone == 2);
}

static void test_bw(void)
{
    quicly_cc_t cc = {NULL, 12000};
    quicly_cc_bbr_counters_init(&cc);

    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_UP, 0, 1, 0, 0), 1000000, 0, 100);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_UP, 0, 1, 0, 0), 3000000, 0, 200);
    quicly_cc_bbr_counters_update(&cc, QUICLY_CC_BBR_NOTIFY_ACK, observed(QUICLY_CC_BBR_PROBE_BW_UP, 0, 1, 0, 0), 2000000, 0, 300);
    ok(cc.state.bbr.bw_latest == 2000000);
    ok(cc.state.bbr.bw_maximum == 3000000);

}

static void test_pacer_rate(void)
{
    quicly_cc_t cc = {NULL, 12000};
    quicly_cc_bbr_counters_init(&cc);

    /* zero until BBR reports a rate, which is what keeps `calc_pacer_send_rate` on its cwnd/SRTT formula */
    ok(cc.pacer_rate == 0);

    /* bytes/sec in, bytes/msec out */
    ok(quicly_cc_bbr_calc_pacer_rate(1500000) == 1500);

    /* rounds down rather than up: better for the pacer to lag than to overshoot */
    ok(quicly_cc_bbr_calc_pacer_rate(1500999) == 1500);

    /* a rate too small to represent clamps to 1, because 0 would mean "no rate" and hand pacing back to cwnd/SRTT */
    ok(quicly_cc_bbr_calc_pacer_rate(400) == 1);
    ok(quicly_cc_bbr_calc_pacer_rate(0.5) == 1);

    /* absurd rates saturate rather than wrap */
    ok(quicly_cc_bbr_calc_pacer_rate(1e18) == UINT32_MAX);

    /* no rate at all releases the override */
    ok(quicly_cc_bbr_calc_pacer_rate(0) == 0);
    ok(quicly_cc_bbr_calc_pacer_rate(-1) == 0);
}

static void test_telemetry_disabled(void)
{
    unsetenv("QUICLY_BBR_CSV_DIR");
    quicly_cc_bbr_telemetry_t *telemetry = quicly_cc_bbr_telemetry_create();
    ok(telemetry == NULL);

    /* recording into a disabled ring is a no-op rather than a crash, so the adapter need not test */
    quicly_cc_bbr_sample_t sample = {0};
    quicly_cc_bbr_telemetry_record(telemetry, &sample);
    ok(quicly_cc_bbr_telemetry_num_recorded(telemetry) == 0);
    quicly_cc_bbr_telemetry_destroy(telemetry);
}

/**
 * Removes the single `bbr-*.csv` the telemetry is expected to have written into `dir`, returning its first line (caller frees) or
 * NULL if no such file exists. The file name carries a per-process sequence number that the test cannot predict, hence the scan.
 */
static char *take_csv_first_line(const char *dir)
{
    DIR *dp;
    struct dirent *ent;
    char *line = NULL;

    if ((dp = opendir(dir)) == NULL)
        return NULL;
    while ((ent = readdir(dp)) != NULL) {
        if (strncmp(ent->d_name, "bbr-", 4) != 0)
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        FILE *fp;
        if ((fp = fopen(path, "r")) != NULL) {
            char buf[512];
            if (fgets(buf, sizeof(buf), fp) != NULL)
                line = strdup(buf);
            fclose(fp);
        }
        remove(path);
    }
    closedir(dp);

    return line;
}

static void test_telemetry_ring(void)
{
    char dir[] = "/tmp/quicly-bbr-test-XXXXXX";
    ok(mkdtemp(dir) != NULL);
    setenv("QUICLY_BBR_CSV_DIR", dir, 1);
    setenv("QUICLY_BBR_CSV_ROWS", "4", 1);

    quicly_cc_bbr_telemetry_t *telemetry = quicly_cc_bbr_telemetry_create();
    ok(telemetry != NULL);

    /* overfill: 6 rows into a ring of 4 retains the last 4, in order */
    for (uint64_t i = 0; i < 6; ++i) {
        quicly_cc_bbr_sample_t sample = {0};
        sample.now_us = 1000 + i;
        sample.notification = QUICLY_CC_BBR_NOTIFY_ACK;
        sample.cwnd = 10000 + i;
        sample.cc_state = observed(QUICLY_CC_BBR_PROBE_BW_CRUISE, 1, 1, 0, 0);
        sample.bw = 500000;
        quicly_cc_bbr_telemetry_record(telemetry, &sample);
    }
    ok(quicly_cc_bbr_telemetry_num_recorded(telemetry) == 6);
    ok(quicly_cc_bbr_telemetry_num_retained(telemetry) == 4);

    char *buf = NULL;
    size_t buflen = 0;
    FILE *fp = open_memstream(&buf, &buflen);
    quicly_cc_bbr_telemetry_dump(telemetry, fp);
    fclose(fp);

    /* header, then the four surviving rows oldest-first */
    ok(strncmp(buf, "now_us,notification,", 20) == 0);
    ok(strstr(buf, "\n1002,ack,") != NULL);
    ok(strstr(buf, "\n1005,ack,") != NULL);
    ok(strstr(buf, "\n1001,ack,") == NULL); /* evicted */
    ok(strstr(buf, "probe_bw_cruise") != NULL);
    ok(strstr(buf, ",500000\n") != NULL);
    /* the first retained row must precede the last */
    ok(strstr(buf, "\n1002,ack,") < strstr(buf, "\n1005,ack,"));

    size_t num_lines = 0;
    for (const char *p = buf; (p = strchr(p, '\n')) != NULL; ++p)
        ++num_lines;
    ok(num_lines == 5);

    free(buf);

    /* destroy writes the same content to the configured directory */
    quicly_cc_bbr_telemetry_destroy(telemetry);
    char *first_line = take_csv_first_line(dir);
    ok(first_line != NULL);
    if (first_line != NULL) {
        ok(strncmp(first_line, "now_us,notification,", 20) == 0);
        free(first_line);
    }
    rmdir(dir);

    unsetenv("QUICLY_BBR_CSV_DIR");
    unsetenv("QUICLY_BBR_CSV_ROWS");
}

void test_cc_bbr(void)
{
    subtest("state-decode", test_state_decode);
    subtest("state-entries", test_state_entries);
    subtest("ack-phase-and-notify", test_ack_phase_and_notify);
    subtest("app-limited", test_app_limited);
    subtest("reused-counters", test_reused_counters);
    subtest("undo-in-startup", test_undo_in_startup);
    subtest("bw", test_bw);
    subtest("pacer-rate", test_pacer_rate);
    subtest("telemetry-disabled", test_telemetry_disabled);
    subtest("telemetry-ring", test_telemetry_ring);
}
