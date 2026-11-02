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
  target/current limit, GPAL output-state readback, and CV/CC regulation.
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
- CR framing, no CRLF. Replies are bounded to 72 bytes and a two-second
  transaction deadline, with up to 500 ms per byte read/write. Commands are
  mutex-serialized through the complete reply, with no inserted inter-command
  delay. No verified manufacturer requirement supports the former 100 ms gap.
- Framing/I/O failure latches a fault. There is no automatic retry, including
  ambiguous writes or OFF. Inspect physical output if communication fails.
  Explicit close/open waits two seconds, flushes stale buffers, and re-probes;
  it never repeats a failed setting. Query state before deciding further action.

## Backend responsiveness and freshness

Complete validated GETD (V/I/regulation), GETS (target/limit) and GPAL
(output-state) responses are cached as separate groups. The existing read-only
open probe seeds all three; it adds no discovery commands. Acquisition getters
return the last validated group under a separate short mutex, even while a
serial reply or setter holds the wire mutex. Connection ID is immutable; limit
and range reads also avoid the wire mutex. There is no worker thread/framework.

Outside acquisition, groups are reused for 250 ms from reply completion; an
expired group is actually queried, with a second cache check after taking the
wire mutex. A write expires the affected group's idle freshness and performs
mandatory GETS (VOLT/CURR) or GPAL (SOUT) readback before returning success.
Acquisition readers may see the previous acknowledged actual snapshot while a
setter completes, never a fabricated requested value. Any transaction fault or
close invalidates all groups; reconnect publishes usable snapshots only after
the complete probe succeeds. Each group is coherent, not an atomic snapshot
across all groups or across separate frontend getter calls.

A 10 ms source timer attempts the wire mutex without waiting and defers only
when it is busy. This timer is a polling tick, not an inter-command delay; idle
queries and mandatory setter readbacks can run back-to-back. GETD slots alternate
with age-gated GETS/GPAL slots (500 ms minimum refresh age; approximately 0.5–1 s
status refresh on a responsive, uncontended link). Only GETD counts as a sample.
No inter-command pacing state or sleep remains. Physical I/O is still synchronous
and bounded by the existing two-second transaction deadline: slow hardware can
still block the session thread, setters still wait for serial access, and
competing writes/slow replies can delay status refresh.
Lock order is wire then cache, never cache then wire; the cache lock never
covers serial I/O, sleeps or session callbacks.

Offline held-wire getter latency fell from 900.760 ms / six extra commands to
under 1 ms / zero extra commands (50 ms regression threshold); see the exact
measurements and commands in `tests/bk1697-driver.md`. This is a backend serial-
seam measurement, **not a SmuView UI measurement**. A subsequent no-delay
regression over six setters plus their six actual readbacks fell from
1200.969 ms to 0.114 ms on the instantaneous serial model. The two-second
open quarantine remains separate and unchanged.

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
