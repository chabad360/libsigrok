# Offline BK1697 real-driver integration tests

`bk1697_driver.c` links the **real static libsigrok archive** and selects
`bkprecision-1697` from `sr_driver_list()`. Discovery, configuration, device
lifecycle, sessions and packet conversion use public API calls. No parser or
driver source is compiled into the executable, no driver context is inspected,
and no dependencies are replaced. The internal header is used for serial seam
and session-source prototypes only.

GNU linker `--wrap` replaces exactly eight serial boundary functions:
`serial_open`, `serial_close`, `serial_flush`, `serial_read_nonblocking`,
`serial_read_blocking`, `serial_write_blocking`, `serial_source_add`, and
`serial_source_remove`. The fake serial source delegates to the **real session
source/event loop** using a timer instead of a descriptor. It never opens the
fake `/offline/bk1697-no-hardware` connection. No hardware access is needed.

## Build/run hook

From the libsigrok source directory, after building the native driver with
serial support in `build-hardware`:

```sh
timeout 150 sh tests/bk1697-driver.sh
```

From this workspace root, the exact equivalent is:

```sh
timeout 150 sh libsigrok/tests/bk1697-driver.sh
```

The hook compiles with `cc -std=c99 -Wall -Wextra -Werror`, source/build include
directories, `--whole-archive build-hardware/.libs/libsigrok.a`, the eight wrap
flags, system `pkg-config --static --libs libsigrok` dependencies **excluding
`-lsigrok`**, and `-lm`. If available, `libtirpc` pkg-config libraries are also
included: this archive enables VXI RPC, but the system libsigrok `.pc` does not
list RPC. The executable is temporary and removed on exit. The script can be
invoked with `sh`; no executable permission is required.

To use another build directory/compiler or select one GLib test:

```sh
BK1697_BUILD_DIR=/absolute/path/to/build CC=cc \
  timeout 150 sh tests/bk1697-driver.sh -p /bk1697/concurrent
```

The build directory must correspond to the script's source tree. Tests avoid
calling frame-helper symbols directly, so master versus distro 0.5.2 helper
renames do not require test changes. Distro 0.5.2 has not yet been built/run
with this test; that remains a parent verification step.

## Automatic integration (parent-owned)

Keep this an optional separate executable/hook, not part of `tests/main.c`.
Enable only when the native driver, static archive, serial support and GNU-ld
wrapping are available. The parent may register the shell hook in `TESTS` with
`BK1697_BUILD_DIR` set to the absolute build directory, or translate its compile
command into a conditional `check_PROGRAMS` target. Do not enable these linker
flags globally or require them on other platforms. System pkg-config libraries
are appropriate for the current offline hook; an Automake target should use
the configured build's dependency flags instead. Build the archive before
running the hook. The existing pure-parser test remains independent.

## Checks

Nine GLib cases cover:

- `settings`: explicit-connection-only scan; exactly GMAX/GETS/GETD/GPAL on
  scan/open; no close writes; repeated public open/close rejection without
  transport traffic; 7- and 9-digit GETD scaling and CV/CC; GETS and GPAL
  readback; SOUT active-low polarity, ACK-only and data-plus-ACK replies,
  followed by a query proving full response consumption; minimum/maximum
  VOLT/CURR writes; out-of-range, off-step and nonfinite values rejected
  without wire traffic or settings mutation.
- `faults-reconnect`: zero-byte timeout, read error, short/failed write,
  malformed digits, missing ACK, CRLF, overlong reply, invalid panel
  indicators, pending unsolicited input and preflight read failure. Each
  faults the connection; later valid writes, reads and an open-without-close
  perform **no more wire I/O**. Explicit close/reopen flushes a staged late ACK
  after at least two seconds and performs exactly the four read-only probes,
  without replaying the ambiguous command.
- `deadline`: slow but arriving bytes exceed the overall two-second deadline;
  fault latching and explicit recovery still apply.
- `revalidation`: invalid maximum and out-of-envelope GETS fail reopen and
  close the transport; 40.2 V / 5.02 A headroom is accepted as readback but
  never as a write target.
- `acquisition`: real session start/run/termination, two samples in both GETD
  formats, header/end, balanced frames, two analog channels, numerical
  conversion, precision/unit and regulation metadata; configuration read
  re-entry from analog callbacks proves the wire mutex is not held there.
- `time-limit`: time-limit termination before any measurement frame.
- `acquisition-fault`: a polling timeout emits no sample, stops cleanly,
  latches the fault, then recovers and acquires after explicit reconnect.
- `concurrent`: a condition-variable barrier holds an acquisition GETD reply
  while another thread attempts settings. No preflight read or write can
  enter the transport while that reply is in flight. VOLT/CURR/SOUT and
  distinct GETS/GPAL replies complete while acquisition is still active;
  three frames retain correct measurement values and no queued bytes remain.
- `smaller-maximum`: GMAX-derived advertised ranges and stricter rejection,
  with unchanged readback.

Every completed successful transaction checks the next command is paced at
least 100 ms later. Every flush checks the two-second reconnect quarantine.
No clock/sleep functions are mocked. Allow about 95 seconds for the whole
suite and an external watchdog (150 seconds on this machine); deadlocks should
fail the test job instead of hanging indefinitely.

## Scope/limits

This checks native driver behavior at the serial API, not libserialport,
physical UART framing, USB/serial adapters or real-device electrical behavior.
The nonzero-current 9-digit scaling is a protocol expectation, not new hardware
evidence. Transport open/flush/close/source-registration failures, disabled
channels, and stress testing arbitrary thread schedules are not exhaustive.
Only one deterministic competing configuration thread is used. Driver sources
and portable build integration are intentionally untouched.
