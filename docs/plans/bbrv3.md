# BBRv3 in quicly: plan and design record

Status as of 2026-10-25, after three review rounds and the first phase 5 run on a real path. Work
lives on quicly branch `bbr-dev` (Twingate's fork, based on `libsdwan-quicly-2026-09-29`) and
libsdwan branch `bbr`; the quicly stack is pushed as draft PRs (§14). This file is the single
record of what was built, why, what was measured, what was rejected, and what is still open. It is
written for future sessions picking the work up cold; it does not rely on any other document.

## Decisions at a glance

One line per decision; the section named holds the reasoning and evidence.

Project:
- Vendor picoquic's `bbr.c` and write an adapter; fix bugs upstream, not in the copy. The copy
  differs only in its includes and two RTT checks turned off (§1, §6).
- Change upstream quicly only where BBR needs it; record other review suggestions as decisions
  (§1, §11).
- Always compiled, chosen at runtime, no `#ifdef`; off by default in libsdwan (§1, §10).
- Name the param structs after BBR until a second controller uses them (§10).

quicly core:
- Fractional `now` passed to every controller; the existing ones truncate it everywhere, so their
  behaviour is unchanged (§3.1).
- Pacing rate, dispose, `requires_pacing` and zero-byte ACKs as optional zero-default hooks (§3).
- Find `requires_pacing` by scanning the registered factories, not by allocating the pacer after
  `init_cc` (§10).
- `promote_path` disposes the old controller; `quicly_set_cc`'s same-type check stays (§3.3).
- Snapshots through `quicly_sentmap_set_snapshot()`, so `quicly_sentmap_commit` keeps upstream's
  signature (§3.7).
- 32-bit snapshot deltas: 40 bytes per sent packet, not 48 (§3.7).
- Delivered and lost bytes counted per path in `egress.delivery`, reset by a migration
  (§3.7, §11).
- The ACK snapshot skips late-acked packets (§3.7, §11).
- `cc_on_sent` carries `app_limited` (restart from idle per the draft) and `next_pn`
  (§3.8, §11).
- ACK inputs built in static helpers, not inline in `handle_ack_frame` (§11).
- Not done: ECN delta de-duplication; a "filled cwnd" bit instead of `cc_limited` (§11).

Adapter and algorithm:
- BBR refuses a switch into it on a live connection (§5).
- No long-RTT startup: `rtt_min` held at 0, helpers are asserting stubs that do not abort
  (§5, §11).
- No late-ack handler; no PTO/timeout hook (§10, §11).
- Option "R" on; picoquic's RTT-based Startup exit and saturation check off (§6, §9).
- Losses also reach BBR on the next ACK's notification, as picoquic passes them (§5).
- Previous RTT sample repeated on ACKs without one, as picoquic does (§11).
- Exported `picoquic_*` / `BBR*` names kept (§11).
- State enum mirrored by hand, checked at each re-sync (§10).
- Rejected after measuring: cruise pacing margin, smaller send quantum without the UP gate (§10).

Delivery rate and observability:
- The draft's sample with a minimum-RTT floor, not picoquic's 1 ms (§4).
- Structured log `quicly:bbr` instead of CSV; row built only while logging; no `cc_log` hook yet
  (§7, §11).
- libsdwan `quic_log`: full path, create-only, direct fd, stopped by `dup2` (§7, §8).
- Compact stats and counters; `quicly_cc_bbr_state_name` public for libsdwan's stats (§2, §7).

Process:
- Eight stacked PRs under the umbrella branch `bbr`, which carries this plan; fixes folded into the
  commit that introduced the code (§14).
- Testing, including a BBR smoke test, waits for phase 5 (§12, §13).

## 1. Goal and constraints

- Add BBRv3 as a quicly congestion controller, selectable by name as `bbr`, without changing the
  behaviour of quicly's existing controllers (reno, pico, cubic, cuback, cubic-legacy).
- The algorithm is picoquic's `bbr.c` (BBRv3), vendored unmodified. We write an adapter, not an
  algorithm.
- Upstreamability is a hard constraint: Fastly (quicly's maintainer) has no BBR plans, and the
  intent is to offer this upstream after internal validation. So: small, generic changes to quicly
  core; no picoquic types in quicly's public headers; no cost to non-BBR connections beyond what is
  measured and stated; no build-time `#ifdef` for BBR (quicly has no precedent for compiling out a
  controller).
- Ship default OFF in libsdwan (default is cubic-legacy), selectable at runtime, with a rollback
  path.
- Project style (both repos): C99 in quicly, snake_case, minimal comments wrapped at 100 columns,
  one declaration per statement, no speculative error handling, keep diffs to vendored or upstream
  code compact.

## 2. Architecture

```
quicly core (lib/quicly.c, sentmap, cc.h)      <- generic hooks, per-packet snapshots, delivery rate
        |  quicly_cc_type_t callbacks + BBR-only param structs
lib/cc-bbr.c  (the adapter)                    <- maps quicly callbacks onto picoquic's notify API
        |  picoquic_path_t / picoquic_per_ack_state_t
lib/picoquic-bbr.c (vendored picoquic bbr.c)   <- the algorithm, untouched
include/quicly/picoquic-bbr.h                  <- hand-written shim declaring only what bbr.c uses
```

Headers, split by audience:
- `include/quicly/picoquic-bbr.h`: picoquic types and constants copied verbatim (about 240 lines,
  includes only `<stdint.h>`/`<stddef.h>`). Private: included only by `picoquic-bbr.c` and
  `cc-bbr.c`. It must never enter the `quicly.h` include chain. A symbol a future `bbr.c` starts
  using is a compile error against this header, not a silent bug.
- `include/quicly/cc-bbr.h`: quicly-facing, picoquic-free. It is in the public chain because
  `cc.h` includes it, so it holds only what is used outside the adapter: the BBR-only per-ACK and
  per-loss param structs, `quicly_cc_bbr_log()`, and `QUICLY_CC_BBR_OBSERVED_STATE` with
  `quicly_cc_bbr_state_name()` (for libsdwan's stats, so it stays public and in PR 6 even though
  quicly's first caller is the PR 7 log). The state and notification enums, the
  other observe-word accessors, the log row, the notification names and the pacing-rate
  conversion are private to `cc-bbr.c`.

Every notification follows the same shape in the adapter: copy quicly state into picoquic's path
and per-ack structs, refresh the callback arguments the cc_common helpers read, call BBR, copy
`path.cwin` back into `cc->cwnd`, then update counters and the log row. Forgetting the read-back
is a silent no-op (BBR runs, nothing changes), the single most likely wiring failure.

## 3. Changes to quicly core

Each item is a separate, generic hook. Existing controllers leave every new field at zero/NULL.

1. **Sub-millisecond time for controllers.** quicly already computed fractional-ms time in
   `conn->stash.now_double` (loss detection and the sentmap used it) but handed controllers the
   truncated `int64_t`. `now` is now `double` on the five CC callbacks and `init_cc`; the existing
   controllers truncate it wherever they use it (on store and in cubic's elapsed time), so reno,
   pico, cubic and cubic-legacy are byte-identical to before in the seeded simulator; only BBR uses
   the fraction (decided 2026-10-21: no behaviour change for existing controllers). BBR's round
   timing, min_rtt filter and ProbeRTT thresholds are microsecond quantities. Throughput effect:
   none measurable (all deltas under the simulator's 0.59% seed-to-seed spread); it is a
   correctness change. Public API change for out-of-tree controllers. Jumpstart resume times A/B
   over 6 runs each: unchanged.
2. **`quicly_cc_t::pacing_rate`** (bytes/ms). When non-zero the pacer uses it instead of deriving
   a rate from cwnd/SRTT. BBR's model is "send at bw x pacing_gain"; without this its rate was
   computed and discarded.
3. **`cc_dispose`** (optional vtable slot), called from `quicly_free`, from `promote_path` before
   it re-initializes the controller, and from `quicly_set_cc` on a type change. `quicly_set_cc`
   switches first and disposes a copy of the old `quicly_cc_t` only if the switch succeeded, so a
   refused switch leaves the old controller intact. Its `old.type != cc` check is needed: a
   same-type switch returns 1 with the live state in place, and disposing it would free state in
   use (a review called the check redundant; dismissed). BBR keeps picoquic's state on the heap.
4. **`requires_pacing`** (type bit). The pacer is allocated when the context enables pacing or
   the controller requires it. The controller is built after the pacer, so `create_connection`
   finds the type by scanning `quicly_cc_all_types` for the `init_cc` factory (an unknown custom
   factory gets 0). Without it, an unpaced BBR left a mean bottleneck queue of 89801 B vs 77417 B.
5. **`wants_zero_byte_acks`** (type bit). `cc_on_acked` is also called for an ACK that newly
   acknowledges packets carrying no congestion-controlled bytes. BBR counts rounds by packet
   number and needs those ACKs. Still skipped when nothing is newly acknowledged: the send-time
   snapshots are valid only for a newly acked packet, and a delta against zero would hand BBR the
   whole connection's delivered total (about 100x overestimate into a max filter). The
   `QUICTRACE_CC_ACK` probe keeps its old condition.
6. **BBR-only parameter structs**, one trailing pointer argument each:
   `cc_on_acked(..., const quicly_cc_bbr_ack_params_t *)` and
   `cc_on_lost(..., const quicly_cc_bbr_loss_params_t *)`. quicly core always passes non-NULL
   (NULL for a CE report on loss); other controllers ignore it; tests pass NULL; BBR asserts it.
   - Ack params: `delivered_since_sent`, `inflight_prior`, `lost_since_sent`,
     `bandwidth_estimate`, `app_limited`, `ecn_counts[3]`.
   - Loss params: `delivered_since_sent`, `inflight_prior`, `lost_since_sent` (includes the lost
     packet itself), `app_limited`.
   - Why a trailing struct, not an all-in-one ack-state struct: the existing positional
     parameters and the pico/cubic bodies stay untouched (one signature line each), about a third
     of the upstream diff, and every BBR input since has been added as a field with no signature
     churn. Why named for BBR, not a generic "rate sample": no other rate-based controller is
     planned, and a generic name would promise a contract nobody else uses.
7. **Per-packet send-time snapshots** recorded by `quicly_sentmap_set_snapshot()`, called just before
   `quicly_sentmap_commit` so upstream's commit signature stays unchanged:
   - `delivered_prior` (low 32 bits of total bytes acked), `lost_prior` (low 32 bits of total bytes lost),
     `inflight_prior` (u32, bytes in flight before this packet), `app_limited` (1 spare bit),
     `delivered_time_prior_us` and `delivered_sent_time_prior_us` (low 32 bits of µs, for the
     delivery-rate estimator).
   - The newest newly acked packet's snapshots (late acks excluded) become deltas in the ack
     params; a lost packet's become the loss params. The 32-bit fields are subtracted in 32 bits,
     so wrap is harmless for one flight's worth.
   - Cost, paid by every connection: `quicly_sent_t` 32 -> 48 bytes, sentmap block 536 -> 792.
     `inflight_prior` sits in pre-existing padding. Deferred alternative: a per-entry trailing
     region sized per controller at connection creation (non-BBR pays zero). Rejected for now:
     the measured cost is smaller than first assumed, and the machinery is not justified while
     the BBR change is already large. `delivered_prior` was narrowed to u32 (saves 8 bytes of
     padding): its delta, the bytes acked during one packet's lifetime, stays exact below 4 GiB,
     3.4 s at 10 Gbit/s. Also rejected: `#ifdef` (mixed deployments still pay) and a
     packet-number-keyed side ring (must be sized to the in-flight window, re-derives a worse
     sentmap).
8. **App-limited** comes from `!quicly_ratemeter_is_cc_limited()`, which treats pacing and a full
   send buffer as our own limits. The cwnd/2 `cc_limited` heuristic was tried first and marked
   nearly every paced ACK app-limited, freezing startup. The same value is passed to `cc_on_sent`
   (trailing `int app_limited`, a public callback change) for BBR's restart-from-idle rule, along
   with `next_pn`, which a round started there records. Bulk transfers: 0-0.08% of ACKs
   app-limited, output unchanged; it matters for traffic with pauses (not yet measured).
9. **ECN counts.** The ack params carry cumulative ECT(0)/ECT(1)/CE including the current ACK;
   quicly updates its totals after calling the controller, so they are computed from the frame
   first. BBRv3 reacts when the CE share per round exceeds 20%; the same share ends startup. The
   zero-byte `cc_on_lost` (quicly's CE event, used by Cubic/Reno) is ignored by BBR, so
   `num_ecn_loss_episodes` is never counted for BBR. Simulated: classic CoDel and sparse marking
   unchanged; step marking 0.5 ms at 75 MB/s queue 0.55 -> 0.37 ms, CE -75%, throughput -4%;
   step 2 ms at 1 MB/s / 100 ms queue 6.46 -> 4.55 ms, throughput -13%. libsdwan sets and reads
   no ECN bits on its sockets, so in production the counts stay zero (layer 2, socket support,
   is a product decision; layer 3 needs marking paths).
10. **Delivery-rate estimator** (section 4).
11. **Three calls to `quicly_cc_bbr_log(conn, &conn->egress.cc)`**, after the sent, lost and
    acked callbacks, for the log point (section 7).

## 4. Delivery-rate estimation

BBR's bandwidth filter (`max_bw`) is a windowed max of one delivery-rate sample per ACK; pacing
rate, cwnd and BDP derive from it. Without a sample, picoquic's BBR divides bytes delivered by
the RTT. The RTT includes queueing, so that sample shrinks as the queue grows even when the path
delivers just as fast, and BBR reads a filling queue as a full pipe (before the fix, BBR on a
~560 Mbit/s path left startup at ~37 Mbit/s and re-entered startup five times).

Algorithm: draft-ietf-ccwg-bbr-06 §4.1.2. For each ACK, let P be the newest newly acked packet:

```c
uint32_t now_us = (uint32_t)(uint64_t)(conn->stash.now_double * 1000);
uint32_t sent_us = (uint32_t)(uint64_t)(largest_newly_acked.sent_at * 1000);
uint64_t bandwidth_estimate = 0;
if ((int32_t)(sent_us - conn->egress.delivery.delivered_sent_time_us) >= 0) {
    uint32_t received_over = now_us - largest_newly_acked.delivered_time_prior_us;
    uint32_t sent_over = sent_us - largest_newly_acked.delivered_sent_time_prior_us;
    uint32_t interval = sent_over > received_over ? sent_over : received_over;
    if (interval >= conn->egress.loss.rtt.minimum * 1000) {
        uint64_t delivered = (uint32_t)conn->super.stats.num_bytes.ack_received - largest_newly_acked.delivered_prior;
        bandwidth_estimate = delivered * 1000000 / interval;
    }
    conn->egress.delivery.delivered_sent_time_us = sent_us;
}
conn->egress.delivery.delivered_time_us = now_us;
```

- `delivered`: bytes acknowledged since P was sent. `received_over`: from the last ACK before P
  was sent to this ACK. `sent_over`: from sending the packet that ACK covered to sending P.
  Dividing by the longer interval gives the lower rate, which protects the max filter from
  bunched ACKs and from our own bursts. Both ends of the ACK interval went through the same
  queue, so the queue cancels.
- Skip ACKs of older packets (`IsNewestPacket`): no sample, reference not moved.
- Reference point moves on every ACK (`delivered_time_us` always, `delivered_sent_time_us` when P
  is newer). This is where we follow the draft rather than picoquic.
- Reset after idle: in `commit_send_packet`, if nothing is in flight, both reference times are set
  to now, so idle time is not measured. Not yet exercised by any test (bulk transfers only).
- **Minimum-RTT floor, not picoquic's 1 ms.** picoquic moves its reference only when it produces
  a sample, so a rejected interval keeps growing until it clears 1 ms. With the draft's rules the
  interval is about one RTT and never grows. An earlier version mixed the draft's idle reset with
  picoquic's 1 ms floor: on a sub-ms path (GCE lab, ~55 µs RTT) a slowly paced sender drained
  inflight before each send, every send reset the reference, every sample was under 1 ms and
  dropped, and BBR stayed at its handshake estimate (~3.5 Mbit/s). Fixed by using quicly's
  `loss.rtt.minimum` (updated from this ACK before sampling; `FLT_MAX` before the first RTT
  sample, so nothing passes). Lab A/B, 3 alternating runs: median 701 -> 2204 Mbit/s (cubic about
  2294), ACKs producing a sample 1-3% -> 100%; laptop internet path (~13 ms) 509 -> 520 Mbit/s;
  simulator byte-identical for every path of 5 ms RTT or more.
- 32-bit times: only differences are taken, valid for intervals under ~71 minutes (ordering test
  ~35 minutes). The conversion goes through `uint64_t` on purpose: converting an out-of-range
  double straight to `uint32_t` is undefined; x86-64 keeps the low bits, arm64 saturates to
  0xFFFFFFFF, which would make every interval zero and silently disable the estimator. The arm64
  case is reasoned from the C standard, not run on Apple hardware.
- The adapter copies a non-zero sample into picoquic's `path.bandwidth_estimate`; zero keeps the
  previous estimate.

Comparison:

|                       | draft §4.1.2                    | picoquic                    | quicly                          |
|-----------------------|---------------------------------|-----------------------------|---------------------------------|
| bytes counted from    | total delivered when P was sent | total at the reference ACK  | total delivered when P was sent |
| reference point moves | every ACK                       | only on a sample            | every ACK                       |
| minimum interval      | min_rtt                         | 1 ms                        | quicly's minimum RTT            |
| reset after idle      | yes                             | no                          | yes                             |
| time width            | unspecified                     | 64-bit µs                   | low 32 bits of µs               |

Measured (simulator, seeded): 1 MB/s / 100 ms path standing queue 23.5 -> 4.2 ms, throughput
unchanged; 75 MB/s / 10 ms clean or 1% loss unchanged; ACKs batched every ~1 ms (like our tunnel)
throughput -1%, queue -0.3 ms. ACK-stall sensitivity (throughput MB/s, without / with):

| stalls         | without | with        |
|----------------|---------|-------------|
| 1 ms, 5% / 20% | 73.4    | 72.9 / 72.5 |
| 2 ms, 20%      | 50.8    | 51.3        |
| 4 ms, 5%       | 33.2    | 32.0        |
| 4 ms, 20%      | 25.0    | 13.2        |

Stalls of 2 ms or more cost BBR 30-55% either way (startup re-entered ~10 times); only the extreme
case is worse with the estimator, because the send interval spans the stalls. On a real tunnel the
samples agree with the raw delivered counter.

## 5. The adapter (`lib/cc-bbr.c`)

| quicly callback  | picoquic notification | notes                                                         |
|------------------|-----------------------|---------------------------------------------------------------|
| `cc_on_acked`    | acknowledgement       | snapshots, app-limited, rate sample, ECN counts               |
| `cc_on_lost`     | repeat                | lost packet's snapshots; a CE report (bytes == 0) is ignored. Bytes and loss ranges also go on the next ACK's notification |
| `cc_on_sent`     | restart_from_idle     | nothing else in flight and the sender app-limited (draft rule; picoquic also waits ~1 s) |
| `cc_on_late_ack` | (NULL)                | picoquic undoes a spurious loss only in PTO recovery, which needs the timeout notification we never send |
| `cc_dispose`     | `alg_delete`          | then frees the wrapper; without it each connection leaked 840 B |

- **Per-connection wrapper** (`quicly_cc_bbr_conn_t`, heap): `picoquic_path_t` (must be first:
  `conn_of()` casts the path pointer back), zeroed `picoquic_cnx_t` and `picoquic_quic_t`
  (`BBRInitRandom` dereferences `cnx->quic->use_predictable_random` and `cnx->client_mode`), the
  callback arguments, a back pointer to `quicly_cc_t`, the latest send quantum, and the latest
  log row plus a "new since read" flag.
- **Path fields BBR owns** are never re-synced after init (re-syncing wipes the state machine):
  `cwin`, `rtt_min`, `is_cca_probing_up`, `is_cc_data_updated`, `is_ssthresh_initialized`,
  `bandwidth_estimate_max`, `congestion_alg_state`. Inputs synced each call: `bytes_in_transit`,
  `delivered` (running total), `rtt_sample` (µs), `smoothed_rtt`, `rtt_variant`, `send_mtu`,
  `bandwidth_estimate` (from the sample), ECN totals. `nb_retransmit` is left at 0: BBR reads it
  only in PTO recovery, which is never entered without the timeout notification.
- **cc_common helpers** bbr.c links against: `picoquic_cc_get_sequence_number` /
  `_get_ack_number` read the current callback's `next_pn` and largest-acked (round counting
  depends on these; garbage here makes BBR sit in one state forever). A loss keeps the last real
  largest-acked value. `hystart_test`, `hystart_loss_volume_test`, `slow_start_increase` and
  `picoquic_update_pacing_data` serve only `startup_long_rtt` and are asserting stubs. They do not
  `abort()`: under NDEBUG they return 0, which stalls that startup, and in the field a stalled
  startup is better than a crash. The invariant is written next to `rtt_min = 0`.
- **Pacing output** leaves through `picoquic_update_pacing_rate()`, which the adapter implements:
  `cc->pacing_rate = quicly_cc_bbr_calc_pacer_rate(rate)` (bytes/s double -> bytes/ms; a positive
  rate never rounds to 0, which would mean "no rate"; saturates instead of wrapping) and stores
  the send quantum. `picoquic_update_pacing_data()` is a stub (only `startup_long_rtt` uses it).
- **PRNG and start time.** splitmix64 plus rejection-sampled uniform, copied from picoquic's
  util.c. A stub returning 0 pinned the ProbeBW wait to its minimum so BBR re-probed constantly.
  `alg_init` gets the real creation time: it seeds the PRNG (competing flows must not probe in
  lockstep) and starts the min_rtt/ProbeRTT/extra_acked timers, which look long expired at zero.
  Simulated at 1 MB/s / 100 ms: mean queue 77 -> 51 KB, standing queue 79 -> 51 ms, inflight
  1.81 -> 1.48 BDP. Still open: `BBRInitRandom` also mixes in `client_mode` and
  `unique_path_id`, both 0 in our wrapper, so client and server seeds coincide when start times
  match.
- **Option "R"** passed to `alg_init` turns off picoquic's rapid-start probing (section 6).
- **State and counters** in `cc->state.bbr` (kept minimal, read by libsdwan): wrapper pointer,
  latest observe word `cc_state` (state via `QUICLY_CC_BBR_OBSERVED_STATE`), `bw`, `startups`
  (including the first), `probe_rtts`. The generic `quicly_cc_t` counters are filled in too:
  cwnd min/max, `num_loss_episodes` (in-recovery edges; BBR never undoes one), slow-start exit (filled_pipe
  edge).
- **Switching.** BBR is in `quicly_cc_all_types`. pico family and cubic-legacy accept BBR as a
  source and reset to their initial state; `quicly_set_cc` then frees BBR's state. BBR's own
  `cc_switch` refuses a switch into BBR on a live connection (it would need a clock and a pacer
  that `cc_switch` cannot provide); a refused switch leaves the current controller in place.
  libsdwan never switches a live connection; it restarts connections instead (section 8).
- **Traps** that fail silently: cwnd read-back omitted; zero-byte ACKs gated off (rounds never
  advance); BBR-owned path fields re-synced; `path.cnx` NULL (crash at init, broken rounds); RTT
  quantized to ms; `app_limited` hardcoded; no pacer (rate discarded, BBR runs window-only).

## 6. Vendored picoquic

- `lib/picoquic-bbr.c` is picoquic `picoquic/bbr.c` at upstream `afdf5f27`, byte-identical except
  the include block (three picoquic includes replaced by `#include "quicly/picoquic-bbr.h"`) and
  the two RTT checks below (PR 6).
  Verify: `diff <(git -C ~/dev/picoquic show afdf5f27:picoquic/bbr.c) lib/picoquic-bbr.c`.
- Two RTT-based checks are turned off by commenting out their `#define`s:
  `RTTJitterBufferStartup` (Startup exit on RTT rise) and `RTTJitterBufferProbe`
  (`BBRCheckPathSaturated`, which resets the bandwidth estimate and enters Drain when RTT > 2x
  min_rtt while pacing > 3x the delivery rate). `IsRTTTooHigh` is guarded with its only caller.
  Evidence in §9. Neither check is in the BBR draft.
- Re-sync: copy the new `bbr.c`, re-apply the include edit and the two `#define` edits, rebuild;
  new symbols fail to compile against the shim. In libsdwan, quicly's own build dir must rebuild
  (`lab.sh build` / the `quic-build` target); `--target node` alone does not re-run quicly's build.
- `afdf5f27` includes picoquic PR #2265, which fixed (a) `rounds_since_bw_probe` never
  incremented (Reno-coexistence probe dead; after the fix BBR probes about every 63 rounds),
  (b) experiment-flag macros selected before `BBRExperiment` was defined (all options ignored),
  (d) our issue #2264, ProbeRTT starvation: `BBROnExitRecovery` reset the ProbeRTT and min_rtt
  timers on every recovery exit, so on a lossy path ProbeRTT never ran and min_rtt froze after
  ~1 s, plus a ProbeRTT margin 100x too small, the initial window in the wrong unit, and a
  rate/bytes mix-up in startup-resume.
- Possibly upstream, unverified: `sent_cwin_limited` (`sender.c:888-891`) is tested before the
  packet joins `bytes_in_transit`, so with sends gated on `bytes_in_transit < cwin` it is almost
  never set and `BBRProbeInflightHiUpward` rarely runs.
- Still open upstream, **unreported, ours to file**: (c) the rapid-start latch.
  `probe_probe_bw_quickly` has four set sites and one clear site, and the clear sits inside the
  DOWN -> CRUISE branch that its own fast probing prevents reaching. We avoid it with option "R".
- We compile the `RTTJitterBuffer` variants (`#define`d on in bbr.c), not the draft-matching
  `#else` code. The margin branch in `BBRUpdateMinRTT` resets the ProbeRTT timer when the
  7-sample jitter-buffer min is within 5% of min_rtt; in the simulator's fixed delay it fires on
  almost every ACK, so **the simulator never shows ProbeRTT**. On the real tunnel ProbeRTT ran
  once in 20 s.
- Minor, not reported: `BBRCheckProbeRTT` sets min_rtt to the current queued sample on entry.

Experiment flags (the option string; all default on, each letter turns one off):

| letter | flag                             | effect                                                     | for us                                 |
|--------|----------------------------------|------------------------------------------------------------|----------------------------------------|
| E      | `do_early_exit`                  | none: never read                                           | irrelevant                             |
| R      | `do_rapid_start`                 | re-probe after min_rtt + up to 250 ms instead of 2-3 s     | **off**: latches (bug c)               |
| H      | `do_handle_suspension`           | re-enter startup when PTO recovery ends                    | inert: no timeout notification sent    |
| L      | `do_control_lost`                | hold cwnd on "lost feedback"                               | inert: never notified                  |
| D      | `do_exit_probeBW_up_on_delay`    | leave ProbeBW-UP on RTT rise or inflight > 1.25 BDP        | on; gated off below 128 µs min_rtt     |
| A      | `do_enter_probeBW_after_limited` | probe as soon as an app-limited period (>3 rounds) ends    | on; suits bursty request/response      |

Numeric options: `T` Wi-Fi shadow RTT (off), `Q` send-quantum ratio (default 0.001 s), `S` test
seed. Keep "R" alone. D and A have not been measured in isolation. With "R": simulator cruise
1.3% -> 69% of the run, standing queue 51 -> 26 ms, inflight 1.20 BDP, throughput unchanged
(measured on a constant-rate link; rapid start targets variable-rate paths, retest there).

## 7. Observability

**quicly side: the `quicly:bbr` event.** One event per BBR notification, emitted through picotls's
structured log (`ptls_log`), the mechanism quicly's own events use. `quicly_cc_bbr_log()` returns
immediately for other controllers and when BBR was not notified by that callback (cc_on_sent and
CE reports usually return early, so no stale rows). `QUICLY_LOG_CONN` reads quicly.c's private
connection state, so the adapter follows picotls's `PTLS_LOG_CONN` pattern through
`quicly_get_tls(conn)` and `PTLS_LOG__DO_LOG`; the event carries `conn` and `conn_id` like quicly's
events, so it joins with them. Fields: `now_us`, `notification`, `bytes_in_transit`, `delivered`,
`rtt_sample_us`, `smoothed_rtt_us`, `bytes_acked`, `bytes_lost`, `delivered_since_sent`,
`lost_since_sent`, `inflight_prior`, `is_app_limited`, `cwnd`, `pacing_rate`, `send_quantum`,
`state`, `ack_phase`, `filled_pipe`, `in_recovery`, `packet_conservation`,
`idle_restart`, `bw`, `bw_estimate`. The old per-connection CSV and its env vars are gone.

**How picotls logging works.** Log points are function-local statics named `module:name`, each
caching a 32-bit bitmap of interested sinks. Sinks are fds registered with
`ptls_log_add_fd(fd, sample_ratio, points, snis, addresses, appdata)` into one of 32 slots, each
with filters (event names, SNI, peer address, sampling ratio, appdata). Each TLS connection has a
log state (peer address set by quicly at creation, random sampling value, cached bitmap). A global
generation counter bumps on sink changes; bitmaps recompute lazily. With no interested sink a log
point costs one cached check. Events are built in a thread-local buffer and written with one
`write()` per sink under a global mutex, on the thread that logs (the network thread). `EAGAIN`
drops the event and counts it; any other write error closes the fd and frees the slot, the only
way a sink is removed. Compiled out on Windows (`PTLS_HAVE_LOG` is 0 when `_WINDOWS` is defined).
Quirk: at write time picotls re-applies the per-connection filter only when the connection's
cached state is stale, so with two sinks registered a peer-filtered sink can receive other
connections' events (same code upstream; one stray event per capture restart in our usage).

**libsdwan: `command quic_log`** (cmd interface, enabled by setting in the field):
`start <path> [points=a,b] [peer=addr,...]`, `stop`, `status`. One capture at a time.
- picotls writes straight to the file's fd from the network thread. Stop swaps a read-only
  `/dev/null` in at the same fd number with `dup2`; picotls's next write fails with EBADF and it
  drops the sink. This replaced a socketpair plus reader thread (rejected as too complex). Costs
  accepted: disk writes on the network thread inside picotls's mutex (normally page cache); no
  size cap yet; `events_lost` meaningless for regular files.
- The cmd interface is unauthenticated and the client may run as root, so the file is created
  only: `O_CREAT|O_EXCL|O_NOFOLLOW`, mode 0600. Existing files and symlinks are refused. A
  storage-directory-only design was rejected (product hosts may not configure one); streaming
  over the cmd connection was considered and not needed.
- Peer addresses picotls cannot parse would silently widen the capture to every peer, so they are
  rejected. Unfiltered captures run about 35 MB/s at ~1 Gbit/s; `points=quicly:bbr` about
  11 MB/s.
- Usage text lists example points: `quicly:bbr`, `quicly:packet_lost`, `quicly:cc_congestion`,
  `quicly:cc_ack_received`, `quicly:packet_sent`, `quicly:packet_acked`.

**libsdwan: production stats and counters** (cmd interface `stats` / `counters`, from
`quicly_get_stats`). Kept deliberately small; deep debugging uses captures.
- stats, every connection (existing): cwnd, ssthresh, cwnd min/max/initial, slow-start exit,
  loss-episode undo counts, `cc.type`, RTT smoothed/latest, delivery rate; (new) `rtt.minimum`,
  `cc.pacing_rate`. BBR connections (new): `cc.bbr.state` (name), `cc.bbr.bw`.
- counters (new, BBR only): `cc.bbr.startups`, `cc.bbr.probe_rtts`. They catch repeated startup
  (already visible: 5 startups in a 3 s upload) and ProbeRTT never running.
- Dropped as not useful in a snapshot: per-state entry counts, per-notification counts, ACK and
  app-limited counts, ack-phase changes, PTO-recovery and idle-restart counts (always zero
  today), peak bw, send quantum, `rtt.variance`, ECN episodes (libsdwan has no ECN).

## 8. libsdwan integration

- Controller selection precedence: compiled default `cubic-legacy` < SD
  `[client|connector].config.transports.quic.congestion_control` (`CUBIC`, `CUBIC_LEGACY`, `BBR`,
  mapped to quicly names) < env `TWINGATE_QUIC_CC` (quicly names only; `BBR` there falls back to
  the default with a log line) < cmd-interface `set_congestion_control <name>`.
- `set_congestion_control` restarts live connections (close + normal reconnect) so each comes back
  born with the new controller; it never calls `quicly_set_cc` in place. A connection's
  controller is fixed for its lifetime, which is also what a future per-controller sentmap layout
  would rely on. Rejected names leave live connections alone.
- libsdwan pins quicly as a submodule; its recorded pointer is bumped only when quicly is pushed.
- `node` ignores SIGPIPE; other hosts are not relied on for that.

## 9. Measured behaviour

Simulator baseline that exposed the core problem (1 MB/s, 100 ms min RTT, 100 KB tail-drop queue,
seed 12345, 20 s): standing queue ~79 ms, inflight 1.8x BDP, cruise 1.3% of samples. Root cause
was the rapid-start latch plus the zero PRNG pinning the probe wait to exactly min_rtt
(DOWN -> REFILL -> UP, ~100 ms each, bypassing CRUISE). Ruled out: pacing, the loss signal (no
loss at all with a 200 KB buffer and the queue still ~80 KB), time granularity.

| change                            | standing queue | inflight / BDP | cruise |
|-----------------------------------|----------------|----------------|--------|
| baseline                          | 79 ms          | 1.81           | 1.3%   |
| real PRNG + start time            | 51 ms          | 1.48           | low    |
| option "R"                        | 26 ms          | 1.20           | 69%    |

The delivery-rate estimator, added later on the same path, cut the standing queue again from
23.5 to 4.2 ms with throughput unchanged.

Loss inputs: `lost_since_sent` on ACKs revived `IsInflightTooHigh` (inflight_hi cap, startup exit
on loss, recovery). Clean path: byte-identical (a cruising BBR rarely meets the 2% gate). 2% random
loss: mean queue 20267 -> 15339 B, throughput +1.4%. The loss-path snapshots (lost packet) added a
small effect: 2% loss at 1 MB/s / 100 ms queue 4.02 -> 3.23 ms, cruise 85 -> 87%.

Real path (laptop wired GbE -> GCP connector, iperf3 single stream): 510-570 Mbit/s, base RTT
8-10 ms, ~0.4% loss, standing queue 5-7 ms, inflight 1.3-1.6x BDP, cruise 58-79%. `max_bw` sits
15-20% above mean delivery (the path delivers 66-84 MB/s in 5-20 ms windows and the max filter
latches the top; load-sensitive: ~75 MB/s at 1-5 ms queue vs ~69 at 8-10 ms). What bounds the
queue is `inflight_hi`, set by early losses, differing per run. Run-to-run variance is large; only
compare alternated A/B pairs from the same binary.

Phase 5, same path (2026-10-23/24). The link is a Verizon Fios upload; ping to the first ISP hop
showed the queue forms between the home router and that hop. It allocates upload capacity on
demand: each rate step queues for 20-40 ms even at 100-300 Mbit/s, and after ~30 s idle delivery
ramps from near 0 to full rate over ~400 ms (RTT up to 100-118 ms). It also bursts to ~790 Mbit/s
for ~50 ms (~1.4 MB above the ~580 sustained rate) and loses 0.3-0.4% even with no queue.
- **Loss fix.** The adapter passed 0 newly lost bytes on ACKs, so `loss_in_round` never set and
  `bw_lo` / `inflight_lo` stayed unset; with the Startup exit off, cwnd held ~2.2 MB and a 20 ms
  standing queue. Carrying losses to the next ACK: cruise RTT 25-30 -> 9-10 ms, cwnd ~0.75 MB.
- **RTT-based Startup exit off.** It fired on every rate step's delay: 5 Startups, first second
  116-218 Mbit/s. Off (with the loss fix): one Startup, first second 368-446, total 518-541 vs
  366-490, loss 0.36-0.43% vs 0.30-0.39%. Startup builds a ~21 ms RTT queue for ~0.5 s and ends
  on the plateau.
- **Saturation check off.** After idle it reset `bw` to 20-67 Mbit/s mid-ramp. First second after
  30 s idle, five runs each: median 352 vs 152 Mbit/s. Without it, the loss-based lower bounds
  still hold ~300 Mbit/s for ~0.5 s after the ramp until the next probe.
- ProbeRTT rarely runs once the queue is gone: picoquic's margin (5% of min_rtt plus two packets)
  keeps postponing it while RTT stays near the minimum.

GCE lab (client, relay, connector in one zone, ~55 µs RTT): BBR ~2.2-2.6 Gbit/s upload, cubic
~2.3; the client CPU is the limit (inflight 40-65 KB vs cwnd 160-330 KB).

## 10. Decisions not adopted (with evidence)

- **PTO/timeout notification** (`cc_on_pto` slot calling picoquic's `timeout`). quicly follows
  RFC 9002 (a PTO does not mark packets lost; persistent congestion is implemented for no
  controller); picoquic treats a PTO as a loss. Measured: simulator unchanged without outages;
  real-tunnel 2.2-2.7 s iptables outage: back to 90% after the first ACK in 0.36 s (off) vs
  0.37-0.40 s (hook), peak RTT and post-outage RTT no better, and startup re-entry leaves a deeper
  queue. A vtable slot for no gain. If ever added, pass option H. Untested: repeated short
  suspensions (Wi-Fi power save), which picoquic's suspension handling targets.
- **Cruise pacing margin** (pace at 0.90 x max_bw): `max_bw` decayed and throughput spiralled to
  84 Mbit/s. The overestimate can only be fixed on the input side.
- **Smaller send quantum plus removing the 128 µs ProbeBW-UP gate.** quicly's pacer bursts 8-10
  packets and ignores BBR's quantum (up to 64 KB), so BBR's 3 x quantum floor reserves window for
  bursts that never happen. Simulator, 2 Gbit/s / 30 µs: either change alone did nothing (smaller
  quantum alone was worse), both together UP 32%-then-stuck -> 2.3%, RTT 793 -> 245 µs, cwnd
  199 -> 62 KB, throughput -0.7%; 10 ms path unchanged. Lab: `Q0.00005` UP 100% -> 55-67%,
  throughput possibly -2%; `Q0.00002` UP 13% / 7.8% / 99.7% (one run stuck), throughput noise,
  RTT unchanged (CPU-bound). Not adopted: unreliable, no visible gain, needs a vendored change.
  Likely remaining blocker: the `max_bw` overestimate. Alternative not tried: make the pacer
  honour BBR's send quantum.
- **Per-entry trailing sentmap region** (section 3.7): deferred.
- **`#ifdef` for BBR**: no; always compiled, runtime-selected, like every other controller.
- **Compile-time check of the state enum against picoquic's**: impossible without editing the
  vendored file (picoquic's enum is private to bbr.c). Numeric mirror kept; values must be checked
  by hand at each re-sync, as the comment in cc-bbr.h now says.
- **Generic "rate sample" naming for the param structs** (suggested in review): kept BBR naming.
- **Allocating the pacer after `init_cc`** (instead of finding `requires_pacing` by scanning
  `quicly_cc_all_types` for the factory): would remove `cc_requires_pacing()` and cover custom
  `init_cc` factories, but either adds a new unwind path to `create_connection` or allocates a pacer
  for every connection. Not done, to avoid changing upstream connection setup; every in-tree factory
  and libsdwan work today. If an upstream reviewer objects to the scan, offer the unconditional
  early allocation (freed after `init_cc` when not needed).

## 11. Review outcomes and open issues

From an adversarial four-model review of the whole stack, verified, not yet fixed:
1. **[FIXED] Restart-from-idle never fired.** quicly adds the packet to bytes in flight before
   calling `cc_on_sent`, so the adapter never saw zero. Fixed by treating "this packet is the only
   one in flight" as idle and passing the packet's app-limited flag to `cc_on_sent` (new trailing
   parameter), so the draft's condition applies: without it, a short path's pacer drains the
   flight between packets and every send looked like a restart (500+ false restarts in 8 s on
   loopback). Now one restart per pause, none mid-transfer. A restart that ends ProbeRTT
   starts a round; `cc_on_sent` now carries `next_pn` for it (it used to reuse the last ACK's).
2. **[FIXED] `quicly_set_cc` disposed before knowing the switch worked.** It now keeps a copy of
   `egress.cc`, switches, and disposes the copy only on success. A test switching a live
   connection to BBR, to a refusing controller, then to pico segfaulted before and passes now.
3. **[FIXED] Late ACKs over-undid loss episodes.** picoquic undoes a spurious loss only in PTO
   recovery, which the adapter never enters, so BBR never undoes an episode. The late-ack handler
   was a no-op and is gone (`cc_on_late_ack` is NULL), and `num_loss_episodes` counts the episodes
   BBR reacted to. (Round 2 found the first fix, an undo guarded on leaving recovery, could never
   run.)
4. **[FIXED] cc-bbr.h claimed a compile-time enum check that does not exist.** The comment now says
   the values are checked by hand at each re-sync.

From the round-2 review (2026-10-20):
- [FIXED] Path promotion re-initialized BBR without disposing it: debug builds aborted on the
  init assert, release builds leaked the state on every migration. `promote_path` now calls
  `cc_dispose` first and passes `now_double`. Round 3 (2026-10-21) found the snapshot deltas still
  came from the connection-wide stats counters, which keep counting the old path; they now come
  from per-path totals in `egress.delivery`, counted where the controller is told of acks and
  losses and reset by `promote_path`, which also replaced the adapter clamp.
- [FIXED] An ACK of only late-acked packets reached BBR as a delivery sample, counting the packet's
  own (spurious, already reported) loss in `lost_since_sent`. The snapshot now comes from
  `largest_cc_acked`, the newest newly acked packet that was not a late ack, which also gates the
  zero-byte call.
- [FIXED] The late-ack handler was a no-op (BBR undoes a loss only in PTO recovery) and is gone.
- [DONE] The CC's ack inputs moved out of `handle_ack_frame` into static helpers
  (`sample_delivery_rate`, `ecn_counts_with_ack`, `delivered_since_sent` / `lost_since_sent`, shared
  with the loss path, and `delivery_time_us`); `largest_cc_acked` is a `quicly_sent_packet_t` copy.
- [DONE] The log sample is built only while the `quicly:bbr` log point is on. Not done: an optional
  `cc_log` vtable hook in place of the three `quicly_cc_bbr_log` calls in quicly.c (an upstream
  reviewer would likely ask for it).
- [DONE] The helpers only `startup_long_rtt` calls (Hystart tests, slow-start increase, pacing
  data) are asserting stubs; `filter_rtt_min_max` and `cb_args.is_cwnd_limited` are gone.

Review items to consider:
- [FIXED] Switching *into* BBR mid-connection: BBR now refuses it (start time 0, no pacer).
- [DECIDED: not doing] Duplicate ECN delta logic in `handle_ack_frame` (precomputed counts vs the
  existing update). Removing it reorders upstream code; both paths add the same deltas on every
  ACK the controller sees, and libsdwan does not use ECN.
- [FIXED] `startup_long_rtt` trap on paths over 250 ms: `rtt_min` is written only when ProbeRTT
  exits, so a ProbeRTT before the pipe fills (an app-limited connection still in startup) could
  enter that state, where the no-op `picoquic_update_pacing_data` and unfilled timestamps freeze
  pacing and the window. The adapter resets `rtt_min` to 0 after each notification, so long paths
  always use BBR's own startup. Making the state work instead would mean filling those inputs.
- Sentmap memory for every controller (section 3.7).

Algorithm behaviour:
- **`max_bw` overestimate** on load-sensitive paths (section 9). Deferred; a fix changes what
  feeds the filter (quantile, excluding margin-limited samples).
- **ProbeBW-UP stuck below 128 µs min RTT**: 94-100% of the time in the lab, ~1% on the internet
  path, reproducible in the simulator (`-d 0.00003`). Two blockers in picoquic's UP exit: the
  whole check is gated on `min_rtt > 128 µs`, and the inflight test compares against
  `BBRInflightWithBw(1.25)`, which `BBRQuantizationBudget` raises to 3 x send quantum, equal to
  cwnd when the BDP is tiny. draft-06 exits UP on a bandwidth plateau instead. Throughput is
  unaffected; risks if the network is the bottleneck on such a path: pacing stays at 1.25 x
  max_bw, max_bw cannot decay, unfair to cubic. Deferred; consider reporting upstream.
- Resolved: **Startup re-entered several times** on the internet path. The link's capacity
  allocation delay tripped picoquic's RTT-based Startup exit; it is now off (§6, §9).
- **ACK stalls of 2 ms or more** cost BBR 30-55% (section 4); a userspace connector under CPU load
  could stall that long.
- `BBRInitRandom` seeds coincide for client and server when start times match (section 5).

Decided, not doing (round 2):
- Exported BBR symbols: 17 carry a `picoquic_` or `BBR` prefix and can clash only with picoquic,
  which libsdwan does not link; if it ever does, the `picoquic_cc_*` shims would collide. The
  generic `update_windowed_max_filter` / `start_windowed_max_filter_period` are kept too.
- Repeated RTT sample on ACKs that carry none (zero-byte ACKs, gap fills): BBR gets the previous
  `rtt.latest`, as picoquic's BBR gets `path_x->rtt_sample`. It is a real recent measurement;
  passing 0 instead would read as a 0 µs RTT and could set min_rtt to 0.
- `is_cwnd_limited`: BBR gets quicly's `cc_limited` (in flight >= cwnd/2), not "this packet filled
  cwnd". Only `BBRProbeInflightHiUpward` reads it; it differs only in ProbeBW_UP with partly
  app-limited traffic, and the extra `inflight_hi` growth is corrected on loss. picoquic's own
  `sent_cwin_limited` is checked before the packet joins `bytes_in_transit` and it only sends below
  cwin, so it is almost never set (read, not run; candidate upstream report). If revisited: a
  `cwnd_full` snapshot bit (in flight including the packet >= cwnd), ~12 lines in PRs 4 and 6.

Observability follow-ups:
- An env var to start a capture at process start (local runs), and a JSON-lines-to-CSV script.
- Optional capture size cap (a 1 s timer checking file size).
- Windows: `quic_event_log.cpp` is guarded by `PTLS_HAVE_LOG`, which depends on `_WINDOWS`; if the
  Windows build defines only `_WIN32`, the POSIX code would fail to compile. Not built for
  Windows yet.

## 12. Testing

- quicly has no BBR unit tests; all existing tests pass and the existing suite covers the
  changed generic paths only indirectly. BBR tests are planned for the testing phase.
- Why no test caught the core problems: every assertion bounds completion time or throughput, and
  these defects cost neither; they only park data in the queue. picoquic's ~30 BBR tests passed
  with four real bugs present. Add assertions on standing queue, inflight-vs-BDP and cruise
  occupancy on a long single BBR flow; healthy vs broken numbers are far apart (26 vs 79 ms queue,
  69% vs 1.3% cruise, 1.20 vs 1.81 BDP). A ten-line `cc_on_sent` test would have caught open
  issue 1.
- Simulator: `t/simulator.c`, e.g.
  `QUICLY_SIM_SEED=1 ./simulator -b 10000000 -d 0.01 -l 10 -- -c bbr`. `QUICLY_SIM_SEED` makes runs
  byte-identical (TLS randomness also routed through it), the basis of every "byte-identical"
  claim here. Limits: fixed delay means ProbeRTT never shows; long `-a` stalls drive BBR to low
  utilisation (use ~1 ms to mimic the tunnel); multi-flow runs with `-s` stagger assert.
- Real runs: libsdwan's verify skill drives the headless client in the GCE perf lab or locally
  (`lab.sh build|deploy|launch|iperf|stats|collect|cleanup`, `--local` for this machine). Never
  restart the lab's connector or relay. Use at least 3 alternating A/B rounds per arm.
- quicly's `cli` with `-C bbr -e <file>` is the quickest end-to-end check of the log point.
- Known flaky tests: e2e `jumpstart-resume` (needs the resumed fetch within 0.2999 s; occasional
  +25 ms = max ACK delay); `lossy` (`t/lossy.c:408`, `bidi-stats-75%`, about 1 run in 9; default
  controller, not BBR). Rerun before suspecting a change.
- Still to run: netem matrix (loss, delay, reorder, variable bandwidth, buffer depth, high BDP);
  BBR-vs-BBR and BBR-vs-Cubic fairness; real-path A/B vs Cubic on relay and direct paths,
  including a lossy path; ASan/leak under connection churn and CC restarts; mobile, Wi-Fi
  handoff, satellite; traffic with pauses (app-limited, idle reset, restart-from-idle).

## 13. Phases

1. Re-integrate on current quicly: done.
2. Structural hooks (vtable params, sub-ms time, pacing): done; trailing sentmap region deferred.
3. Protocol correctness (rate estimate, loss inputs, app-limited, zero-byte ACKs, PRNG, ECN layer
   1, ProbeRTT fix via upstream sync): done; PTO hook not adopted.
4. Hardening: experiment flags settled; observability done (log point, capture command, stats);
   three review rounds addressed (section 11). Remaining: capture env var and JSON-to-CSV script,
   verify-skill docs.
5. Testing and validation (section 12), including BBR unit tests.
6. Rollout: default off; internal dogfood; opt-in customers on relay and direct paths; staged
   rollout with go/no-go metrics (throughput vs Cubic, loss, RTT inflation, fairness,
   connection failures); then the upstream quicly PR.

## 14. PR organization

The quicly work is reviewed as a GitHub stack (stacked pull requests, `gh stack`) on top of an
umbrella branch:

```
umbrella PR:  bbr  ->  libsdwan-quicly-2026-09-29
stack:        (bbr) <- bbr-1-simulator <- bbr-2-cc-time <- ... <- bbr-8-delivery-rate
```

- `bbr` carries one commit of its own, this plan, so the umbrella PR has content from the start.
  Its description is `BBR_UMBRELLA_PR.md`: overview, the Notion spec link, the sub-PRs and the
  project-wide decisions.
- Each layer is one sub-PR whose description (in `BBR_PRS.md`) ends with the decisions about its
  own code:

1. `bbr-1-simulator`: `QUICLY_SIM_SEED` and freeing sender connections at exit.
2. `bbr-2-cc-time`: sub-ms time for controllers.
3. `bbr-3-cc-extensions`: pacing rate, `cc_dispose`, `requires_pacing`, zero-byte ACKs.
4. `bbr-4-delivery-snapshots`: ACK snapshots and params, loss params, app-limited, ECN counts,
   per-path delivery counters.
5. `bbr-5-vendor-picoquic`: `picoquic-bbr.c` and the shim header, not built.
6. `bbr-6-controller`: the adapter with its minimal state block (unregistered), then registration
   plus switching from BBR.
7. `bbr-7-logging`: the `quicly:bbr` log point.
8. `bbr-8-delivery-rate`: the estimator with the minimum-RTT floor, then its `bw_estimate` log
   field.

Working rules:
- Development happens on `bbr-dev`; each fix is folded into the stack commit that introduced the
  code it touches, and every later layer is rebuilt. The stack's tip stays byte-identical to
  `bbr-dev` minus this plan.
- Restacking is local; publishing is pushing every layer (`gh stack push`, remote `origin`; the
  repo also has h2o's `upstream`). Once layers start merging, GitHub rebases the ones above, so
  fetch (`gh stack sync`) before any further local restack.
- Land the stack into `bbr` with `gh stack merge`, then merge the umbrella into the base.
- The libsdwan side (CC selection, `set_congestion_control`, `quic_log`, stats/counters) goes in its
  own libsdwan PR, which bumps the quicly submodule once quicly is pushed.
