#!/bin/bash
# build_git.sh — build git as an IEC shared object (experimental).
#
# The package build in the parent tree produces a guest executable on purpose:
# git starts helpers, hooks, pagers and editors, and that route is the one the
# sandbox has been tested with. This script takes the other road and links the
# whole program into libgit.so, entered through iec_git_main, with IEC's
# fs/tty/exec shims taking over its file and process calls.
#
# What it does:
#   1. builds git's objects with -fPIC, reusing the static zlib/pcre2/openssl/
#      libcurl artifacts the parent tree already built;
#   2. renames the `main` in common-main.o to iec_git_main;
#   3. links git.o + builtin/*.o + libgit.a + IEC shims into libgit.so.
#
# What it does not package yet:
#   * git-remote-http / git-remote-https (remote-curl is a second program);
#   * templates, the CA bundle, and the runtime GIT_EXEC_PATH layout.
#   A basic `git --version` / `git init` / `git status` needs none of those.
#
# Usage:
#   scripts/optional/build_git.sh
#   GIT_VER=2.56.0 scripts/optional/build_git.sh
#
# Output:
#   platform/ohos/har/libs/arm64-v8a/libgit.so
# Entry:
#   iec_git_main (arity 2)
set -euo pipefail

IEC_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PARENT="$(cd "$IEC_ROOT/.." && pwd)"

# The parent tree owns the toolchain paths and the third-party build outputs.
if [ -f "$PARENT/scripts/env.sh" ]; then
	# shellcheck disable=SC1091
	source "$PARENT/scripts/env.sh"
fi

GIT_VER="${GIT_VER:-2.56.0}"
GIT_TARBALL="${GIT_TARBALL:-$PARENT/thirdparty/git-$GIT_VER.tar.xz}"
GIT_SRC="${GIT_SRC:-$IEC_ROOT/build/git-src}"
BUILD="${BUILD:-$IEC_ROOT/build/git-so}"
OUTDIR="${OUTDIR:-$IEC_ROOT/platform/ohos/har/libs/arm64-v8a}"
OUT="$OUTDIR/libgit.so"

OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
CLANG="${CLANG:-$OHOS_SDK/native/llvm/bin/clang}"
CLANG_DIR="${CLANG_DIR:-$(dirname "$CLANG")}"
TARGET="${TARGET:-aarch64-linux-ohos}"
SYSROOT="${SYSROOT:-$OHOS_SDK/native/sysroot}"
AR="${AR:-$CLANG_DIR/llvm-ar}"
RANLIB="${RANLIB:-$CLANG_DIR/llvm-ranlib}"
NM="${NM:-$CLANG_DIR/llvm-nm}"
READELF="${READELF:-$CLANG_DIR/llvm-readelf}"
OBJCOPY="${OBJCOPY:-$CLANG_DIR/llvm-objcopy}"
JOBS="${JOBS:-$(nproc)}"

SYSROOT_EXT="${SYSROOT_EXT:-$PARENT/build/sysroot-ext-arm64-v8a}"
DEPS_LIB="${SYSROOT_EXT_LIB:-$SYSROOT_EXT/usr/lib/aarch64-linux-ohos}"
DEPS_INC="${SYSROOT_EXT_INC:-$SYSROOT_EXT/usr/include}"
OPENSSL="${OPENSSL:-$PARENT/build/openssl}"
CURL_PREFIX="${CURL_PREFIX:-$PARENT/build/curl}"

CC_TARGET="$CLANG --target=$TARGET --sysroot=$SYSROOT"
TRIPLE_FLAGS="--target=$TARGET --sysroot=$SYSROOT"
LD_MUSL="-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1"
NO_FORTIFY="-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"

log() { echo "=== $* ===" >&2; }
err() { echo "ERROR: $*" >&2; exit 1; }

for f in \
	"$DEPS_LIB/libz.a" \
	"$DEPS_LIB/libpcre2-8.a" \
	"$OPENSSL/lib/libssl.a" \
	"$CURL_PREFIX/lib/libcurl.a"; do
	[ -f "$f" ] || err "missing $f (run the parent tree's dependency builds first)"
done

if [ ! -f "$GIT_SRC/Makefile" ]; then
	[ -s "$GIT_TARBALL" ] || err "missing git source tarball: $GIT_TARBALL"
	log "extract $GIT_TARBALL"
	mkdir -p "$GIT_SRC"
	tar xf "$GIT_TARBALL" -C "$GIT_SRC" --strip-components=1
fi

mkdir -p "$BUILD" "$OUTDIR"

log "build pthread cancellation stubs"
$CC_TARGET -O2 -fPIC -c "$IEC_ROOT/compat/pthread_cancel_stub.c" \
	-o "$BUILD/pthread_cancel_stub.o"

log "build git objects (-fPIC)"
make -C "$GIT_SRC" -j"$JOBS" \
	prefix=/usr \
	gitexecdir=/usr/libexec/git-core \
	template_dir=/usr/share/git-core/templates \
	CC="$CC_TARGET -fuse-ld=lld" \
	AR="$AR" RANLIB="$RANLIB" \
	CFLAGS="-O2 -fPIC -I$DEPS_INC $NO_FORTIFY" \
	LDFLAGS="-fuse-ld=lld $TRIPLE_FLAGS -L$DEPS_LIB -L$OPENSSL/lib -L$CURL_PREFIX/lib $LD_MUSL" \
	LDFLAGS_APPEND="$BUILD/pthread_cancel_stub.o" \
	NO_GETTEXT=YesPlease NO_ICONV=YesPlease NO_PERL=YesPlease NO_PYTHON=YesPlease \
	NO_TCLTK=YesPlease NO_EXPAT=YesPlease NO_INSTALL_HARDLINKS=YesPlease \
	SKIP_DASHED_BUILT_INS=YesPlease NO_REGEX=NeedsStartEnd NO_RUST=YesPlease \
	USE_LIBPCRE2=YesPlease \
	ZLIB_PATH="$SYSROOT_EXT/usr" \
	OPENSSLDIR="$OPENSSL" \
	CURLDIR="$CURL_PREFIX" \
	CURL_CFLAGS="-I$CURL_PREFIX/include" \
	CURL_LDFLAGS="-L$CURL_PREFIX/lib -lcurl -lssl -lcrypto -lz" \
	CURL_CONFIG="$CURL_PREFIX/bin/curl-config" \
	git > "$BUILD/git-make.log" 2>&1 \
	|| { tail -50 "$BUILD/git-make.log" >&2; err "git build failed (see $BUILD/git-make.log)"; }

[ -f "$GIT_SRC/git.o" ] || err "git.o was not produced"
[ -f "$GIT_SRC/libgit.a" ] || err "libgit.a was not produced"
[ -f "$GIT_SRC/common-main.o" ] || err "common-main.o was not produced"

log "rename main -> iec_git_main"
"$OBJCOPY" --redefine-sym main=iec_git_main \
	"$GIT_SRC/common-main.o" "$BUILD/common-main-iec.o"
"$NM" "$BUILD/common-main-iec.o" | grep ' T iec_git_main$' >/dev/null \
	|| err "iec_git_main was not created"

# IEC's client-side shims: the linked program's open/stat/execve calls go to the
# services, and tty/shim.c supplies the single open() entry for both modules.
log "build IEC shims"
SHIM_CFLAGS=(-O2 -fPIC -DINT_WARP -D_GNU_SOURCE -I "$IEC_ROOT" -I "$IEC_ROOT/common")
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/tty/shim.c" -o "$BUILD/iec_tty_shim.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -DIEC_SHIM_MERGED_OPEN -c "$IEC_ROOT/fs/shim.c" -o "$BUILD/iec_fs_shim.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/exec/shim.c" -o "$BUILD/iec_exec_shim.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/common/rpc.c" -o "$BUILD/iec_rpc.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/common/sock_client.c" -o "$BUILD/iec_sock_client.o"

WRAPS=(symlink symlinkat readlink readlinkat lstat stat fstatat
       open openat unlink unlinkat rename renameat access faccessat realpath
       link linkat opendir readdir scandir fopen freopen syscall
       close dup dup2 dup3 fcntl ioctl isatty ttyname ttyname_r
       tcgetattr tcsetattr tcgetpgrp tcsetpgrp getpt grantpt unlockpt ptsname
       openpty login_tty forkpty
       execve execv execvp execvpe execl execlp execle)
WRAP_FLAGS=()
for name in "${WRAPS[@]}"; do
	WRAP_FLAGS+=("-Wl,--wrap=$name")
done

# The same objects the executable's link line uses: git.o, every builtin, and
# libgit.a (the library objects), with common-main.o replaced by the renamed
# copy. The static third-party libraries follow.
BUILTIN_OBJS=$(find "$GIT_SRC/builtin" -name '*.o' -print)

log "link $OUT"
# shellcheck disable=SC2086
$CC_TARGET -fuse-ld=lld -shared -o "$OUT" \
	"$BUILD/common-main-iec.o" "$GIT_SRC/git.o" $BUILTIN_OBJS \
	"$GIT_SRC/libgit.a" "$BUILD/pthread_cancel_stub.o" \
	"$BUILD/iec_tty_shim.o" "$BUILD/iec_fs_shim.o" \
	"$BUILD/iec_exec_shim.o" "$BUILD/iec_rpc.o" "$BUILD/iec_sock_client.o" \
	-L"$DEPS_LIB" -L"$OPENSSL/lib" -L"$CURL_PREFIX/lib" \
	-lcurl -lssl -lcrypto -lz -lpcre2-8 -lpthread -ldl -lm \
	-Wl,-soname,libgit.so "${WRAP_FLAGS[@]}"

"$NM" -D "$OUT" | grep ' iec_git_main$' >/dev/null \
	|| err "iec_git_main is not exported"
if "$NM" -D -u "$OUT" | grep -E '__wrap_|__real_' >/dev/null; then
	err "unresolved __wrap_/__real_ symbols"
fi

"$READELF" -d "$OUT" | grep -E 'NEEDED|SONAME' >&2
log "done: $OUT ($(du -h "$OUT" | cut -f1))"
cat >&2 <<EOF

To try it on device, write this launcher to <fake root>/bin/git:

  #IECSO
  /data/storage/el1/bundle/libs/arm64/libgit.so
  iec_git_main
  2

Only the git binary is in the object; remote helpers, templates and the CA
bundle still have to be provided separately.
EOF
