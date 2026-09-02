/*
 * Telemetry and counters for BBR. See include/quicly/cc-bbr.h.
 *
 * This file must not include any picoquic header; it works purely off values the adapter hands it.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "quicly/cc.h"

#define CSV_PATH_MAX 1024

struct st_quicly_cc_bbr_telemetry_t {
    /**
     * where the CSV is written on destroy
     */
    char *path;
    /**
     * number of slots in `samples`
     */
    size_t capacity;
    /**
     * index of the next slot to write
     */
    size_t pos;
    /**
     * total recorded, which keeps counting after the ring wraps
     */
    size_t num_recorded;
    quicly_cc_bbr_sample_t samples[1]; /* variable length */
};

const char *quicly_cc_bbr_state_name(unsigned state)
{
    static const char *names[] = {"startup",     "drain",     "probe_bw_down",    "probe_bw_cruise", "probe_bw_refill",
                                  "probe_bw_up", "probe_rtt", "startup_long_rtt", "startup_resume"};

    return state < QUICLY_CC_BBR_NUM_STATES ? names[state] : "invalid";
}

const char *quicly_cc_bbr_notification_name(unsigned notification)
{
    static const char *names[] = {"ack", "repeat", "timeout", "restart_from_idle", "spurious_repeat"};

    return notification < QUICLY_CC_BBR_NUM_NOTIFICATIONS ? names[notification] : "invalid";
}

quicly_cc_bbr_telemetry_t *quicly_cc_bbr_telemetry_create(void)
{
    const char *dir = getenv("QUICLY_BBR_CSV_DIR");
    if (dir == NULL || *dir == '\0')
        return NULL;

    size_t capacity = QUICLY_CC_BBR_TELEMETRY_DEFAULT_ROWS;
    const char *rows = getenv("QUICLY_BBR_CSV_ROWS");
    if (rows != NULL) {
        long v = strtol(rows, NULL, 10);
        if (v > 0)
            capacity = (size_t)v;
    }

    /* one file per connection; `seq` only separates connections within this process, hence the per-run directory */
    static unsigned seq;
    char path[CSV_PATH_MAX];
    int pathlen = snprintf(path, sizeof(path), "%s/bbr-%u.csv", dir, seq);
    if (pathlen < 0 || (size_t)pathlen >= sizeof(path))
        return NULL;

    quicly_cc_bbr_telemetry_t *telemetry;
    if ((telemetry = malloc(offsetof(quicly_cc_bbr_telemetry_t, samples) + sizeof(telemetry->samples[0]) * capacity)) == NULL)
        return NULL;
    if ((telemetry->path = malloc((size_t)pathlen + 1)) == NULL) {
        free(telemetry);
        return NULL;
    }
    memcpy(telemetry->path, path, (size_t)pathlen + 1);
    telemetry->capacity = capacity;
    telemetry->pos = 0;
    telemetry->num_recorded = 0;
    ++seq;

    return telemetry;
}

void quicly_cc_bbr_telemetry_record(quicly_cc_bbr_telemetry_t *telemetry, const quicly_cc_bbr_sample_t *sample)
{
    if (telemetry == NULL)
        return;

    telemetry->samples[telemetry->pos] = *sample;
    if (++telemetry->pos == telemetry->capacity)
        telemetry->pos = 0;
    ++telemetry->num_recorded;
}

size_t quicly_cc_bbr_telemetry_num_retained(const quicly_cc_bbr_telemetry_t *telemetry)
{
    if (telemetry == NULL)
        return 0;
    return telemetry->num_recorded < telemetry->capacity ? telemetry->num_recorded : telemetry->capacity;
}

size_t quicly_cc_bbr_telemetry_num_recorded(const quicly_cc_bbr_telemetry_t *telemetry)
{
    return telemetry != NULL ? telemetry->num_recorded : 0;
}

void quicly_cc_bbr_telemetry_dump(quicly_cc_bbr_telemetry_t *telemetry, FILE *fp)
{
    if (telemetry == NULL)
        return;

    fprintf(fp, "now_us,notification,bytes_in_transit,delivered,rtt_sample_us,smoothed_rtt_us,bytes_acked,bytes_lost,"
                "delivered_since_sent,lost_since_sent,inflight_prior,is_app_limited,cwnd,pacing_rate,send_quantum,state,ack_phase,"
                "filled_pipe,in_recovery,pto_recovery,packet_conservation,idle_restart,bw\n");

    size_t retained = quicly_cc_bbr_telemetry_num_retained(telemetry),
           oldest = telemetry->num_recorded < telemetry->capacity ? 0 : telemetry->pos;

    for (size_t i = 0; i < retained; ++i) {
        const quicly_cc_bbr_sample_t *s = telemetry->samples + (oldest + i) % telemetry->capacity;
        fprintf(fp,
                "%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s,%u,%u,%u,%u,%u,%u,%" PRIu64 "\n",
                s->now_us, quicly_cc_bbr_notification_name(s->notification), s->bytes_in_transit, s->delivered, s->rtt_sample_us,
                s->smoothed_rtt_us, s->bytes_acked, s->bytes_lost, s->delivered_since_sent, s->lost_since_sent, s->inflight_prior,
                (unsigned)s->is_app_limited, s->cwnd, s->pacing_rate, s->send_quantum,
                quicly_cc_bbr_state_name(QUICLY_CC_BBR_OBSERVED_STATE(s->cc_state)),
                QUICLY_CC_BBR_OBSERVED_ACK_PHASE(s->cc_state), QUICLY_CC_BBR_OBSERVED_FILLED_PIPE(s->cc_state),
                QUICLY_CC_BBR_OBSERVED_IN_RECOVERY(s->cc_state), QUICLY_CC_BBR_OBSERVED_PTO_RECOVERY(s->cc_state),
                QUICLY_CC_BBR_OBSERVED_PACKET_CONSERVATION(s->cc_state), QUICLY_CC_BBR_OBSERVED_IDLE_RESTART(s->cc_state), s->bw);
    }
}

void quicly_cc_bbr_telemetry_destroy(quicly_cc_bbr_telemetry_t *telemetry)
{
    if (telemetry == NULL)
        return;

    if (telemetry->num_recorded != 0) {
        FILE *fp;
        if ((fp = fopen(telemetry->path, "w")) != NULL) {
            quicly_cc_bbr_telemetry_dump(telemetry, fp);
            fclose(fp);
        }
    }

    free(telemetry->path);
    free(telemetry);
}

void quicly_cc_bbr_counters_init(quicly_cc_t *cc)
{
    memset(&cc->state.bbr, 0, sizeof(cc->state.bbr));

    /* the generic counters BBR reuses, initialized as the other controllers do */
    cc->cwnd_initial = cc->cwnd_maximum = cc->cwnd;
    cc->cwnd_minimum = UINT32_MAX;
    cc->cwnd_exiting_slow_start = 0;
    cc->exit_slow_start_at = INT64_MAX;
    cc->num_loss_episodes = 0;
    cc->num_loss_episodes_undone = 0;
    cc->num_loss_episodes_undone_in_startup = 0;
    cc->num_ecn_loss_episodes = 0;
}

void quicly_cc_bbr_counters_update(quicly_cc_t *cc, quicly_cc_bbr_notification_t notification, uint64_t cc_state, uint64_t bw,
                                   int is_app_limited, int64_t now)
{
    uint64_t prev = cc->state.bbr.prev_cc_state;
    /* on the first notification there is nothing to diff against, so every set bit counts as an edge */
    int is_first = !cc->state.bbr.prev_cc_state_valid;

    if (notification < QUICLY_CC_BBR_NUM_NOTIFICATIONS)
        ++cc->state.bbr.num_notify[notification];

    /* state machine edges */
    unsigned state = QUICLY_CC_BBR_OBSERVED_STATE(cc_state);
    if (state < QUICLY_CC_BBR_NUM_STATES && (is_first || state != QUICLY_CC_BBR_OBSERVED_STATE(prev)))
        ++cc->state.bbr.num_state_entries[state];
    if (!is_first && QUICLY_CC_BBR_OBSERVED_ACK_PHASE(cc_state) != QUICLY_CC_BBR_OBSERVED_ACK_PHASE(prev))
        ++cc->state.bbr.num_ack_phase_changes;
    if (QUICLY_CC_BBR_OBSERVED_PTO_RECOVERY(cc_state) && (is_first || !QUICLY_CC_BBR_OBSERVED_PTO_RECOVERY(prev)))
        ++cc->state.bbr.num_pto_recovery;

    /* generic counters BBR reuses: filling the pipe is BBR's exit from slow start, entering recovery is a loss episode */
    if (QUICLY_CC_BBR_OBSERVED_FILLED_PIPE(cc_state) && cc->cwnd_exiting_slow_start == 0) {
        cc->cwnd_exiting_slow_start = cc->cwnd;
        cc->exit_slow_start_at = now;
    }
    if (QUICLY_CC_BBR_OBSERVED_IN_RECOVERY(cc_state) && (is_first || !QUICLY_CC_BBR_OBSERVED_IN_RECOVERY(prev)))
        ++cc->num_loss_episodes;

    switch (notification) {
    case QUICLY_CC_BBR_NOTIFY_ACK:
        ++cc->state.bbr.num_acks;
        if (is_app_limited)
            ++cc->state.bbr.num_acks_app_limited;
        break;
    case QUICLY_CC_BBR_NOTIFY_RESTART_FROM_IDLE:
        ++cc->state.bbr.num_idle_restarts;
        break;
    case QUICLY_CC_BBR_NOTIFY_SPURIOUS_REPEAT:
        /* BBR undoes its response to the loss, so the episode is retracted the way cubic retracts one. Unlike cubic we do not also
         * roll back `cwnd_exiting_slow_start`: leaving startup is BBR's own `filled_pipe` decision, not a consequence of the loss.
         */
        if (cc->num_loss_episodes != 0)
            --cc->num_loss_episodes;
        ++cc->num_loss_episodes_undone;
        if (!QUICLY_CC_BBR_OBSERVED_FILLED_PIPE(cc_state))
            ++cc->num_loss_episodes_undone_in_startup;
        break;
    default:
        break;
    }

    if (bw != 0) {
        cc->state.bbr.bw_latest = bw;
        if (cc->state.bbr.bw_maximum < bw)
            cc->state.bbr.bw_maximum = bw;
    }

    /* derived from cc->cwnd, so these stay pinned if the adapter forgets to copy `path->cwin` back */
    if (cc->cwnd_maximum < cc->cwnd)
        cc->cwnd_maximum = cc->cwnd;
    if (cc->cwnd_minimum > cc->cwnd)
        cc->cwnd_minimum = cc->cwnd;

    cc->state.bbr.prev_cc_state = cc_state;
    cc->state.bbr.prev_cc_state_valid = 1;
}

void quicly_cc_bbr_counters_update_pacing(quicly_cc_t *cc, uint64_t pacing_rate, uint64_t send_quantum)
{
    cc->state.bbr.pacing_rate_latest = pacing_rate;
    if (cc->state.bbr.pacing_rate_maximum < pacing_rate)
        cc->state.bbr.pacing_rate_maximum = pacing_rate;
    cc->state.bbr.send_quantum = send_quantum;
}
