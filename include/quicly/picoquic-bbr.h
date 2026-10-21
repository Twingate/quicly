/*
 * bbr.h -- self-contained picoquic shim for the vendored picoquic BBRv3
 * implementation (bbr.c).
 *
 * This header replaces the three picoquic headers that bbr.c originally
 * included (picoquic_internal.h, cc_common.h, picoquic_utils.h) and what they
 * pulled in. It declares EXACTLY the picoquic symbols that
 * bbr.c references, with identical names and field types copied verbatim from
 * the picoquic sources, so that future re-syncs of bbr.c remain a clean copy.
 *
 * It deliberately does NOT #include any picoquic or quicly header.
 */

#ifndef PICOQUIC_BBR_SHIM_H
#define PICOQUIC_BBR_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Constants (copied verbatim from picoquic headers)                  */
/* ------------------------------------------------------------------ */

/* picoquic_internal.h */
#define PICOQUIC_MAX_PACKET_SIZE 1536
#define PICOQUIC_INITIAL_RTT 250000ull /* 250 ms */
#define PICOQUIC_TARGET_RENO_RTT 100000ull /* 100 ms */
#define PICOQUIC_TARGET_SATELLITE_RTT 610000ull /* 610 ms */
#define PICOQUIC_MINRTT_MARGIN 128 /* Typical RTT measurement uncertainty (e.g. scheduling) */
#define PICOQUIC_MINRTT_THRESHOLD 128 /* RTT MIN below which CC ignores RTT changes */
#define PICOQUIC_CWIN_INITIAL (10 * PICOQUIC_MAX_PACKET_SIZE)
#define PICOQUIC_CC_ALGO_NUMBER_BBR 5

/* picoquic.h */
#define PICOQUIC_ECN_ECT_0 0x02

/* cc_common.h */
#define PICOQUIC_MIN_MAX_RTT_SCOPE 7

/* picoquic_utils.h -- function-like macros */
#define PICOQUIC_BYTES_FROM_RATE(microseconds, bps) (((uint64_t)(microseconds)*((uint64_t)(bps)))/1000000ull)
#define PICOQUIC_RATE_FROM_BYTES(bytes, microseconds) ((((uint64_t)(bytes))*1000000ull)/((uint64_t)(microseconds)))

/* picoquic.h -- marks unused function parameters. */
#ifndef UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define UNUSED(x) x __attribute__((unused))
#else
#define UNUSED(x) x
#endif
#endif

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

typedef struct st_picoquic_cnx_t picoquic_cnx_t;
typedef struct st_picoquic_quic_t picoquic_quic_t;
typedef struct st_picoquic_path_t picoquic_path_t;

/* ------------------------------------------------------------------ */
/* Enums (copied verbatim from picoquic.h / picoquic_internal.h)      */
/* ------------------------------------------------------------------ */

/* Subset of picoquic_state_enum: only the value bbr.c compares against. */
typedef enum {
    picoquic_state_ready
} picoquic_state_enum;

/* picoquic.h */
typedef enum {
    picoquic_congestion_notification_acknowledgement,
    picoquic_congestion_notification_repeat,
    picoquic_congestion_notification_timeout,
    picoquic_congestion_notification_spurious_repeat,
    picoquic_congestion_notification_rtt_measurement,
    picoquic_congestion_notification_ecn_ec,
    picoquic_congestion_notification_cwin_blocked,
    picoquic_congestion_notification_seed_cwin,
    picoquic_congestion_notification_reset,
    picoquic_congestion_notification_lost_feedback, /* notification of lost feedback */
    picoquic_congestion_notification_restart_from_idle /* before ack-eliciting restart from idle */
} picoquic_congestion_notification_t;

/* ------------------------------------------------------------------ */
/* Structs (minimal subsets, exact names/types from picoquic sources) */
/* ------------------------------------------------------------------ */

/* picoquic_internal.h -- copied verbatim (used as a value member). */
typedef struct st_picoquic_pacing_t {
    uint64_t rate;
    uint64_t evaluation_time;
    int64_t bucket_max;
    uint64_t packet_time_microsec;
    uint64_t quantum_max;
    uint64_t rate_max;
    int bandwidth_pause;
    /* High precision variables should only be used inside pacing.c */
    int64_t bucket_nanosec;
    int64_t packet_time_nanosec;
} picoquic_pacing_t;

/* cc_common.h -- copied verbatim (used as a value member). */
typedef struct st_picoquic_min_max_rtt_t {
    uint64_t last_rtt_sample_time;
    uint64_t rtt_filtered_min;
    int nb_rtt_excess;
    int sample_current;
    int is_init;
    double smoothed_drop_rate;
    uint64_t smoothed_bytes_sent_16;
    uint64_t smoothed_bytes_lost_16;
    uint64_t last_lost_packet_number;
    uint64_t sample_min;
    uint64_t sample_max;
    uint64_t samples[PICOQUIC_MIN_MAX_RTT_SCOPE];
} picoquic_min_max_rtt_t;

/* picoquic.h -- copied verbatim. */
typedef struct st_picoquic_per_ack_state_t {
    uint64_t rtt_measurement; /* RTT as measured when receiving the ACK */
    uint64_t send_delay; /* Delay between send time of acked packet and prior ack. */
    uint64_t one_way_delay; /* One way delay when receiving the ACK, 0 if unknown */
    uint64_t nb_bytes_acknowledged; /* Number of bytes acknowledged by this ACK */
    uint64_t nb_bytes_newly_lost; /* Number of bytes in packets found lost because of this ACK */
    uint64_t nb_loss_ranges_newly_lost; /* Discontiguous loss ranges found by this ACK */
    uint64_t nb_bytes_lost_since_packet_sent; /* Bytes lost since the packet was sent */
    uint64_t nb_bytes_delivered_since_packet_sent; /* Bytes acked since the packet was sent */
    uint64_t inflight_prior;
    uint64_t lost_packet_number;
    uint64_t lost_packet_sent_time;
    int pc; /* Using int type instead of pc enum to avoid include dependencies */
    unsigned int is_app_limited : 1; /* App marked limited at time of ACK? */
    unsigned int is_cwnd_limited: 1; /* path marked CWIN limited after packet was sent. */
} picoquic_per_ack_state_t;

/* picoquic_internal.h -- minimal subset of the fields bbr.c references. */
struct st_picoquic_quic_t {
    unsigned int use_predictable_random : 1; /* For logging tests */
};

/* picoquic_internal.h -- minimal subset of the fields bbr.c references. */
struct st_picoquic_cnx_t {
    picoquic_quic_t* quic;
    picoquic_state_enum cnx_state;
    unsigned int client_mode : 1; /* Is this connection the client side? */
};

/* picoquic_internal.h -- minimal subset of the fields bbr.c references. */
struct st_picoquic_path_t {
    struct st_picoquic_cnx_t* cnx;
    uint64_t unique_path_id;

    /* Remote ECN counters attributed to packets sent on this path. */
    uint64_t ecn_ect0_total_remote;
    uint64_t ecn_ect1_total_remote;
    uint64_t ecn_ce_total_remote;

    unsigned int is_cc_data_updated : 1;
    unsigned int is_ssthresh_initialized : 1;
    unsigned int is_cca_probing_up : 1; /* congestion control algorithm is seeking more bandwidth */

    uint64_t nb_retransmit; /* Number of timeout retransmissions since last ACK */

    /* Time measurement */
    uint64_t rtt_sample;
    uint64_t smoothed_rtt;
    uint64_t rtt_variant;
    uint64_t rtt_min;

    /* MTU */
    size_t send_mtu;

    /* Bandwidth measurement */
    uint64_t delivered; /* The total amount of data delivered so far on the path */
    uint64_t bandwidth_estimate; /* In bytes per second */
    uint64_t bandwidth_estimate_max; /* Maximum of bandwidth estimate over life of path */
    uint64_t peak_bandwidth_estimate; /* Bytes/sec, peak over a short interval */

    /* Congestion control state */
    uint64_t cwin;
    uint64_t bytes_in_transit;
    uint64_t last_sender_limited_time;
    uint64_t last_time_acked_data_frame_sent;
    void* congestion_alg_state;
    picoquic_pacing_t pacing;
};

/* ------------------------------------------------------------------ */
/* Congestion algorithm vtable (copied verbatim from picoquic.h)      */
/* bbr.c defines an instance of picoquic_congestion_algorithm_t.       */
/* ------------------------------------------------------------------ */

typedef void (*picoquic_congestion_algorithm_init)(picoquic_path_t* path_x, char const * option_string, uint64_t current_time);
typedef void (*picoquic_congestion_algorithm_notify)(
    picoquic_cnx_t* cnx,
    picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    picoquic_per_ack_state_t * ack_state,
    uint64_t current_time);
typedef void (*picoquic_congestion_algorithm_delete)(picoquic_path_t* cnx);
typedef void (*picoquic_congestion_algorithm_observe)(
    picoquic_path_t* path_x, uint64_t * cc_state, uint64_t * cc_param);

typedef struct st_picoquic_congestion_algorithm_t {
    char const * congestion_algorithm_id;
    uint8_t congestion_algorithm_number;
    uint8_t ecn_mark;
    picoquic_congestion_algorithm_init alg_init;
    picoquic_congestion_algorithm_notify alg_notify;
    picoquic_congestion_algorithm_delete alg_delete;
    picoquic_congestion_algorithm_observe alg_observe;
} picoquic_congestion_algorithm_t;

/* ------------------------------------------------------------------ */
/* Function prototypes (implemented elsewhere in picoquic)            */
/* exact signatures from cc_common.h / picoquic_utils.h /             */
/* picoquic_internal.h                                                */
/* ------------------------------------------------------------------ */

/* cc_common.h */
uint64_t picoquic_cc_get_sequence_number(picoquic_cnx_t* cnx, picoquic_path_t* path_x);
uint64_t picoquic_cc_get_ack_number(picoquic_cnx_t* cnx, picoquic_path_t * path_x);
int picoquic_cc_hystart_loss_volume_test(picoquic_min_max_rtt_t* rtt_track, picoquic_congestion_notification_t event, uint64_t nb_bytes_newly_acked, uint64_t nb_bytes_newly_lost);
int picoquic_cc_hystart_test(picoquic_min_max_rtt_t* rtt_track, uint64_t rtt_measurement, uint64_t packet_time, uint64_t current_time, int is_one_way_delay_enabled);
uint64_t picoquic_cc_slow_start_increase(picoquic_path_t* path_x, uint64_t nb_delivered);

/* picoquic_utils.h */
uint64_t picoquic_test_uniform_random(uint64_t* random_context, uint64_t rnd_max);

/* picoquic_internal.h */
void picoquic_update_pacing_data(picoquic_path_t * path_x, int slow_start);
void picoquic_update_pacing_rate(picoquic_path_t* path_x, double pacing_rate, uint64_t quantum);

#ifdef __cplusplus
}
#endif

#endif /* PICOQUIC_BBR_SHIM_H */
