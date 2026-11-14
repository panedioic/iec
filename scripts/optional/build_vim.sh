#!/bin/bash
# build_vim.sh — build Vim as an IEC shared object.
#
# Why: the command .so on the device today (BetterSH scripts/build_vim.sh) was
# linked with the legacy guest shims (bsh_client.o talking the old CMD TTY
# protocol). Under the IEC exec route nothing serves that protocol, so every
# termios/ioctl call in the guest lands on the raw socketpair: isatty(0/1)
# answer 0 ("Vim: Warning: Input is not from a terminal"), tcsetattr() fails,
# and vim renders its welcome screen but never sees a keystroke (input sits in
# the tty server's canonical buffer until a newline arrives). This build
# follows the bash/git pattern instead: the whole of vim linked into one
# libvim.so with the IEC client shims baked in (tty/fs/exec over the IEC
# services), where tcsetattr/isatty/winsize actually reach the tty server --
# measured on-device with the bash build, which uses the identical shim set.
#
# Configure notes (mirrors the working BetterSH build, which is the proven
# recipe on this sandbox):
#   * ncurses is cross-built static + widec with an embedded
#     xterm-256color terminfo fallback (--with-fallbacks), so the guest needs
#     no terminfo database on the device. Host /usr/bin/tic is required for
#     that fallback generation.
#   * vim: FEAT_NORMAL, no GUI/x/wayland, multibyte on, --with-tlib=ncursesw.
#     out-of-tree ncurses, in-tree vim configure (vim does not support
#     configuring outside src/). A configure change needs a full rebuild:
#     distclean + rm objects (a stale objects/ once dragged GUI undefineds
#     into the .so and dlopen failed on it).
#
# Runtime: a minimal runtime subset (defaults.vim, filetype/syntax/core
# highlight files) is packed to platform/ohos/har/src/main/resources/rawfile/
# tools/vim-runtime.tar, next to npm.tar. The host app unpacks it to
# <sysroot>/vim-runtime and exports VIMRUNTIME=... there (EntryAbility
# bootstrap), so vim finds defaults.vim and syntax on startup without the
# 50 MB full runtime.
#
# Registration (host app side, mirrors bash):
#   add('vim', 'libvim.so', 'iec_vim_main');     // arity 2, primary 0
#
# Usage:
#   scripts/optional/build_vim.sh
#   VIM_VER=9.2.1036 scripts/optional/build_vim.sh
#
# Output:
#   platform/ohos/har/libs/arm64-v8a/libvim.so
# Entry:
#   iec_vim_main (arity 2)
set -euo pipefail

IEC_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENV_FILE="$IEC_ROOT/scripts/env.sh"
[ -f "$ENV_FILE" ] || ENV_FILE="$IEC_ROOT/scripts/env.sh.example"
# shellcheck disable=SC1090
source "$ENV_FILE"
# shellcheck disable=SC1091
source "$IEC_ROOT/scripts/lib/common.sh"

VER="${VIM_VER:-${IEC_VIM_VER:-9.2.1036}}"
NC_VER="${NC_VER:-6.5}"
URL="${IEC_VIM_URL:-https://github.com/vim/vim/archive/refs/tags/v$VER.tar.gz}"
TAR="$IEC_THIRDPARTY/vim-$VER.tar.gz"
# A git checkout named exactly "vim" wins (someone fetched it on purpose);
# then the tagged tarball; finally the host repository's thirdparty/vim (used
# in place, same tree the legacy BetterSH build configures in-tree -- configure
# artifacts there are fine, this build reconfigures the same way it does).
SRC=""
for cand in "$IEC_THIRDPARTY/vim" "$IEC_THIRDPARTY/vim-$VER"; do
	[ -f "$cand/src/version.h" ] && { SRC="$cand"; break; }
done
if [ -z "$SRC" ]; then
	if [ -f "$IEC_ROOT/../thirdparty/vim/src/version.h" ]; then
		SRC="$IEC_ROOT/../thirdparty/vim"
		iec_log "using host repo checkout: $SRC"
	else
		if [ ! -s "$TAR" ]; then
			iec_log "download $URL"
			curl -L --retry 3 -m 600 -o "$TAR.part" "$URL" \
				|| iec_err "vim download failed"
			mv "$TAR.part" "$TAR"
		fi
		rm -rf "$IEC_THIRDPARTY/vim-$VER"
		tar xzf "$TAR" -C "$IEC_THIRDPARTY"
		SRC="$IEC_THIRDPARTY/vim-$VER"
	fi
fi
[ -f "$SRC/src/version.h" ] || iec_err "unexpected vim source layout: $SRC"

# ncurses and zlib: prefer a fetched tree, fall back to the host repository's
# submodules (this is where the proven configure flags come from anyway).
NC_SRC=""
for cand in "$IEC_THIRDPARTY/ncurses-$NC_VER" "$IEC_THIRDPARTY/ncurses"; do
	[ -f "$cand/configure" ] && { NC_SRC="$cand"; break; }
done
[ -n "$NC_SRC" ] || NC_SRC="$IEC_ROOT/../thirdparty/ncurses"
[ -f "$NC_SRC/configure" ] || iec_err "ncurses source not found ($NC_SRC)"

ZSRC=""
for cand in "$IEC_THIRDPARTY/zlib-1.3.1" "$IEC_THIRDPARTY/zlib"; do
	[ -f "$cand/configure" ] && { ZSRC="$cand"; break; }
done
[ -n "$ZSRC" ] || ZSRC="$IEC_ROOT/../thirdparty/zlib"
[ -f "$ZSRC/configure" ] || iec_err "zlib source not found ($ZSRC)"

BUILD="${BUILD:-$IEC_ROOT/build/vim-so}"
NC_BUILD="$BUILD/ncurses"
ZBUILD="$BUILD/zlib"
OUTDIR="${OUTDIR:-$IEC_ROOT/platform/ohos/har/libs/arm64-v8a}"
OUT="${OUT:-$OUTDIR/libvim.so}"

AR="${AR:-$CLANG_DIR/llvm-ar}"
RANLIB="${RANLIB:-$CLANG_DIR/llvm-ranlib}"
NM="${NM:-$CLANG_DIR/llvm-nm}"
READELF="${READELF:-$CLANG_DIR/llvm-readelf}"
OBJCOPY="${OBJCOPY:-$CLANG_DIR/llvm-objcopy}"

CC_TARGET="$CLANG --target=$TARGET --sysroot=$SYSROOT"
NO_FORTIFY="-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"

iec_log "vim $VER -> $OUT (ncurses $NC_VER from $NC_SRC)"

if [ ! -x /usr/bin/tic ]; then
	iec_err "host tic is required (ncurses terminfo fallback generation)"
fi
mkdir -p "$BUILD" "$OUTDIR"

iec_log "1) ncurses (cross static widec, fallback xterm-256color) -> $NC_BUILD"
if [ ! -f "$NC_BUILD/config.status" ]; then
	mkdir -p "$NC_BUILD"
	( cd "$NC_BUILD" && CC="$CC_TARGET -fuse-ld=lld" CFLAGS="-O2 -fPIC $NO_FORTIFY" \
	  "$NC_SRC/configure" \
		--host=aarch64-unknown-linux-musl --build=x86_64-pc-linux-gnu \
		--prefix=/usr --without-shared --without-debug --without-ada \
		--without-cxx --without-progs --without-manpages --without-tests \
		--with-fallbacks=xterm-256color > "$BUILD/ncurses-config.log" 2>&1 ) \
		|| { tail -40 "$BUILD/ncurses-config.log" >&2; iec_err "ncurses configure failed"; }
fi
make -C "$NC_BUILD" -j"$JOBS" libs > "$BUILD/ncurses-make.log" 2>&1 \
	|| { tail -40 "$BUILD/ncurses-make.log" >&2; iec_err "ncurses build failed"; }
[ -f "$NC_BUILD/lib/libncursesw.a" ] \
	|| iec_err "libncursesw.a was not produced"

iec_log "2) zlib (cross static PIC) -> $ZBUILD"
if [ ! -f "$ZBUILD/Makefile" ]; then
	mkdir -p "$ZBUILD"
	cp -a "$ZSRC/." "$ZBUILD/"
	( cd "$ZBUILD" && CC="$CC_TARGET -fuse-ld=lld" CFLAGS="-O2 -fPIC $NO_FORTIFY" \
	  ./configure --static > "$BUILD/zlib-config.log" 2>&1 ) \
		|| { tail -40 "$BUILD/zlib-config.log" >&2; iec_err "zlib configure failed"; }
fi
make -C "$ZBUILD" -j"$JOBS" > "$BUILD/zlib-make.log" 2>&1 \
	|| { tail -40 "$BUILD/zlib-make.log" >&2; iec_err "zlib build failed"; }
[ -f "$ZBUILD/libz.a" ] || iec_err "libz.a was not produced"

# vim only supports configuring from inside src/. Like the proven BetterSH
# build: any configure change must be followed by distclean + objects wipe, a
# stale objects/ once carried GUI undefineds into the aggregate and dlopen
# rejected the .so.
iec_log "3) vim (FEAT_NORMAL, in-tree configure) src=$SRC/src"
if [ ! -f "$SRC/src/auto/config.mk" ]; then
	make -C "$SRC/src" distclean >/dev/null 2>&1 || true
	rm -rf "$SRC/src/objects"
	rm -f "$SRC/src/auto/config.cache"
	( cd "$SRC/src" && \
	  CC="$CC_TARGET -fuse-ld=lld" CFLAGS="-O2 -fPIC $NO_FORTIFY" \
	  CPPFLAGS="-I$NC_BUILD/include -I$ZBUILD" \
	  LDFLAGS="-L$NC_BUILD/lib -L$ZBUILD" \
	  PKG_CONFIG=false \
	  ./configure --host=aarch64-unknown-linux-musl --build=x86_64-pc-linux-gnu \
		--with-tlib=ncursesw --with-features=normal --without-x \
		--with-wayland=no --disable-gui --disable-channel --disable-netbeans \
		--disable-xim --enable-multibyte --with-compiledby=IEC \
		--prefix=/data/storage/el2/base/files \
		> "$BUILD/vim-config.log" 2>&1 ) \
		|| { tail -40 "$BUILD/vim-config.log" >&2; iec_err "vim configure failed"; }
fi
make -C "$SRC/src" -j"$JOBS" vim > "$BUILD/vim-make.log" 2>&1 \
	|| { tail -40 "$BUILD/vim-make.log" >&2; iec_err "vim build failed"; }
[ -f "$SRC/src/objects/main.o" ] || iec_err "vim objects are missing"

# The object list comes from the objects/ directory: make vim builds exactly
# those into the vim binary (host tool objects live elsewhere). Kept as an
# array -- a quoted string would make the shell pass literal quote characters
# on to the driver.
OBJS=()
for o in "$SRC"/src/objects/*.o; do
	OBJS+=("$o")
done
[ "${#OBJS[@]}" -gt 0 ] || iec_err "no vim objects found"

# exit() in a fork() of the app would run the host's atexit handlers; see
# compat/exit_wrap.c. vim calls exit() on quit.
$CC_TARGET -O2 -fPIC -c "$IEC_ROOT/compat/exit_wrap.c" -o "$BUILD/exit_wrap.o"

iec_log "rename main -> iec_vim_main"
"$OBJCOPY" --redefine-sym main=iec_vim_main \
	"$SRC/src/objects/main.o" "$BUILD/main-iec.o"
"$NM" "$BUILD/main-iec.o" | grep ' T iec_vim_main$' >/dev/null \
	|| iec_err "iec_vim_main was not created"
# Swap the renamed main.o into the object list (it must be the only copy of
# main() in the link).
for i in "${!OBJS[@]}"; do
	if [ "${OBJS[$i]}" = "$SRC/src/objects/main.o" ]; then
		OBJS[$i]="$BUILD/main-iec.o"
	fi
done

# IEC's client-side shims: tty/fs/exec over the IEC services. tty/shim.c
# supplies the single open() entry for the fs module (IEC_SHIM_MERGED_OPEN),
# same shape as the bash build.
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
# No -Wl,-Bsymbolic-functions: with the IEC shims it killed bash outright
# (SIGSEGV storm, see build_bash.sh) -- vim gets the same treatment.
$CC_TARGET -fuse-ld=lld -shared -o "$OUT" \
	"${OBJS[@]}" \
	"$NC_BUILD/lib/libncursesw.a" "$ZBUILD/libz.a" \
	"$BUILD/exit_wrap.o" \
	"$BUILD/iec_tty_shim.o" "$BUILD/iec_fs_shim.o" \
	"$BUILD/iec_exec_shim.o" "$BUILD/iec_rpc.o" "$BUILD/iec_sock_client.o" \
	"$BUILD/iec_uname_shim.o" \
	-Wl,-soname,"$(basename "$OUT")" \
	"${WRAP_FLAGS[@]}"

"$NM" -D "$OUT" | grep ' iec_vim_main$' >/dev/null \
	|| iec_err "iec_vim_main is not exported"
if "$NM" -D -u "$OUT" | grep -E '__wrap_|__real_' >/dev/null; then
	iec_err "unresolved __wrap_/__real_ symbols"
fi

"$READELF" -d "$OUT" | grep -E 'NEEDED|SONAME' >&2

# Runtime subset: what an interactive first run needs (defaults.vim is sourced
# on startup, filetype/syntax right after). Full runtime is ~50 MB with doc/.
RT="$SRC/runtime"
if [ -f "$RT/defaults.vim" ]; then
	RTDIR="$IEC_ROOT/platform/ohos/har/src/main/resources/rawfile/tools"
	RTAR="$RTDIR/vim-runtime.tar"
	mkdir -p "$RTDIR"
	rm -f "$RTAR"
	tar -C "$RT" -cf "$RTAR" \
		defaults.vim filetype.vim ftplugin.vim indent.vim \
		colors/default.vim colors/lists/default.vim \
		autoload/dist/ft.vim \
		syntax/syntax.vim syntax/synload.vim syntax/syncolor.vim \
		syntax/nosyntax.vim \
		syntax/vim.vim syntax/c.vim syntax/cpp.vim syntax/sh.vim \
		syntax/make.vim syntax/python.vim syntax/diff.vim syntax/help.vim \
		syntax/conf.vim syntax/json.vim
	iec_log "vim-runtime.tar: $(du -h "$RTAR" | cut -f1)"
else
	iec_log "no runtime tree in $SRC -- vim-runtime.tar skipped"
fi

iec_log "vim ready: $OUT ($(du -h "$OUT" | cut -f1))"
