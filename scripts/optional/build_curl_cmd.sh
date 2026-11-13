#!/bin/bash
# build_curl_cmd.sh — the curl command-line tool as an IEC shared object.
#
# build_curl.sh builds static libcurl.a for the git build and deliberately
# deletes the CLI binary it configured and compiled along the way. This
# script finishes that job: it takes the already-built tool objects from the
# same build tree, renames their main to iec_curl_main, and links them with
# libcurl.a and the IEC shims into libcurlcmd.so -- a `curl` the guest can
# exec, TLS included. toybox wget has no https support, and upstream
# installer scripts (kimi-code's is the concrete case) are curl-or-nothing.
#
# The tool's CA lookup: the session rc exports CURL_CA_BUNDLE at the bundle
# the app ships (usr/share/git-core/cacert.pem, see IecInstallCaBundle), so
# no certificate data is embedded here.
#
# Usage:
#   scripts/optional/build_curl_cmd.sh
#
# Output:
#   platform/ohos/har/libs/arm64-v8a/libcurlcmd.so
# Entry:
#   iec_curl_main (arity 2)
set -euo pipefail

IEC_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENV_FILE="$IEC_ROOT/scripts/env.sh"
[ -f "$ENV_FILE" ] || ENV_FILE="$IEC_ROOT/scripts/env.sh.example"
# shellcheck disable=SC1090
source "$ENV_FILE"
# shellcheck disable=SC1091
source "$IEC_ROOT/scripts/lib/common.sh"

BUILD="${BUILD:-$IEC_ROOT/build/curl-build}"      # build_curl.sh's build tree
CMD_BUILD="${CMD_BUILD:-$IEC_ROOT/build/curl-cmd}"  # renamed object + shims
OUTDIR="${OUTDIR:-$IEC_ROOT/platform/ohos/har/libs/arm64-v8a}"
OUT="$OUTDIR/libcurlcmd.so"

DEPS_LIB="${IEC_DEPS_LIB:-$IEC_DEPS_DIR/usr/lib/aarch64-linux-ohos}"
OPENSSL="${OPENSSL:-$IEC_OPENSSL_DIR}"
CURL_PREFIX="${CURL_PREFIX:-$IEC_CURL_DIR}"

AR="${AR:-$CLANG_DIR/llvm-ar}"
NM="${NM:-$CLANG_DIR/llvm-nm}"
READELF="${READELF:-$CLANG_DIR/llvm-readelf}"
OBJCOPY="${OBJCOPY:-$CLANG_DIR/llvm-objcopy}"

CC_TARGET="$CLANG --target=$TARGET --sysroot=$SYSROOT"

iec_log "curl CLI -> $OUT"

# The libcurl build also produces the tool; keep the two in sync.
[ -f "$CURL_PREFIX/lib/libcurl.a" ] || "$IEC_ROOT/scripts/optional/build_curl.sh"
[ -d "$BUILD/src" ] || iec_err "curl build tree missing: $BUILD (run build_curl.sh)"
make -C "$BUILD" src/curl >> "$BUILD/curl-cmd-make.log" 2>&1 \
	|| { tail -30 "$BUILD/curl-cmd-make.log" >&2; iec_err "curl tool build failed"; }

# The tool objects are prefixed `curl-` by automake (target name); since 8.22
# some of them live in the src/toolx/ subdirectory. Locate the one defining
# main() dynamically instead of hardcoding the file name, and leave it out of
# the object list: its renamed copy takes its place. Objects of other helper
# binaries built in src/ (curlinfo) define their own main and are excluded the
# same way.
MAIN_OBJ=""
TOOL_OBJS=()
for o in "$BUILD"/src/curl-*.o "$BUILD"/src/toolx/curl-*.o; do
	[ -f "$o" ] || continue
	if "$NM" "$o" | grep -q ' T main$'; then
		[ -z "$MAIN_OBJ" ] && MAIN_OBJ="$o"
		continue
	fi
	TOOL_OBJS+=("$o")
done
[ -n "$MAIN_OBJ" ] || iec_err "no object defining main() found in $BUILD/src"
[ "${#TOOL_OBJS[@]}" -gt 20 ] \
	|| iec_err "only ${#TOOL_OBJS[@]} tool objects found -- unexpected layout"

mkdir -p "$CMD_BUILD" "$OUTDIR"

iec_log "rename main -> iec_curl_main ($(basename "$MAIN_OBJ"))"
"$OBJCOPY" --redefine-sym main=iec_curl_main \
	"$MAIN_OBJ" "$CMD_BUILD/curl-main-iec.o"
"$NM" "$CMD_BUILD/curl-main-iec.o" | grep ' T iec_curl_main$' >/dev/null \
	|| iec_err "iec_curl_main was not created"

# exit() in a fork() of the app would run the host's atexit handlers; see the
# comment in compat/exit_wrap.c. The tool calls exit() on fatal paths.
$CC_TARGET -O2 -fPIC -c "$IEC_ROOT/compat/exit_wrap.c" \
	-o "$CMD_BUILD/exit_wrap.o"

# IEC's client-side shims: the linked program's open/stat/execve calls go to
# the services, and tty/shim.c supplies the single open() entry for both
# modules (IEC_SHIM_MERGED_OPEN, same shape as the git build).
SHIM_CFLAGS=(-O2 -fPIC -DINT_WARP -D_GNU_SOURCE -I "$IEC_ROOT" -I "$IEC_ROOT/common")
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/tty/shim.c" -o "$CMD_BUILD/iec_tty_shim.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -DIEC_SHIM_MERGED_OPEN -c "$IEC_ROOT/fs/shim.c" -o "$CMD_BUILD/iec_fs_shim.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/exec/shim.c" -o "$CMD_BUILD/iec_exec_shim.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/common/rpc.c" -o "$CMD_BUILD/iec_rpc.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/common/sock_client.c" -o "$CMD_BUILD/iec_sock_client.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/compat/uname_shim.c" -o "$CMD_BUILD/iec_uname_shim.o"

WRAPS=(symlink symlinkat readlink readlinkat lstat stat fstatat fstat
       chmod fchmod fchmodat
       open openat unlink unlinkat rename renameat access faccessat realpath
       link linkat opendir readdir scandir fopen freopen utimensat futimens posix_spawn posix_spawnp syscall
       close dup dup2 dup3 fcntl ioctl isatty ttyname ttyname_r
       tcgetattr tcsetattr tcgetpgrp tcsetpgrp getpt grantpt unlockpt ptsname
       openpty login_tty forkpty
       execve execv execvp execvpe execl execlp execle
       uname exit)
WRAP_FLAGS=()
for name in "${WRAPS[@]}"; do
	WRAP_FLAGS+=("-Wl,--wrap=$name")
done

iec_log "link $OUT"
$CC_TARGET -fuse-ld=lld -shared -o "$OUT" \
	"$CMD_BUILD/curl-main-iec.o" "${TOOL_OBJS[@]}" \
	"$CURL_PREFIX/lib/libcurl.a" "$CMD_BUILD/exit_wrap.o" \
	"$CMD_BUILD/iec_tty_shim.o" "$CMD_BUILD/iec_fs_shim.o" \
	"$CMD_BUILD/iec_exec_shim.o" "$CMD_BUILD/iec_rpc.o" "$CMD_BUILD/iec_sock_client.o" \
	"$CMD_BUILD/iec_uname_shim.o" \
	-L"$DEPS_LIB" -L"$OPENSSL/lib" \
	-lssl -lcrypto -lz -lpthread -ldl -lm \
	-Wl,-soname,libcurlcmd.so "${WRAP_FLAGS[@]}" \
	-Wl,-Bsymbolic-functions

"$NM" -D "$OUT" | grep ' iec_curl_main$' >/dev/null \
	|| iec_err "iec_curl_main is not exported"
if "$NM" -D -u "$OUT" | grep -E '__wrap_|__real_' >/dev/null; then
	iec_err "unresolved __wrap_/__real_ symbols"
fi

"$READELF" -d "$OUT" | grep -E 'NEEDED|SONAME' >&2
iec_log "curl CLI ready: $OUT"
