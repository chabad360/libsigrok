# BK Precision 1697

This driver supports **1697 only**. Supply an explicit serial `conn`; there is
no port enumeration, USB-ID match, force-detect, or model autodetection. The
connection is fixed to 9600 baud, 8N1, without flow control. The protocol has no
model identification command: the user selects the model, and the read-only
GMAX/GETS/GETD/GPAL probe checks protocol plausibility, not unique identity.

Scan and open query only those four commands. Close sends no commands. In
particular, no keypad lock, remote/local, output switching, or settings writes
occur as side effects of connection management. Only explicit configuration
requests send VOLT, CURR, or SOUT.

## Evidence and safety

Wire-format evidence is in the sibling project `bk1697/`:
`BK1697Protocol.cpp`, `BK1697Limits.h`, `tests/ProtocolTests.cpp`, and
`hardware-readonly-capture.md`. This is a native C implementation, not a C++
ABI bridge. The offline test includes literal physical captures.

- GMAX and GETS have three-digit voltage/current fields, /10 V and /100 A.
  Observed GMAX is `402502` (40.2 V / 5.02 A). GETS readback is not clamped.
- Writes remain nominal 1–40 V in 0.1 V steps and 0.01–5 A in 0.01 A steps,
  further restricted by GMAX. Unsupported precision, nonfinite values, and
  out-of-range requests are rejected, not silently rounded/clamped.
- Seven-digit GETD uses the manual's /10 V and /100 A scaling. Observed
  nine-digit GETD uses /100 V and /1000 A. The latter voltage scaling matches
  GPAL; nonzero-current scaling still needs an authorized physical load test.
- GETD's last digit is 0 for CV, 1 for CC. Output state is queried from GPAL's
  complementary active-low ON/OFF indicators, not inferred from measurements
  or remembered from SOUT.
- SOUT uses zero to enable and one to disable. Both observed ACK-only and
  documented six-digit data-plus-ACK replies are consumed completely.
- Replies are printable CR-terminated lines followed by `OK\r` (or only
  `OK\r` for ACKs). CRLF, excess width, corrupt fields, and missing ACKs fail.
  Receive storage is bounded to 72 bytes; transactions have a two-second
  overall deadline and at most 500 ms for a byte read/write.
- All commands are mutex-serialized, with at least 100 ms from completion
  to the next command. A failure latches the connection faulted: no automatic
  retries, including ambiguous writes or OFF. Unexpected pending bytes before
  a command also fault rather than being silently discarded.
- Explicit reconnect waits two seconds, flushes stale serial buffers, and
  re-runs the read-only probe. It does not retry a failed setting/output
  request. Reconnection is recovery, not confirmation that a prior write
  succeeded; query the actual state before deciding what to do next.

## Offline tests and parent integration

The parent's test-registration changes should add a **separate test executable**
(not a suite in `tests/main.c`) from:

- `tests/check_bkprecision_1697.c`
- `src/hardware/bkprecision-1697/parse.c` and `parse.h`

Use the normal library include directories/GLib flags, link GLib and `-lm`,
and register the executable with Automake `TESTS`. Compile the parser directly
into the test: its symbols are private and are not public libsigrok API. It
need not depend on driver enablement or Check. Registration was intentionally
left to the parent to avoid concurrent test-build edits.

After configuring libsigrok, an equivalent manual command from its root is:

```sh
cc -std=c99 -Wall -Wextra -Werror $(pkg-config --cflags glib-2.0) \
  -Iinclude -I/path/to/build/include \
  tests/check_bkprecision_1697.c src/hardware/bkprecision-1697/parse.c \
  $(pkg-config --libs glib-2.0) -lm -o /tmp/check_bkprecision_1697
/tmp/check_bkprecision_1697
```

For a normal driver build, regenerate Autotools and configure with
`--enable-bkprecision-1697` and serial support. Driver source registration is
already in `Makefile.am`/`configure.ac`; there is no manual `src/drivers.c`
registration in this tree.

## Remaining verification

Parser/setting tests do not exercise serial timeouts, mutex contention,
connection lifecycle, or acquisition/session integration. A future mock/PTY
integration test should cover partial writes, delayed ACKs, fault latching,
reopen, pacing, limit termination, and concurrent configuration/acquisition.
No hardware access is required or performed by these tests. This implementation
uses synchronous bounded serial transactions in the acquisition callback, like
comparable PSU drivers; a failed query can block the session thread up to the
transaction deadline. GPAL validates encoding width/range and output indicators,
not the complete seven-segment display semantics.
