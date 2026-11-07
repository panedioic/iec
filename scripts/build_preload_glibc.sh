#!/bin/bash
# build_preload_glibc.sh — the glibc-ABI IEC guest shim (libiec_preload.so).
#
# Background: on the kLoader route the glibc guests (hdc, the SDK toolchain,
# ...) lose the SIGSYS downgrade after their first execve(), and anything they
# spawn dies on seccomp (the hdc start server was the measured victim). The old
# procmgr solved this by picking a shim compiled against the guest's own libc
# (by PT_INTERP, see ChildRunElf in procmgr.cpp) and setting it as LD_PRELOAD,
# so every exec inside the guest loops back to the process creator and the
# environment is reinstalled. The IEC counterpart is libiec_preload.so built by
# this script: the repository's existing iec_preload target (the INT_PRELOAD
# forms of the tty/exec/fs shims + rpc/sock_client), which SpawnLoader sets as
# LD_PRELOAD whenever it sees a glibc interpreter (EnsureGuestShim in
# exec/exec_loader.cpp).
#
# ⚠ ABI: the shim must match the guest's libc. A musl-built .so preloaded into
# a glibc process crashes at load time (the two libcs are not interchangeable;
# see the ChildRunElf split notes in procmgr), so this script always uses the
# **glibc cross toolchain** (aarch64-linux-gnu-gcc, on the host
# `apt install gcc-aarch64-linux-gnu`). The NEEDED entries point at libc.so.6,
# satisfied by the glibc runtime that ships with the HAP in bundle libs.
#
# Usage: scripts/build_preload_glibc.sh
#   Output: platform/ohos/har/libs/arm64-v8a/libiec_preload.so (HAP → bundle libs)
set -euo pipefail

IEC_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

GLIBC_CC="${AARCH64_GLIBC_CC:-aarch64-linux-gnu-gcc}"
command -v "$GLIBC_CC" >/dev/null || {
	echo "missing toolchain: $GLIBC_CC (on the host: apt install gcc-aarch64-linux-gnu)" >&2
	exit 1
}

OUT_DIR="$IEC_ROOT/platform/ohos/har/libs/arm64-v8a"
OUT="$OUT_DIR/libiec_preload.so"
mkdir -p "$OUT_DIR"

SRC=(
	"$IEC_ROOT/tty/shim.c"
	"$IEC_ROOT/exec/shim.c"
	"$IEC_ROOT/fs/shim.c"
	"$IEC_ROOT/misc/getpass.c"
	"$IEC_ROOT/misc/account.c"
	"$IEC_ROOT/common/rpc.c"
	"$IEC_ROOT/common/sock_client.c"
)
for f in "${SRC[@]}"; do [ -f "$f" ] || { echo "missing source: $f" >&2; exit 1; }; done

echo "==> building $OUT (glibc ABI, $GLIBC_CC)"
"$GLIBC_CC" -O2 -Wall -shared -fPIC \
	-DINT_PRELOAD -DIEC_SHIM_MERGED_OPEN -D_GNU_SOURCE -D__OHOS__ \
	-I"$IEC_ROOT" -I"$IEC_ROOT/common" \
	"${SRC[@]}" -o "$OUT" -ldl -lpthread

RE="${READELF:-aarch64-linux-gnu-readelf}"
command -v "$RE" >/dev/null || RE=readelf

# Check 1: glibc form (NEEDED points at libc.so.6, not musl's libc.so)
if "$RE" -d "$OUT" | grep -q 'NEEDED.*\[libc\.so\]'; then
	echo "ERROR: NEEDED is musl's libc.so -- wrong ABI" >&2
	exit 1
fi
"$RE" -d "$OUT" | grep -q 'NEEDED.*libc\.so\.6' || {
	echo "ERROR: no NEEDED libc.so.6 -- not a glibc build" >&2
	exit 1
}

# Check 2: the preload entry points are really exported (without them the shim
# does nothing)
syms="$(mktemp)"
trap 'rm -f "$syms"' EXIT
"$RE" --dyn-syms "$OUT" > "$syms"
for sym in execve execv execvp execvpe execl execlp execle \
           open openat stat lstat readlink unlink opendir readdir; do
	grep -qE " $sym\$" "$syms" || {
		echo "ERROR: $sym is not exported (preload would be ineffective)" >&2
		exit 1
	}
done

echo "==> done: $OUT"
"$RE" -d "$OUT" | grep NEEDED || true
