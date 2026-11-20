/*
 * Inputs and state names for the BBR congestion controller. Does not include
 * any picoquic header to keep the namespaces separate.
 */

#ifndef quicly_cc_bbr_h
#define quicly_cc_bbr_h

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct st_quicly_cc_t;

/**
 * Per-ACK inputs that only BBR consumes in cc_on_acked.
 */
typedef struct st_quicly_cc_bbr_ack_params_t {
    /**
     * bytes delivered while the packet was in flight; the per-packet fields describe the
     * newest newly acked packet that was not a late ack
     */
    uint64_t delivered_since_sent;
    /**
     * bytes in flight when the packet was sent
     */
    uint64_t inflight_prior;
    /**
     * bytes lost while the packet was in flight
     */
    uint64_t lost_since_sent;
    /**
     * if the packet was sent while app-limited
     */
    int app_limited;
    /**
     * cumulative ECT(0), ECT(1) and CE counts reported by the peer, including this ack
     */
    uint64_t ecn_counts[3];
} quicly_cc_bbr_ack_params_t;

/**
 * Per-loss inputs that only BBR consumes in cc_on_lost, from the lost packet's send-time snapshots.
 */
typedef struct st_quicly_cc_bbr_loss_params_t {
    /**
     * Bytes delivered while the lost packet was in flight.
     */
    uint64_t delivered_since_sent;
    /**
     * Bytes in flight as of when the lost packet was sent.
     */
    uint64_t inflight_prior;
    /**
     * Bytes lost while the lost packet was in flight, including the packet itself.
     */
    uint64_t lost_since_sent;
    /**
     * Whether the sender was app-limited when the lost packet was sent.
     */
    int app_limited;
} quicly_cc_bbr_loss_params_t;

/**
 * BBR's current state from cc.state.bbr.cc_state; see quicly_cc_bbr_state_name()
 */
#define QUICLY_CC_BBR_OBSERVED_STATE(w) ((unsigned)((w)&0xff))

/**
 * name of a BBR state; "invalid" if out of range
 */
const char *quicly_cc_bbr_state_name(unsigned state);


#ifdef __cplusplus
}
#endif

#endif
