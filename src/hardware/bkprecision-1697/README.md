# BK Precision 1697

Select explicitly with `bkprecision-1697:conn=PORT`. The connection is fixed to
9600 baud, 8N1, no flow control. No port enumeration or model-ID command exists:
selecting this driver asserts the model; read-only GMAX/GETS/GETD/GPAL/GOVP queries
check protocol plausibility. Only 1697 is advertised.

Scan/open do not change settings, output, protection configuration or keypad
state. Close stops acquisition without changing physical output. Only explicit
configuration requests send VOLT, CURR, SOUT or SOVP. Closing a frontend is not an
output-OFF operation.

## Capabilities and wire format

- Voltage/current analog acquisition, sample/time limits, queried voltage target
  and current limit, actual output state, CV/CC, read-only `ovp_active`, and
  read/write/list `ovp_threshold` (double, volts).
- GMAX/GETS have three-digit V/I fields in 0.1 V / 0.01 A units. The observed
  `402502` maximum means 40.2 V / 5.02 A; readback retains this headroom.
- Voltage/current targets are restricted to nominal 1–40 V, step 0.1 V, and 0.01–5 A,
  step 0.01 A, further restricted by GMAX; voltage is also restricted by GOVP.
  Nonfinite/off-step/out-of-range requests are rejected, not silently rounded
  or clamped.
- Seven-digit GETD: three V digits /10, three I digits /100, mode 0=CV/1=CC.
  Captured nine-digit firmware: four V digits /100, four I digits /1000, mode.
  Voltage scaling agrees with GPAL; loaded nonzero-current scaling/CC accuracy
  remains unverified.
- GPAL's complementary active-low output indicators determine enabled state.
  Its character 65 (zero-based index 64) is the active-low FAULT/OVP indicator.
  Both state getters share the GPAL response; OVP metadata updates frontend
  indicators. Neither state is inferred from voltage or a communication error.
- SOUT zero enables and one disables. ACK-only and six-digit-data-plus-ACK
  replies are consumed completely. Replies use CR, not CRLF, and are bounded
  to 72 bytes. Transactions are serialized through the complete reply, with a
  two-second overall deadline and at most 500 ms per read/write call.

## Upper programming limit, not adjustable tracking OVP

The standard `SR_CONF_OVER_VOLTAGE_PROTECTION_THRESHOLD` key maps to the
instrument's upper programming voltage ceiling (GOVP/SOVP, sometimes called UVL).
It does **not** adjust the independent fixed tracking OVP, which trips when actual
voltage exceeds the target by approximately 10%. Existing GPAL `ovp_active`
remains the actual FAULT/OVP lamp state; it is not inferred from GOVP.

- `GOVP00\r` replies `ddd\rOK\r`; `ddd / 10` is volts.
- `SOVP00ddd\r` replies `OK\r`, followed by mandatory actual GOVP readback.
- Threshold GET/SET/LIST uses double volts: **1.0 through GMAX voltage**, in
  **0.1 V steps**. With observed GMAX=40.2 V, `SOVP00402\r` is legal so the
  observed factory 40.2 V limit can be restored. Threshold is not capped at 40 V.
- Voltage-target SET/LIST maximum is **min(40.0 V, GMAX voltage, GOVP)**. Each
  target write queries fresh actual GOVP first, even with a fresh cache or during
  acquisition, and rejects excess without sending VOLT or raising the ceiling.
- Before a threshold write, fresh GETS is mandatory. A ceiling below the current
  target is rejected with no SOVP, VOLT or output write; lowering behavior is
  undocumented. Lower the target explicitly first. Equality is permitted.
- Nonfinite, off-step and out-of-range inputs are rejected without mutating
  commands. Invalid precision/envelope requests need no prequery. ACKs alone do
  not establish actual settings: a valid threshold readback that differs from the
  canonical request (`counts / 10.0`) returns `SR_ERR` and logs requested/actual
  volts. It retains the actual cache, does not retry or latch a communication
  fault, and permits a subsequent SET without reopening. This mismatch check is
  specific to the threshold; existing voltage/current setter behavior is unchanged.
  Transport/framing readback failures still fault rather than fabricating a value.
- Periodic GOVP polls publish standard threshold metadata only when actual volts
  differ from the last notified value. A complete successful probe seeds this
  baseline; unchanged refreshes do not reset an uncommitted frontend numeric edit.
  Every explicit threshold SET attempt schedules one actual-value confirmation on
  the next normal GOVP poll, including no-op, ignored or rejected requests. Multiple
  attempts before that poll coalesce. This rolls back an optimistic frontend edit
  without sending setting/output writes for a rejected request. Zero in the
  last-notified field marks pending confirmation (valid thresholds are >=1 V).
  Ordinary reads and setter readbacks update the actual cache without consuming
  pending notifications. The notified value
  advances only after packet reservation, so stop/restart cannot consume an
  undelivered change; reconnect probing reseeds it. This is independent of cache
  freshness and adds no command or wait.
  Target LIST shares GOVP's cache (250 ms idle TTL, last actual snapshot
  while acquiring); target SET always rechecks the wire. Protection against
  concurrent front-panel changes between prequery and write cannot be atomic.

There is **no OVP-enable operation** and no regulation setter: CV/CC is read-only.
Memory slots, timers and key-lock functions have no matching standard keys here;
no invented keys or operations are exposed. No scan/open/stop/close writes change
protection, targets, output or keypad state.

## Conventional polling and cached getters

The five-command read-only probe seeds four complete response groups: GETD
(measurements and regulation), GETS (setpoints), GPAL (output and OVP-active),
and GOVP (upper programming voltage limit). During acquisition, getters
return the last validated group under a short cache lock, without waiting for
serial I/O. Idle callers reuse a group for 250 ms; expired groups are queried.
Snapshots are coherent per group, not atomic across separate getter calls.

A per-device timer-only source polls at 10 ms scheduling intervals. GETD
measurement slots alternate with GETS/GPAL/GOVP refresh opportunities once those
status groups are at least 500 ms old. This timer is not command spacing or a
100 Hz sample-rate promise. Only GETD increments the sample count. Actual rate
and status age depend on wire time, device replies, and competing control calls.

There is no blanket inter-command delay or artificial open quarantine. Open
flushes queued input and performs the normal probe. VOLT/CURR setters confirm
actual GETS readback; SOUT confirms GPAL and SOVP confirms GOVP. Requested values
are never fabricated into the cache. During a setter, active getters can return
the preceding actual snapshot until readback completes.

**Measured firmware turnaround:** GETS immediately after a VOLT ACK failed in
7/30 physical trials, and immediately after CURR in 4/30. A 1 ms post-ACK wait
passed 30/30 for each. Therefore only VOLT/CURR wait 1 ms before their GETS
readback. SOUT001-to-GPAL passed 30/30 without a gap and is not delayed. This is
observed compatibility handling, not a manufacturer timing specification.
Parent direct SOVP-ACK-to-GOVP trials passed **30/30 at zero gap** and **30/30 at
1 ms**, alternating 5/6 V limits and restoring 40.2 V. Therefore SOVP retains
**no delay** before mandatory GOVP readback. Evidence:
`bk1697/libsigrok/hardware-ovp-timing.log` relative to the workspace root.

Wire transactions and lifecycle changes use a wire mutex; snapshots and local
limits use a short cache mutex. Lock order is wire then cache. Cache access never
covers I/O, sleeps or session callbacks. Local limits do not acquire wire.
Physical I/O remains synchronous: slow replies can block the acquisition thread,
while setters and idle cache misses can wait for wire access.

## Failures and lifecycle

An I/O/framing failure stops active acquisition and invalidates cached state.
There is no automatic retry of an ambiguous write. Explicit close/open flushes
queued input and re-probes; it never repeats a failed setting. This assumes
ordinary serialized device behavior, not arbitrarily delayed unsolicited data.
If communications fail, inspect physical output rather than assuming it is OFF.

Packet reservations defer END until an in-flight packet reaches every callback,
and prevent subsequent delivery after stop/close. No mutex is held across
frontend callbacks. These guards cover reproduced callback/lifecycle bugs, not
speculative instrument behavior.

## Verification

Offline `make check` runs the parser/setting tests and, when Check is installed,
the upstream suite. Optional Linux/GNU-linker real-driver integration tests:

```sh
BK1697_BUILD_DIR=/absolute/build/path sh tests/bk1697-driver.sh
```

See `tests/bk1697-driver.md` for framing, bounds, cached latency, actual-readback,
concurrency, lifecycle, timer-source and OVP-metadata regressions. No hardware is
opened by these tests.

Parent hardware/GUI OVP evidence (logs under workspace-root `bk1697/libsigrok/`):

- `hardware-ovp-driver.log`: actual driver limits 5/6/5 V read back correctly,
  then 40.2 V was restored. Software guards rejected a ceiling below the target,
  out-of-range/off-step/nonfinite limits and a target above the ceiling. Physical
  target **4.2 V**, current limit **3.5 A** and output **OFF** were preserved.
- `gui-ovp-control-heartbeat.log`: actual SmuView **0.0.4** numeric OVP field
  enabled, min **1**, max **40.2**, step **0.1 V**, values
  **40.2 -> 5 -> 6 -> 40.2 V**. The enable toggle remained disabled as unsupported
  (`toggle_enabled=false`); the OVP LED remained inactive. Startup was
  **713.776 ms (about 714 ms)**. These are parent measurements of the preceding
  GOVP/SOVP implementation, not a hardware retest of the mismatch fix.

On an earlier tested build, actual SmuView 0.0.4 startup improved from 7.60 s to 0.55 s,
and a nine-getter batch from 1232 ms to approximately 0.14 ms. A subsequent
low-voltage exercise and 500-sample capture completed at approximately 25 Hz,
including successful settings/output restoration. These results are not loaded
accuracy or exhaustive timing guarantees. Independent electrical calibration and
physical FAULT activation/UI indication require separate verification.

An earlier voltage discrepancy recovered after a power cycle; native and
ngscopeclient A/B tests subsequently agreed within 0.01 V at 2.5/3/5 V. The
original fault trigger remains unknown, and no compensating conversion was added.
