/*
 * The BBR adapter: quicly_cc_type_t on top of picoquic's bbr.c, vendored as lib/picoquic-bbr.c.
 * Each notification syncs quicly's state into picoquic's path and per-ack structs, calls BBR,
 * and copies path.cwin back into cc->cwnd. Only this file and the vendored one see picoquic types.
 */

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "quicly/picoquic-bbr.h"
#include "quicly.h"
#include "quicly/cc.h"
#include "quicly/cc-bbr.h"

/* BBR lives in the vendored picoquic-bbr.c; the adapter reaches it through its vtable. */
extern picoquic_congestion_algorithm_t picoquic_bbr_algorithm_struct;

/**
 * BBR's primary state. Numeric values MUST mirror picoquic's `picoquic_bbr_alg_state_t`, which
 * `picoquic_bbr_observe()` returns raw. That enum is private to `lib/picoquic-bbr.c`, so nothing
 * can check the two against each other; recheck them by hand whenever that file is re-synced.
 */
typedef enum en_quicly_cc_bbr_state_t {
    QUICLY_CC_BBR_STARTUP = 0,
    QUICLY_CC_BBR_DRAIN,
    QUICLY_CC_BBR_PROBE_BW_DOWN,
    QUICLY_CC_BBR_PROBE_BW_CRUISE,
    QUICLY_CC_BBR_PROBE_BW_REFILL,
    QUICLY_CC_BBR_PROBE_BW_UP,
    QUICLY_CC_BBR_PROBE_RTT,
    QUICLY_CC_BBR_STARTUP_LONG_RTT,
    QUICLY_CC_BBR_STARTUP_RESUME,
    QUICLY_CC_BBR_NUM_STATES
} quicly_cc_bbr_state_t;

/**
 * Notifications the adapter forwards to BBR
 */
typedef enum en_quicly_cc_bbr_notification_t {
    QUICLY_CC_BBR_NOTIFY_ACK = 0,
    QUICLY_CC_BBR_NOTIFY_REPEAT,
    QUICLY_CC_BBR_NOTIFY_RESTART_FROM_IDLE,
    QUICLY_CC_BBR_NUM_NOTIFICATIONS
} quicly_cc_bbr_notification_t;

/**
 * The rest of the word `picoquic_bbr_observe()` packs its state into. Layout fixed by picoquic.
 */
#define QUICLY_CC_BBR_OBSERVED_ACK_PHASE(w) ((unsigned)(((w) >> 8) & 0xff))
#define QUICLY_CC_BBR_OBSERVED_FILLED_PIPE(w) ((unsigned)(((w) >> 16) & 1))
#define QUICLY_CC_BBR_OBSERVED_IN_RECOVERY(w) ((unsigned)(((w) >> 17) & 1))
#define QUICLY_CC_BBR_OBSERVED_PACKET_CONSERVATION(w) ((unsigned)(((w) >> 19) & 1))
#define QUICLY_CC_BBR_OBSERVED_IDLE_RESTART(w) ((unsigned)(((w) >> 20) & 1))

/**
 * One bbr log row: inputs synced in, outputs read back, and BBR's own view. One per notification.
 */
typedef struct st_quicly_cc_bbr_sample_t {
    /**
     * time of the notification, microseconds
     */
    uint64_t now_us;
    /**
     * which notification this row is for; a `quicly_cc_bbr_notification_t`
     */
    uint8_t notification;
    /**
     * value of `is_app_limited` fed to BBR for this ACK
     */
    uint8_t is_app_limited;
    /**
     * inputs synced into `picoquic_path_t` / `picoquic_per_ack_state_t`
     */
    uint64_t bytes_in_transit;
    uint64_t delivered;
    uint64_t rtt_sample_us;
    uint64_t smoothed_rtt_us;
    uint64_t bytes_acked;
    uint64_t bytes_lost;
    uint64_t delivered_since_sent;
    uint64_t lost_since_sent;
    uint64_t inflight_prior;
    /**
     * outputs read back out after the call
     */
    uint64_t cwnd;
    uint64_t pacing_rate;
    uint64_t send_quantum;
    /**
     * BBR's own view: the packed word from `picoquic_bbr_observe()` and its bandwidth estimate
     */
    uint64_t cc_state;
    uint64_t bw;
    /**
     * the delivery rate measured for this ack, before BBR's filtering; 0 when not measured
     */
    uint64_t bw_estimate;
} quicly_cc_bbr_sample_t;

static const char *quicly_cc_bbr_notification_name(unsigned notification);

/**
 * Converts BBR's pacing rate (bytes/sec) to quicly's bytes/ms. A positive rate never becomes 0,
 * which would mean "no rate"; an implausible one saturates.
 */
static uint32_t quicly_cc_bbr_calc_pacer_rate(double bytes_per_sec)
{
    if (!(bytes_per_sec > 0))
        return 0;

    double per_msec = bytes_per_sec / 1000;
    if (per_msec >= UINT32_MAX)
        return UINT32_MAX;
    return per_msec < 1 ? 1 : (uint32_t)per_msec;
}

/* ---- time seam: quicly time is ms; BBR wants microseconds ------------------------------------ */
static uint64_t bbr_now_us(double now_ms)
{
    /* scale before truncating, otherwise the sub-millisecond part is lost here */
    return now_ms > 0 ? (uint64_t)(now_ms * 1000) : 0;
}
static uint64_t rtt_ms_to_us(float rtt_ms)
{
    /* rtt fields are float ms on this quicly; * 1000 keeps sub-ms precision (no int truncation) */
    return rtt_ms > 0 ? (uint64_t)(rtt_ms * 1000.0f) : 0;
}

/* ---- cb_args: callback params the cc_common helpers pull, refreshed per notify -------------- */
typedef struct st_quicly_cc_bbr_cb_args_t {
    uint64_t next_pn;          /* `next_pn` param; feeds picoquic_cc_get_sequence_number() */
    uint64_t largest_acked_pn; /* feeds picoquic_cc_get_ack_number() */
} quicly_cc_bbr_cb_args_t;

/*
 * Per-connection state, hung off cc->state.bbr.path. `path` must be first: conn_of() recovers
 * the wrapper from the picoquic_path_t pointer bbr.c passes back. bbr.c reads cnx and quic at
 * init, so they are real, zeroed objects.
 */
typedef struct st_quicly_cc_bbr_conn_t {
    picoquic_path_t path;
    picoquic_cnx_t cnx;
    picoquic_quic_t quic;
    quicly_cc_bbr_cb_args_t cb_args;
    quicly_cc_t *cc;
    uint64_t send_quantum;         /* the latest from picoquic_update_pacing_rate */
    quicly_cc_bbr_sample_t sample; /* the latest notification, for the bbr log point */
    uint8_t has_new_sample;
    /* losses since the last ack notification. picoquic passes them on the ack whose processing
     * found them; quicly finds them after cc_on_acked, so BBR gets them on the next ack. */
    uint64_t newly_lost;
    uint64_t newly_lost_ranges;
    uint64_t latest_lost_pn; /* UINT64_MAX before the first loss */
} quicly_cc_bbr_conn_t;

static quicly_cc_bbr_conn_t *conn_of(picoquic_path_t *path_x)
{
    /* path is the first member, so the path pointer is the wrapper pointer */
    return (quicly_cc_bbr_conn_t *)path_x;
}

/* ---- picoquic cc_common reimplementation (bbr.c links against these; external linkage) ------- */

uint64_t picoquic_cc_get_sequence_number(picoquic_cnx_t *cnx, picoquic_path_t *path_x)
{
    (void)cnx;
    return conn_of(path_x)->cb_args.next_pn;
}

uint64_t picoquic_cc_get_ack_number(picoquic_cnx_t *cnx, picoquic_path_t *path_x)
{
    (void)cnx;
    return conn_of(path_x)->cb_args.largest_acked_pn;
}

/* The pacing rate leaves BBR through this callback, not a field. Stored in cc->pacing_rate, which
 * the quicly pacer consumes in place of its cwnd/SRTT estimate. */
void picoquic_update_pacing_rate(picoquic_path_t *path_x, double pacing_rate, uint64_t quantum)
{
    quicly_cc_t *cc = conn_of(path_x)->cc;
    cc->pacing_rate = quicly_cc_bbr_calc_pacer_rate(pacing_rate);
    conn_of(path_x)->send_quantum = quantum;
}

/* splitmix64, copied verbatim from picoquic's util.c. BBR draws its ProbeBW wait jitter from
 * this; a constant return makes competing flows probe in lockstep. */
static uint64_t picoquic_test_random(uint64_t *random_context)
{
    uint64_t z;
    *random_context += 0x9e3779b97f4a7c15;
    z = *random_context;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

uint64_t picoquic_test_uniform_random(uint64_t *random_context, uint64_t rnd_max)
{
    uint64_t rnd = 0;

    if (rnd_max > 0) {
        uint64_t rnd_min = UINT64_MAX % rnd_max;

        do {
            rnd = picoquic_test_random(random_context);
        } while (rnd < rnd_min);
        rnd %= rnd_max;
    }

    return rnd;
}

/* The rest of what bbr.c links against serves only startup_long_rtt, which the adapter keeps
 * unreachable (notify holds path.rtt_min at 0). */

int picoquic_cc_hystart_test(picoquic_min_max_rtt_t *rtt_track, uint64_t rtt_measurement, uint64_t packet_time,
                             uint64_t current_time, int is_one_way_delay_enabled)
{
    assert(!"startup_long_rtt is unreachable");
    return 0;
}

int picoquic_cc_hystart_loss_volume_test(picoquic_min_max_rtt_t *rtt_track, picoquic_congestion_notification_t event,
                                         uint64_t nb_bytes_newly_acked, uint64_t nb_bytes_newly_lost)
{
    assert(!"startup_long_rtt is unreachable");
    return 0;
}

uint64_t picoquic_cc_slow_start_increase(picoquic_path_t *path_x, uint64_t nb_delivered)
{
    assert(!"startup_long_rtt is unreachable");
    return 0;
}

void picoquic_update_pacing_data(picoquic_path_t *path_x, int slow_start)
{
    assert(!"startup_long_rtt is unreachable");
}

/* ---- adapter core ---------------------------------------------------------------------------- */

/* Decode BBR's current state word once, for both the log sample and the counters. */
static uint64_t bbr_observe(quicly_cc_bbr_conn_t *bc, uint64_t *bw)
{
    uint64_t cc_state = 0;
    uint64_t cc_param = 0;
    picoquic_bbr_algorithm_struct.alg_observe(&bc->path, &cc_state, &cc_param);
    *bw = cc_param;
    return cc_state;
}

/*
 * Populate the path inputs common to all notifications. Fields BBR owns (`cwin`,
 * `is_cca_probing_up`, `is_cc_data_updated`, `is_ssthresh_initialized`, `bandwidth_estimate_max`,
 * `congestion_alg_state`) are left untouched after init - re-syncing them wipes the state machine.
 */
static void sync_inputs(quicly_cc_bbr_conn_t *bc, const quicly_loss_t *loss, uint32_t max_udp_payload_size)
{
    bc->path.bytes_in_transit = loss->sentmap.bytes_in_flight;
    bc->path.rtt_sample = rtt_ms_to_us(loss->rtt.latest);
    bc->path.smoothed_rtt = (uint64_t)(loss->rtt.smoothed * 1000);
    bc->path.rtt_variant = (uint64_t)(loss->rtt.variance * 1000);
    bc->path.send_mtu = max_udp_payload_size;
    /* set on the ack path when a sample is measured; an ack without one leaves the previous
     * value, which BBR reuses (picoquic's interface has no "no sample") */
}

/* Refresh cb_args from the callback parameters so the cc_common helpers see this call's values. */
static void sync_cb_args(quicly_cc_bbr_conn_t *bc, uint64_t next_pn, uint64_t largest_acked_pn)
{
    bc->cb_args.next_pn = next_pn;
    bc->cb_args.largest_acked_pn = largest_acked_pn;
}

/* Initializes the BBR state block and the generic counters BBR reuses. Call after `cwnd` is set. */
static void counters_init(quicly_cc_t *cc)
{
    memset(&cc->state.bbr, 0, sizeof(cc->state.bbr));
    cc->cwnd_initial = cc->cwnd_maximum = cc->cwnd;
    cc->cwnd_minimum = UINT32_MAX;
    cc->cwnd_exiting_slow_start = 0;
    cc->exit_slow_start_at = INT64_MAX;
    cc->num_loss_episodes = 0;
    cc->num_loss_episodes_undone = 0;
    cc->num_loss_episodes_undone_in_startup = 0;
    cc->num_ecn_loss_episodes = 0;
}

/* Updates the counters from one notification, for stats and for quicly's ACK-frequency logic
 * (num_loss_episodes). Call after `cc->cwnd` has been read back. */
static void counters_update(quicly_cc_t *cc, uint64_t cc_state, uint64_t bw, double now)
{
    uint64_t prev = cc->state.bbr.cc_state;
    unsigned state = QUICLY_CC_BBR_OBSERVED_STATE(cc_state);

    if (state != QUICLY_CC_BBR_OBSERVED_STATE(prev)) {
        if (state == QUICLY_CC_BBR_STARTUP)
            ++cc->state.bbr.startups;
        else if (state == QUICLY_CC_BBR_PROBE_RTT)
            ++cc->state.bbr.probe_rtts;
    }
    if (QUICLY_CC_BBR_OBSERVED_FILLED_PIPE(cc_state) && cc->cwnd_exiting_slow_start == 0) {
        cc->cwnd_exiting_slow_start = cc->cwnd;
        cc->exit_slow_start_at = (int64_t)now;
    }
    if (QUICLY_CC_BBR_OBSERVED_IN_RECOVERY(cc_state) && !QUICLY_CC_BBR_OBSERVED_IN_RECOVERY(prev))
        ++cc->num_loss_episodes;
    if (bw != 0)
        cc->state.bbr.bw = bw;
    if (cc->cwnd_maximum < cc->cwnd)
        cc->cwnd_maximum = cc->cwnd;
    if (cc->cwnd_minimum > cc->cwnd)
        cc->cwnd_minimum = cc->cwnd;

    cc->state.bbr.cc_state = cc_state;
}

/* named `logpoint` because PTLS_LOG__DO_LOG refers to it by that name */
PTLS_LOG_DEFINE_POINT(quicly, bbr, logpoint);

/* The log sample for this notification: bc->sample, cleared, when the bbr log point is on; NULL
 * otherwise, so nothing is filled or copied while logging is off. */
static quicly_cc_bbr_sample_t *start_sample(quicly_cc_bbr_conn_t *bc)
{
    if (PTLS_LIKELY(ptls_log_point_maybe_active(&logpoint) == 0))
        return NULL;
    memset(&bc->sample, 0, sizeof(bc->sample));
    return &bc->sample;
}

/*
 * Shared tail of every notify path: run BBR, copy its window out, fold the observation into
 * the counters and, if logging, the sample. `sample` carries the caller-filled inputs; this fills
 * the outputs.
 */
static void notify(quicly_cc_bbr_conn_t *bc, quicly_cc_t *cc, picoquic_congestion_notification_t pq_notification,
                   quicly_cc_bbr_notification_t q_notification, picoquic_per_ack_state_t *as, int is_app_limited, uint64_t now_us,
                   double now, quicly_cc_bbr_sample_t *sample)
{
    picoquic_bbr_algorithm_struct.alg_notify(&bc->cnx, &bc->path, pq_notification, as, now_us);

    /* OUTPUT read-back: BBR's window is in path.cwin. Without this line BBR is inert. */
    cc->cwnd = bc->path.cwin > UINT32_MAX ? UINT32_MAX : (uint32_t)bc->path.cwin;

    uint64_t bw = 0;
    uint64_t cc_state = bbr_observe(bc, &bw);
    counters_update(cc, cc_state, bw, now);

    if (sample != NULL) {
        sample->now_us = now_us;
        sample->notification = (uint8_t)q_notification;
        sample->is_app_limited = (uint8_t)(is_app_limited != 0);
        sample->bytes_in_transit = bc->path.bytes_in_transit;
        sample->delivered = bc->path.delivered;
        sample->cwnd = cc->cwnd;
        sample->pacing_rate = cc->pacing_rate;
        sample->send_quantum = bc->send_quantum;
        sample->cc_state = cc_state;
        sample->bw = bw;
        bc->has_new_sample = 1;
    }

    /* BBR keeps its min RTT in bbr_state->min_rtt. path.rtt_min only selects picoquic's long-RTT
     * startup, an alternate mode for long-latency paths (like satellite) that we don't support.
     * Only BBRExitProbeRTT writes it, and the check that reads it runs before that in the same
     * notification, so clearing it here keeps that startup, and the stubs it calls, unreachable. */
    bc->path.rtt_min = 0;
}

void quicly_cc_bbr_log(quicly_conn_t *conn, quicly_cc_t *cc)
{
    if (cc->type != &quicly_cc_type_bbr)
        return;
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    if (!bc->has_new_sample)
        return;
    bc->has_new_sample = 0;

    /* QUICLY_LOG_CONN needs quicly.c's private state; this mirrors picotls's PTLS_LOG_CONN */
    uint32_t active = ptls_log_point_maybe_active(&logpoint);
    if (PTLS_LIKELY(active == 0))
        return;
    ptls_t *tls = quicly_get_tls(conn);
    ptls_log_conn_state_t *conn_state = ptls_get_log_state(tls);
    active &= ptls_log_conn_maybe_active(conn_state, ptls_log_getsni_ptls(tls));
    if (PTLS_LIKELY(active == 0))
        return;
    const quicly_cc_bbr_sample_t *s = &bc->sample;
    PTLS_LOG__DO_LOG(quicly, bbr, conn_state, ptls_log_getsni_ptls(tls), 1, {
        PTLS_LOG_ELEMENT_PTR(conn, conn);
        if (conn_state->conn_id != 0)
            PTLS_LOG_ELEMENT_NUMBER(conn_id, conn_state->conn_id);
        PTLS_LOG_ELEMENT_NUMBER(now_us, s->now_us);
        PTLS_LOG_ELEMENT_SAFESTR(notification, quicly_cc_bbr_notification_name(s->notification));
        PTLS_LOG_ELEMENT_NUMBER(bytes_in_transit, s->bytes_in_transit);
        PTLS_LOG_ELEMENT_NUMBER(delivered, s->delivered);
        PTLS_LOG_ELEMENT_NUMBER(rtt_sample_us, s->rtt_sample_us);
        PTLS_LOG_ELEMENT_NUMBER(smoothed_rtt_us, s->smoothed_rtt_us);
        PTLS_LOG_ELEMENT_NUMBER(bytes_acked, s->bytes_acked);
        PTLS_LOG_ELEMENT_NUMBER(bytes_lost, s->bytes_lost);
        PTLS_LOG_ELEMENT_NUMBER(delivered_since_sent, s->delivered_since_sent);
        PTLS_LOG_ELEMENT_NUMBER(lost_since_sent, s->lost_since_sent);
        PTLS_LOG_ELEMENT_NUMBER(inflight_prior, s->inflight_prior);
        PTLS_LOG_ELEMENT_BOOL(is_app_limited, s->is_app_limited);
        PTLS_LOG_ELEMENT_NUMBER(cwnd, s->cwnd);
        PTLS_LOG_ELEMENT_NUMBER(pacing_rate, s->pacing_rate);
        PTLS_LOG_ELEMENT_NUMBER(send_quantum, s->send_quantum);
        PTLS_LOG_ELEMENT_SAFESTR(state, quicly_cc_bbr_state_name(QUICLY_CC_BBR_OBSERVED_STATE(s->cc_state)));
        PTLS_LOG_ELEMENT_NUMBER(ack_phase, QUICLY_CC_BBR_OBSERVED_ACK_PHASE(s->cc_state));
        PTLS_LOG_ELEMENT_BOOL(filled_pipe, QUICLY_CC_BBR_OBSERVED_FILLED_PIPE(s->cc_state));
        PTLS_LOG_ELEMENT_BOOL(in_recovery, QUICLY_CC_BBR_OBSERVED_IN_RECOVERY(s->cc_state));
        PTLS_LOG_ELEMENT_BOOL(packet_conservation, QUICLY_CC_BBR_OBSERVED_PACKET_CONSERVATION(s->cc_state));
        PTLS_LOG_ELEMENT_BOOL(idle_restart, QUICLY_CC_BBR_OBSERVED_IDLE_RESTART(s->cc_state));
        PTLS_LOG_ELEMENT_NUMBER(bw, s->bw);
        PTLS_LOG_ELEMENT_NUMBER(bw_estimate, s->bw_estimate);
    });
}

static void bbr_on_acked(quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t bytes, uint64_t largest_acked, uint32_t inflight,
                         int cc_limited, uint64_t next_pn, double now, uint32_t max_udp_payload_size,
                         const quicly_cc_bbr_ack_params_t *bbr_params)
{
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    uint64_t now_us = bbr_now_us(now);

    /* BBR's inputs from quicly's send-time snapshots */
    assert(bbr_params != NULL);
    uint64_t delivered_since_sent = bbr_params->delivered_since_sent;
    uint64_t inflight_prior = bbr_params->inflight_prior;
    uint64_t lost_since_sent = bbr_params->lost_since_sent;
    (void)inflight;

    /* delivered running total: BBR reads path.delivered and measures its own deltas from it */
    bc->path.delivered += bytes;

    /* From quicly's rate meter, which counts pacing and a full send buffer as limits of our own
     * rather than the application's; the cwnd/2 heuristic behind `cc_limited` does not. */
    int is_app_limited = bbr_params->app_limited;

    /* a measured rate replaces BBR's delivered/rtt fallback; 0 means no sample, keep the last */
    if (bbr_params->bandwidth_estimate != 0)
        bc->path.bandwidth_estimate = bbr_params->bandwidth_estimate;

    bc->path.ecn_ect0_total_remote = bbr_params->ecn_counts[0];
    bc->path.ecn_ect1_total_remote = bbr_params->ecn_counts[1];
    bc->path.ecn_ce_total_remote = bbr_params->ecn_counts[2];

    sync_inputs(bc, loss, max_udp_payload_size);
    sync_cb_args(bc, next_pn, largest_acked);

    picoquic_per_ack_state_t as;
    memset(&as, 0, sizeof(as));
    as.rtt_measurement = rtt_ms_to_us(loss->rtt.latest);
    as.nb_bytes_acknowledged = bytes;
    as.nb_bytes_delivered_since_packet_sent = delivered_since_sent;
    as.nb_bytes_lost_since_packet_sent = lost_since_sent;
    as.nb_bytes_newly_lost = bc->newly_lost;
    as.nb_loss_ranges_newly_lost = bc->newly_lost_ranges;
    bc->newly_lost = 0;
    bc->newly_lost_ranges = 0;
    as.inflight_prior = inflight_prior;
    as.is_app_limited = is_app_limited;
    as.is_cwnd_limited = cc_limited;

    quicly_cc_bbr_sample_t *sample = start_sample(bc);
    if (sample != NULL) {
        sample->rtt_sample_us = rtt_ms_to_us(loss->rtt.latest);
        sample->smoothed_rtt_us = (uint64_t)(loss->rtt.smoothed * 1000);
        sample->bytes_acked = bytes;
        sample->delivered_since_sent = delivered_since_sent;
        sample->lost_since_sent = lost_since_sent;
        sample->bw_estimate = bbr_params->bandwidth_estimate;
        sample->inflight_prior = inflight_prior;
    }
    notify(bc, cc, picoquic_congestion_notification_acknowledgement, QUICLY_CC_BBR_NOTIFY_ACK, &as, is_app_limited, now_us, now,
           sample);
}

static void bbr_on_lost(quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t bytes, uint64_t lost_pn, uint64_t next_pn, double now,
                        uint32_t max_udp_payload_size, const quicly_cc_bbr_loss_params_t *bbr_params)
{
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    uint64_t now_us = bbr_now_us(now);

    /* zero bytes means CE was reported; BBR takes that from the ECN counts on the ack instead */
    if (bytes == 0)
        return;

    bc->newly_lost += bytes;
    if (bc->latest_lost_pn == UINT64_MAX || lost_pn != bc->latest_lost_pn + 1)
        ++bc->newly_lost_ranges;
    bc->latest_lost_pn = lost_pn;

    sync_inputs(bc, loss, max_udp_payload_size);
    /* a loss does not change the largest acknowledged packet number, so keep the last one */
    sync_cb_args(bc, next_pn, bc->cb_args.largest_acked_pn);

    picoquic_per_ack_state_t as;
    memset(&as, 0, sizeof(as));
    as.lost_packet_number = lost_pn;
    as.nb_bytes_newly_lost = bytes;
    as.nb_loss_ranges_newly_lost = 1;
    /* BBR checks whether losses during a bandwidth probe crossed its threshold, and where */
    assert(bbr_params != NULL);
    as.nb_bytes_delivered_since_packet_sent = bbr_params->delivered_since_sent;
    as.nb_bytes_lost_since_packet_sent = bbr_params->lost_since_sent;
    as.inflight_prior = bbr_params->inflight_prior;
    as.is_app_limited = bbr_params->app_limited;

    quicly_cc_bbr_sample_t *sample = start_sample(bc);
    if (sample != NULL) {
        sample->bytes_lost = bytes;
        sample->delivered_since_sent = bbr_params->delivered_since_sent;
        sample->lost_since_sent = bbr_params->lost_since_sent;
        sample->inflight_prior = bbr_params->inflight_prior;
    }
    notify(bc, cc, picoquic_congestion_notification_repeat, QUICLY_CC_BBR_NOTIFY_REPEAT, &as, 0, now_us, now, sample);
}

static void bbr_on_sent(quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t bytes, uint64_t next_pn, double now, int app_limited)
{
    /* restart_from_idle, as the BBR draft defines it: nothing was in flight before this packet (quicly
     * has already counted it) and the sender was app-limited, not drained by the pacer or cwnd.
     * picoquic also waits about a second of idle; here the effect clears on the next ack anyway. */
    if (loss->sentmap.bytes_in_flight > bytes || !app_limited)
        return;
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    uint64_t now_us = bbr_now_us(now);
    uint64_t inflight_prior = loss->sentmap.bytes_in_flight - bytes;

    sync_inputs(bc, loss, (uint32_t)bc->path.send_mtu);
    sync_cb_args(bc, next_pn, bc->cb_args.largest_acked_pn);

    picoquic_per_ack_state_t as;
    memset(&as, 0, sizeof(as));
    as.inflight_prior = inflight_prior;
    as.is_app_limited = 1;

    quicly_cc_bbr_sample_t *sample = start_sample(bc);
    if (sample != NULL)
        sample->inflight_prior = inflight_prior;
    notify(bc, cc, picoquic_congestion_notification_restart_from_idle, QUICLY_CC_BBR_NOTIFY_RESTART_FROM_IDLE, &as, 1, now_us, now,
           sample);
}

static void bbr_reset(quicly_cc_t *cc, uint32_t initcwnd, const quicly_cc_conf_t *conf, double now)
{
    /* a controller that is already BBR must have been disposed first, or its state would leak */
    assert(cc->type != &quicly_cc_type_bbr || cc->state.bbr.path == NULL);

    quicly_cc_bbr_conn_t *bc;
    if ((bc = malloc(sizeof(*bc))) == NULL) {
        assert(!"quicly_cc_type_bbr: out of memory allocating BBR connection state");
        abort();
    }

    memset(bc, 0, sizeof(*bc));
    bc->path.cnx = &bc->cnx;
    bc->cnx.quic = &bc->quic;

    memset(cc, 0, sizeof(*cc));
    cc->type = &quicly_cc_type_bbr;
    cc->conf = conf;
    cc->cwnd = initcwnd;

    /* counters_init memsets the whole cc->state.bbr block, so it must run before we store
     * path there. */
    counters_init(cc);
    cc->ssthresh = UINT32_MAX;
    cc->state.bbr.path = bc;
    bc->cc = cc;

    bc->path.smoothed_rtt = PICOQUIC_INITIAL_RTT;
    bc->path.cwin = initcwnd;
    bc->path.send_mtu = 1280; /* conservative initial MTU; every real notify overwrites it */
    bc->latest_lost_pn = UINT64_MAX;

    /* "R" turns off picoquic's rapid-start probing, which latches on and then keeps BBR out of
     * cruise. The time seeds BBR's PRNG and starts its min_rtt / probe_rtt / extra_acked stamps,
     * which read as long expired if they begin at zero. */
    picoquic_bbr_algorithm_struct.alg_init(&bc->path, "R", bbr_now_us(now));

    /* BBR starts in Startup; record it so state changes count from here */
    cc->state.bbr.cc_state = bbr_observe(bc, &cc->state.bbr.bw);
    cc->state.bbr.startups = 1;
}

static void bbr_init(quicly_init_cc_t *self, quicly_cc_t *cc, const quicly_cc_conf_t *conf, uint16_t max_udp_payload_size,
                     double now)
{
    (void)self;
    bbr_reset(cc, quicly_cc_calc_initial_cwnd(conf->initcwnd_packets, max_udp_payload_size), conf, now);
}

static int bbr_switch(quicly_cc_t *cc)
{
    /* starting BBR mid-connection would need a clock and a pacer that cc_switch cannot provide */
    return cc->type == &quicly_cc_type_bbr;
}

static void bbr_dispose(quicly_cc_t *cc)
{
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    picoquic_bbr_algorithm_struct.alg_delete(&bc->path);
    free(bc);
    cc->state.bbr.path = NULL;
}

/* ---- names ----------------------------------------------------------------------------------- */

const char *quicly_cc_bbr_state_name(unsigned state)
{
    static const char *names[] = {"startup",     "drain",     "probe_bw_down",    "probe_bw_cruise", "probe_bw_refill",
                                  "probe_bw_up", "probe_rtt", "startup_long_rtt", "startup_resume"};

    return state < QUICLY_CC_BBR_NUM_STATES ? names[state] : "invalid";
}

static const char *quicly_cc_bbr_notification_name(unsigned notification)
{
    static const char *names[] = {"ack", "repeat", "restart_from_idle"};

    return notification < QUICLY_CC_BBR_NUM_NOTIFICATIONS ? names[notification] : "invalid";
}

/* ---- vtable ---------------------------------------------------------------------------------- */

quicly_init_cc_t quicly_cc_bbr_init = {bbr_init};

/* Field order must match struct st_quicly_cc_type_t. No cc_on_late_ack: BBR undoes a loss only
 * in PTO recovery, which the adapter never enters. */
quicly_cc_type_t quicly_cc_type_bbr = {"bbr",
                                       &quicly_cc_bbr_init,
                                       bbr_on_acked,
                                       bbr_on_lost,
                                       bbr_on_sent,
                                       bbr_switch,
                                       NULL, /* cc_on_late_ack */
                                       NULL, /* cc_jumpstart */
                                       NULL, /* cc_update_cc_limited */
                                       bbr_dispose,
                                       1, /* requires_pacing: BBR's output is a rate */
                                       1 /* wants_zero_byte_acks: BBR counts rounds by packet number */};
