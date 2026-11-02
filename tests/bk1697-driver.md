# Offline BK1697 driver tests — simplified driver

The integration executable links the real static archive and exercises discovery,
configuration, sessions, callbacks and lifecycle through public APIs. No driver
private context is inspected and no hardware is accessed. The internal header
supplies serial and session-source seam declarations only.

## Build and run

Review-fix baseline: branch `arch-bkprecision-1697`, HEAD `41b9dcc6`.
Original GOVP/SOVP baseline: `a46f4d8e`.
The older simplification/timing evidence below predates this addition. The existing
`build-performance` is frozen for parent measurements and was **not rebuilt or
modified**. This work uses:

`/home/mendel/Documents/Projects/drivers/libsigrok-arch/build-simplified`

From `/home/mendel/Documents/Projects/drivers/libsigrok-arch`:

```sh
mkdir -p build-simplified
cd build-simplified
PKG_CONFIG_PATH=/home/mendel/Documents/Projects/drivers/.build-deps/check/usr/lib/pkgconfig \
  ../configure --disable-all-drivers --enable-bkprecision-1697 --enable-demo \
  --disable-cxx --enable-static > configure.log 2>&1
make -j4 > build.log 2>&1
cd ..
BK1697_BUILD_DIR="$PWD/build-simplified" timeout 120 \
  sh tests/bk1697-driver.sh > build-simplified/driver-tests.log 2>&1
LD_LIBRARY_PATH=/home/mendel/Documents/Projects/drivers/.build-deps/check/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
  make -C build-simplified check -j4 > build-simplified/check.log 2>&1
git diff --check
```

The hook still defaults to `build-hardware`; always supply the override here.
It compiles with `-std=c99 -Wall -Wextra -Werror`, uses the selected build's real
static archive, and obtains dependency libraries from system pkg-config without
linking system libsigrok. Local Check needs the runtime directory above.
Existing upstream Check assertion-format warnings are outside this change.

Six serial I/O/open/close/flush functions are simulated. Legacy
`serial_source_add/remove` wrappers now reject calls rather than hiding serial
readiness behind a fake timer. The two keyed session-source wrappers verify the
actual driver registration (key=device, fd=-1, events=0, timeout=10) and forward
unchanged to the real session implementation. Dispatch verifies no readiness
events. Source removal uses the same device key. No parser or driver source is
compiled directly into the integration executable.

## Native GOVP/SOVP verification (current)

This exposes only standard `SR_CONF_OVER_VOLTAGE_PROTECTION_THRESHOLD`
GET/SET/LIST, double volts. It is the **upper programming voltage ceiling**, not
the fixed tracking OVP that trips at actual voltage about 10% above target.
Existing GPAL OVP-active behavior is unchanged. No OVP-enable or regulation
setter, custom memory/timer/key-lock keys, worker, quarantine or global delay
was added. Scan/open now perform exactly five read-only probes.

Bounds: threshold **1.0..GMAX voltage**, step **0.1 V**, including factory
**40.2 V / 402** restoration. Target write/list maximum is
**min(40 V, GMAX voltage, GOVP)**. Target writes refresh actual GOVP first;
threshold writes refresh GETS and reject a new ceiling below the current target.
Neither rejection changes the ceiling, target or output. SOVP ACK must be followed
by actual GOVP readback. A valid actual value differing from canonical requested
counts/10.0 now returns `SR_ERR` with requested/actual voltage in the log. The
actual snapshot stays cached, with no retry, communication-fault latch or forced
reopen. This new mismatch rule applies only to threshold SET, not old V/I setters.
No requested value is fabricated into the cache.

Polling now uses a small explicit static sequence
`GETD, GETS, GETD, GPAL, GETD, GOVP` and `G_N_ELEMENTS` wrap, rather than enum
arithmetic or a hard-coded modulus. Status age-gating, GETD fallback, limits
and sample accounting are unchanged.

### Red/green evidence

The parser and public driver serial seams were exercised before implementation.
All four RED commands below exited 134 from assertions, not watchdog expiry:

| Test | RED | GREEN |
| --- | --- | --- |
| `/bk1697/upper-voltage` | typed three-digit reply rejected at a partial boundary | all split boundaries, 1.0/40.2 V and malformed width/digit/ACK/trailer pass |
| `/bk1697/upper-voltage-getter` | public GET returned SR_ERR_ARG | probe seed, independent 250 ms GOVP TTL, shared target LIST and capability scope pass |
| `/bk1697/upper-voltage-setting` | SET returned SR_ERR_NA | limits, smaller GMAX, ignored ACK, actual readback, bidirectional guards and 402 restore pass |
| `/bk1697/upper-voltage-poll/change` | zero of two expected metadata stages | two actual front-panel values and cached callback getters pass |

Evidence: `build-simplified/govp-{parser,getter,setting,meta}-red.log`.
Getter and setting focused GREEN logs are `govp-{getter,setting}-green.log`;
`govp-driver-tests.log` and `tests/check_bkprecision_1697.log` contain final GREEN
for all cases. To reproduce RED, run against the corresponding preceding
implementation (the completed driver correctly passes these tests).

Commands from the repository root:

```sh
cc -std=c99 -Wall -Wextra -Werror -Iinclude $(pkg-config --cflags glib-2.0) \
  tests/check_bkprecision_1697.c src/hardware/bkprecision-1697/parse.c \
  $(pkg-config --libs glib-2.0) -lm -o build-simplified/bk1697-parser
build-simplified/bk1697-parser -p /bk1697/upper-voltage
make -C build-simplified -j4
BK1697_BUILD_DIR="$PWD/build-simplified" timeout 30 \
  sh tests/bk1697-driver.sh -p /bk1697/upper-voltage-setting
# The same hook runs focused getter or upper-voltage-poll/change tests.
BK1697_BUILD_DIR="$PWD/build-simplified" timeout 120 sh tests/bk1697-driver.sh
LD_LIBRARY_PATH=/home/mendel/Documents/Projects/drivers/.build-deps/check/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
  make -C build-simplified check -j4
git diff --check
```

Final verification: **43/43 integration cases** (all prior 34 plus nine),
**6/6 pure parser/setting cases**, **76/76 upstream Check checks**, zero
failures/errors; `git diff --check` passes. The ready build is
`/home/mendel/Documents/Projects/drivers/libsigrok-arch/build-simplified`,
including `.libs/libsigrok.so` and `.libs/libsigrok.a`.
No changes were made to `build-performance`, packages or hardware.

Additional coverage includes mandatory GOVP readback failure while idle and
active (fault latch, immediate END, no fabricated threshold); invalid late GOVP
probe bounds; front-panel metadata callback stop/close ordering; and cached
getters with the wire held by GETD, VOLT ACK or SOVP ACK. The setter-ACK hold is
explicitly command-gated so the new prequeries cannot accidentally substitute
a held GOVP/GETS reply for the original ACK-contention test.

A verification run measured held-wire getters at **0.027 ms**, held VOLT-ACK getters
at **0.051 ms**, and held SOVP-ACK getters at **0.038 ms**, all with **zero extra
commands**, retaining the <50 ms assertions. The command sequence now has 14
rather than 12 writes/readbacks because each VOLT is preceded by GOVP. The
original <80 ms total/max-gap bounds and VOLT/CURR >=1 ms minima are unchanged.
Measured minima were **1.053/1.023 ms**; ten other gaps totaled **0.027 ms**
(<5 ms, still rejects blanket 1 ms spacing). A separate exact 24-command
GETS/SOVP/GOVP sequence totaled **0.022 ms** in gaps (<5 ms), rejecting even a
SOVP-only 1 ms wait.

### Review-fix red/green (`41b9dcc6`)

The existing public `/bk1697/upper-voltage-setting` regression was changed first
so an ignored SOVP requesting 16 V with valid actual 15 V must fail SET. RED
returned `SR_OK` and failed the non-success assertion (exit 134), recorded in
`build-simplified/govp-mismatch-red.log`. After the fix, the same test passes in
`govp-mismatch-green.log`: exactly **3 commands** (GETS/SOVP/GOVP), actual **15 V**
retained in the cache and target LIST, and zero extra getter commands. The next
SET uses `(double)16.1f` and succeeds against canonical **16.1 V** readback with
exactly **3 commands**, proving wire-precision comparison and no retry. Open/close
counters remain **2/1** across both calls: no reopen or fault recovery is needed.

Reproduction from the repository root:

```sh
# RED uses the changed test against the preceding implementation/archive.
BK1697_BUILD_DIR="$PWD/build-simplified" timeout 30 \
  sh tests/bk1697-driver.sh -p /bk1697/upper-voltage-setting
# GREEN after rebuilding the fix:
make -C build-simplified -j4
BK1697_BUILD_DIR="$PWD/build-simplified" timeout 30 \
  sh tests/bk1697-driver.sh -p /bk1697/upper-voltage-setting
```

Final review-fix rerun: **43/43 integration**, **6/6 parser/setting**, and
**76/76 upstream Check checks**, zero failures/errors; `git diff --check` passes.
Logs: `build-simplified/govp-review-{build,driver-tests,check}.log`. Existing timing
checks are unchanged: VOLT/CURR minimum gaps **1.054/1.053 ms**, ten other gaps
**0.029 ms** total; 24 GETS/SOVP/GOVP gaps **0.023 ms** total. The explicit polling
sequence retains live-status refresh, exact sample accounting and lifecycle tests.
The rebuilt shared/static libraries in `build-simplified/.libs/` are ready for
parent packaging. No package changes, hardware actions, agents or commits were used.

### Parent hardware/GUI evidence and remaining limits

Logs read from workspace-root `bk1697/libsigrok/` (no hardware accessed here):

- `hardware-ovp-timing.log`: direct SOVP->GOVP **30/30 at zero gap**,
  **30/30 at 1 ms**, alternating 5/6 V; 40.2 V restored, GETS `042350`
  and output OFF preserved. **No SOVP delay is added**. Only measured
  VOLT/CURR 1 ms waits remain.
- `hardware-ovp-driver.log`: actual driver limits 5/6/5 V with matching actual
  readbacks and restoration to 40.2 V; software ceiling/target/range/step/finite
  guards rejected unsafe requests. Target **4.2 V**, current limit **3.5 A**
  and output **OFF** were preserved.
- `gui-ovp-control-heartbeat.log`: actual SmuView **0.0.4** numeric OVP field
  enabled, min **1**, max **40.2**, step **0.1 V**; displayed
  **40.2 -> 5 -> 6 -> 40.2 V**. Enable toggle unsupported/disabled
  (`toggle_enabled=false`), OVP LED inactive, startup **713.776 ms (~714 ms)**.

These parent results precede this review fix; no new physical mismatch or GUI
retest was performed. The timing trials are evidence for this unit, not an
exhaustive firmware guarantee. A dropped GOVP reply still faults rather than
retrying SOVP. Front-panel changes between a safety prequery and the write cannot
be atomic. Active snapshots can lag until the normal >=500 ms status refresh;
idle TTL stays 250 ms. Physical tracking-trip behavior, electrical accuracy and
loaded CC scaling still require separate verification.

Review was performed locally against the working-tree diff from `41b9dcc6`,
using `HACKING` and the supplied spec; no agents or commits were used.

## Test-first evidence

Each new behavioral regression was run against the preceding implementation
before its fix. Focused command (substitute the test name below):

```sh
BK1697_BUILD_DIR="$PWD/build-simplified" timeout 30 \
  sh tests/bk1697-driver.sh -p /bk1697/open-without-delay
```

| Regression | RED | GREEN (focused run) |
| --- | --- | --- |
| `open-without-delay` | 2000.098 ms; failed <80 ms bound | 0.006 ms; flush and exactly four read-only probes |
| `held-wire-limits` | 300.125 ms; failed <50 ms bound | 0.039 ms, zero extra commands |
| `timer-source` | legacy serial source count 1, expected 0 | real timer: 10 callbacks over 100 ms, companion timer 10, balanced removal |
| `callback-setter-fault/ack` | source removals 0, expected 1 | ACK and readback fault cases both pass; deferred END ordering preserved |

All RED runs exited 134 from assertions, not watchdog expiry. Logs are
`build-simplified/{open,limits,timer,setter-fault}-{red,green}.log`.
The held-wire test uses the existing bounded 300 ms condition-variable watchdog,
so a blocked local setter cannot hang the RED run indefinitely. Open latency is
measured on instantaneous fake serial I/O; it asserts no artificial delay, not
a manufacturer timing guarantee or minimum quiet period.

## Scoped setting waits and GPAL OVP: test-first evidence

The parent's `bk1697/libsigrok/hardware-write-timing-stress.log` reports dropped
GETS replies directly after VOLT ACK: 23/30 successes without a gap, 30/30 with
1 ms, 30/30 with 3 ms, output OFF and 1.5/1.6 V settings. The initial VOLT-only wait was later extended to CURR after the parent observed
a CURR-ACK-to-GETS restoration failure following a 300-frame exercise (output
already OFF). `bk1697/libsigrok/hardware-current-timing-stress.log` reports
26/30 successes at zero gap and 30/30 at 1 ms; `hardware-off-timing-stress.log`
reports SOUT001->GPAL 30/30 at both zero and 1 ms. Only successful VOLT and CURR
ACKs wait 1 ms before GETS. SOUT and read commands remain undelayed. This is
observed firmware behavior, not a manufacturer specification. No global gap or
open quarantine was restored.

- Initial `voltage-readback-delay` (historical): RED measured a 0.000 ms VOLT-to-GETS minimum and
  failed the >=1 ms bound. Focused GREEN measured a 1.055 ms minimum, with the
  ten other gaps totaling 0.037 ms; all 12 actual commands took 2.187 ms. The
  existing <80 ms no-global-spacing regression remains unchanged. The aggregate
  non-VOLT bound (<5 ms for ten gaps) also rejects a blanket 1 ms gap without
  requiring a fragile sub-millisecond bound on every scheduler interruption.
- Current `setting-readback-delay` replaces the VOLT-only test without changing
  the integration count. RED measured CURR->GETS 0.000 ms (VOLT 1.060 ms),
  failing the CURR >=1 ms bound. GREEN measured VOLT 1.061 ms and CURR 1.054 ms;
  the eight remaining gaps totaled 0.043 ms, and all 12 commands took 4.320 ms.
  Exactly two VOLT and two CURR readbacks are checked; the remaining sequence
  includes both SOUT000 and SOUT001. The <5 ms aggregate bound rejects blanket
  1 ms spacing, and the original <80 ms no-global-spacing checks remain.
  Logs: `build-simplified/setting-delay-{red,green}.log`.
- Parser `panel-ovp`: RED failed the synthetic active assertion. GREEN validates
  the actual literal inactive GPAL fixture, synthetic active state at index 64,
  unchanged output bits, every partial boundary, and rejection of indicator `2`.
- `ovp-getter`: RED returned SR_ERR_NA. GREEN verifies initial false from the
  probe, true/false after GPAL expiry, shared response reuse with enabled, and
  no inference that fault means output OFF or ON.
- `ovp-poll/activate-clear`: RED reached zero of two expected metadata stages
  within its bounded session. GREEN observes true then false through normal
  GPAL rotation, verifies cached getters issue no UART commands in callbacks,
  and checks regulation/sample accounting remains separate from OVP metadata.
- `ovp-poll/callback-stop` and `callback-close`: verify the triggering metadata
  reaches a later observer before END, balanced source removal, and no packets
  after END. Held-reply snapshot tests now include OVP; fault-latching tests
  require an error, not a fabricated OVP value, after communication failure.

Evidence logs: `build-simplified/voltage-delay-{red,green}.log`,
`ovp-parser-{red,green}.log`, `ovp-getter-{red,green}.log`, and
`ovp-meta-{red,green}.log`. All new RED failures were assertions (exit 134).
For the parser case, build `tests/check_bkprecision_1697` then run it with
`-p /bk1697/panel-ovp`; integration cases use the existing shell hook and `-p`.

## Prior verification (before GOVP/SOVP)

Final rerun after scoped VOLT/CURR timing and cached/metadata OVP support:

- **34/34 integration cases pass** (the preceding 29 plus five).
- **5/5 parser/setting cases pass**.
- **76/76 upstream Check checks pass**, zero failures/errors.
- `git diff --check` passes.

Earlier simplification cases are open-without-delay, held-wire-limits, timer-source,
active-setter-fault, callback-setter-fault/ack, callback-setter-fault/readback,
and late-probe-validation. The timer test runs a second timer with the same
fd=-1 and a different key in the same real session; it requires multiple callbacks
but no busy loop and exactly one driver add/remove. The callback fault cases
verify immediate source removal, no nested END, triggering-packet delivery to a
later observer before END, and no subsequent analog/frame-end/meta packets.
The external setter-fault case requires END without waiting for another poll.
Late-probe tests reject out-of-envelope GETD and semantically invalid GPAL after
earlier probe groups succeeded, without serving partially seeded caches.

Existing coverage remains: grouped 250 ms idle TTL reuse/expiry; active getters
under held wire and held setter ACK; mandatory actual GETS/GPAL readback, including
ACKs which do not change actual settings and readback failure; 120 GETD polls
produce exactly 120 samples while both status groups refresh at least twice;
100 ms spacing is **not** required. The uncached 12-command no-delay regression
continues to enforce total time and maximum reply-to-next-command gap <80 ms on
the instantaneous model. Fault cases explicitly expire caches when transport
injection requires it, and preserve timeout, short/failed write, malformed reply,
CRLF, overlong frame, panel indicators, pending input and read-error checks.

Explicit close/reopen flushes staged stale bytes and executes GMAX/GETS/GETD/GPAL/GOVP
only. No timed quarantine or automatic retry/replay of ambiguous writes is
required. Programming bounds/headroom, smaller GMAX, both GETD formats,
precision/units/regulation, time/sample limits, wire exclusion under a held
reply, re-entry, stop/close from callbacks, repeated stop from END and explicit
restart remain covered. Complete prior reply consumption is checked at every
write. No existing wire-completion or no-interleaving assertions were removed.

## Design and scope

The transport query handles framing and bounded I/O only. A narrow typed group
table/helper selects GETD/GETS/GPAL/GOVP, validates envelopes and publishes snapshots
for idle misses, probe, acquisition and setter readback. No reverse command-name
comparison is needed. `cache_ready` gates all groups after a complete successful
probe; per-snapshot valid flags are redundant and removed. Boolean response
fields use `gboolean`. Local limit setters use only the short cache mutex.
GPAL index 64 supplies the active-low FAULT/OVP state, validated as binary,
exposed through the read-only OVP-active capability and sent as boolean metadata
on GPAL polls. Output bits remain independent. Packet reservations preserve END
ordering for stop/close from an OVP metadata callback.

No worker framework, extra trailer check, maximum cache age, replacement quiet
delay or parser cursor was added. The only new serial settling wait is 1 ms
after successful VOLT or CURR ACK before mandatory GETS; no other commands inherit
it. Intentional waits also include actual synchronous
serial I/O (two-second overall deadline, up to 500 ms per read/write), wire-mutex
contention for wire operations, and the real 10 ms timer tick. TTL/status ages
remain 250/500 ms freshness rules, not serial-command sleeps. Test-only expiry
waits, contention observation and watchdogs remain.

Removing the quiet delay deliberately does not claim protection against bytes
which arrive after the open flush; the device is expected to answer ordinary
serialized commands sanely. Faults still latch until explicit reconnect, and no
ambiguous write is retried. Active getters may retain previous actual snapshots
during a setter; groups are not globally atomic. Slow serial I/O still blocks
the session thread and may delay status freshness. Hardware behavior, loaded
current/CC scaling, electrical accuracy and arbitrary thread schedules remain
unverified by this offline work. Parent-reported GUI/performance measurements
are attributed in the driver README and the hardware/GUI evidence section above;
physical tracking-trip activation remains parent-owned. Two simultaneous physical devices are not emulated;
per-device key registration and same-fd timer coexistence are tested directly.

Package integration and master porting remain parent-owned. No hardware,
agents, package edits or commits were used.
