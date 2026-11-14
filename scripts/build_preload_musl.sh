#!/bin/bash
# build_preload_musl.sh — the musl-ABI IEC guest shim (libiec_preload_musl.so).
#
# The glibc counterpart (build_preload_glibc.sh) covers guests like hdc and
# the SDK toolchain. This script builds the same INT_PRELOAD shim set against
# the **OHOS musl** (the SDK clang targeting aarch64-linux-ohos -- the exact
# recipe the command .so's use for their baked-in shims), for musl guests on
# the kLoader route: a musl-dynamic guest's interpreter is resolved to the
# bundle's ld-musl-aarch64.so.1, and without a preload its execve() lands on
# the kernel and dies with EACCES like any guest (measured with the musl
# testprogs: exec self/sibling/dependent all fail 125/126/124, and the fs
# emulations -- symlink, the pty block -- are absent too).
#
# Old procmgr kept two shim builds and picked one by the guest's PT_INTERP
# (ChildRunElf); EnsureGuestShim in exec/exec_loader.cpp does the same split
# now -- ld-linux -> libiec_preload.so, ld-musl -> libiec_preload_musl.so.
#
# ⚠ ABI: this shim must match the libc the musl guest actually runs on, which
# is the bundle's OHOS musl (ReadInterpImage resolves the interpreter basename
# into bundle libs). Compiling with the OHOS clang/sysroot gives exactly that
# ABI; the NEEDED entry is musl's libc.so.
#
# Static musl guests are out of reach by design: a -static binary has no
# dynamic loader, so LD_PRELOAD cannot reach it (same limitation as the old
# procmgr's static guests).
#
# Usage: scripts/build_preload_musl.sh
#   Output: platform/ohos/har/libs/arm64-v8a/libiec_preload_musl.so (HAP -> bundle libs)
set -euo pipefail

IEC_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$IEC_ROOT/scripts/env.sh" 2>/dev/null || source "$IEC_ROOT/scripts/env.sh.example"

CC_TARGET="$CLANG --target=$TARGET --sysroot=$SYSROOT"

OUT_DIR="$IEC_ROOT/platform/ohos/har/libs/arm64-v8a"
OUT="$OUT_DIR/libiec_preload_musl.so"
mkdir -p "$OUT_DIR"

SRC=(
	"$IEC_ROOT/tty/shim.c"
	"$IEC_ROOT/exec/shim.c"
	"$IEC_ROOT/fs/shim.c"
	"$IEC_ROOT/common/rpc.c"
	"$IEC_ROOT/common/sock_client.c"
)
for f in "${SRC[@]}"; do [ -f "$f" ] || { echo "missing source: $f" >&2; exit 1; }; done

echo "==> building $OUT (musl ABI, OHOS clang)"
$CC_TARGET -O2 -Wall -shared -fPIC \
	-DINT_PRELOAD -DIEC_SHIM_MERGED_OPEN -D_GNU_SOURCE \
	-I"$IEC_ROOT" -I"$IEC_ROOT/common" \
	"${SRC[@]}" -o "$OUT"

RE="${READELF:-$CLANG_DIR/llvm-readelf}"

# Check 1: musl form (NEEDED is musl's libc.so, not glibc's libc.so.6)
if "$RE" -d "$OUT" | grep -q 'NEEDED.*libc\.so\.6'; then
	echo "ERROR: NEEDED is glibc's libc.so.6 -- wrong ABI" >&2
	exit 1
fi
"$RE" -d "$OUT" | grep -q 'NEEDED.*\[libc\.so\]' || {
	echo "ERROR: no NEEDED libc.so -- not a musl build" >&2
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
