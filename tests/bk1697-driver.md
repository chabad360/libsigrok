# Offline BK1697 real-driver integration tests

`bk1697_driver.c` links the **real static libsigrok archive** and selects
`bkprecision-1697` from `sr_driver_list()`. Discovery, configuration, device
lifecycle, sessions and packet conversion use public APIs. No parser or driver
source is compiled into this executable and no driver context is inspected.
The internal header supplies serial seam and session-source prototypes only.

GNU linker `--wrap` replaces exactly eight serial boundary functions:
`serial_open`, `serial_close`, `serial_flush`, `serial_read_nonblocking`,
`serial_read_blocking`, `serial_write_blocking`, `serial_source_add`, and
`serial_source_remove`. The fake serial source delegates to the **real session
event loop**, using a 10 ms timer instead of a descriptor. The fake
`/offline/bk1697-no-hardware` connection is never opened. No hardware is needed.

## Isolated performance build and commands

Commands below run from `/home/mendel/Documents/Projects/drivers/libsigrok-arch`
on branch `arch-bkprecision-1697`, starting at `66af36e2`. The build enables only
BK1697 and demo, with C++ bindings off. It does not change package definitions.
The local Check dependency already exists outside this source tree.

```sh
autoreconf -fi
mkdir -p build-performance
cd build-performance
PKG_CONFIG_PATH=/home/mendel/Documents/Projects/drivers/.build-deps/check/usr/lib/pkgconfig \
  ../configure --disable-all-drivers --enable-bkprecision-1697 --enable-demo \
  --disable-cxx --enable-static > configure.log 2>&1
make -j4 > build.log 2>&1
cd ..
```

The script still defaults to `build-hardware`; use the explicit override:

```sh
BK1697_BUILD_DIR="$PWD/build-performance" timeout 30 \
  sh tests/bk1697-driver.sh -p /bk1697/snapshot-latency
BK1697_BUILD_DIR="$PWD/build-performance" timeout 210 \
  sh tests/bk1697-driver.sh > build-performance/driver-tests.log 2>&1
LD_LIBRARY_PATH=/home/mendel/Documents/Projects/drivers/.build-deps/check/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
  make -C build-performance check -j4 > build-performance/check.log 2>&1
```

The hook compiles with `cc -std=c99 -Wall -Wextra -Werror`, source/build include
directories, `--whole-archive` of the selected build's static library, the eight
wrap flags, system `pkg-config --static --libs libsigrok` dependencies
**excluding `-lsigrok`**, and `-lm`. It includes libtirpc when available. It never
links the system libsigrok implementation. Its temporary executable is removed
on exit. The build directory must correspond to this source tree.

An initial `make check` without `LD_LIBRARY_PATH` passed the parser executable
but could not load `libcheck.so.0` for `tests/main` (exit 127). Supplying the local
runtime directory fixed that environment failure; no package/source fix was
needed. Existing upstream Check deprecated-assertion format warnings are not
introduced by this refactor.

## Historical cache-refactor red / green timing (before spacing removal)

The **first change was the public-API latency regression**, before driver edits.
The source was autoreconf'd/configured/built as above with unchanged driver code.
Running the single-test command above against that baseline archive produced:

```text
held-wire getters: 900.760 ms, 6 extra commands
assertion failed (getter_elapsed < 50000): (900760 < 50000)
exit 134
```

An acquisition GETD reply is held at the serial boundary while another thread
reads voltage, current, regulation, target, limit, enabled, connection ID and
sample limit. A condition-variable watchdog releases the held reply after at
most 300 ms if getters have not completed, so RED cannot deadlock on the held
reply. The external timeout also bounds setup/session failures. The failure
includes the watchdog delay and the six paced synchronous getter commands.

After grouped snapshots and non-waiting acquisition scheduling, rebuilding and
running the same regression first produced **0.076 ms, zero extra commands**.
A subsequent full run produced **0.050 ms, zero extra commands**. The final
regression additionally reads the advertised voltage range while wire is held
and verifies the old actual target (12 V). Active-setter coverage holds a setter
ACK instead of GETD and checks the same no-I/O/under-50-ms behavior. Historical
full-run results before spacing removal (21/21 integration cases passed):

| Measurement | Original baseline | Cache refactor, still paced |
| --- | ---: | ---: |
| Held-GETD getter batch | 900.760 ms, 6 extra commands (RED) | 0.031 ms, 0 extra commands |
| Getter batch during held setter ACK | Not separately measured | 0.089 ms, 0 extra commands |
| Expired idle GETS pair | Not separately measured | 0.027 ms, 1 shared query |
| Maximum live GETS / GPAL refresh gap | No periodic status refresh | 806.519 / 806.449 ms |

The idle number follows an explicit TTL-expiry wait and a ready pacing slot;
it is not a physical serial round-trip estimate. That run of `make check` passed both
executables: four GLib parser/setting cases and 76 upstream Check checks, zero
failures/errors. `git diff --check` passed. Logs are in
`build-performance/{driver-tests.log,check.log,tests/main.log}`. Measurements are
backend-only on this machine; no SmuView GUI latency was measured.

## Inter-command spacing removal: test-first red / green

There is no verified manufacturer requirement for the old 100 ms gap. The
original conservative workaround for a missing reply is not such a source.
Spacing has therefore been removed, without changing serialization, CR/OK
framing, reply consumption, timeout/fault handling or mandatory actual readback.

Before editing driver code, `no-command-spacing` was added and run against the
existing paced `build-performance` archive. It performs two rounds of
VOLT + GETS, CURR + GETS, SOUT + GPAL through the public config API. All 12
commands are uncached. Both total elapsed time and the maximum measured gap from
prior reply completion to next write must be under 80 ms on the instantaneous
serial model. The existing minimum-100-ms assertion was replaced by scoped gap
measurement; all complete-reply and concurrency assertions were retained.

```text
RED:   12 uncached commands: 1200.969 ms total, 100.084 ms maximum reply-to-command gap
       assertion failed (wire.max_command_gap < 80000): (100084 < 80000); exit 134
GREEN: 12 uncached commands: 0.114 ms total, 0.078 ms maximum reply-to-command gap
       PASS
```

Exact commands from this source directory (RED before driver edits; GREEN after
removing spacing and rebuilding):

```sh
BK1697_BUILD_DIR="$PWD/build-performance" timeout 30 \
  sh tests/bk1697-driver.sh -p /bk1697/no-command-spacing \
  > build-performance/no-spacing-red.log 2>&1
make -C build-performance -j4 > build-performance/build.log 2>&1
BK1697_BUILD_DIR="$PWD/build-performance" timeout 30 \
  sh tests/bk1697-driver.sh -p /bk1697/no-command-spacing \
  > build-performance/no-spacing-green.log 2>&1
BK1697_BUILD_DIR="$PWD/build-performance" timeout 210 \
  sh tests/bk1697-driver.sh > build-performance/driver-tests-no-spacing.log 2>&1
LD_LIBRARY_PATH=/home/mendel/Documents/Projects/drivers/.build-deps/check/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
  make -C build-performance check -j4 > build-performance/check-no-spacing.log 2>&1
```

Final full-run results: **22/22 integration cases passed**, four parser/setting
cases passed, and all **76 upstream Check checks passed** (zero failures/errors).
`git diff --check` passed. The final no-delay batch was **0.086 ms total**, with
**0.051 ms maximum reply-to-command gap**; held-GETD getters took 0.042 ms and
held-setter getters 0.045 ms, both with zero extra commands. Live GETS/GPAL maximum
refresh gaps were 524.714/524.742 ms, with 120 GETD polls producing 120 samples.
The focused GREEN timing above is retained separately from this full-run timing.

Readback-fault coverage was strengthened as a separate test-first correction:
requiring two commands for the failed setter first exposed that the emulator's
`fail_readback` was interrupting the write ACK (15 commands instead of expected
16, exit 134). The injection now occurs on GETS after the ACK succeeds, and the
assertion passes. All fault/no-ambiguous-retry and concurrency assertions remain.
Focused evidence is in `build-performance/readback-seam-{red,green}.log`.

Intentional driver waits remain separate: the existing two-second open
quarantine before flushing; synchronous serial reads/writes with a two-second
overall transaction deadline and at most 500 ms per read/write; and wire-mutex
contention for setters/idle misses/lifecycle operations. The 10 ms source tick,
250 ms idle TTL and 500 ms status refresh age are scheduling/freshness rules,
not sleeps between serial commands. Tests additionally use real 260 ms cache-
expiry waits, a 50 ms contention observation and bounded held-reply watchdogs.
No hardware or SmuView UI timing was measured.

## Checks

There are **22 GLib integration cases** (the previous 21, plus no-command-spacing):

- `no-command-spacing`: six repeated VOLT/CURR/SOUT setter calls produce exactly
  12 actual commands in the expected setter/GETS-or-GPAL sequence. The instant
  serial model must complete the command batch and each reply-to-next-command
  gap in under 80 ms, excluding setup/quarantine. Replies must be fully consumed;
  no clock or sleep helper is mocked.

- `snapshot-latency`: cached acquisition getters and range-list reads finish
  under 50 ms with no transport commands while a GETD reply owns the wire lock.
- `active-setter-snapshot`: the same guarantee while a setter ACK is held;
  readers retain previous actual values until successful actual readback.
- `idle-groups`: probe-seeded reuse; expired GETS serves target/limit together,
  GETD serves voltage/current/regulation together, and GPAL serves repeated
  enabled reads with no duplicate commands; 260 ms real waits explicitly force
  TTL misses and observe changed values; close/reopen re-seeds.
- `actual-readback`: fake ACKs deliberately leave settings/output unchanged;
  VOLT/CURR must issue GETS and SOUT must issue GPAL before returning. Getters
  report actual old values, not requested ones. Readback failure invalidates all
  cached state, latches the fault and requires explicit reconnect.
- `live-status`: uncommanded settings/output changes become visible during live
  polling without getter I/O. At least two refreshes of each status group occur,
  with maximum gaps below 1.1 s (scheduler slack) on this responsive fake link;
  exactly 120 GETD transactions produce exactly 120 samples, excluding GETS/GPAL.
  The window was increased from 20 to 120 samples because faster acquisition
  otherwise ends before the status refresh age; freshness assertions remain.
- `settings`: explicit-connection-only scan; exactly GMAX/GETS/GETD/GPAL on
  scan/open; no close writes; repeated public open/close rejection without
  transport traffic; 7-/9-digit GETD scaling and CV/CC; GETS/GPAL actual readback;
  SOUT active-low polarity, ACK-only and data-plus-ACK replies; minimum/maximum
  VOLT/CURR; invalid/off-step/nonfinite settings rejected without wire traffic.
  Format changes explicitly expire GETD so a valid cache cannot hide the test.
- `faults-reconnect`: timeout, read error, short/failed write, malformed digits,
  missing ACK, CRLF, overlong response, invalid GPAL indicators, pending input
  and preflight failure. GPAL fault injection explicitly expires its cache.
  Faulted reads/writes/open-without-close do no more I/O. Explicit close/reopen
  quarantines and flushes a staged late ACK, then performs four read-only probes.
  Closed getters cannot return prior cached device values.
- `deadline`: explicitly expired GETD encounters slow arriving bytes beyond the
  two-second overall deadline; fault latching and explicit recovery still apply.
- `revalidation`: invalid GMAX and out-of-envelope GETS fail reopen/close the
  transport; 40.2 V / 5.02 A headroom remains legal readback, never write targets.
- `acquisition`: real session start/run/termination, both GETD formats, balanced
  frames, numerical values/units/precision, regulation metadata, sample limits,
  and configuration re-entry from analog callbacks outside the wire lock.
- `time-limit`: stops before a measurement frame when the time limit is due.
- `acquisition-fault`: timeout emits no sample, stops, invalidates the cached
  snapshot and latches the fault, then recovers only after explicit reconnect.
- `concurrent`: a held GETD reply excludes competing configuration transport
  access; VOLT/CURR/SOUT and actual GETS/GPAL replies finish while acquisition is
  active; samples retain correct values and no response bytes are left queued.
- `smaller-maximum`: advertised ranges and programming bounds honor GMAX.
- Five `callback-stop/*` cases stop from HEADER, FRAME_BEGIN, ANALOG, FRAME_END,
  and META; `callback-close/analog` closes from a callback; `close-active`
  removes the source and ends acquisition even when public close already marked
  the device inactive. A later observer sees the triggering packet before END,
  no packets arrive after END, repeated stop from END is idempotent, and explicit
  reopen/start does not inherit acquisition or samples from the old generation.

Every write checks complete prior reply consumption, preserving the existing
no-interleaving and held-reply assertions. No minimum command gap is required.
Every flush checks the unchanged two-second open quarantine.
No clocks/sleeps are mocked. Allow roughly three minutes, with a 210-second
external watchdog for the full suite.

## Automatic integration (parent-owned)

Keep this an optional separate executable/hook, not part of `tests/main.c`.
Enable only with the native driver, static archive, serial support and GNU-ld
wrapping. The parent may register the hook with `BK1697_BUILD_DIR` set to its
build, or translate it into a conditional Automake target using configured
dependency flags. Do not enable linker wrap flags globally or require them on
other platforms. The existing pure-parser test remains independent. Master
porting and package integration are outside this change.

## Scope / risks

The driver uses no background worker. Active getters never acquire the wire
mutex. Idle misses, setters and lifecycle operations remain synchronous. The
acquisition callback trylocks wire and defers when busy; there is no pacing
state or delay. The 10 ms source tick is polling cadence, not inter-command
spacing. Physical reply I/O still blocks the session thread
up to the bounded deadline. Busy setters or slow hardware can delay refresh
beyond the nominal 0.5–1 s target. Groups are coherent individually, not a global
cross-group snapshot. During a setter, active readers may see the previous
acknowledged actual value; faults and close invalidate every group.

This tests the serial API, not libserialport, UART/USB framing, physical timing,
electrical behavior, or the SmuView UI. Nonzero 9-digit current scaling remains
unverified under load. Transport open/flush/close/source-registration failures,
disabled channels and arbitrary cross-thread lifecycle schedules are not
exhaustive. No hardware or package edits were made and no commits were created.
