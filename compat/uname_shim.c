// compat/uname_shim.c — make uname(2) report "Linux" inside the sandbox.
//
// On this device `uname -s` answers "HarmonyOS" (the kernel is HongMeng, not
// Linux), and any tool that decides the host OS from uname then misidentifies
// the platform. The most direct victims: hvigor's isLinux() picking a .dylib
// that does not exist (see the parent tree's shim/os_shim.c for the full
// story), and kimi-code's install.sh, which errors out with "unsupported OS:
// HarmonyOS" before downloading anything.
//
// Why fix it here and not in every third-party script: guest programs run
// inside someone else's toolchain, and the toolchain should believe the host
// is Linux -- that is the sandbox's job. uname is the standard entry point
// for that question (configure, make, python's platform, node's os.type all
// go through it), so intercepting it once at link time is steadier than
// patching a pile of third-party code. Same `--wrap=<symbol>` convention the
// command builds already use for the rest of the shims.
//
// Trade-off: only sysname is rewritten; release/version/machine/nodename stay
// as the kernel reported them -- those are the truth about this machine and
// editing them would mislead (uname -m must keep saying aarch64). The honest
// cost is that a guest can no longer see the fact "HarmonyOS"; for debugging
// set BSH_UNAME_REAL=1 to get the real value back, or read /proc/version.
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

extern int __real_uname(struct utsname *buf);

int __wrap_uname(struct utsname *buf)
{
    int rc = __real_uname(buf);
    if (rc != 0 || buf == NULL) return rc;

    /* Escape hatch: set this to see the real value (HarmonyOS) again. */
    const char *real = getenv("BSH_UNAME_REAL");
    if (real && *real && real[0] != '0') return rc;

    strncpy(buf->sysname, "Linux", sizeof(buf->sysname) - 1);
    buf->sysname[sizeof(buf->sysname) - 1] = '\0';
    return rc;
}
