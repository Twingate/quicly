/*
 * Replacement for picoquic's cc_common.c (BBR_POC_PLAN.md §2.5, §8 step 5).
 *
 * `bbr.c` calls exactly five functions from picoquic's cc_common.c, three of which dereference `path_x->cnx` (picoquic's
 * connection object, which does not exist here). Rather than vendor cc_common.c and fabricate a `picoquic_cnx_t`/`pkt_ctx` that
 * satisfies it, this file reimplements those three against quicly state, and copies the remaining two (plus the pure helper they
 * both call) verbatim, since none of the three touch anything connection-specific.
 *
 * Containment: this file, `picoquic-bbr/bbr.c`, and `cc-bbr.c` are the only ones that may include a vendored picoquic header. See
 * BBR_POC_PLAN.md §2.8.
 */

#include "picoquic_internal.h"
#include "cc_common.h"
#include "quicly/cc.h"

/*
 * The three functions below need values quicly already hands the adapter as callback parameters on every `cc_on_*` invocation
 * (`next_pn`, `largest_acked`, `cc_limited` — see `struct st_quicly_cc_type_t` in include/quicly/cc.h) and, for the pacing
 * functions, the `quicly_cc_t*` the adapter is driving. None of that is reachable from a bare `picoquic_path_t*` on its own, so
 * the adapter (`cc-bbr.c`, step 6) is expected to populate one of these per connection and make it recoverable from the
 * `picoquic_path_t*` bbr.c hands back into these functions (e.g. by embedding `picoquic_path_t` as the first member of a wrapper
 * struct and casting).
 *
 * step 6 TODO: replace `quicly_cc_bbr_shim_of()` below with the real per-connection lookup and delete this stub. Until then it is
 * unreachable dead code — nothing in this tree calls `picoquic_bbr_notify()` yet (BBR_POC_PLAN.md §8's acceptance bar for this
 * step is compiling and linking with the notify path never exercised), so returning NULL here is safe: if this ever executes
 * before step 6 replaces it, that is itself the bug to fix.
 */
typedef struct st_quicly_cc_bbr_shim_t {
    /**
     * next packet number to send; mirrors the `next_pn` parameter every `cc_on_*` callback already receives
     */
    uint64_t next_pn;
    /**
     * largest acked packet number; mirrors `cc_on_acked`'s `largest_acked` parameter
     */
    uint64_t largest_acked_pn;
    /**
     * whether the flow is currently cwnd-limited; mirrors `cc_on_acked`'s `cc_limited` parameter
     */
    int is_cwnd_limited;
    /**
     * the connection's congestion controller state, for the pacing callbacks to reach `cc->pacer_rate` / `cc->state.bbr`
     */
    quicly_cc_t *cc;
} quicly_cc_bbr_shim_t;

static quicly_cc_bbr_shim_t *quicly_cc_bbr_shim_of(picoquic_path_t *path_x)
{
    (void)path_x;
    return NULL;
}

uint64_t picoquic_cc_get_sequence_number(picoquic_cnx_t *cnx, picoquic_path_t *path_x)
{
    (void)cnx; /* the parameter exists for cc_common.c API compatibility; quicly has no equivalent object to pass here */
    return quicly_cc_bbr_shim_of(path_x)->next_pn;
}

uint64_t picoquic_cc_get_ack_number(picoquic_cnx_t *cnx, picoquic_path_t *path_x)
{
    (void)cnx;
    return quicly_cc_bbr_shim_of(path_x)->largest_acked_pn;
}

uint64_t picoquic_cc_slow_start_increase(picoquic_path_t *path_x, uint64_t nb_delivered)
{
    return quicly_cc_bbr_shim_of(path_x)->is_cwnd_limited ? nb_delivered : 0;
}

/*
 * The pacing rate leaves BBR through this callback, not a field (BBR_POC_PLAN.md §2.6) - not part of cc_common.c, but declared in
 * the same vendored `picoquic_internal.h` and belongs alongside the other picoquic-facing shims for the same reason.
 */
void picoquic_update_pacing_rate(picoquic_path_t *path_x, double pacing_rate, uint64_t quantum)
{
    quicly_cc_t *cc = quicly_cc_bbr_shim_of(path_x)->cc;
    cc->pacer_rate = quicly_cc_bbr_calc_pacer_rate(pacing_rate);
    cc->state.bbr.send_quantum = quantum;
}

void picoquic_update_pacing_data(picoquic_path_t *path_x, int slow_start)
{
    /* window-based fallback, used only by startup_long_rtt; no-op is fine for the POC per BBR_POC_PLAN.md §2.6 */
    (void)path_x;
    (void)slow_start;
}

/*
 * Reached only from `BBRInitRandom` via `bbr_state->random_context`, itself private to bbr.c. A fixed return is fine for a POC
 * (BBR_POC_PLAN.md §8 step 5) - this is picoquic's PRNG helper (normally lib/util.c), not anything quicly owns.
 */
uint64_t picoquic_test_uniform_random(uint64_t *random_context, uint64_t rnd_max)
{
    (void)random_context;
    (void)rnd_max;
    return 0;
}

/*
 * Copied verbatim from picoquic's cc_common.c: pure functions operating only on the `picoquic_min_max_rtt_t` passed in. No quicly
 * state involved, so nothing to adapt.
 */

void picoquic_cc_filter_rtt_min_max(picoquic_min_max_rtt_t *rtt_track, uint64_t rtt)
{
    int x = rtt_track->sample_current;
    int x_max;

    rtt_track->samples[x] = rtt;

    rtt_track->sample_current = x + 1;
    if (rtt_track->sample_current >= PICOQUIC_MIN_MAX_RTT_SCOPE) {
        rtt_track->is_init = 1;
        rtt_track->sample_current = 0;
    }

    x_max = (rtt_track->is_init) ? PICOQUIC_MIN_MAX_RTT_SCOPE : x + 1;

    rtt_track->sample_min = rtt_track->samples[0];
    rtt_track->sample_max = rtt_track->samples[0];

    for (int i = 1; i < x_max; i++) {
        if (rtt_track->samples[i] < rtt_track->sample_min) {
            rtt_track->sample_min = rtt_track->samples[i];
        } else if (rtt_track->samples[i] > rtt_track->sample_max) {
            rtt_track->sample_max = rtt_track->samples[i];
        }
    }
}

int picoquic_cc_hystart_loss_volume_test(picoquic_min_max_rtt_t *rtt_track, picoquic_congestion_notification_t event,
                                         uint64_t nb_bytes_newly_acked, uint64_t nb_bytes_newly_lost)
{
    int ret = 0;

    rtt_track->smoothed_bytes_lost_16 -= rtt_track->smoothed_bytes_lost_16 / 16;
    rtt_track->smoothed_bytes_lost_16 += nb_bytes_newly_lost;
    rtt_track->smoothed_bytes_sent_16 -= rtt_track->smoothed_bytes_sent_16 / 16;
    rtt_track->smoothed_bytes_sent_16 += nb_bytes_newly_acked + nb_bytes_newly_lost;

    if (rtt_track->smoothed_bytes_sent_16 > 0) {
        rtt_track->smoothed_drop_rate = ((double)rtt_track->smoothed_bytes_lost_16) / ((double)rtt_track->smoothed_bytes_sent_16);
    } else {
        rtt_track->smoothed_drop_rate = 0;
    }

    switch (event) {
    case picoquic_congestion_notification_acknowledgement:
        ret = rtt_track->smoothed_drop_rate > PICOQUIC_SMOOTHED_LOSS_THRESHOLD;
        break;
    case picoquic_congestion_notification_timeout:
        ret = 1;
    default:
        break;
    }

    return ret;
}

int picoquic_cc_hystart_test(picoquic_min_max_rtt_t *rtt_track, uint64_t rtt_measurement, uint64_t packet_time,
                             uint64_t current_time, int is_one_way_delay_enabled)
{
    int ret = 0;

    if (is_one_way_delay_enabled && rtt_measurement == 0) {
        return 0;
    }

    if (current_time > rtt_track->last_rtt_sample_time + 1000) {
        picoquic_cc_filter_rtt_min_max(rtt_track, rtt_measurement);
        rtt_track->last_rtt_sample_time = current_time;

        if (rtt_track->is_init) {
            uint64_t delta_max;

            if (rtt_track->rtt_filtered_min == 0 || rtt_track->rtt_filtered_min > rtt_track->sample_max) {
                rtt_track->rtt_filtered_min = rtt_track->sample_max;
            }
            delta_max = rtt_track->rtt_filtered_min / 4;
            if (delta_max < packet_time) {
                delta_max = packet_time;
            }

            if (rtt_track->sample_min > rtt_track->rtt_filtered_min) {
                if (rtt_track->sample_min > rtt_track->rtt_filtered_min + delta_max) {
                    rtt_track->nb_rtt_excess++;
                    if (rtt_track->nb_rtt_excess >= PICOQUIC_MIN_MAX_RTT_SCOPE) {
                        /* RTT increased too much, get out of slow start! */
                        ret = 1;
                    }
                }
            } else {
                rtt_track->nb_rtt_excess = 0;
            }
        }
    }

    return ret;
}
