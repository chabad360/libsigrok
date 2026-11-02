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
  mutex-serialized and paced at least 100 ms after the prior reply completes.
- Framing/I/O failure latches a fault. There is no automatic retry, including
  ambiguous writes or OFF. Inspect physical output if communication fails.
  Explicit close/open waits two seconds, flushes stale buffers, and re-probes;
  it never repeats a failed setting. Query state before deciding further action.

## Physical verification and unresolved voltage discrepancy

The driver was exercised on an unloaded 1697: discovery, settings/readback,
output switching, acquisition, invalid-setting rejection, and live voltage
changes during acquisition worked. However, a 2.5 V target produced reported
measurements around 3.2–3.4 V. Direct serial tests outside libsigrok reproduced
this, and separately encoded GPAL measured-voltage fields agreed approximately.
It persisted after settling and at 20 mA and 1 A current limits. Setpoint replies
continued to report 2.5 V. No compensating conversion has been applied.

**The cause remains unresolved.** Software decoding/firmware interpretation,
actual terminal voltage, calibration, and instrument behavior have not been
independently distinguished. Front-panel inspection and an external meter are
still required. Successful ACKs/readback do not establish electrical accuracy.
The instrument was restored to 1.0 V / 0.01 A, output OFF after the tests.
No loaded current/CC behavior or SmuView GUI has been physically verified.

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
