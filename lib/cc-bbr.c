/*
 * The BBR adapter (BBR_POC_PLAN.md §9). Implements quicly's `quicly_cc_type_t` on top of picoquic's `bbr.c`, which runs
 * unmodified. This file and `cc-bbr-common.c` are the only quicly-side files that see picoquic types; the rest of quicly reaches
 * BBR through `include/quicly/cc-bbr.h`, which pulls in no picoquic header (§2.8).
 *
 * Shape of every notification (§9): sync quicly state into `path_x` + a `picoquic_per_ack_state_t`, refresh the shim so the
 * cc_common helpers can see the callback's parameters, call `picoquic_bbr_notify()`, then copy `path->cwin` back into
 * `cc->cwnd`. The read-back is the single most important line here (§11 trap 1): omit it and BBR runs correctly but its output
 * never reaches quicly, which looks like "BBR does nothing" rather than a wiring bug.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "picoquic_internal.h"
#include "quicly.h"
#include "quicly/cc.h"
#include "quicly/cc-bbr.h"
#include "cc-bbr-shim.h"

/* picoquic's four BBR entry points live in the vendored bbr.c; only the vtable and observe are non-static (§2.1). */
extern picoquic_congestion_algorithm_t picoquic_bbr_algorithm_struct;

/*
 * quicly_cc_bbr_state_t (in cc-bbr.h, kept free of picoquic headers) must match the numeric values of picoquic's
 * picoquic_bbr_alg_state_t, since `picoquic_bbr_observe()` returns that raw value in the low byte of cc_state and the telemetry
 * decodes it through quicly_cc_bbr_state_t. picoquic's enum is *not* referenceable here - it is defined inside bbr.c, not a
 * header - so the check is against the literal values that bbr.c's enum assigns (picoquic_bbr_alg_startup = 0, in declaration
 * order). If a picoquic sync reorders that enum, this will not catch it; the guard against that is re-checking the order in
 * bbr.c against this list at sync time (noted in lib/picoquic-bbr/README.md). What this *does* catch is an accidental reorder of
 * our own enum. In a function body because PTLS_BUILD_ASSERT expands to a statement (quicly is C99, no file-scope
 * _Static_assert); the function is referenced from bbr_init so it is not dead-stripped-with-warning.
 */
static void bbr_assert_enum_values(void)
{
#define ASSERT_ENUM(a, v) PTLS_BUILD_ASSERT((int)(a) == (v))
    ASSERT_ENUM(QUICLY_CC_BBR_STARTUP, 0);
    ASSERT_ENUM(QUICLY_CC_BBR_DRAIN, 1);
    ASSERT_ENUM(QUICLY_CC_BBR_PROBE_BW_DOWN, 2);
    ASSERT_ENUM(QUICLY_CC_BBR_PROBE_BW_CRUISE, 3);
    ASSERT_ENUM(QUICLY_CC_BBR_PROBE_BW_REFILL, 4);
    ASSERT_ENUM(QUICLY_CC_BBR_PROBE_BW_UP, 5);
    ASSERT_ENUM(QUICLY_CC_BBR_PROBE_RTT, 6);
    ASSERT_ENUM(QUICLY_CC_BBR_STARTUP_LONG_RTT, 7);
    ASSERT_ENUM(QUICLY_CC_BBR_STARTUP_RESUME, 8);
#undef ASSERT_ENUM
}

/*
 * Per-connection state, hung off `cc->state.bbr.path` (a void* in cc.h, so the rest of quicly never sees this layout). `path`
 * MUST be the first member: bbr.c hands a `picoquic_path_t*` back into the cc_common shims and the pacing callbacks, and
 * `quicly_cc_bbr_shim_of()` (bottom of this file) recovers this wrapper by casting that pointer straight back. `cnx` and `quic`
 * are real, zeroed picoquic objects because unmodified bbr.c dereferences `path_x->cnx->quic->use_predictable_random` and
 * `path_x->cnx->client_mode` with the real picoquic types at init (§2.5) - a hand-rolled partial struct would not type-check
 * inside bbr.c.
 */
typedef struct st_quicly_cc_bbr_conn_t {
    picoquic_path_t path;
    picoquic_cnx_t cnx;
    picoquic_quic_t quic;
    quicly_cc_bbr_shim_t shim;
} quicly_cc_bbr_conn_t;

quicly_cc_bbr_shim_t *quicly_cc_bbr_shim_of(picoquic_path_t *path_x)
{
    /* path is the first member, so the path pointer is the wrapper pointer */
    return &((quicly_cc_bbr_conn_t *)path_x)->shim;
}

/* Decode BBR's current state word (§6 / cc-bbr.h) once, for both telemetry and the counters. */
static uint64_t bbr_observe(quicly_cc_bbr_conn_t *bc, uint64_t *bw)
{
    uint64_t cc_state = 0, cc_param = 0;
    picoquic_bbr_algorithm_struct.alg_observe(&bc->path, &cc_state, &cc_param);
    *bw = cc_param;
    return cc_state;
}

/*
 * Populate the picoquic path inputs that do not depend on the specific notification (§2.4 middle table). Fields BBR owns
 * (`cwin`, `rtt_min`, `is_cca_probing_up`, `is_cc_data_updated`, `is_ssthresh_initialized`, `bandwidth_estimate_max`,
 * `congestion_alg_state`) are deliberately left untouched after init - re-syncing them wipes the state machine (§11 trap 3).
 */
static void sync_inputs(quicly_cc_bbr_conn_t *bc, quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t max_udp_payload_size)
{
    bc->path.bytes_in_transit = loss->sentmap.bytes_in_flight;
    bc->path.rtt_sample = loss->rtt.latest_us;
    bc->path.smoothed_rtt = (uint64_t)loss->rtt.smoothed * 1000;
    bc->path.rtt_variant = (uint64_t)loss->rtt.variance * 1000;
    bc->path.send_mtu = max_udp_payload_size;
    bc->path.nb_retransmit = loss->pto_count > 0 ? (uint64_t)loss->pto_count : 0;
    /* leave bandwidth_estimate at 0 so BBR derives the rate from rtt_measurement (§2.3) */
}

/* Refresh the shim from the callback parameters so the cc_common helpers see this call's values (§2.5). */
static void sync_shim(quicly_cc_bbr_conn_t *bc, uint64_t next_pn, uint64_t largest_acked_pn, int is_cwnd_limited)
{
    bc->shim.next_pn = next_pn;
    bc->shim.largest_acked_pn = largest_acked_pn;
    bc->shim.is_cwnd_limited = is_cwnd_limited;
}

/*
 * The shared tail of every notify path: run BBR, copy its window out (trap 1), and fold the observation into the counters and
 * telemetry. `sample` carries the inputs already filled by the caller; this fills the outputs.
 */
static void notify(quicly_cc_bbr_conn_t *bc, quicly_cc_t *cc, picoquic_congestion_notification_t pq_notification,
                   quicly_cc_bbr_notification_t q_notification, picoquic_per_ack_state_t *as, int is_app_limited, uint64_t now_us,
                   int64_t now, quicly_cc_bbr_sample_t *sample)
{
    picoquic_bbr_algorithm_struct.alg_notify(&bc->cnx, &bc->path, pq_notification, as, now_us);

    /* OUTPUT read-back (§9 part 3, trap 1): BBR's window is in path.cwin. Without this line BBR is inert. */
    cc->cwnd = bc->path.cwin > UINT32_MAX ? UINT32_MAX : (uint32_t)bc->path.cwin;

    uint64_t bw = 0, cc_state = bbr_observe(bc, &bw);
    quicly_cc_bbr_counters_update(cc, q_notification, cc_state, bw, is_app_limited, now);

    if (bc->shim.cc->state.bbr.telemetry != NULL) {
        sample->now_us = now_us;
        sample->notification = (uint8_t)q_notification;
        sample->is_app_limited = (uint8_t)(is_app_limited != 0);
        sample->bytes_in_transit = bc->path.bytes_in_transit;
        sample->delivered = bc->path.delivered;
        sample->cwnd = cc->cwnd;
        sample->pacing_rate = cc->pacer_rate;
        sample->send_quantum = cc->state.bbr.send_quantum;
        sample->cc_state = cc_state;
        sample->bw = bw;
        quicly_cc_bbr_telemetry_record(cc->state.bbr.telemetry, sample);
    }
}

static void bbr_on_acked(quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t bytes, uint64_t largest_acked, uint32_t inflight,
                         int cc_limited, uint64_t next_pn, int64_t now, uint32_t max_udp_payload_size)
{
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    uint64_t now_us = cc->now_us; /* true microseconds from quicly (quicly_cc_t::now_us); `now` is only ms */

    /* Delivery-rate sample: bytes delivered since the largest acked packet was *sent*, computed by quicly from that packet's
     * send-time snapshot (quicly_cc_t::delivered_since_sent). Feeding this ACK's `bytes` instead would understate the rate by the
     * in-flight packet count per RTT and pin BBR's bandwidth estimate - the "pacing never increases" symptom. */
    uint64_t delivered_since_sent = cc->delivered_since_sent;

    /* the current-inflight param is unused: BBR wants inflight_prior (the send-time snapshot), taken from cc->inflight_prior */
    (void)inflight;

    /* running total of delivered bytes: BBR reads path.delivered, and the deltas below are measured against it (§2.4/§3) */
    bc->path.delivered += bytes;

    /* app-limited is hardcoded to 0 on the ACK path (BBR_POC_PLAN.md §5 documented fallback; real accounting is §12
     * out-of-scope). The `cc_limited` callback parameter is NOT a usable substitute here: it is quicly's `inflight >= cwnd/2`
     * heuristic, and BBR's pacer deliberately holds inflight well below cwnd, so that heuristic reports "app-limited" on nearly
     * every ack of a pipe-filling flow - which makes BBRCheckStartupFullBandwidth() bail and freezes startup forever (the first
     * bring-up symptom seen). §9.1's ratemeter signal (`pn_cwnd_limited`, which correctly counts a paced flow as cc-limited) is
     * the right fix, but it needs the `quicly_conn_t`, which is opaque outside quicly.c and not reachable from here. 0 means
     * BBR treats every sample as bandwidth-probing, which is correct for a bulk sender and inflates the estimate only when the
     * application is genuinely starved - acceptable for the POC. `restart_from_idle` still passes a true app-limited signal. */
    int is_app_limited = 0;

    sync_inputs(bc, cc, loss, max_udp_payload_size);
    sync_shim(bc, next_pn, largest_acked, cc_limited); /* is_cwnd_limited is a distinct signal, still driven by cc_limited */

    picoquic_per_ack_state_t as;
    memset(&as, 0, sizeof(as));
    as.rtt_measurement = loss->rtt.latest_us;
    as.nb_bytes_acknowledged = bytes;
    as.nb_bytes_delivered_since_packet_sent = delivered_since_sent;
    as.inflight_prior = cc->inflight_prior;
    as.is_app_limited = is_app_limited;
    as.is_cwnd_limited = cc_limited;

    quicly_cc_bbr_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.rtt_sample_us = loss->rtt.latest_us;
    sample.smoothed_rtt_us = (uint64_t)loss->rtt.smoothed * 1000;
    sample.bytes_acked = bytes;
    sample.delivered_since_sent = delivered_since_sent;
    sample.inflight_prior = cc->inflight_prior;
    notify(bc, cc, picoquic_congestion_notification_acknowledgement, QUICLY_CC_BBR_NOTIFY_ACK, &as, is_app_limited, now_us, now,
           &sample);
}

static void bbr_on_lost(quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t bytes, uint64_t lost_pn, uint64_t next_pn,
                        int64_t now, uint32_t max_udp_payload_size)
{
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    uint64_t now_us = cc->now_us; /* true microseconds from quicly (quicly_cc_t::now_us); `now` is only ms */

    sync_inputs(bc, cc, loss, max_udp_payload_size);
    sync_shim(bc, next_pn, lost_pn, 0);

    picoquic_per_ack_state_t as;
    memset(&as, 0, sizeof(as));
    as.lost_packet_number = lost_pn;
    as.nb_bytes_newly_lost = bytes;
    as.nb_loss_ranges_newly_lost = bytes != 0 ? 1 : 0;

    quicly_cc_bbr_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.bytes_lost = bytes;
    notify(bc, cc, picoquic_congestion_notification_repeat, QUICLY_CC_BBR_NOTIFY_REPEAT, &as, 0, now_us, now, &sample);
}

static void bbr_on_persistent_congestion(quicly_cc_t *cc, const quicly_loss_t *loss, int64_t now)
{
    /* Delivered to BBR as the RTO/timeout notification. quicly reports persistent congestion separately from ordinary loss;
     * picoquic surfaces the same condition through `timeout`, which drives BBROnEnterRTO (§2.7). */
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    uint64_t now_us = cc->now_us; /* true microseconds from quicly (quicly_cc_t::now_us); `now` is only ms */

    sync_inputs(bc, cc, loss, bc->path.send_mtu != 0 ? (uint32_t)bc->path.send_mtu : 0);
    sync_shim(bc, bc->shim.next_pn, bc->shim.largest_acked_pn, 0);

    picoquic_per_ack_state_t as;
    memset(&as, 0, sizeof(as));
    /* BBROnEnterRTO reads only lost_packet_number; we have no specific pn here, so use the last one the shim saw. */
    as.lost_packet_number = bc->shim.largest_acked_pn;

    quicly_cc_bbr_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    notify(bc, cc, picoquic_congestion_notification_timeout, QUICLY_CC_BBR_NOTIFY_TIMEOUT, &as, 0, now_us, now, &sample);
}

static void bbr_on_sent(quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t bytes, int64_t now)
{
    /* restart_from_idle: BBR only acts when the flow was idle (inflight_prior == 0) and app-limited (§2.7). bytes_in_transit
     * here still reflects the pre-send value the loss module holds, which is what BBROnTransmit wants. */
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    uint64_t now_us = cc->now_us; /* true microseconds from quicly (quicly_cc_t::now_us); `now` is only ms */
    uint64_t inflight_prior = loss->sentmap.bytes_in_flight;

    /* only a genuine restart-from-idle is interesting; skip the notify otherwise to avoid churning BBR on every sent packet */
    if (inflight_prior != 0)
        return;

    sync_inputs(bc, cc, loss, bc->path.send_mtu != 0 ? (uint32_t)bc->path.send_mtu : 0);
    sync_shim(bc, bc->shim.next_pn, bc->shim.largest_acked_pn, 0);

    picoquic_per_ack_state_t as;
    memset(&as, 0, sizeof(as));
    as.inflight_prior = inflight_prior;
    as.is_app_limited = 1;

    quicly_cc_bbr_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.inflight_prior = inflight_prior;
    notify(bc, cc, picoquic_congestion_notification_restart_from_idle, QUICLY_CC_BBR_NOTIFY_RESTART_FROM_IDLE, &as, 1, now_us,
           now, &sample);
}

static void bbr_on_late_ack(quicly_cc_t *cc, uint64_t pn, int64_t now)
{
    /* spurious_repeat: a packet previously deemed lost was later acked, so BBR should undo its loss response (§2.7, cheap win
     * via the existing cubic-undo detector). */
    quicly_cc_bbr_conn_t *bc = cc->state.bbr.path;
    uint64_t now_us = cc->now_us; /* true microseconds from quicly (quicly_cc_t::now_us); `now` is only ms */

    picoquic_per_ack_state_t as;
    memset(&as, 0, sizeof(as));
    as.lost_packet_number = pn;

    quicly_cc_bbr_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    notify(bc, cc, picoquic_congestion_notification_spurious_repeat, QUICLY_CC_BBR_NOTIFY_SPURIOUS_REPEAT, &as, 0, now_us, now,
           &sample);
}

static int bbr_switch(quicly_cc_t *cc);

static void bbr_reset(quicly_cc_t *cc, uint32_t initcwnd)
{
    /* Preserve the wrapper allocation across a reset if one already exists (switching cc types calls reset). */
    quicly_cc_bbr_conn_t *bc = cc->type == &quicly_cc_type_bbr ? cc->state.bbr.path : NULL;
    quicly_cc_bbr_telemetry_t *telemetry = bc != NULL ? cc->state.bbr.telemetry : NULL;

    if (bc == NULL) {
        if ((bc = malloc(sizeof(*bc))) == NULL) {
            /* nothing sensible to do in a CC reset path; a NULL here would crash on first notify, which is the POC's problem to
             * hit loudly rather than mask */
            assert(!"quicly_cc_type_bbr: out of memory allocating BBR connection state");
            abort();
        }
        telemetry = quicly_cc_bbr_telemetry_create();
    }

    memset(bc, 0, sizeof(*bc));
    bc->path.cnx = &bc->cnx;
    bc->cnx.quic = &bc->quic;

    memset(cc, 0, sizeof(*cc));
    cc->type = &quicly_cc_type_bbr;
    cc->cwnd = initcwnd;

    /* counters_init memsets and initializes the whole cc->state.bbr block, so it must run before we store path/telemetry there -
     * otherwise it would zero the pointers back out (this ordering bug was the first bring-up crash). It seeds cwnd_initial /
     * cwnd_maximum from cc->cwnd, hence cc->cwnd set above first. */
    quicly_cc_bbr_counters_init(cc);
    cc->ssthresh = UINT32_MAX;
    cc->state.bbr.path = bc;
    cc->state.bbr.telemetry = telemetry;
    bc->shim.cc = cc;

    /* seed rtt_min once, then BBR owns it (§2.4); leave it at the picoquic "unset" sentinel until quicly has a sample */
    bc->path.smoothed_rtt = PICOQUIC_INITIAL_RTT;
    bc->path.cwin = initcwnd;
    /* only matters for BBR's initial pacing quantum before the first ack; every real notify overwrites it via sync_inputs. 1280
     * = the conservative initial-epoch MTU quicly starts connections at (DEFAULT_INITIAL_EGRESS_MAX_UDP_PAYLOAD_SIZE). */
    bc->path.send_mtu = 1280;

    picoquic_bbr_algorithm_struct.alg_init(&bc->path, NULL, 0);

    /* alg_init set path.cwin from BBR's own initial-window logic; adopt it so cwnd and the state machine start consistent */
    cc->cwnd = bc->path.cwin > UINT32_MAX ? UINT32_MAX : (uint32_t)bc->path.cwin;
    cc->cwnd_initial = cc->cwnd_maximum = cc->cwnd;
}

static void bbr_init(quicly_init_cc_t *self, quicly_cc_t *cc, uint32_t initcwnd, int64_t now)
{
    (void)bbr_assert_enum_values; /* referenced so the compile-time checks are not dropped as an unused function */
    bbr_reset(cc, initcwnd);
}

static int bbr_switch(quicly_cc_t *cc)
{
    if (cc->type == &quicly_cc_type_bbr)
        return 1;
    bbr_reset(cc, cc->cwnd);
    return 1;
}

static void bbr_dispose(quicly_cc_t *cc)
{
    if (cc->type != &quicly_cc_type_bbr)
        return;
    /* dumps the CSV (if telemetry was enabled) and frees the ring, then the per-connection picoquic wrapper */
    quicly_cc_bbr_telemetry_destroy(cc->state.bbr.telemetry);
    cc->state.bbr.telemetry = NULL;
    free(cc->state.bbr.path);
    cc->state.bbr.path = NULL;
}

quicly_init_cc_t quicly_cc_bbr_init = {bbr_init};

quicly_cc_type_t quicly_cc_type_bbr = {"bbr",
                                       &quicly_cc_bbr_init,
                                       bbr_on_acked,
                                       bbr_on_lost,
                                       bbr_on_persistent_congestion,
                                       bbr_on_sent,
                                       bbr_switch,
                                       bbr_on_late_ack,
                                       NULL,
                                       NULL,
                                       bbr_dispose};
