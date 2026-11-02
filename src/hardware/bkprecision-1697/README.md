# BK Precision 1697

Supports **1697 only**, selected explicitly with `bkprecision-1697:conn=PORT`.
The serial link is fixed to 9600 baud, 8N1, no flow control. There is no port
enumeration, USB-ID match, or unique model identification: explicit selection
asserts the model; read-only GMAX/GETS/GETD/GPAL queries check plausibility.

Scan/open never change settings, output, or keypad/remote state. Close stops
acquisition without changing the physical output. Only explicit configuration
requests send VOLT, CURR, or SOUT. Do not assume closing a frontend disables
output.

## Capabilities and protocol

- Voltage/current analog acquisition, sample/time limits, actual queried voltage
  target/current limit, GPAL output-state and FAULT/OVP-active readback, and CV/CC
  regulation. `over-voltage-protection-active` is read-only.
- GMAX/GETS: three-digit voltage/current fields, /10 V and /100 A. Observed
  GMAX `402502` means 40.2 V / 5.02 A; readback retains that headroom.
- Writes: nominal 1–40 V in 0.1 V steps and 0.01–5 A in 0.01 A steps, further
  restricted by GMAX. Off-step/nonfinite/out-of-range requests are rejected.
- Seven-digit GETD: /10 V and /100 A, followed by 0=CV or 1=CC. Captured
  nine-digit firmware: /100 V and /1000 A plus mode. Voltage scaling agrees
  with GPAL; **nonzero-current scaling remains unverified under load**.
- Output state uses complementary active-low GPAL ON/OFF indicators, never
  inferred from voltage. SOUT zero enables, one disables; both ACK-only and
  six-digit-data-plus-ACK reply variants are consumed completely.
- GPAL character 65 (zero-based index 64) is the active-low FAULT/OVP lamp:
  `0` means active, `1` inactive; other characters are rejected. The OVP getter
  shares the GPAL cache with output state. Each normal acquisition GPAL poll
  publishes boolean OVP metadata so the frontend can update its OVP indicator.
  This is not inferred from measured voltage, output state or transport errors.
- CR framing, no CRLF. Replies are bounded to 72 bytes and a two-second
  transaction deadline, with up to 500 ms per byte read/write. Commands are
  mutex-serialized through the complete reply, with no blanket inter-command
  delay. Successful VOLT and CURR ACKs wait 1 ms before mandatory GETS readback;
  SOUT and ordinary GETS/GETD/GPAL queries have no inserted delay.
- Framing/I/O failure latches a fault. There is no automatic retry, including
  ambiguous writes or OFF. Inspect physical output if communication fails.
  Explicit close/open flushes queued input and runs the read-only probe;
  it never repeats a failed setting. Query state before deciding further action.

## Backend responsiveness and freshness

Complete validated GETD (V/I/regulation), GETS (target/limit) and GPAL
(output-state) responses are cached as separate groups. The existing read-only
open probe seeds all three; it adds no discovery commands. Acquisition getters
return the last validated group under a separate short mutex, even while a
serial reply or setter holds the wire mutex. Connection ID is immutable; limit
and range reads also avoid the wire mutex. Local sample/time-limit setters use
only the same short cache mutex, even while serial I/O is blocked. There is no
worker thread/framework.

Outside acquisition, groups are reused for 250 ms from reply completion; an
expired group is actually queried, with a second cache check after taking the
wire mutex. A write expires the affected group's idle freshness and performs
mandatory GETS (VOLT/CURR) or GPAL (SOUT) readback before returning success.
Acquisition readers may see the previous acknowledged actual snapshot while a
setter completes, never a fabricated requested value. Any transaction fault or
close invalidates all groups; reconnect publishes usable snapshots only after
the complete probe succeeds. Each group is coherent, not an atomic snapshot
across all groups or across separate frontend getter calls. A single
`cache_ready` gate is published only after all groups are seeded; there are no
per-snapshot valid flags. A small typed query table/helper selects GETD/GETS/GPAL,
validates their envelopes and publishes snapshots. The wire-query routine only
handles the serialized framed transaction, not reverse command-name lookup.

A real timer-only source (fd=-1, events=0) is keyed by the device instance and
removed by that same key, so multiple timers do not share an application-wide
-1 source key. Its 10 ms callback attempts the wire mutex and defers only
when it is busy. This timer is a polling tick, not an inter-command delay; idle
queries and SOUT readbacks can run back-to-back. GETD slots alternate
with age-gated GETS/GPAL slots (500 ms minimum refresh age; approximately 0.5–1 s
status refresh on a responsive, uncontended link). Only GETD counts as a sample.
No global pacing state or open quarantine remains. The scoped VOLT/CURR wait never
holds the cache mutex. Physical I/O is still synchronous
and bounded by the existing two-second transaction deadline: slow hardware can
still block the session thread, setters still wait for serial access, and
competing writes/slow replies can delay status refresh.
Lock order is wire then cache, never cache then wire; the cache lock never
covers serial I/O, sleeps or session callbacks. A wire fault in an active setter
stops acquisition immediately; END is sent unlocked, or deferred until the
current session packet has reached every callback.

Offline held-wire getter latency fell from 900.760 ms / six extra commands to
under 1 ms / zero extra commands (50 ms regression threshold); see the exact
measurements and commands in `tests/bk1697-driver.md`. This is a backend serial-
seam measurement, **not a SmuView UI measurement**. A subsequent no-delay
regression over six setters plus their six actual readbacks fell from
1200.969 ms to 0.114 ms on the instantaneous serial model. The subsequent
simplification removed the unconditional open quarantine: open/flush/probe
changed from 2000.098 ms to 0.006 ms, and held-wire local limit set/get from
300.125 ms to 0.039 ms. No quiet-delay substitute was added. Only queued-input
flush and a successful fresh probe are required for explicit reconnect; no
automatic replay of an ambiguous write was added.

## Observed setting readback timing (parent hardware evidence)

The parent traced dropped GETS replies immediately following successful VOLT
ACKs. Its existing `bk1697/libsigrok/hardware-write-timing-stress.log` reports
30 direct-serial iterations with output OFF, alternating 1.5/1.6 V: 23/30
successes at zero gap, 30/30 at 1 ms and 30/30 at 3 ms. The driver therefore
inserts **1 ms after successful VOLT or CURR ACK before GETS**, and nowhere else.
The parent subsequently completed a 300-frame exercise but observed a dropped
GETS during CURR restoration, with output already OFF. Its
`bk1697/libsigrok/hardware-current-timing-stress.log` reports 26/30 successes at
zero gap and 30/30 at 1 ms. Its `hardware-off-timing-stress.log` reports
SOUT001->GPAL succeeding 30/30 at both zero gap and 1 ms, so SOUT is not delayed.
This is a narrowly scoped observed-firmware workaround, not a manufacturer
timing specification or a guarantee for every unit. No retry, blanket command
gap or open quarantine was restored.

The parent also reports real SmuView startup improving from 7.6 s to 0.55 s,
a nine-getter batch from 1232 ms to 0.14 ms, and capture from 5.2 to 25 Hz during
the preceding simplifications. These are parent measurements, not new hardware
or GUI tests performed here. Final controlled physical fault/OVP activation and
UI presentation testing remain with the parent; offline tests prove decoding,
cached getters, metadata activation/clear and callback ordering only.

## Historical voltage discrepancy

Earlier unloaded-device reports recorded a 2.5 V target with measurements around
3.2–3.4 V, reproduced outside libsigrok and approximately corroborated by GPAL.
No compensating conversion was applied. The parent user's existing artifact
`bk1697/libsigrok/post-restart-comparison.md` subsequently reports recovery after
a power cycle: native and ngscopeclient results track 2.5/3/5 V within 0.01 V.
That artifact is not changed by this refactor. The original fault's root cause
is still unknown; loaded current/CC behavior remains untested. Successful ACKs
and readback do not independently establish electrical accuracy. This refactor
used no hardware and makes no new electrical or GUI verification claim.

## Offline verification

`make check` always runs the GLib parser/setting tests, including literal
captured responses, both formats, malformed replies, and programming bounds.
The original upstream suite runs too when the optional Check library is present.
No hardware is touched by these tests.

On Linux with a GNU-compatible linker and a built static library, run the
optional public-API integration tests:

```sh
BK1697_BUILD_DIR=/absolute/build/path sh tests/bk1697-driver.sh
```

See `tests/bk1697-driver.md`. They simulate only the serial boundary and cover
faults/reconnect, commands/readback, acquisition/limits, serialized control,
callback re-entry/stop/close, and no packets after END. Physical timing and all
possible cross-thread lifecycle schedules are not proved. Synchronous bounded
transactions can block a session callback up to their deadline. GPAL validates
width/range/output indicators, not every display segment's meaning.
