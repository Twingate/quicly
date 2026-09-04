Headers vendored (not a submodule — plain copies, per BBR_POC_PLAN.md §8) from:

    https://github.com/private-octopus/picoquic
    commit 6db7f43
    path: picoquic/{picoquic_internal.h,picoquic.h,picoquic_utils.h,cc_common.h,picohash.h,picosplay.h}

Lives under `lib/`, not `deps/`: every other subdirectory of `deps/` (`klib`,
`picotest`, `picotls`) is a real git submodule, and this deliberately is not
one — a full picoquic submodule would pull in an entire independent QUIC
stack (its own transport, other CC algorithms, sample apps, fuzzers) to reach
the ~2600 lines actually used here, which cuts against this being a temporary,
minimal POC integration (BBR_POC_PLAN.md §0/§12). Putting a non-submodule
directory under `deps/` would misrepresent it as one, so it sits here instead.

`bbr.c` in this directory is the same picoquic `bbr.c`, also unmodified and
vendored for the same reason — it lives here rather than loose in `lib/`
because it's vendored code too, not authored for quicly. Only `bbr.c` itself
and `lib/cc-bbr-common.c` (quicly's replacement for picoquic's `cc_common.c`,
which stays in `lib/` since it's original code) may include anything under
this directory — see the containment rule in BBR_POC_PLAN.md §2.8.

To re-sync after a picoquic update: replace these six headers and `bbr.c`
verbatim from the new commit (same filenames, no rename needed), record the
new commit hash here, then re-verify `lib/cc-bbr-common.c` still matches (the
five functions it replaces from `cc_common.c` may have changed signature).
