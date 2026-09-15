/*
 * Internal glue shared between the two picoquic-aware translation units of the BBR POC: the adapter (`cc-bbr.c`) and the
 * cc_common replacement (`cc-bbr-common.c`). See BBR_POC_PLAN.md §2.5.
 *
 * This header includes a picoquic header, so it is itself subject to the §2.8 containment rule: only `cc-bbr.c` and
 * `cc-bbr-common.c` may include it. Nothing under `include/quicly/` may.
 */

#ifndef quicly_cc_bbr_shim_h
#define quicly_cc_bbr_shim_h

#include "picoquic_internal.h" /* picoquic_path_t */
#include "quicly/cc.h"         /* quicly_cc_t */

/*
 * picoquic's `bbr.c` reaches back into "connection" state through `path_x->cnx` and through the two cc_common helpers
 * (`picoquic_cc_get_sequence_number`/`_ack_number`) and `picoquic_cc_slow_start_increase`. quicly has no picoquic connection
 * object, but every value those paths need already arrives at the adapter as a parameter of the `cc_on_*` callback that drives a
 * given `picoquic_bbr_notify()` call. This struct is where the adapter parks those values so the cc_common shims can read them
 * back; the adapter refreshes it immediately before each notify call.
 */
typedef struct st_quicly_cc_bbr_shim_t {
    /**
     * next packet number to send; the `next_pn` parameter every `cc_on_*` callback receives. Feeds
     * `picoquic_cc_get_sequence_number()`, which drives BBR's round and recovery bookkeeping.
     */
    uint64_t next_pn;
    /**
     * largest acked packet number so far; feeds `picoquic_cc_get_ack_number()`.
     */
    uint64_t largest_acked_pn;
    /**
     * whether the flow is currently cwnd-limited; the `cc_limited` parameter of `cc_on_acked`. Feeds
     * `picoquic_cc_slow_start_increase()`.
     */
    int is_cwnd_limited;
    /**
     * the connection's congestion controller, for the pacing callbacks to reach `cc->pacer_rate` / `cc->state.bbr`.
     */
    quicly_cc_t *cc;
} quicly_cc_bbr_shim_t;

/**
 * Recovers the shim for a given picoquic path. The adapter allocates a wrapper whose first member is the `picoquic_path_t` it
 * hands to bbr.c, so a `picoquic_path_t*` coming back out of bbr.c can be cast straight to the wrapper. Defined in `cc-bbr.c`
 * (the wrapper layout is private to it); `cc-bbr-common.c` only calls it.
 */
quicly_cc_bbr_shim_t *quicly_cc_bbr_shim_of(picoquic_path_t *path_x);

#endif
