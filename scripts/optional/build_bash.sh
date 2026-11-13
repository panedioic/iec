#!/bin/bash
# build_bash.sh — build GNU bash as an IEC shared object (experimental).
#
# Why: upstream installer scripts assume bash -- `set -euo pipefail`, `[[ =~ ]]`
# with BASH_REMATCH. mksh cannot run them; the kimi-code install.sh is the
# concrete victim. The .so follows the git pattern: the whole program linked
# into libbash.so, entered through iec_bash_main. bash's main takes
# (argc, argv, envp), so the launcher must say arity 3 (EntryAbility passes it).
#
# Configure notes:
#   * --without-bash-malloc: bash's malloc breaks on musl.
#   * --disable-nls: no gettext on the OHOS sysroot.
#   * No curses/termcap exists in the sysroot, so readline falls back to the
#     termcap implementation bundled in bash (lib/termcap). tgetent finds no
#     terminal database at runtime, so interactive line editing degrades;
#     script use -- `curl ... | bash`, which is what this build exists for --
#     never touches that path.
#   * Process substitution wants /dev/fd; bash falls back to FIFOs by itself.
#
# The link reuses the exact objects upstream links into the `bash` binary:
# they are captured from `make -n bash` before the real build (once the
# binary exists, make -n would print nothing), then replayed against a
# -shared link with the IEC shims. `main` in shell.o is renamed to
# iec_bash_main by objcopy, the same trick build_git.sh uses on common-main.o.
#
# Usage:
#   scripts/optional/build_bash.sh
#   BASH_VER=5.2.15 scripts/optional/build_bash.sh
#
# Output:
#   platform/ohos/har/libs/arm64-v8a/libbash.so
# Entry:
#   iec_bash_main (arity 3)
set -euo pipefail

IEC_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENV_FILE="$IEC_ROOT/scripts/env.sh"
[ -f "$ENV_FILE" ] || ENV_FILE="$IEC_ROOT/scripts/env.sh.example"
# shellcheck disable=SC1090
source "$ENV_FILE"
# shellcheck disable=SC1091
source "$IEC_ROOT/scripts/lib/common.sh"

VER="${BASH_VER:-${IEC_BASH_VER:-5.2.15}}"
URL="${IEC_BASH_URL:-https://ftp.gnu.org/gnu/bash/bash-$VER.tar.gz}"
TAR="$IEC_THIRDPARTY/bash-$VER.tar.gz"
SRC="$IEC_THIRDPARTY/bash-$VER"
BUILD="${BUILD:-$IEC_ROOT/build/bash-so}"
OUTDIR="${OUTDIR:-$IEC_ROOT/platform/ohos/har/libs/arm64-v8a}"
OUT="${OUT:-$OUTDIR/libbash.so}"

AR="${AR:-$CLANG_DIR/llvm-ar}"
RANLIB="${RANLIB:-$CLANG_DIR/llvm-ranlib}"
NM="${NM:-$CLANG_DIR/llvm-nm}"
READELF="${READELF:-$CLANG_DIR/llvm-readelf}"
OBJCOPY="${OBJCOPY:-$CLANG_DIR/llvm-objcopy}"

CC_TARGET="$CLANG --target=$TARGET --sysroot=$SYSROOT"
NO_FORTIFY="-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"

iec_log "bash $VER -> $OUT"

if [ ! -f "$SRC/configure" ]; then
	if [ ! -s "$TAR" ]; then
		iec_log "download $URL"
		curl -L --retry 3 -m 600 -o "$TAR.part" "$URL" \
			|| iec_err "bash download failed"
		mv "$TAR.part" "$TAR"
	fi
	tar xzf "$TAR" -C "$IEC_THIRDPARTY"
fi
[ -f "$SRC/configure" ] || iec_err "unexpected bash source layout: $SRC"

mkdir -p "$BUILD" "$OUTDIR"

mkdir -p "$BUILD" "$OUTDIR"

if [ ! -f "$SRC/shell.o" ]; then
	iec_log "configure bash (in-tree, musl cross)"
	( cd "$SRC" && \
	  ./configure \
		--host=aarch64-unknown-linux-musl --build=x86_64-pc-linux-gnu \
		--without-bash-malloc --disable-nls --disable-rpath \
		CC="$CC_TARGET -fuse-ld=lld" \
		CFLAGS="-O2 -fPIC $NO_FORTIFY" \
		LDFLAGS="-fuse-ld=lld --target=$TARGET --sysroot=$SYSROOT" \
		AR="$AR" RANLIB="$RANLIB" \
		> "$BUILD/configure.log" 2>&1 ) \
		|| { tail -40 "$BUILD/configure.log" >&2; iec_err "bash configure failed"; }

	iec_log "build bash"
	make -C "$SRC" -j"$JOBS" bash > "$BUILD/make.log" 2>&1 \
		|| { tail -40 "$BUILD/make.log" >&2; iec_err "bash build failed"; }
fi
[ -f "$SRC/bash" ] || iec_err "bash binary was not produced"

# Capture the upstream link line -- the authoritative object/library list for
# the `bash` binary, including the bundled readline/history/glob/tilde/
# builtins archives and the termcap library configure fell back to.
#
# The line is read back from the build log of the real `make bash` run rather
# than replayed via `make -n`: a dry run before the build dies in the
# builtins sub-make (the generated pathnames.h does not exist yet), and one
# after the build finds everything up to date; --always-make would want to
# regenerate the Makefile itself and needs autoconf. The log of the actual
# build always contains the executed link command.
LINK_LINE="$(grep -e ' -o bash ' "$BUILD/make.log" | tail -1)"
[ -n "$LINK_LINE" ] || iec_err "no link line found in $BUILD/make.log"

LINK_TOKENS=()
for tok in $LINK_LINE; do
	case "$tok" in
		-o) ;;
		bash) ;;
		*.o|*.a)
			case "$tok" in
				/*) LINK_TOKENS+=("$tok") ;;
				*)  LINK_TOKENS+=("$SRC/$tok") ;;
			esac
			;;
		-L*)  # -L./builtins and friends: make them absolute against $SRC
			case "$tok" in
				-L/*) LINK_TOKENS+=("$tok") ;;
				*)    LINK_TOKENS+=("-L$SRC/${tok#-L}") ;;
			esac
			;;
		-l*) LINK_TOKENS+=("$tok") ;;   # keep whatever configure asked for
	esac
done
[ "${#LINK_TOKENS[@]}" -gt 10 ] \
	|| iec_err "link line parse found only ${#LINK_TOKENS[@]} objects"
printf '%s\n' "${LINK_TOKENS[@]}" | grep -q 'builtins' \
	|| iec_err "builtins library missing from the captured link line"

# exit() in a fork() of the app would run the host's atexit handlers; see the
# comment in compat/exit_wrap.c. bash calls exit() everywhere.
$CC_TARGET -O2 -fPIC -c "$IEC_ROOT/compat/exit_wrap.c" \
	-o "$BUILD/exit_wrap.o"

iec_log "rename main -> iec_bash_main"
"$OBJCOPY" --redefine-sym main=iec_bash_main \
	"$SRC/shell.o" "$BUILD/shell-iec.o"
"$NM" "$BUILD/shell-iec.o" | grep ' T iec_bash_main$' >/dev/null \
	|| iec_err "iec_bash_main was not created"

# Replace the original shell.o in the replayed list with the renamed copy.
FINAL_TOKENS=()
for tok in "${LINK_TOKENS[@]}"; do
	if [ "$tok" = "$SRC/shell.o" ]; then
		FINAL_TOKENS+=("$BUILD/shell-iec.o")
	else
		FINAL_TOKENS+=("$tok")
	fi
done

# IEC's client-side shims: the linked program's open/stat/execve calls go to
# the services, and tty/shim.c supplies the single open() entry for both
# modules (IEC_SHIM_MERGED_OPEN, same shape as the git build).
SHIM_CFLAGS=(-O2 -fPIC -DINT_WARP -D_GNU_SOURCE -I "$IEC_ROOT" -I "$IEC_ROOT/common")
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/tty/shim.c" -o "$BUILD/iec_tty_shim.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -DIEC_SHIM_MERGED_OPEN -c "$IEC_ROOT/fs/shim.c" -o "$BUILD/iec_fs_shim.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/exec/shim.c" -o "$BUILD/iec_exec_shim.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/common/rpc.c" -o "$BUILD/iec_rpc.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/common/sock_client.c" -o "$BUILD/iec_sock_client.o"
$CC_TARGET "${SHIM_CFLAGS[@]}" -c "$IEC_ROOT/compat/uname_shim.c" -o "$BUILD/iec_uname_shim.o"

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
# No -Wl,-Bsymbolic-functions here, unlike the other command builds.
#
# Measured on-device: with it, EVERY `bash -c <anything>` dies in a SIGSEGV
# storm (uniform stat(".") recursion, sp in the thread guard page, garbage
# lr) while `bash --version` -- which exits before shell initialization --
# survives; the identical object set without -Bsymbolic-functions runs
# cleanly (exit codes, [[ =~ ]], pipefail, script files all verified), and a
# shim-less .so of the same objects is fine both ways, so the wraps and the
# route are exonerated. The other suspects were eliminated on the way: arity
# 2 vs 3 makes no difference, and the legacy bshguest shim is not loaded in
# the server process at all. Mechanism (musl dlopen interposition interacting
# with -Bsymbolic direct binding and bash's bundled libc-duplicate functions
# such as glob/fnmatch) is plausible but not proven; if this ever needs
# revisiting, bisect with BASH_NOBSYM=1 / BASH_NOSHIM=1 knobs -- kept below
# exactly for that.
BSYM_FLAGS=()
if [ "${BASH_FORCE_BSYM:-0}" = "1" ]; then
	BSYM_FLAGS=(-Wl,-Bsymbolic-functions)
	iec_log "bisect: -Bsymbolic-functions forced back on"
fi
if [ "${BASH_NOSHIM:-0}" = "1" ]; then
	iec_log "bisect: shims and wraps dropped"
	$CC_TARGET -fuse-ld=lld -shared -o "$OUT" \
		"${FINAL_TOKENS[@]}" \
		-Wl,-soname,"$(basename "$OUT")" \
		"${BSYM_FLAGS[@]}"
else
	$CC_TARGET -fuse-ld=lld -shared -o "$OUT" \
		"${FINAL_TOKENS[@]}" \
		"$BUILD/exit_wrap.o" \
		"$BUILD/iec_tty_shim.o" "$BUILD/iec_fs_shim.o" \
		"$BUILD/iec_exec_shim.o" "$BUILD/iec_rpc.o" "$BUILD/iec_sock_client.o" \
	"$BUILD/iec_uname_shim.o" \
		-Wl,-soname,"$(basename "$OUT")" "${WRAP_FLAGS[@]}" \
		"${BSYM_FLAGS[@]}"
fi

"$NM" -D "$OUT" | grep ' iec_bash_main$' >/dev/null \
	|| iec_err "iec_bash_main is not exported"
if "$NM" -D -u "$OUT" | grep -E '__wrap_|__real_' >/dev/null; then
	iec_err "unresolved __wrap_/__real_ symbols"
fi

"$READELF" -d "$OUT" | grep -E 'NEEDED|SONAME' >&2
iec_log "bash ready: $OUT"
