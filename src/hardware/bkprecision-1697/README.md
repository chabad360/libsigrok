# BK Precision 1697

Select explicitly with `bkprecision-1697:conn=PORT`. The connection is fixed to
9600 baud, 8N1, no flow control. No port enumeration or model-ID command exists:
selecting this driver asserts the model; read-only GMAX/GETS/GETD/GPAL queries
check protocol plausibility. Only 1697 is advertised.

Scan/open do not change settings, output, protection configuration or keypad
state. Close stops acquisition without changing physical output. Only explicit
configuration requests send VOLT, CURR or SOUT. Closing a frontend is not an
output-OFF operation.

## Capabilities and wire format

- Voltage/current analog acquisition, sample/time limits, queried voltage target
  and current limit, actual output state, CV/CC, and read-only `ovp_active`.
- GMAX/GETS have three-digit V/I fields in 0.1 V / 0.01 A units. The observed
  `402502` maximum means 40.2 V / 5.02 A; readback retains this headroom.
- Programming is restricted to nominal 1–40 V, step 0.1 V, and 0.01–5 A,
  step 0.01 A, further restricted by GMAX. Nonfinite/off-step/out-of-range
  requests are rejected, not silently rounded or clamped.
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

## Conventional polling and cached getters

The read-only probe seeds three complete response groups: GETD (measurements and
regulation), GETS (setpoints), GPAL (output and OVP). During acquisition, getters
return the last validated group under a short cache lock, without waiting for
serial I/O. Idle callers reuse a group for 250 ms; expired groups are queried.
Snapshots are coherent per group, not atomic across separate getter calls.

A per-device timer-only source polls at 10 ms scheduling intervals. GETD
measurement slots alternate with GETS/GPAL refresh opportunities once those
status groups are at least 500 ms old. This timer is not command spacing or a
100 Hz sample-rate promise. Only GETD increments the sample count. Actual rate
and status age depend on wire time, device replies, and competing control calls.

There is no blanket inter-command delay or artificial open quarantine. Open
flushes queued input and performs the normal probe. VOLT/CURR setters confirm
actual GETS readback; SOUT confirms GPAL. Requested values are never fabricated
into the cache. During a setter, active getters can return the preceding actual
snapshot until readback completes.

**Measured firmware turnaround:** GETS immediately after a VOLT ACK failed in
7/30 physical trials, and immediately after CURR in 4/30. A 1 ms post-ACK wait
passed 30/30 for each. Therefore only VOLT/CURR wait 1 ms before their GETS
readback. SOUT001-to-GPAL passed 30/30 without a gap and is not delayed. This is
observed compatibility handling, not a manufacturer timing specification.

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

On the tested unit, actual SmuView 0.0.4 startup improved from 7.60 s to 0.55 s,
and a nine-getter batch from 1232 ms to approximately 0.14 ms. A subsequent
low-voltage exercise and 500-sample capture completed at approximately 25 Hz,
including successful settings/output restoration. These results are not loaded
accuracy or exhaustive timing guarantees. Independent electrical calibration and
physical FAULT activation/UI indication require separate verification.

An earlier voltage discrepancy recovered after a power cycle; native and
ngscopeclient A/B tests subsequently agreed within 0.01 V at 2.5/3/5 V. The
original fault trigger remains unknown, and no compensating conversion was added.
