/*
 * Telemetry and counters for the BBR congestion controller.
 *
 * BBR itself is picoquic's `bbr.c`, driven by an adapter (`lib/cc-bbr.c`). This header is the narrow surface between that adapter
 * and the rest of quicly: it deliberately pulls in *no* picoquic header, so that the two QUIC stacks' header namespaces never meet.
 * See BBR_POC_PLAN.md §2.8.
 *
 * BBR's own state is private to picoquic's `bbr.c` (`picoquic_bbr_state_t` is declared in no header), so everything here is derived
 * from what the adapter can legitimately see: the packed word returned by `picoquic_bbr_observe()`, the `picoquic_path_t` fields
 * BBR writes, the pacing rate BBR hands back through `picoquic_update_pacing_rate()`, and the adapter's own inputs.
 */

#ifndef quicly_cc_bbr_h
#define quicly_cc_bbr_h

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>

struct st_quicly_cc_t;

/**
 * BBR's primary state. The values mirror picoquic's `picoquic_bbr_alg_state_t` (`bbr.c`) and are decoded straight out of
 * `picoquic_bbr_observe()`, so **the order must be kept identical**. `lib/cc-bbr.c` is the only place that can see both enums;
 * it static-asserts the correspondence.
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
 * The notifications the adapter forwards to BBR. Mirrors the subset of `picoquic_congestion_notification_t` that the POC wires
 * (BBR_POC_PLAN.md §2.7); the adapter maps between the two.
 */
typedef enum en_quicly_cc_bbr_notification_t {
    QUICLY_CC_BBR_NOTIFY_ACK = 0,
    QUICLY_CC_BBR_NOTIFY_REPEAT,
    QUICLY_CC_BBR_NOTIFY_TIMEOUT,
    QUICLY_CC_BBR_NOTIFY_RESTART_FROM_IDLE,
    QUICLY_CC_BBR_NOTIFY_SPURIOUS_REPEAT,
    QUICLY_CC_BBR_NUM_NOTIFICATIONS
} quicly_cc_bbr_notification_t;

/**
 * Accessors for the word `picoquic_bbr_observe()` packs its state into. Layout is fixed by picoquic (`bbr.c`), not by us.
 */
#define QUICLY_CC_BBR_OBSERVED_STATE(w) ((unsigned)((w)&0xff))
#define QUICLY_CC_BBR_OBSERVED_ACK_PHASE(w) ((unsigned)(((w) >> 8) & 0xff))
#define QUICLY_CC_BBR_OBSERVED_FILLED_PIPE(w) ((unsigned)(((w) >> 16) & 1))
#define QUICLY_CC_BBR_OBSERVED_IN_RECOVERY(w) ((unsigned)(((w) >> 17) & 1))
#define QUICLY_CC_BBR_OBSERVED_PTO_RECOVERY(w) ((unsigned)(((w) >> 18) & 1))
#define QUICLY_CC_BBR_OBSERVED_PACKET_CONSERVATION(w) ((unsigned)(((w) >> 19) & 1))
#define QUICLY_CC_BBR_OBSERVED_IDLE_RESTART(w) ((unsigned)(((w) >> 20) & 1))

/**
 * One telemetry row: everything the adapter synced in, everything it read back out, and BBR's own view at that moment. Written once
 * per notification.
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
     * BBR's own view: the packed word from `picoquic_bbr_observe()` and the bandwidth estimate it reports
     */
    uint64_t cc_state;
    uint64_t bw;
} quicly_cc_bbr_sample_t;

/**
 * In-memory ring of `quicly_cc_bbr_sample_t`, dumped as CSV when the connection closes. Opaque; heap-allocated only when telemetry
 * is switched on, so a connection running without it pays nothing.
 */
typedef struct st_quicly_cc_bbr_telemetry_t quicly_cc_bbr_telemetry_t;

/**
 * Default number of rows retained. Override with `QUICLY_BBR_CSV_ROWS`.
 */
#define QUICLY_CC_BBR_TELEMETRY_DEFAULT_ROWS 4096

/**
 * Allocates a telemetry ring iff `QUICLY_BBR_CSV_DIR` names a directory, returning NULL otherwise (i.e., NULL means "telemetry is
 * off", not "out of memory" - it is not an error). The CSV is written into that directory by `quicly_cc_bbr_telemetry_destroy`.
 */
quicly_cc_bbr_telemetry_t *quicly_cc_bbr_telemetry_create(void);
/**
 * Appends a row, evicting the oldest if the ring is full. No-op when `telemetry` is NULL, so callers need not test.
 */
void quicly_cc_bbr_telemetry_record(quicly_cc_bbr_telemetry_t *telemetry, const quicly_cc_bbr_sample_t *sample);
/**
 * Writes the retained rows as CSV, oldest first, including a header line. Exposed separately from `_destroy` so that a caller (or a
 * test) can dump without tearing down.
 */
void quicly_cc_bbr_telemetry_dump(quicly_cc_bbr_telemetry_t *telemetry, FILE *fp);
/**
 * Dumps to the configured file, then frees. No-op when `telemetry` is NULL.
 */
void quicly_cc_bbr_telemetry_destroy(quicly_cc_bbr_telemetry_t *telemetry);
/**
 * Number of rows retained and total number recorded (the latter keeps counting past the ring's capacity).
 */
size_t quicly_cc_bbr_telemetry_num_retained(const quicly_cc_bbr_telemetry_t *telemetry);
size_t quicly_cc_bbr_telemetry_num_recorded(const quicly_cc_bbr_telemetry_t *telemetry);

/**
 * Name of a BBR state, for CSV and logs. Returns "invalid" for out-of-range input rather than asserting, as the value originates
 * from vendored code.
 */
const char *quicly_cc_bbr_state_name(unsigned state);
/**
 * Name of a notification.
 */
const char *quicly_cc_bbr_notification_name(unsigned notification);

/**
 * Initializes the counters, both the BBR-specific block and the generic `quicly_cc_t` fields that BBR reuses. Call from the BBR
 * congestion controller's init, after `cwnd` has been set.
 */
void quicly_cc_bbr_counters_init(struct st_quicly_cc_t *cc);
/**
 * Updates the counters from one notification. Call *after* the notify call and *after* copying `path->cwin` back into `cc->cwnd`:
 * `cwnd_minimum` / `cwnd_maximum` are derived from `cc->cwnd`, so a forgotten read-back (BBR_POC_PLAN.md §9, trap 1) shows up here
 * as a congestion window that never moves.
 *
 * @param cc_state         the packed word from `picoquic_bbr_observe()`
 * @param bw               BBR's bandwidth estimate, bytes/sec
 * @param is_app_limited   the value fed to BBR for this ACK; ignored for other notifications
 * @param now              current time in milliseconds, for `exit_slow_start_at`
 */
void quicly_cc_bbr_counters_update(struct st_quicly_cc_t *cc, quicly_cc_bbr_notification_t notification, uint64_t cc_state,
                                   uint64_t bw, int is_app_limited, int64_t now);
/**
 * Records the pacing rate BBR handed back. Call from the `picoquic_update_pacing_rate()` implementation (BBR_POC_PLAN.md §2.6).
 */
void quicly_cc_bbr_counters_update_pacing(struct st_quicly_cc_t *cc, uint64_t pacing_rate, uint64_t send_quantum);

#ifdef __cplusplus
}
#endif

#endif
