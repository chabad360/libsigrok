# Offline BK1697 driver tests — simplified driver

The integration executable links the real static archive and exercises discovery,
configuration, sessions, callbacks and lifecycle through public APIs. No driver
private context is inspected and no hardware is accessed. The internal header
supplies serial and session-source seam declarations only.

## Build and run

Baseline: branch `arch-bkprecision-1697`, HEAD `1a8e652e`. The existing
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

## Final verification

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

Explicit close/reopen flushes staged stale bytes and executes GMAX/GETS/GETD/GPAL
only. No timed quarantine or automatic retry/replay of ambiguous writes is
required. Programming bounds/headroom, smaller GMAX, both GETD formats,
precision/units/regulation, time/sample limits, wire exclusion under a held
reply, re-entry, stop/close from callbacks, repeated stop from END and explicit
restart remain covered. Complete prior reply consumption is checked at every
write. No existing wire-completion or no-interleaving assertions were removed.

## Design and scope

The transport query handles framing and bounded I/O only. A narrow typed group
table/helper selects GETD/GETS/GPAL, validates envelopes and publishes snapshots
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
are attributed in the driver README; final physical OVP/UI testing is parent-owned. Two simultaneous physical devices are not emulated;
per-device key registration and same-fd timer coexistence are tested directly.

Package integration and master porting remain parent-owned. No hardware,
agents, package edits or commits were used.
