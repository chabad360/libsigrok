#!/bin/sh
# Optional Linux/GNU-ld test hook, deliberately separate from portable tests.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${BK1697_BUILD_DIR:-$src/build-hardware}
exe=$(mktemp "${TMPDIR:-/tmp}/bk1697-driver.XXXXXX")
trap 'rm -f "$exe"' EXIT HUP INT TERM
# The system .pc supplies dependencies only; never link its libsigrok.
# VXI-enabled archives also need RPC, absent from some distro .pc files.
packages=libsigrok
if pkg-config --exists libtirpc; then
    packages="$packages libtirpc"
fi
libs=
for flag in $(pkg-config --static --libs $packages); do
    case "$flag" in
        -lsigrok) ;;
        *) libs="$libs $flag" ;;
    esac
done
${CC:-cc} -std=c99 -Wall -Wextra -Werror \
    $(pkg-config --cflags libsigrok) \
    -I"$build" -I"$build/include" -I"$src/include" -I"$src/src" \
    "$src/tests/bk1697_driver.c" \
    -Wl,--whole-archive "$build/.libs/libsigrok.a" -Wl,--no-whole-archive \
    -Wl,--wrap=serial_open,--wrap=serial_close,--wrap=serial_flush \
    -Wl,--wrap=serial_read_nonblocking,--wrap=serial_read_blocking \
    -Wl,--wrap=serial_write_blocking,--wrap=serial_source_add,--wrap=serial_source_remove \
    -Wl,--wrap=sr_session_fd_source_add,--wrap=sr_session_source_remove_internal \
    $libs -lm -o "$exe"
"$exe" "$@"
